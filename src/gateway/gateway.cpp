/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "gateway.hpp"
#include "App.h"
#include "quantlib/v2/envelope.pb.h"
#include "session/capabilities.hpp"
#include "session/supervisor.hpp"
#include "threadhost.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace qlpb = quantlib::v2;

namespace qlservice {

    namespace {

        //! Per-connection state uWebSockets carries for us.
        struct SocketData {
            std::uint64_t id = 0;
        };

        using WS = uWS::WebSocket<false, true, SocketData>;

        void logf(const char* what, const std::string& detail) {
            std::fprintf(stderr, "[qlservice] %s: %s\n", what, detail.c_str());
        }

    }


    struct Gateway::Impl {
        explicit Impl(Options o) : options(o) {}

        // -------------------------------------------------------------------
        // State. All of it lives on the loop thread and none of it is locked.
        // -------------------------------------------------------------------

        Options options;

        std::unique_ptr<ThreadProcessHost> host;
        std::unique_ptr<Supervisor> supervisor;

        struct Conn {
            WS* ws = nullptr;
            //! Sessions still open on this socket, closed when it goes.
            std::set<std::string> openSessions;
        };

        std::map<std::uint64_t, Conn> conns;

        //! session_id -> the connection its frames go out on.
        /*! Outlives the session itself: a CloseSession still has an Ack to
            deliver, so the route is dropped when the socket goes, not when the
            session does.
        */
        std::map<std::string, std::uint64_t> sessionConn;

        //! Requests the client is still owed a terminal frame for.
        /*! The supervisor deliberately does not know request ids — it reports
            a disrupted session and nothing more — so failing what was in
            flight after a kill is only possible from here (DESIGN §9.5).
        */
        std::map<std::string, std::set<std::uint64_t>> outstanding;

        std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> deadlines;

        std::uint64_t nextConnId = 0;
        std::uint64_t nextSessionId = 0;

        //! The socket whose frame is being handled, if any.
        /*! A rejection often names a session that has no route — one that was
            never opened, one that belongs to another connection, or a frame
            that did not parse at all. Routing by session alone would leave
            those undeliverable, and the client waiting forever for the
            terminal frame every request is promised. The loop handles one
            inbound frame at a time, so this is unambiguous while it is set.
        */
        WS* inbound = nullptr;

        // -------------------------------------------------------------------
        // Bookkeeping
        // -------------------------------------------------------------------

        void track(const std::string& sessionId, std::uint64_t requestId) {
            outstanding[sessionId].insert(requestId);
        }

        void untrack(const std::string& sessionId, std::uint64_t requestId) {
            auto it = outstanding.find(sessionId);
            if (it == outstanding.end())
                return;
            it->second.erase(requestId);
            if (it->second.empty())
                outstanding.erase(it);
        }

        void emit(const std::string& sessionId,
                  std::uint64_t requestId,
                  const std::function<void(qlpb::ServerFrame&)>& fill) {
            qlpb::ServerFrame out;
            out.set_request_id(requestId);
            out.set_session_id(sessionId);
            out.set_terminal(true);
            fill(out);
            deliver(out);
        }

        void fail(const std::string& sessionId,
                  std::uint64_t requestId,
                  qlpb::Error::Code code,
                  const std::string& message) {
            emit(sessionId, requestId, [&](qlpb::ServerFrame& out) {
                out.mutable_error()->set_code(code);
                out.mutable_error()->set_message(message);
            });
        }

        // -------------------------------------------------------------------
        // Outbound
        // -------------------------------------------------------------------

        void deliver(const qlpb::ServerFrame& frame) {
            if (frame.terminal())
                untrack(frame.session_id(), frame.request_id());

            WS* ws = nullptr;
            auto owner = sessionConn.find(frame.session_id());
            if (owner != sessionConn.end()) {
                auto conn = conns.find(owner->second);
                if (conn != conns.end())
                    ws = conn->second.ws;
            } else {
                // No route: a rejection for a session this connection does not
                // own, or none at all. It still has to reach whoever asked.
                ws = inbound;
            }
            if (ws == nullptr)
                return; // the socket that asked for this is gone

            // Progress is the only frame class we may drop, and dropping it
            // ourselves is what stops uWebSockets from dropping a terminal one
            // silently once the buffer fills (DESIGN §9.3). One in flight per
            // connection: every Progress supersedes the last.
            if (!frame.terminal() && frame.has_progress() && ws->getBufferedAmount() > 0)
                return;

            std::string payload;
            if (!frame.SerializeToString(&payload)) {
                logf("serialization failed", frame.session_id());
                return;
            }

            const auto status = ws->send(payload, uWS::OpCode::BINARY);
            if (status == WS::DROPPED && frame.terminal()) {
                // The policy above failed: a client has been left waiting for
                // a result that was computed. Loud, because it breaks the
                // exactly-once terminal guarantee of DESIGN §3.
                logf("terminal frame dropped by backpressure",
                     frame.session_id() + " request " + std::to_string(frame.request_id()));
            }
        }

