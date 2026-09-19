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

    //! The most expiries, and separately the most strikes, one variance
    //! surface may have.
    /*! A traded surface is tens of expiries by tens of strikes. The cap is
        not about pricing cost but about the check it guards: expiries times
        strikes has to equal the number of volatilities sent, and two axes of
        65,536 multiply to 2^32, which an int reads as zero. A surface with no
        volatilities then passed, and the loop after it read past the end of
        the list into a 34 GB matrix. Checked before a single expiry is parsed.
    */
    constexpr int kMaxSurfaceAxisPoints = 1000;

    //! The most labels one correlation matrix may have.
    /*! Every basket engine here takes a handful of assets, and the positive
        semi-definite check each matrix goes through is an eigendecomposition,
        cubic in the label count. Like the surface cap, it also bounds the
        n x n the value count is checked against, which for 65,537 labels
        overflowed an int into a count a client could meet.
    */
    constexpr int kMaxCorrelationLabels = 100;

    // -----------------------------------------------------------------------
    // Engine sizes
    // -----------------------------------------------------------------------
    //
    // Every count below reaches QuantLib as a loop bound or an allocation, and
    // none of them can be interrupted once the engine is running: a lattice of
    // four billion steps allocates for as long as memory lasts, and a Monte
    // Carlo of 10^15 paths runs for days on a thread that a kill can only
    // disown (DESIGN §3). The limits are sized like the ones above -- to catch
    // a typo or a hostile frame, not to ration an honest request -- and are
    // checked together at the top of Session::price, so a book entry and a
    // sweep point meet the same rule as a single price.

    //! The most steps one lattice may take. Desks use hundreds to a few
    //! thousand; time grows with the square of this.
    constexpr std::uint32_t kMaxLatticeSteps = 10000;

    //! The most time steps, and separately the most asset steps, one custom
    //! finite-difference grid may have. PRESET_FINE is 2000 x 800.
    constexpr std::uint32_t kMaxFdTimeSteps = 10000;
    constexpr std::uint32_t kMaxFdAssetSteps = 10000;

    //! The most paths one Monte Carlo may draw. A hundred million is minutes
    //! on one core, and ten more digits is a request nobody meant.
    constexpr std::uint64_t kMaxMcSamples = 100000000;

    //! The most time steps per year one Monte Carlo path may take.
    constexpr std::uint32_t kMaxMcTimeStepsPerYear = 10000;

    //! The most batches a batched Monte Carlo may split into.
    /*! Each batch builds an engine and sends a Progress frame, so a batch of
        one path over a million samples is a million engines and a million
        frames -- slower than the calculation it reports on, and a flood on
        the socket. Bounds progress_every_paths from below, relative to the
        sample count.
    */
    constexpr std::uint64_t kMaxMcBatches = 10000;

    //! The most evaluations one implied-volatility root find may take.
    //! QuantLib's own default is 100.
    constexpr std::uint32_t kMaxImpliedVolatilityEvaluations = 1000;

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
