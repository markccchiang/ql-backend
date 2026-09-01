/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file session.hpp
    \brief one client's live QuantLib object graph
*/

#ifndef qlservice_session_session_hpp
#define qlservice_session_session_hpp

#include "conventions/registry.hpp"
#include "quantlib/v1/envelope.pb.h"
#include <ql/handle.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/shared_ptr.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>
#include <ql/time/schedule.hpp>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace QuantLib {
    class GeneralizedBlackScholesProcess;
    class VanillaOption;
    // RateHelper is not a class: it is a typedef for
    // BootstrapHelper<YieldTermStructure> (ratehelpers.hpp:47), so it cannot
    // be forward declared and the header above has to be included.
}

namespace qlservice {

    //! One client session: a live QuantLib object graph plus its quotes.
    /*! A Session is owned by exactly one worker thread for its whole life and
        is not thread-safe by design. Under `QL_ENABLE_SESSIONS` the QuantLib
        singletons it touches — `Settings`, `IndexManager`, `ObservableSettings`
        — are `thread_local` (`ql/patterns/singleton.hpp:92`), so the isolation
        is per thread, and moving a Session between threads would silently
        change the evaluation date it prices against.

        The point of keeping it alive is the lazy graph: a quote write
        invalidates exactly the instruments that depend on it, and the next
        price recomputes only those. Rebuilding per request would discard that.

        Everything within a Session serializes. Two prices cannot overlap even
        when neither mutates market data, because `NPV()` writes its cache as
        it computes (`ql/patterns/lazyobject.hpp`). Concurrency comes from more
        Sessions, never from more threads on one — see DESIGN §2.1.
    */
    class Session {
      public:
        //! Values the worker reports back for one price request.
        struct PriceOutcome {
            QuantLib::Real npv = 0.0;
            std::map<std::string, QuantLib::Real> results;
            double calculationSeconds = 0.0;
        };

        //! Called between Monte Carlo batches; returning false aborts.
        /*! The only place this layer can interrupt a calculation, and it works
            only because the worker owns the batching. Inside a single engine
            call QuantLib cannot be stopped (DESIGN §3).
        */
        using ProgressSink = std::function<bool(
            QuantLib::Size done, QuantLib::Size total, QuantLib::Real runningNpv)>;

        explicit Session(const quantlib::v1::OpenSession& msg);

        //! Live forwarding/discount curve by id; throws when unknown.
        /*! An unknown id must not return an empty handle: an index built on
            one prices happily until the first forecast and then fails a long
            way from the cause.
        */
        QuantLib::Handle<QuantLib::YieldTermStructure> curve(const std::string& curveId) const;

        //! Applies a batch of quote writes, then commits once.
        /*! The whole batch runs under one UpdateGuard so dependent instruments
            recalculate once rather than once per quote.
        */
        void apply(const quantlib::v1::UpdateMarket& msg);

        PriceOutcome price(const quantlib::v1::PriceRequest& msg, const ProgressSink& progress);

        //! True once a commit failed and the graph is only partly invalidated.
        /*! A dirty Session is not repaired in place: the worker drops it and
            the gateway replays the session log into a fresh one (DESIGN §2.1).
        */
        bool dirty() const { return dirty_; }

        const QuantLib::Date& evaluationDate() const { return evaluationDate_; }

      private:
        void buildCurves(const quantlib::v1::OpenSession& msg);

        //! One bootstrap helper from one pillar quote.
        QuantLib::ext::shared_ptr<QuantLib::RateHelper>
        buildHelper(const quantlib::v1::CurvePillar& pillar,
                    const quantlib::v1::CurveDefinition& def,
                    const std::string& fieldPath);

        //! Builds a FlatForward from one quote, for a curve with no pillars.
        QuantLib::Handle<QuantLib::YieldTermStructure>
        makeFlatCurve(const quantlib::v1::CurveDefinition& def, const std::string& fieldPath);

