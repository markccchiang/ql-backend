/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "supervisor.hpp"
#include <ql/errors.hpp>
#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace qlpb = quantlib::v2;

namespace qlbackend {

    // -----------------------------------------------------------------------
    // SessionLog
    // -----------------------------------------------------------------------

    SessionLog::SessionLog(std::string sessionId, qlpb::OpenSession open)
    : sessionId_(std::move(sessionId)), open_(std::move(open)) {}


    void SessionLog::record(const qlpb::UpdateMarket& msg) {
        // Fold each update into the quote it targets. The log therefore holds
        // the session's current state rather than its history, which is what
        // keeps replay proportional to the number of distinct quotes instead
        // of to how long the user has been dragging a slider.
        std::map<std::string, int> index;
        for (int i = 0; i < open_.market_size(); ++i)
            if (open_.market(i).has_quote())
                index[open_.market(i).id()] = i;

        for (const auto& u : msg.quotes()) {
            auto it = index.find(u.quote_id());
            QL_REQUIRE(it != index.end(),
                       "update for quote '" << u.quote_id() << "' that the session never defined");
            open_.mutable_market(it->second)->mutable_quote()->set_value(u.value());
        }

        // Fixings are graph input the replay has to reproduce as well, and
        // they fold the same way: one series per index, one row per date, a
        // later value for a date superseding the earlier. Appending an
        // object per update instead would grow the replay by one per
        // keystroke, since the editor sends the whole series each time.
        for (const auto& f : msg.fixings()) {
            qlpb::FixingSeries* series = nullptr;
            for (int i = 0; i < open_.market_size() && series == nullptr; ++i)
                if (open_.market(i).has_fixings() &&
                    open_.market(i).fixings().index_id() == f.index_id())
                    series = open_.mutable_market(i)->mutable_fixings();
            if (series == nullptr) {
                // A fresh object needs an id the replay's duplicate check
                // will pass; the client's own ids are in the same namespace.
                std::string id = "fixings:" + f.index_id();
                for (int n = 2; std::any_of(open_.market().begin(), open_.market().end(),
                                            [&](const auto& obj) { return obj.id() == id; });
                     ++n)
                    id = "fixings:" + f.index_id() + ":" + std::to_string(n);
                auto* obj = open_.add_market();
                obj->set_id(id);
                series = obj->mutable_fixings();
                series->set_index_id(f.index_id());
            }
            for (const auto& row : f.fixings()) {
                const auto key = row.date().SerializeAsString();
                bool replaced = false;
                for (auto& held : *series->mutable_fixings()) {
                    if (held.date().SerializeAsString() == key) {
                        held.set_value(row.value());
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                    *series->add_fixings() = row;
            }
        }
    }


    std::optional<double> SessionLog::quoteValue(const std::string& quoteId) const {
        for (const auto& obj : open_.market())
            if (obj.id() == quoteId && obj.has_quote())
                return obj.quote().value();
        return std::nullopt;
    }


    std::vector<qlpb::ClientFrame> SessionLog::replayFrames() const {
        qlpb::ClientFrame frame;
        frame.set_session_id(sessionId_);
        *frame.mutable_open_session() = open_;
        return {frame};
    }


    // -----------------------------------------------------------------------
    // Supervisor
    // -----------------------------------------------------------------------

    Supervisor::Supervisor(ProcessHost& host,
                           FrameSink sink,
                           DisruptionSink disrupted,
                           DroppedSink dropped,
                           DeadlineTimer armTimer,
                           Options options)
    : host_(host), sink_(std::move(sink)), disrupted_(std::move(disrupted)),
      dropped_(std::move(dropped)), armTimer_(std::move(armTimer)), options_(options) {
        QL_REQUIRE(sink_, "a frame sink is required");
        QL_REQUIRE(disrupted_, "a disruption sink is required");
        QL_REQUIRE(dropped_, "a dropped-session sink is required");
        QL_REQUIRE(armTimer_, "a deadline timer is required");
        QL_REQUIRE(options_.stopGrace.count() >= 0, "the stop grace cannot be negative");
        QL_REQUIRE(options_.sessionsPerSharedWorker > 0,
                   "a shared worker must be able to hold at least one session");
    }


    Supervisor::Supervisor(ProcessHost& host,
                           FrameSink sink,
                           DisruptionSink disrupted,
                           DroppedSink dropped,
                           DeadlineTimer armTimer)
    : Supervisor(host, std::move(sink), std::move(disrupted), std::move(dropped),
                 std::move(armTimer), Options()) {}


    std::string Supervisor::acquireSeat(Placement placement) {
        if (placement == Placement::Shared) {
            // First worker with room. Filling one process before opening the
            // next keeps thread counts predictable; spreading sessions thinly
            // would only matter if workers competed for cores, and under
            // configuration C the shared pool runs sub-second work.
            for (auto& [workerId, worker] : workers_) {
                if (worker.placement == Placement::Shared &&
                    worker.sessions < options_.sessionsPerSharedWorker) {
                    ++worker.sessions;
                    return workerId;
                }
            }
        }

        auto workerId = host_.spawn(placement);
        QL_REQUIRE(workers_.find(workerId) == workers_.end(),
                   "process host returned worker id '" << workerId << "' twice");
        workers_.emplace(workerId, WorkerState{placement, 1});
        return workerId;
    }


    void Supervisor::releaseSeat(const std::string& workerId) {
        auto it = workers_.find(workerId);
        if (it == workers_.end())
            return; // the process is already gone; nothing to account for

        QL_REQUIRE(it->second.sessions > 0, "releasing a seat on an empty worker");
        if (--it->second.sessions > 0)
            return;

        host_.kill(workerId);
        workers_.erase(it);
    }


    void Supervisor::detach(const std::string& sessionId, SessionState& state) {
        qlpb::ClientFrame close;
        close.set_session_id(sessionId);
        close.mutable_close_session();
        host_.send(state.workerId, close);
        releaseSeat(state.workerId);
    }


    void Supervisor::rebuildOn(const std::string& sessionId, const SessionState& state) {
        for (const auto& f : state.log.replayFrames())
            host_.send(state.workerId, f);

        qlpb::ClientFrame frame;
        frame.set_session_id(sessionId);
        for (const auto& entry : state.pendingUpdates) {
            // request_id 0: the client has one answer coming for this write
            // and it comes from the seat the write was sent to. A second
            // would be a second terminal frame on one id.
            *frame.mutable_update_market() = entry.second;
            host_.send(state.workerId, frame);
        }
    }


    void Supervisor::killWorker(const std::string& workerId) {
        host_.kill(workerId);
        workers_.erase(workerId);
        recoverSessionsOn(workerId);
    }


    void Supervisor::recoverSessionsOn(const std::string& workerId) {
        // Collected first: replay() drops a session that fails twice, and
        // erasing from the map underneath the loop would invalidate it.
        std::vector<std::string> affected;
        for (const auto& [sessionId, state] : sessions_)
            if (state.workerId == workerId)
                affected.push_back(sessionId);

        for (const auto& sessionId : affected) {
            auto it = sessions_.find(sessionId);
            if (it == sessions_.end())
                continue;

            // A cancel whose grace was still running has been served the hard
            // way by the kill; its targets cannot come back.
            // Writes that were in flight died with the worker unacked, so
            // they never reached the graph the replay rebuilds.
            it->second.pendingUpdates.clear();

            terminateCancelTargets(sessionId, it->second);
            replay(sessionId, it->second);

            // Co-tenants lost their in-flight work to a kill they had nothing
            // to do with, and their session is back but their requests are
            // not. Only the gateway holds those request ids.
            disrupted_(sessionId);
        }
    }


    void Supervisor::emitError(const std::string& sessionId,
                               std::uint64_t requestId,
                               qlpb::Error::Code code,
                               const std::string& message) {
        qlpb::ServerFrame out;
        out.set_request_id(requestId);
        out.set_session_id(sessionId);
        out.set_terminal(true);
        out.mutable_error()->set_code(code);
        out.mutable_error()->set_message(message);
        sink_(out);
    }


    Supervisor::Placement Supervisor::placementFor(const qlpb::PriceRequest& msg) {
        switch (msg.engine().method()) {
            case qlpb::Engine::METHOD_MONTE_CARLO:
            case qlpb::Engine::METHOD_FINITE_DIFFERENCE:
                return Placement::Sacrificial;
            default:
                return Placement::Shared;
        }
    }


    Supervisor::Placement Supervisor::placementFor(const qlpb::PriceBatch& msg) {
        // One long calculation in the book decides for the whole of it: the
        // batch runs to completion on whichever seat it starts on, and a
        // Monte Carlo eleven trades in would otherwise hold a shared worker
        // for as long as it takes.
        for (const auto& request : msg.requests())
            if (placementFor(request) == Placement::Sacrificial)
                return Placement::Sacrificial;
        return Placement::Shared;
    }


    void Supervisor::openSession(const std::string& sessionId, const qlpb::ClientFrame& frame) {
        QL_REQUIRE(frame.has_open_session(), "expected an OpenSession frame");
        QL_REQUIRE(sessions_.find(sessionId) == sessions_.end(),
                   "session '" << sessionId << "' already exists");

        SessionState state(acquireSeat(Placement::Shared), Placement::Shared,
                           SessionLog(sessionId, frame.open_session()));
        host_.send(state.workerId, frame);
        sessions_.emplace(sessionId, std::move(state));
    }


    void Supervisor::dispatch(const std::string& sessionId, const qlpb::ClientFrame& frame) {
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            // A frame for a session that has been closed, or dropped after a
            // failed replay. The client is told so on the frame's own id
            // rather than having an exception thrown into the gateway's loop,
            // where there is nothing to attribute it to.
            emitError(sessionId, frame.request_id(), qlpb::Error::SESSION_NOT_FOUND,
                      "no session '" + sessionId + "'");
            return;
        }
        auto& state = it->second;

        if (frame.has_close_session()) {
            // Forwarded before the seat is given back, so the worker drops the
            // graph rather than being killed with it still open.
            host_.send(state.workerId, frame);
            releaseSeat(state.workerId);
            sessions_.erase(it);
            return;
        }

        if (frame.has_price() || frame.has_batch()) {
            const auto wanted =
                frame.has_batch() ? placementFor(frame.batch()) : placementFor(frame.price());
            // Not while a cancel is being served. The target is running on the
            // old seat and a CloseSession would queue behind it; it would
            // finish and answer, and a CANCELLED emitted here for the same
            // request would be a second terminal frame on one id. The kill
            // the grace may end in is addressed to the session's worker, and
            // moving the session first would point it at the new one. So the
            // request runs where the session is, once, and the next one
            // relocates: a long calculation on a shared seat for one round
            // is the cheaper wrong.
            const bool cancelling = !state.cancel.targets.empty();
            if (wanted != state.placement && !cancelling) {
                // Moving a session between processes is a replay, not a
                // migration: the graph is thread-bound and cannot be handed
                // over (DESIGN §2). Paying one bootstrap to isolate a long
                // calculation is the trade this design makes.
                detach(sessionId, state);
                // The writes still unacked are not cleared: the close queued
                // behind them, so the old seat applies and answers them, and
                // the log takes each on its Ack. The new seat gets them too.
                state.placement = wanted;
                state.workerId = acquireSeat(wanted);
                rebuildOn(sessionId, state);
            }
        }

        if (frame.has_update_market()) {
            // Held, not recorded. The log may only contain writes the graph
            // accepted, so this waits for the worker's Ack to arrive in
            // onRequestTerminated(). A write that fails validation, or that
            // dies with its worker, never enters the replayable state.
            state.pendingUpdates[frame.request_id()] = frame.update_market();
        }

        if (frame.has_price() && !frame.price().scenarios().empty()) {
            // A sweep that keeps its last value is an UpdateMarket the client
            // did not spell as one. Without this the live graph and the log
            // diverge: the worker holds the swept value, a replay after its
            // death rebuilds the old one, and the client's price silently
            // reverts. Parked like an UpdateMarket and folded in the same
            // place, on the sweep's own terminal frame.
            //
            // Per axis, because keep_final_value is per axis: a grid can leave
            // spot where it ended and put vol back.
            qlpb::UpdateMarket update;
            for (const auto& sc : frame.price().scenarios()) {
                if (!sc.keep_final_value())
                    continue;
                std::optional<double> last;
                switch (sc.points_case()) {
                    case qlpb::Scenario::kExplicit:
                        if (sc.explicit_().values_size() > 0)
                            last = sc.explicit_().values(sc.explicit_().values_size() - 1);
                        break;
                    case qlpb::Scenario::kLinear:
                        last = sc.linear().end();
                        break;
                    case qlpb::Scenario::kRelative:
                        if (sc.relative().factors_size() > 0)
                            if (auto base = state.log.quoteValue(sc.quote_id()))
                                last = *base *
                                       sc.relative().factors(sc.relative().factors_size() - 1);
                        break;
                    default:
                        break;
                }
                // A malformed axis is left for the worker to reject; nothing is
                // parked for it, so nothing can be recorded.
                if (last) {
                    auto* quote = update.add_quotes();
                    quote->set_quote_id(sc.quote_id());
                    quote->set_value(*last);
                }
            }
            if (update.quotes_size() > 0)
                state.pendingUpdates[frame.request_id()] = update;
        }

        host_.send(state.workerId, frame);
    }


    Supervisor::CancelOutcome Supervisor::cancel(const std::string& sessionId,
                                                 std::uint64_t targetRequestId) {
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end())
            return CancelOutcome::NoSession;
        auto& state = it->second;
        auto& pending = state.cancel;

        const auto known =
            std::find(pending.targets.begin(), pending.targets.end(), targetRequestId);
        if (known != pending.targets.end())
            return CancelOutcome::Stopping; // idempotent: a second click must not
                                            // shorten the grace or kill twice

        // Polite first, and aimed: only the named request on this session's
        // seat is asked to stop. A Monte Carlo running with progress enabled
        // checks between batches and stops there, terminating its own request
        // and keeping its session alive. This does nothing at all for an
        // engine called once, which is the common case, so the deferred kill
        // below is what has to work; the grace only buys the cheap outcome
        // when the engine happens to be able to offer it.
        switch (host_.requestStop(state.workerId, sessionId, targetRequestId)) {
            case ProcessHost::StopOutcome::NotFound:
                // Nothing on the seat to stop. Arming the kill anyway is what
                // used to disown every session on the worker 250 ms later for
                // a request that was never going to terminate there.
                return CancelOutcome::NotRunning;

            case ProcessHost::StopOutcome::Dequeued:
                // It will never run, so it will never answer, and nothing is
                // left to kill. A write that was parked for the log never
                // reached the graph and must not enter it either.
                state.pendingUpdates.erase(targetRequestId);
                emitError(sessionId, targetRequestId, qlpb::Error::CANCELLED,
                          "cancelled by client before it started");
                return CancelOutcome::Dequeued;

            case ProcessHost::StopOutcome::Running:
                break;
        }

        pending.targets.push_back(targetRequestId);

        if (pending.timerArmed)
            return CancelOutcome::Stopping; // one deadline per round; the kill
                                            // serves every target on it

        pending.timerArmed = true;
        const auto generation = state.cancelGeneration;
        armTimer_(options_.stopGrace,
                  [this, sessionId, generation] { onStopGraceExpired(sessionId, generation); });
        return CancelOutcome::Stopping;
    }


