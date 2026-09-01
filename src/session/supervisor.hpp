/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file supervisor.hpp
    \brief session log, worker pool and placement, cancel-by-kill with replay
*/

#ifndef qlservice_session_supervisor_hpp
#define qlservice_session_supervisor_hpp

#include "quantlib/v2/envelope.pb.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace qlservice {

    //! The replayable definition of one session.
    /*! A session's graph is fully determined by its OpenSession frame plus the
        UpdateMarket frames that followed, so the supervisor can rebuild it in
        a fresh worker after a kill (DESIGN §2.1). This holds that definition
        in compacted form.

        Compaction is what keeps replay bounded: a later QuoteUpdate supersedes
        an earlier one for the same quote_id, so the log stays at OpenSession
        plus one entry per distinct quote however long the user drags a slider.

        The invariant this depends on: nothing that affects a price may enter a
        worker outside these frames. A session that reads a worker-local clock,
        or takes its evaluation date from anywhere but OpenSession, is not
        replayable and the guarantee silently stops holding.
    */
    class SessionLog {
      public:
        SessionLog(std::string sessionId, quantlib::v2::OpenSession open);

        //! Folds an applied UpdateMarket into the compacted state.
        /*! Called only after the worker acked it, so the log never contains a
            write the graph rejected.
        */
        void record(const quantlib::v2::UpdateMarket& msg);

        //! The frames that rebuild this session, in order.
        /*! One OpenSession carrying the current quote values, and nothing
            else: replaying the update history frame by frame would reach the
            same graph but pay for every intermediate state.

            Each frame carries the session_id, because that is what a process
            host routes on: a replay frame without it reaches the worker
            process but no thread inside it.
        */
        std::vector<quantlib::v2::ClientFrame> replayFrames() const;

        std::size_t marketSize() const { return open_.market_size(); }

      private:
        std::string sessionId_;
        quantlib::v2::OpenSession open_;
    };


    //! Placement and lifecycle for the workers behind the sessions.
    /*! Implements configuration C from DESIGN §2.1: cheap interactive work
        shares a multi-threaded worker process, while long cancellable work
        gets a single-session process that can be killed without collateral.

        The packing is this class's job, not the process host's. Which shared
        worker a session lands on, how many it will hold, and when an empty
        process is retired are placement policy, and keeping them here is what
        makes them testable against a fake host rather than against forks.
    */
    class Supervisor {
      public:
        //! Where a session's next request will run.
        enum class Placement {
            Shared,     //!< a thread in a multi-session process
            Sacrificial //!< sole occupant of a process, safe to kill
        };

        //! Abstracts process control so this layer stays testable.
        /*! Mechanism only: it starts, feeds and stops processes, and decides
            none of the placement. A real implementation forks a worker binary
            and talks to it over a pipe or socket; the tests substitute
            in-process threads.
        */
        class ProcessHost {
          public:
            virtual ~ProcessHost() = default;

            //! Starts a new worker process and returns its id.
            /*! Always a fresh process, and always a fresh id: the supervisor
                calls this only once it has decided that no existing worker
                can take the session. Returning an id it has handed out before
                would silently merge two workers' bookkeeping, so it is
                rejected.
            */
            virtual std::string spawn(Placement placement) = 0;

            //! Delivers one frame to a worker, routed by frame.session_id().
            /*! A shared worker hosts several sessions, so the id in the frame
                is what picks the thread inside it.
            */
            virtual void send(const std::string& workerId,
                              const quantlib::v2::ClientFrame& frame) = 0;
            //! Best-effort stop between Monte Carlo batches.
            virtual void requestStop(const std::string& workerId) = 0;
            //! Unconditional, and takes every session on the process with it.
            /*! The only guaranteed way to stop a calculation.
             */
            virtual void kill(const std::string& workerId) = 0;
        };

        using FrameSink = std::function<void(const quantlib::v2::ServerFrame&)>;

        //! Reports that a session's worker died under it.
        /*! Called once per affected session after a kill, and after any frame
            the supervisor itself emits for that session. Requests in flight on
            a killed process never produce a terminal frame, and their ids are
            known only to the gateway, so it is the gateway that has to fail
            whatever is still outstanding on the session — Error{WORKER_DIED},
            or CANCELLED for the request the client asked to stop.

            The session itself survives: it has already been replayed into a
            fresh worker by the time this is called.
        */
        using DisruptionSink = std::function<void(const std::string& sessionId)>;

        //! Arms a one-shot timer on the event loop that drives this object.
        /*! The supervisor owns no thread and no clock. Every entry point is
            called from the gateway's loop, and the deferred half of a cancel
            has to arrive the same way: the callback must be invoked on that
            loop, not from a timer thread, or the state below needs a lock it
            deliberately does not have.

            There is no unarm. A timer that is no longer wanted fires and finds
            nothing to do, which is why cancel rounds carry a generation.
        */
        using DeadlineTimer = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

        struct Options {
            //! How long a polite stop is given before the worker is killed.
            /*! Long enough for a worker to reach the end of a Monte Carlo
                batch, short enough that a Stop button on an uninterruptible
                engine still feels immediate.
            */
            std::chrono::milliseconds stopGrace{250};

            //! Sessions packed into one shared worker process.
            /*! One session is one thread (DESIGN §2), so this is a thread
                count per process and should be sized against cores rather
                than against how many sessions the frontend opens: a panel
                that fans out a smile opens a session per strike, and the
                excess queues on the pool instead of oversubscribing it.

                It does not apply to sacrificial workers, which are always
                sole occupants — that is what makes them safe to kill.
            */
            std::size_t sessionsPerSharedWorker = 8;
        };

        Supervisor(ProcessHost& host,
                   FrameSink sink,
                   DisruptionSink disrupted,
                   DeadlineTimer armTimer,
                   Options options);

        //! Same, with the default Options.
        /*! A separate overload rather than a default argument: Options carries
            member initializers, which are not yet available at this point of
            the enclosing class definition.
        */
        Supervisor(ProcessHost& host,
                   FrameSink sink,
                   DisruptionSink disrupted,
                   DeadlineTimer armTimer);

        void openSession(const std::string& sessionId, const quantlib::v2::ClientFrame& frame);

        void dispatch(const std::string& sessionId, const quantlib::v2::ClientFrame& frame);

        //! Serves a CancelRequest: stop politely, then kill after the grace.
        /*! Returns as soon as the polite stop has been asked for; it does not
            block for `stopGrace`. A Monte Carlo running with progress enabled
            checks between batches and ends its own request, keeping its worker
            and its session. Nothing else can be interrupted, so when the grace
            expires with the request still running the worker is killed and the
            session replayed (DESIGN §3).

            The terminal CANCELLED frame is emitted exactly once, by whichever
            half served the cancel: the worker on the polite path, this class
            on the kill path. That only holds if the gateway reports terminal
            frames back through onRequestTerminated(), so it can tell the two
            apart.

            A cancel is a request, not a guarantee that no result arrives: a
            calculation that finishes on its own inside the grace terminates
            normally and the kill is dropped.
        */
        void cancel(const std::string& sessionId, const quantlib::v2::ClientFrame& frame);

        //! Tells the supervisor that a request has terminated.
        /*! Called by the gateway for every terminal frame it forwards, cancel
            target or not, and passed the frame rather than just its id because
            two different things depend on what the outcome was:

            - A cancel target that terminates inside its grace window is one
              the polite stop reached, so its pending kill is dropped.
            - An UpdateMarket that terminates with an Ack is one the graph
              accepted, and only then is it folded into the SessionLog. This
              is the sole path by which the log advances past OpenSession;
              without it a replay would silently restore the session to the
              market data it opened with (DESIGN §2.1).

            An update that terminates with an Error is discarded instead, which
            is what keeps a write the worker rejected out of the replayable
            state.
        */
        void onRequestTerminated(const std::string& sessionId,
                                 const quantlib::v2::ServerFrame& frame);

        //! Called when a worker exits on its own.
        /*! Replayed like a cancel. A session that fails to replay twice is
            reported terminally as WORKER_DIED and dropped, so a request that
            reliably kills the worker cannot become a respawn loop.
        */
        void onWorkerDied(const std::string& workerId);

        //! Whether a request goes to a sacrificial process.
        /*! Currently keyed on engine kind, which is a proxy: a 200-path Monte
            Carlo is cheap and a large finite-difference grid is not. Sample
            count, or a client-declared budget, is probably the better signal —
            DESIGN §8.
        */
        static Placement placementFor(const quantlib::v2::PriceRequest& msg);

      private:
        //! The cancels asked for on one session that have not resolved yet.
        struct PendingCancel {
            //! Targets still running. Emptied by termination or by the kill.
            std::vector<std::uint64_t> targets;
            //! One timer per round: further targets ride the same deadline.
            bool timerArmed = false;
        };

        struct SessionState {
            SessionState(std::string workerId, Placement placement, SessionLog log)
            : workerId(std::move(workerId)), placement(placement), log(std::move(log)) {}

            std::string workerId;
            Placement placement = Placement::Shared;
            SessionLog log;
            //! Updates sent to the worker that have not yet come back acked.
            /*! Keyed by request_id, because that is all a terminal frame
                carries. An entry leaves either by being folded into the log on
                an Ack or by being dropped — on an error, or with the worker it
                was sent to.
            */
            std::map<std::uint64_t, quantlib::v2::UpdateMarket> pendingUpdates;
            unsigned replayFailures = 0;
            PendingCancel cancel;
            //! Bumped when a round ends, so its timer fires into nothing.
            std::uint64_t cancelGeneration = 0;
        };

        //! One worker process and how much of it is spoken for.
        struct WorkerState {
            Placement placement = Placement::Shared;
            std::size_t sessions = 0;
        };

        //! Picks the worker a session of this placement will live on.
        /*! Packs into an existing shared worker with room; forks only when
            none has any, and never for Sacrificial, which is a fresh process
            by definition.
        */
        std::string acquireSeat(Placement placement);

        //! Gives back a seat, retiring the process once it holds nothing.
        /*! Retiring keeps the process count tracking live sessions rather
            than the session high-water mark. Holding empty workers warm to
            save a fork instead is DESIGN §8, still open.
        */
        void releaseSeat(const std::string& workerId);

        //! Moves a session off its worker without disturbing its co-tenants.
        /*! A CloseSession, not a kill: under pooling, killing a process to
            relocate one of its sessions would take the others with it.
        */
        void detach(const std::string& sessionId, SessionState& state);

        //! Kills a worker and puts every session it held somewhere else.
        void killWorker(const std::string& workerId);

        //! Replays every session that was on a worker that is now gone.
        void recoverSessionsOn(const std::string& workerId);

        void onStopGraceExpired(const std::string& sessionId, std::uint64_t generation);

        //! Emits one terminal error naming both the request and the session.
        void emitError(const std::string& sessionId,
                       std::uint64_t requestId,
                       quantlib::v2::Error::Code code,
                       const std::string& message);

        //! Terminates every outstanding cancel target with CANCELLED.
        /*! A kill takes every request in flight on the worker, not just the
            named one, and the client is told so for each (envelope.proto,
            CancelRequest).
        */
        void terminateCancelTargets(const std::string& sessionId, SessionState& state);

        void replay(const std::string& sessionId, SessionState& state);

        ProcessHost& host_;
        FrameSink sink_;
        DisruptionSink disrupted_;
        DeadlineTimer armTimer_;
        Options options_;
        std::map<std::string, SessionState> sessions_;
        //! The pool, shared and sacrificial workers alike, keyed by worker id.
        std::map<std::string, WorkerState> workers_;
    };

}

#endif
