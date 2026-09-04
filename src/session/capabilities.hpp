/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file capabilities.hpp
    \brief what this build prices, as data
*/

#ifndef qlservice_session_capabilities_hpp
#define qlservice_session_capabilities_hpp

#include <cstdint>

namespace quantlib::v2 {
    class Capabilities;
}

namespace qlservice {

    //! The most points one sweep may hold, counting the product of its axes.
    /*! A grid multiplies, so a step count one digit too long is not a slow
        request but a session-length one. Advertised in Capabilities so a
        client can refuse it before spending the round trip, and enforced in
        the worker so one that does not look is refused there.

        Sized for what the cap is for. A hundred thousand analytic prices is
        tens of seconds and cancellable between points; the number exists to
        catch a typo, not to ration the desk.
    */
    constexpr std::uint32_t kMaxScenarioPoints = 100000;

    //! Fills the reply to a Hello.
    /*! The lists here and the dispatch in session.cpp are the same fact told
        twice, and the second telling is the one that goes over the wire. They
        have to move together: a style added to Session::priceOption and not
        added here is a style no client will offer, and one added here and not
        there is a client authoring a request that will be refused.

        That is still better than the alternative it replaces, which was every
        client keeping its own copy of HANDLERS.md and discovering the drift
        when a user hit it. The two copies are now in one repository, in
        neighbouring files, and test/smoke_v2.py checks that what is advertised
        can actually be priced.
    */
    void fillCapabilities(quantlib::v2::Capabilities& out);

}

#endif
