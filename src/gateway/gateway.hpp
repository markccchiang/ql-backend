/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file gateway.hpp
    \brief the WebSocket front end: sockets, ids, backpressure, the event loop
*/

#ifndef qlservice_gateway_gateway_hpp
#define qlservice_gateway_gateway_hpp

#include <chrono>
#include <memory>
#include <string>
#include <vector>

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

            //! Browser origins allowed to open a socket.
            /*! A WebSocket upgrade is not subject to the same-origin policy,
                so binding to loopback is not a boundary against a browser: any
                page in any tab can open ws://127.0.0.1 and drive this service.
                There is nothing here to steal, and plenty to spend -- one frame
                can commit a hundred thousand engine calls.

                The rule is that a *present* Origin must be on this list and an
                *absent* one is allowed. Only browsers send the header, so this
                closes the browser path and leaves test/smoke_v2.py and any
                other non-browser client working. An empty list disables the
                check, which is what a deployment behind a proxy that already
                does it should use.
            */
            std::vector<std::string> allowedOrigins{"http://localhost:5173", "http://127.0.0.1:5173",
                                                    "http://localhost:4173", "http://127.0.0.1:4173"};

            //! Sockets served at once.
            /*! The other half of having no authentication: nothing stops one
                client opening sockets until the process runs out of them. A
                refused connection is a far better failure than a gateway that
                cannot accept the one that matters.
            */
            std::size_t maxConnections = 32;

            //! Sessions one socket may hold open.
            /*! A session is a live QuantLib graph on a worker seat, so this is
                the limit that protects the pool rather than the socket. The
                frontend opens one per tab; sixteen tabs is past generous and
                still two orders below what would exhaust the seats.
            */
            std::size_t maxSessionsPerConnection = 16;

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
