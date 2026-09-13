/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file capabilities.hpp
    \brief what this build prices, as data
*/

#ifndef qlbackend_session_capabilities_hpp
#define qlbackend_session_capabilities_hpp

#include <cstdint>

namespace quantlib::v2 {
    class Capabilities;
}

namespace qlbackend {

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

    //! The most trades one PriceBatch may carry.
    /*! The sweep cap's sibling: a 4 MB frame can hold tens of thousands of
        Monte Carlos, each of which runs to completion on the seat it starts
        on. Sized the same way -- a blotter is tens of rows, a thousand is a
        loop that forgot to stop. Advertised as Capabilities.max_batch_entries
        and enforced in the worker, like the sweep ceiling.
    */
    constexpr int kMaxBatchEntries = 1000;

    //! The most points one CurveSample may ask for.
    /*! A curve chart is hundreds of points; ten thousand is more than any
        screen shows. Each point is a term-structure query on the live graph,
        so a sample this size is a request in its own right rather than a
        decoration on one. Advertised as Capabilities.max_curve_sample_points
        and enforced per sample in the session.
    */
    constexpr int kMaxCurveSamplePoints = 10000;

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
