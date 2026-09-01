/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file threadhost.hpp
    \brief a ProcessHost whose "processes" are groups of threads in this process
*/

#ifndef qlservice_gateway_threadhost_hpp
#define qlservice_gateway_threadhost_hpp

#include "session/supervisor.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace qlservice {

    class Worker;

    //! Runs workers as threads in the gateway process instead of as processes.
    /*! The staging host. It makes the gateway, the supervisor, the session
        graph and the registry run end to end in one binary, which is where
        every behaviour that is not process control lives.

        \warning It cannot honour a kill. A thread inside a Monte Carlo cannot
                 be stopped from outside — that is the whole argument of
                 DESIGN §3 — so kill() here asks politely, abandons the worker
                 and drops whatever it says afterwards. The calculation keeps
                 running on a detached thread until it finishes. Cancel is
                 therefore best-effort under this host: it works for a Monte
                 Carlo with progress enabled and for nothing else. A fork-based
                 host is what makes cancel-by-kill real.

        Every method is called on the gateway loop and touches no lock. Frames
        leaving a worker arrive on the worker's own thread and are marshalled
        back onto the loop through `post` before they reach anything
        (DESIGN §9.1).

        Sessions are QL_ENABLE_SESSIONS-isolated per thread, so N sessions in
        one "process" here is exactly as correct as N sessions in one real
        worker process (DESIGN §2).
    */
    class ThreadProcessHost : public Supervisor::ProcessHost {
      public:
        //! Runs a callback on the gateway loop. Safe to call from any thread.
        using Post = std::function<void(std::function<void()>)>;

        //! Emits a frame a worker produced. Always called on the loop.
        using FrameSink = std::function<void(const quantlib::v2::ServerFrame&)>;

        ThreadProcessHost(Post post, FrameSink frames);
        ~ThreadProcessHost() override;

        ThreadProcessHost(const ThreadProcessHost&) = delete;
        ThreadProcessHost& operator=(const ThreadProcessHost&) = delete;

        std::string spawn(Supervisor::Placement placement) override;
        void send(const std::string& workerId, const quantlib::v2::ClientFrame& frame) override;
        void requestStop(const std::string& workerId) override;
        void kill(const std::string& workerId) override;

      private:
        //! One session's thread, plus the flag that disowns its output.
        /*! A killed worker cannot be stopped, so it is disowned instead: the
            flag goes false and every frame it emits afterwards is dropped on
            the loop. Without it, a Monte Carlo abandoned by a cancel would
            deliver its result minutes later, to a request the supervisor has
            already terminated with CANCELLED.
        */
        struct Seat {
            std::unique_ptr<Worker> worker;
            std::shared_ptr<std::atomic<bool>> alive;
        };

        struct Process {
            Supervisor::Placement placement = Supervisor::Placement::Shared;
            std::map<std::string, Seat> seats; //!< by session_id
        };

        //! Destroys a worker off the loop, because ~Worker joins its thread.
        /*! Joining here would stall the event loop for the length of whatever
            the worker is computing.
        */
        static void reap(Seat seat);

        Post post_;
        FrameSink frames_;
        std::map<std::string, Process> processes_;
        std::uint64_t nextWorkerId_ = 0;
    };

}

#endif
