/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "threadhost.hpp"
#include "session/worker.hpp"
#include <ql/errors.hpp>
#include <memory>
#include <thread>
#include <utility>

namespace qlpb = quantlib::v2;

namespace qlservice {

    ThreadProcessHost::ThreadProcessHost(Post post, FrameSink frames)
    : post_(std::move(post)), frames_(std::move(frames)) {
        QL_REQUIRE(post_, "a post callback is required");
        QL_REQUIRE(frames_, "a frame sink is required");
    }


    ThreadProcessHost::~ThreadProcessHost() {
        // Disowned first, then destroyed: the loop is gone by now, so a frame
        // posted from a worker thread would have nowhere to land.
        for (auto& [workerId, process] : processes_) {
            for (auto& [sessionId, seat] : process.seats)
                seat.alive->store(false);
        }
    }


    std::string ThreadProcessHost::spawn(Supervisor::Placement placement) {
        auto workerId = "w-" + std::to_string(++nextWorkerId_);
        processes_[workerId].placement = placement;
        return workerId;
    }


    void ThreadProcessHost::send(const std::string& workerId, const qlpb::ClientFrame& frame) {
        auto process = processes_.find(workerId);
        if (process == processes_.end())
            return; // killed underneath us; the supervisor is already replaying

        const auto& sessionId = frame.session_id();
        QL_REQUIRE(!sessionId.empty(), "a frame reached the host with no session_id to route on");

        auto seat = process->second.seats.find(sessionId);
        if (seat == process->second.seats.end()) {
            // First frame for this session on this process. It is an
            // OpenSession, either the client's or a replay of one; anything
            // else is a frame for a session that is no longer here, and the
            // worker would answer SESSION_NOT_FOUND to a request the
            // supervisor thinks it placed.
            QL_REQUIRE(frame.has_open_session(), "first frame for session '"
                                                     << sessionId << "' on worker '" << workerId
                                                     << "' is not an OpenSession");

            auto alive = std::make_shared<std::atomic<bool>>(true);
            // The sink runs on the worker's thread. It copies the frame and
            // hands it to the loop, which is the only thread allowed to touch
            // the gateway's or the supervisor's state (DESIGN §9.1).
            auto sink = [this, alive](const qlpb::ServerFrame& out) {
                auto copy = std::make_shared<qlpb::ServerFrame>(out);
                post_([this, alive, copy] {
                    if (alive->load())
                        frames_(*copy);
                });
            };

            Seat fresh;
            fresh.alive = alive;
            fresh.worker = std::make_unique<Worker>(sessionId, std::move(sink));
            fresh.worker->start();
            seat = process->second.seats.emplace(sessionId, std::move(fresh)).first;
        }

        seat->second.worker->submit(frame);

        if (frame.has_close_session()) {
            // Submitted, not dropped: the worker still has to answer the Ack,
            // and it drains its queue before its thread exits. The seat is
            // handed to the reaper still alive so that answer gets out.
            auto closing = std::move(seat->second);
            process->second.seats.erase(seat);
            reap(std::move(closing));
        }
    }


    void ThreadProcessHost::requestStop(const std::string& workerId) {
        auto process = processes_.find(workerId);
        if (process == processes_.end())
            return;

        // Every session on the process, because the supervisor addresses a
        // worker rather than a session. Under configuration C anything worth
        // stopping is alone in its process anyway (DESIGN §2.1).
        for (auto& [sessionId, seat] : process->second.seats)
            seat.worker->requestStop();
    }


    void ThreadProcessHost::kill(const std::string& workerId) {
        auto process = processes_.find(workerId);
        if (process == processes_.end())
            return;

        for (auto& [sessionId, seat] : process->second.seats) {
            // The honest part of a kill we cannot perform: ask, disown, and
            // let it finish into a void. The supervisor has already decided
            // this session's requests are over and will replay it elsewhere.
            seat.worker->requestStop();
            seat.alive->store(false);
            reap(std::move(seat));
        }
        processes_.erase(process);
    }


    void ThreadProcessHost::reap(Seat seat) {
        // ~Worker shuts the queue down and joins, which blocks until the
        // request in flight finishes. That wait belongs on a thread of its
        // own, never on the event loop.
        std::thread([seat = std::move(seat)]() mutable { seat.worker.reset(); }).detach();
    }

}
