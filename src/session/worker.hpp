/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file worker.hpp
    \brief the thread that owns a session and serializes its requests
*/

#ifndef qlservice_session_worker_hpp
#define qlservice_session_worker_hpp

#include "quantlib/v1/envelope.pb.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace qlservice {

    class Session;

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
        using FrameSink = std::function<void(const quantlib::v1::ServerFrame&)>;

        Worker(std::string sessionId, FrameSink sink);
        ~Worker();

        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
        Worker(Worker&&) = delete;
        Worker& operator=(Worker&&) = delete;

        //! Starts the thread. The first frame submitted must be OpenSession.
        void start();

        //! Queues a frame. Returns false once the worker is shutting down.
        bool submit(const quantlib::v1::ClientFrame& frame);

        //! Asks the running request to stop between Monte Carlo batches.
        /*! Best-effort and slow: it takes effect only at a batch boundary, and
            never at all inside a single engine call. A guaranteed stop is the
            supervisor killing the process (DESIGN §3).
        */
        void requestStop() { stopRequested_.store(true, std::memory_order_relaxed); }

        void shutdown();

      private:
        void run();
        void serve(const quantlib::v1::ClientFrame& frame);

        //! Rejects a frame that needs a graph this worker does not hold.
        /*! SESSION_NOT_FOUND rather than a calculation failure: the client
            addressed a session that is not here, which it recovers from by
            reopening rather than by fixing its inputs.
        */
        void requireSessionOpen() const;

        void emit(std::uint64_t requestId,
                  bool terminal,
                  const std::function<void(quantlib::v1::ServerFrame&)>& fill);
        void emitError(std::uint64_t requestId,
                       quantlib::v1::Error::Code code,
                       const std::string& message,
                       const std::string& fieldPath = {});

        std::string sessionId_;
        FrameSink sink_;

        // Created on the worker thread in run(), destroyed there too.
        std::unique_ptr<Session> session_;

        std::thread thread_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<quantlib::v1::ClientFrame> queue_;
        bool shuttingDown_ = false;

        std::atomic<bool> stopRequested_{false};
    };

}

#endif