        //! Instantiates the piecewise curve for a traits/interpolator pair.
        /*! `PiecewiseYieldCurve` is a template, so the pair cannot be resolved
            by a registry lookup the way a calendar is: each combination is a
            distinct type and has to be named in source. This is the explicit
            instantiation table, and the reason the schema offers a small fixed
            set rather than an open one.
        */
        QuantLib::Handle<QuantLib::YieldTermStructure>
        makeCurve(const quantlib::v1::CurveDefinition& def,
                  std::vector<QuantLib::ext::shared_ptr<QuantLib::RateHelper>> helpers,
                  const std::string& fieldPath);

        PriceOutcome priceOption(const quantlib::v1::PriceRequest& msg,
                                 const ProgressSink& progress);

        PriceOutcome priceSwap(const quantlib::v1::PriceRequest& msg);

        //! The live handles one quanto engine is assembled from.
        /*! Nothing in here is a value. `QuantoEngine` registers with all four
            members (`ql/pricingengines/quanto/quantoengine.hpp`), so a write
            to any of the seven underlying quotes invalidates the instrument
            and the next price recomputes — the same contract the vanilla path
            gets from its two quotes (DESIGN §5).
        */
        struct QuantoGraph {
            QuantLib::ext::shared_ptr<QuantLib::GeneralizedBlackScholesProcess> process;
            QuantLib::Handle<QuantLib::YieldTermStructure> fxRiskFree;
            QuantLib::Handle<QuantLib::BlackVolTermStructure> fxVol;
            QuantLib::Handle<QuantLib::Quote> correlation;
        };

        //! Resolves a QuantoMarket message against this session's graph.
        QuantoGraph quantoGraph(const quantlib::v1::QuantoMarket& msg,
                                const std::string& fieldPath) const;

        //! Prices any of the four quanto shapes.
        /*! One method rather than four because they differ only in the
            instrument and the inner engine, and `QuantoEngine<Instr, Engine>`
            makes each pair a distinct type. The switch below is therefore an
            explicit instantiation table like `makeCurve`, for the same reason
            (DESIGN §6.1).
        */
        PriceOutcome priceQuantoOption(const quantlib::v1::PriceRequest& msg);

        //! One live quote by id; throws naming the field when unknown.
        QuantLib::Handle<QuantLib::Quote> quoteHandle(const std::string& quoteId,
                                                      const std::string& fieldPath) const;

        //! Parses an expiry and rejects one that is not after the evaluation date.
        /*! An already-expired option is not a pricing failure in QuantLib — it
            reports zero through `setupExpired()` — so it has to be refused
            here or the client gets a confident 0.0 back.
        */
        QuantLib::Date expiryDate(const quantlib::v1::Date& msg,
                                  const std::string& fieldPath) const;

        //! Schedule for one swap leg.
        QuantLib::Schedule schedule(const quantlib::v1::SwapLeg& leg,
                                    const QuantLib::Date& start,
                                    const QuantLib::Date& maturity,
                                    const std::string& fieldPath);

        //! Runs Monte Carlo in batches so progress can be reported.
        PriceOutcome priceInBatches(
            const quantlib::v1::PriceRequest& msg,
            const QuantLib::ext::shared_ptr<QuantLib::VanillaOption>& option,
            const QuantLib::ext::shared_ptr<QuantLib::GeneralizedBlackScholesProcess>& process,
            const ProgressSink& progress);

        /*! Owned, not borrowed, and bound to this session's own curve map:
            the registry resolves forwarding curves while the graph is still
            being built, so it has to see `curves_` as it fills. Curve
            definitions therefore arrive in dependency order.
        */
        ConventionRegistry registry_;

        QuantLib::Date evaluationDate_;

        //! quote_id -> the SimpleQuote instruments actually registered with.
        /*! Held as concrete SimpleQuotes because the session writes them, and
            handed to builders as `Handle<Quote>` so the graph observes them.
            A value copied into an instrument at construction would never
            invalidate anything (AGENTS.md §5.2), which is the failure mode
            where the UI slider moves and the price does not.
        */
        std::map<std::string, QuantLib::ext::shared_ptr<QuantLib::SimpleQuote>> quotes_;

        std::map<std::string, QuantLib::Handle<QuantLib::YieldTermStructure>> curves_;

        bool dirty_ = false;
    };

}

#endif
