/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file gateway.hpp
    \brief the WebSocket front end: sockets, ids, backpressure, the event loop
*/

#ifndef qlservice_gateway_gateway_hpp
#define qlservice_gateway_gateway_hpp

#include <chrono>
#include <memory>
#include <string>

namespace qlservice {

    //! Terminates client connections and drives the supervisor.
    /*! Implements DESIGN §9. The event loop is single threaded and everything
        that touches the supervisor runs on it, which is what lets that class
        hold its state without a lock (DESIGN §1.1).

        uWebSockets is kept out of this header: it is a large template library
        and only the implementation needs it.
    */
    class Gateway {
      public:
        struct Options {
            //! Interface to bind. Loopback by default: this is a pricing
            //! backend with no authentication of any kind (DESIGN §9), so it
            //! has no business being reachable from the network until it has.
            std::string host = "127.0.0.1";

            int port = 9001;

            //! Largest frame accepted from a client.
            /*! uWebSockets defaults to 16 KB, which an OpenSession carrying a
                few hundred pillars exceeds. A limit that low rejects the frame
                at the transport, with no request_id to attribute it to
                (DESIGN §9.3).
            */
            unsigned maxPayloadBytes = 4u * 1024 * 1024;

            //! Buffered bytes past which uWebSockets stops sending.
            /*! Reached only after Progress shedding has failed to keep up, so
                the connection is closed rather than allowed to lose a terminal
                frame silently (DESIGN §9.3).
            */
            unsigned maxBackpressureBytes = 4u * 1024 * 1024;

            unsigned short idleTimeoutSeconds = 120;

            //! How often armed deadlines are checked.
            /*! One repeating timer serves every deadline. uSockets frees a
                timer inside us_timer_close, so closing a one-shot from its own
                callback is a lifetime hazard; a single tick has no such edge.
                The supervisor already tolerates a late timer — cancel rounds
                carry a generation for exactly that reason — and the stop grace
                it arms is 250 ms, so this resolution is ample.
            */
            std::chrono::milliseconds deadlineTick{25};
        };

        explicit Gateway(Options options);
        Gateway();
        ~Gateway();

        Gateway(const Gateway&) = delete;
        Gateway& operator=(const Gateway&) = delete;

        //! Listens, then runs the loop until it ends. False if the port is taken.
        bool run();

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}

#endif
