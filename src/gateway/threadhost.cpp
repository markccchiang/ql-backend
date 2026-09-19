/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "threadhost.hpp"
#include "session/worker.hpp"
#include <ql/errors.hpp>
#include <memory>
#include <thread>
#include <utility>

namespace qlpb = quantlib::v2;

namespace qlbackend {

    ThreadProcessHost::ThreadProcessHost(Post post, FrameSink frames, SessionSink died)
    : post_(std::move(post)), frames_(std::move(frames)), died_(std::move(died)) {
        QL_REQUIRE(post_, "a post callback is required");
        QL_REQUIRE(frames_, "a frame sink is required");
        QL_REQUIRE(died_, "a session death sink is required");
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

            // Same road as the frames, so it lands after the terminal frame
            // of the request that dirtied the graph and before anything the
            // worker answers from its queue afterwards. Once it has landed
            // the seat is disowned, and those later answers -- every one a
            // SESSION_NOT_FOUND for a graph that is no longer there -- are
            // dropped rather than delivered under a session that has just
            // been replayed elsewhere.
            auto died = [this, alive, workerId, sessionId] {
                post_([this, alive, workerId, sessionId] {
                    if (alive->load())
                        onSeatDied(workerId, sessionId);
                });
            };

            Seat fresh;
            fresh.alive = alive;
            fresh.worker = std::make_unique<Worker>(sessionId, std::move(sink), std::move(died));
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


    ThreadProcessHost::StopOutcome ThreadProcessHost::requestStop(const std::string& workerId,
                                                                  const std::string& sessionId,
                                                                  std::uint64_t requestId) {
        // One session's seat, and within it one request. The seats around it
        // belong to other sessions, often other clients, and a stop meant for
        // this one is not theirs to take.
        auto process = processes_.find(workerId);
        if (process == processes_.end())
            return StopOutcome::NotFound;
        auto seat = process->second.seats.find(sessionId);
        if (seat == process->second.seats.end())
            return StopOutcome::NotFound;

        switch (seat->second.worker->requestStop(requestId)) {
            case Worker::StopOutcome::Running:
                return StopOutcome::Running;
            case Worker::StopOutcome::Dequeued:
                return StopOutcome::Dequeued;
            case Worker::StopOutcome::NotFound:
                break;
        }
        return StopOutcome::NotFound;
    }


    void ThreadProcessHost::kill(const std::string& workerId) {
        auto process = processes_.find(workerId);
        if (process == processes_.end())
            return;

        for (auto& [sessionId, seat] : process->second.seats) {
            // The honest part of a kill we cannot perform: ask, disown, and
            // let it finish into a void. The supervisor has already decided
            // this session's requests are over and will replay it elsewhere,
            // so nothing still queued on the seat is worth starting.
            seat.worker->abandon();
            seat.alive->store(false);
            reap(std::move(seat));
        }
        processes_.erase(process);
    }


    void ThreadProcessHost::onSeatDied(const std::string& workerId, const std::string& sessionId) {
        auto process = processes_.find(workerId);
        if (process == processes_.end())
            return;
        auto seat = process->second.seats.find(sessionId);
        if (seat == process->second.seats.end())
            return;

        // The seat goes, the process stays: its other seats are healthy, and
        // the supervisor retires an empty process through kill() once its
        // own count says so. Two views of one fact, kept in step by both
        // moving one seat at a time.
        auto dead = std::move(seat->second);
        process->second.seats.erase(seat);
        dead.alive->store(false);
        reap(std::move(dead));

        died_(sessionId);
    }


    void ThreadProcessHost::reap(Seat seat) {
        // ~Worker shuts the queue down and joins, which blocks until the
        // request in flight finishes. That wait belongs on a thread of its
        // own, never on the event loop.
        std::thread([seat = std::move(seat)]() mutable { seat.worker.reset(); }).detach();
    }

}