    void Supervisor::onRequestTerminated(const std::string& sessionId,
                                         const qlpb::ServerFrame& frame) {
        const auto requestId = frame.request_id();

        auto it = sessions_.find(sessionId);
        if (it == sessions_.end())
            return;
        auto& state = it->second;

        // An UpdateMarket coming back. Only an Ack means the graph took it, so
        // only an Ack advances the replayable state; anything else drops the
        // write and leaves the log describing what the worker actually holds.
        auto update = state.pendingUpdates.find(requestId);
        if (update != state.pendingUpdates.end()) {
            // A kept sweep terminates as a ScenarioResult rather than an Ack,
            // and that is its success frame.
            if (frame.has_ack() || frame.has_scenario_result())
                state.log.record(update->second);
            state.pendingUpdates.erase(update);
        }

        auto& pending = state.cancel;

        auto target = std::find(pending.targets.begin(), pending.targets.end(), requestId);
        if (target == pending.targets.end())
            return; // an ordinary request finishing, or one the kill already terminated

        // It ended by itself inside the grace, so the worker is healthy and
        // the client has its terminal frame already: nothing left to kill.
        pending.targets.erase(target);
        if (!pending.targets.empty())
            return;

        // The timer cannot be unarmed — the gateway's loop owns it — so the
        // round is closed by generation instead and the callback fires into
        // nothing.
        ++state.cancelGeneration;
        pending.timerArmed = false;
    }