        //! A frame produced by a worker, marshalled onto the loop.
        void onWorkerFrame(const qlpb::ServerFrame& frame) {
            if (frame.request_id() == 0) {
                // A replay's own answer. The supervisor rebuilds a session by
                // re-sending its OpenSession with no request id, and a replay
                // is invisible to the client by design (DESIGN §2.1).
                return;
            }

            // Every terminal frame from a worker, cancel target or not: it is
            // how the session log advances and how a polite cancel is told
            // from one that needs a kill (DESIGN §2.1). Frames the supervisor
            // emits itself arrive through its own sink and must not come back
            // to it (DESIGN §9.5).
            if (frame.terminal())
                supervisor->onRequestTerminated(frame.session_id(), frame);

            deliver(frame);
        }

        // -------------------------------------------------------------------
        // Inbound
        // -------------------------------------------------------------------

        void onMessage(WS* ws, std::string_view message, uWS::OpCode opCode) {
            inbound = ws;
            struct Clear {
                WS** slot;
                ~Clear() { *slot = nullptr; }
            } clear{&inbound};

            if (opCode != uWS::OpCode::BINARY) {
                fail({}, 0, qlpb::Error::INVALID_ARGUMENT, "frames must be binary");
                return;
            }

            qlpb::ClientFrame frame;
            if (!frame.ParseFromArray(message.data(), static_cast<int>(message.size()))) {
                // No request id to attribute it to: the frame did not parse.
                fail({}, 0, qlpb::Error::INVALID_ARGUMENT, "malformed ClientFrame");
                return;
            }
            handle(ws, frame);
        }

        void handle(WS* ws, qlpb::ClientFrame& frame) {
            const auto connId = ws->getUserData()->id;
            auto& conn = conns[connId];

            if (frame.has_open_session()) {
                // Server-minted: a client-chosen id would let two connections
                // collide on one graph (DESIGN §9.5).
                const auto sessionId = "s-" + std::to_string(++nextSessionId);
                frame.set_session_id(sessionId);
                sessionConn[sessionId] = connId;
                conn.openSessions.insert(sessionId);
                track(sessionId, frame.request_id());

                try {
                    supervisor->openSession(sessionId, frame);
                } catch (const std::exception& e) {
                    fail(sessionId, frame.request_id(), qlpb::Error::INVALID_ARGUMENT, e.what());
                }
                return;
            }

            if (frame.has_hello()) {
                // Answered here and never forwarded: what the build can price
                // is a property of the service, not of a session, and a client
                // has to be able to ask before it has opened one.
                emit("", frame.request_id(),
                     [](qlpb::ServerFrame& out) { fillCapabilities(*out.mutable_capabilities()); });
                return;
            }

            const std::string sessionId = frame.session_id();
            auto owner = sessionConn.find(sessionId);
            if (sessionId.empty() || owner == sessionConn.end() || owner->second != connId) {
                // Includes a session belonging to someone else's connection,
                // which is the same answer as one that never existed.
                fail(sessionId, frame.request_id(), qlpb::Error::SESSION_NOT_FOUND,
                     "no session '" + sessionId + "' on this connection");
                return;
            }

            if (frame.has_cancel()) {
                try {
                    supervisor->cancel(sessionId, frame);
                } catch (const std::exception& e) {
                    fail(sessionId, frame.request_id(), qlpb::Error::INVALID_ARGUMENT, e.what());
                    return;
                }
                // The cancel is itself a request and nothing downstream
                // answers it: the supervisor acts on the target and never
                // forwards this frame to a worker. Its id terminates here.
                emit(sessionId, frame.request_id(),
                     [](qlpb::ServerFrame& out) { out.mutable_ack(); });
                return;
            }

            if (frame.has_close_session())
                conn.openSessions.erase(sessionId);

            track(sessionId, frame.request_id());
            try {
                supervisor->dispatch(sessionId, frame);
            } catch (const std::exception& e) {
                fail(sessionId, frame.request_id(), qlpb::Error::INVALID_ARGUMENT, e.what());
            }
        }

