/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file worker.hpp
    \brief the thread that owns a session and serializes its requests
*/

#ifndef qlbackend_session_worker_hpp
#define qlbackend_session_worker_hpp

#include "session.hpp"
#include "quantlib/v2/envelope.pb.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace qlbackend {

    //! Owns one Session on one thread and serves its frames one at a time.
    /*! The queue is the point. QuantLib's session isolation is per thread
        (`ql/patterns/singleton.hpp:92`) and a lazy graph cannot serve two
        calculations at once, so requests for a session are queued and run in
        arrival order. Concurrency comes from running more Workers.

        \warning The Session is constructed **inside** `run()`, on the worker's
                 own thread, never by the caller. `Session`'s constructor
                 writes `Settings::instance().evaluationDate()`, which under
                 `QL_ENABLE_SESSIONS` is thread-local: building it on the
                 gateway thread would set the date on that thread and leave the
                 worker pricing against whatever date it happened to hold. The
                 object would look fine and the prices would be wrong.
    */
    class Worker {
      public:
        //! Emits a frame back to the gateway. Called on the worker thread.
        using FrameSink = std::function<void(const quantlib::v2::ServerFrame&)>;

        //! Reports that this worker has dropped its graph. Called on the
        //! worker thread, after the terminal frame of the request that did it.
        /*! A failed commit leaves the graph partly invalidated, and the only
            repair is a replay from the session log (DESIGN §2.1). The worker
            cannot do that itself -- the log lives in the supervisor -- so it
            says so and keeps serving, answering SESSION_NOT_FOUND to whatever
            was queued behind the failure until it is reaped.
        */
        using DeathSink = std::function<void()>;

        Worker(std::string sessionId, FrameSink sink, DeathSink died = {});
        ~Worker();

        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
        Worker(Worker&&) = delete;
        Worker& operator=(Worker&&) = delete;

        //! Starts the thread. The first frame submitted must be OpenSession.
        void start();

        //! Queues a frame. Returns false once the worker is shutting down.
        bool submit(const quantlib::v2::ClientFrame& frame);

        //! What a stop found when it looked for its request.
        enum class StopOutcome {
            Running,  //!< running now: asked to stop at its next boundary
            Dequeued, //!< still queued: removed, and it will never run
            NotFound  //!< neither: it has answered already, or was never here
        };

        //! Stops one request, and only that one.
        /*! A request still waiting in the queue is taken out of it, and the
            caller owes the client its terminal frame, since this worker will
            never produce one. A request that is running is asked to stop at
            its next boundary -- between Monte Carlo batches, sweep points or
            batch entries -- which is best-effort and never happens inside a
            single engine call. A guaranteed stop is the supervisor killing
            the process (DESIGN §3).

            Aimed at a request rather than at the worker, because anything
            broader stops the wrong work: a sweep that happens to be running
            while the client cancels the request queued behind it would be
            cut short for a stop it was never the target of.

            An OpenSession is never stopped or removed. Every later frame on
            this worker needs the graph it builds, and a session the client
            believes open with nothing behind it is worse than a slow one.
        */
        StopOutcome requestStop(std::uint64_t requestId);

        //! Gives up on everything: the queue is dropped and every stop check
        //! from here on says stop.
        /*! For a worker being disowned by a kill. What it would still compute
            nobody will read, so the running request is asked to end at its
            next boundary and nothing queued behind it is started at all.
        */
        void abandon();

        void shutdown();

      private:
        void run();
        void serve(const quantlib::v2::ClientFrame& frame);

        //! Fills a PriceResult from one outcome, engine echo included.
        void fillResult(quantlib::v2::PriceResult& result,
                        const quantlib::v2::PriceRequest& request,
                        const Session::PriceOutcome& outcome) const;

        //! Prices once per scenario point off the one live graph.
        /*! The reason a session is kept alive at all: N prices here cost N
            lazy recomputes of whatever the bumped quote invalidated, against
            N round trips that each rebuild the whole graph (DESIGN §5).
        */
        void serveScenario(const quantlib::v2::ClientFrame& frame,
                           const Session::ProgressSink& progress);

        //! Prices a book of trades off the one live graph, in one reply.
        /*! A failing entry does not fail the batch: it carries the rejection
            it would have been sent on its own, because refusing the whole book
            over one bad trade throws away the prices that were correct. The
            exception is a failure that dirties the graph, after which nothing
            later is trustworthy and the rest is abandoned.
        */
        void serveBatch(const quantlib::v2::ClientFrame& frame);

        static void fillSeries(quantlib::v2::Series& series,
                               const quantlib::v2::ScenarioResult& scenario,
                               quantlib::v2::ResultKind kind);

        //! Two axes as a matrix, row-major with the axis values as its labels.
        static void fillSurface(quantlib::v2::DoubleMatrix& surface,
                                const quantlib::v2::ScenarioResult& scenario,
                                quantlib::v2::ResultKind kind);

        //! One plotted result at one point, NaN where the engine had none.
        static double pointValue(const quantlib::v2::PriceResult& price,
                                 quantlib::v2::ResultKind kind);

        //! Rejects a frame that needs a graph this worker does not hold.
        /*! SESSION_NOT_FOUND rather than a calculation failure: the client
            addressed a session that is not here, which it recovers from by
            reopening rather than by fixing its inputs.
        */
        void requireSessionOpen() const;

        void emit(std::uint64_t requestId,
                  bool terminal,
                  const std::function<void(quantlib::v2::ServerFrame&)>& fill);
        void emitError(std::uint64_t requestId,
                       quantlib::v2::Error::Code code,
                       const std::string& message,
                       const std::string& fieldPath = {});

        //! Drops a dirty graph and reports it. The one path off a failed commit.
        void dropSession();

        std::string sessionId_;
        FrameSink sink_;
        DeathSink died_;

        // Created on the worker thread in run(), destroyed there too.
        std::unique_ptr<Session> session_;

        std::thread thread_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<quantlib::v2::ClientFrame> queue_;
        bool shuttingDown_ = false;
        //! The request being served, 0 between requests. Guarded by mutex_.
        /*! Also 0 while an OpenSession runs, which is how requestStop()
            leaves one alone.
        */
        std::uint64_t current_ = 0;
        //! Set by abandon(). Guarded by mutex_.
        bool abandoned_ = false;

        //! Read at every stop boundary, so an atomic rather than the mutex.
        /*! Only ever true for the request being served: it is set only when
            the target is current_, and reset each time a request starts.
        */
        std::atomic<bool> stopRequested_{false};
    };

}

#endif