    void Supervisor::onStopGraceExpired(const std::string& sessionId, std::uint64_t generation) {
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end())
            return; // session dropped while the timer was running
        auto& state = it->second;

        if (generation != state.cancelGeneration || state.cancel.targets.empty())
            return; // stale timer, or the polite stop won the race

        // Grace spent with the request still running. Killing the process is
        // the only guaranteed stop QuantLib leaves us (DESIGN §3), and under
        // configuration C a long calculation is alone in its process precisely
        // so that this takes nothing else down. On a shared worker it would,
        // which is what killWorker() then has to put back.
        killWorker(state.workerId);
    }


    void Supervisor::terminateCancelTargets(const std::string& sessionId, SessionState& state) {
        for (const auto requestId : state.cancel.targets)
            emitError(sessionId, requestId, qlpb::Error::CANCELLED, "cancelled by client");
        state.cancel.targets.clear();
        state.cancel.timerArmed = false;
        ++state.cancelGeneration;
    }


    void Supervisor::onWorkerDied(const std::string& workerId) {
        // Same recovery as a kill we asked for, minus the kill. Sessions that
        // shared the process are replayed too; under configuration C that is
        // only ever the shared pool, where the work in flight is sub-second
        // (DESIGN §2.1).
        workers_.erase(workerId);
        recoverSessionsOn(workerId);
    }


    void Supervisor::onSessionDied(const std::string& sessionId) {
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end())
            return; // closed before the report arrived
        auto& state = it->second;

        // The seat is already empty on the host's side; this is the count
        // catching up, and it is what retires the process once nothing is
        // left on it.
        releaseSeat(state.workerId);

        // Whatever was queued behind the failing request went with the graph:
        // writes unacked, cancel targets unreachable.
        state.pendingUpdates.clear();
        terminateCancelTargets(sessionId, state);
        replay(sessionId, state);

        // Only the gateway knows which request ids were queued on it.
        if (sessions_.count(sessionId) > 0)
            disrupted_(sessionId);
    }


    void Supervisor::replay(const std::string& sessionId, SessionState& state) {
        if (state.replayFailures >= 2) {
            // A request that reliably kills its worker would otherwise become
            // a respawn loop. Report it terminally and drop the session.
            // Not the answer to any request, so request_id stays 0 and
            // session_id is the only thing that identifies what was lost.
            emitError(sessionId, 0, qlpb::Error::WORKER_DIED,
                      "session '" + sessionId + "' could not be replayed and has been dropped");
            sessions_.erase(sessionId);
            dropped_(sessionId);
            return;
        }

        try {
            state.workerId = acquireSeat(state.placement);
            rebuildOn(sessionId, state);
            state.replayFailures = 0;
        } catch (const std::exception&) {
            // The seat was counted before the send threw, and on a shared
            // worker that count is a thread nobody is using until it is
            // given back.
            releaseSeat(state.workerId);
            ++state.replayFailures;
            replay(sessionId, state);
        }
    }

}