        void onClose(std::uint64_t connId) {
            auto conn = conns.find(connId);
            if (conn == conns.end())
                return;

            // A closed socket closes every session on it (DESIGN §9.4). The
            // definition lives only here, and the client holds results rather
            // than definitions, so there is nothing to resume into.
            const auto sessions = conn->second.openSessions;
            for (const auto& sessionId : sessions) {
                qlpb::ClientFrame close;
                close.set_session_id(sessionId);
                close.mutable_close_session();
                try {
                    supervisor->dispatch(sessionId, close);
                } catch (const std::exception& e) {
                    logf("close on disconnect failed", e.what());
                }
                outstanding.erase(sessionId);
            }

            for (auto it = sessionConn.begin(); it != sessionConn.end();)
                it = (it->second == connId) ? sessionConn.erase(it) : std::next(it);

            conns.erase(conn);
        }

        // -------------------------------------------------------------------
        // Deadlines
        // -------------------------------------------------------------------

        void runDueDeadlines() {
            const auto now = std::chrono::steady_clock::now();
            while (!deadlines.empty() && deadlines.begin()->first <= now) {
                auto callback = std::move(deadlines.begin()->second);
                deadlines.erase(deadlines.begin());
                callback(); // may arm another round
            }
        }
    };


    Gateway::Gateway(Options options) : impl_(std::make_unique<Impl>(options)) {}

    Gateway::Gateway() : Gateway(Options()) {}

    Gateway::~Gateway() = default;


    bool Gateway::run() {
        auto& impl = *impl_;
        auto* loop = uWS::Loop::get();

        impl.host = std::make_unique<ThreadProcessHost>(
            [loop](std::function<void()> fn) { loop->defer(std::move(fn)); },
            [&impl](const qlpb::ServerFrame& frame) { impl.onWorkerFrame(frame); });

        impl.supervisor = std::make_unique<Supervisor>(
            *impl.host,
            // Frames the supervisor raises itself: already its own outcome,
            // so they go straight out and are never reported back to it.
            [&impl](const qlpb::ServerFrame& frame) { impl.deliver(frame); },
            [&impl](const std::string& sessionId) {
                // The session survived and was replayed; the requests that
                // were running on it did not (DESIGN §2.1).
                auto it = impl.outstanding.find(sessionId);
                if (it == impl.outstanding.end())
                    return;
                const auto ids = it->second;
                for (const auto requestId : ids)
                    impl.fail(sessionId, requestId, qlpb::Error::WORKER_DIED,
                              "worker died; the session was replayed and this request was lost");
            },
            [&impl](std::chrono::milliseconds delay, std::function<void()> callback) {
                impl.deadlines.emplace(std::chrono::steady_clock::now() + delay,
                                       std::move(callback));
            });

        auto* timer = us_create_timer(reinterpret_cast<us_loop_t*>(loop), 0, sizeof(Impl*));
        *reinterpret_cast<Impl**>(us_timer_ext(timer)) = &impl;
        const auto tick = static_cast<int>(impl.options.deadlineTick.count());
        us_timer_set(
            timer,
            [](us_timer_t* t) { (*reinterpret_cast<Impl**>(us_timer_ext(t)))->runDueDeadlines(); },
            tick, tick);

        uWS::App::WebSocketBehavior<SocketData> behavior;
        behavior.compression = uWS::DISABLED;
        behavior.maxPayloadLength = impl.options.maxPayloadBytes;
        behavior.idleTimeout = impl.options.idleTimeoutSeconds;
        behavior.maxBackpressure = impl.options.maxBackpressureBytes;
        behavior.closeOnBackpressureLimit = true;
        behavior.sendPingsAutomatically = true;

        behavior.open = [&impl](WS* ws) {
            const auto id = ++impl.nextConnId;
            ws->getUserData()->id = id;
            impl.conns[id].ws = ws;
        };
        behavior.message = [&impl](WS* ws, std::string_view message, uWS::OpCode opCode) {
            impl.onMessage(ws, message, opCode);
        };
        behavior.dropped = [](WS*, std::string_view message, uWS::OpCode) {
            logf("outbound frame dropped", std::to_string(message.size()) + " bytes");
        };
        behavior.close = [&impl](WS* ws, int, std::string_view) {
            impl.onClose(ws->getUserData()->id);
        };

        bool listening = false;
        uWS::App app;
        app.ws<SocketData>("/*", std::move(behavior))
            .listen(impl.options.host, impl.options.port,
                    [&](us_listen_socket_t* token) { listening = token != nullptr; });

        if (!listening) {
            us_timer_close(timer);
            return false;
        }

        app.run();

        us_timer_close(timer);
        return true;
    }

}
