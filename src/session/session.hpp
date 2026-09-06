/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file session.hpp
    \brief one client's live QuantLib object graph
*/

#ifndef qlservice_session_session_hpp
#define qlservice_session_session_hpp

#include "conventions/registry.hpp"
#include "quantlib/v2/envelope.pb.h"
#include <ql/exercise.hpp>
#include <ql/handle.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/math/matrix.hpp>
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
#include <utility>
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

        This is the `v2` session. The market is a namespace of named objects
        rather than a fixed set of fields, and an option is a payoff, an
        exercise, an underlying and a style rather than one message per product
        (DESIGN §6.3).
    */
    class Session {
      public:
        //! Values the worker reports back for one price request.
        struct PriceOutcome {
            QuantLib::Real npv = 0.0;
            std::map<std::string, QuantLib::Real> results;
            double calculationSeconds = 0.0;
            //! Curves and surfaces sampled alongside the price.
            /*! Sampled from the very handles the engine priced against, which
                is the point: the alternative is shipping the term structure
                and re-implementing QuantLib's interpolation in the client,
                which is how a frontend ends up drawing a curve the backend did
                not price with.
            */
            std::vector<quantlib::v2::Series> series;
            //! The cash flows behind the NPV, when asked for.
            /*! A table rather than a number, because the sum of its
                present-value column is the NPV: it is the panel showing its
                working, and the property a client can check.
            */
            std::vector<quantlib::v2::CashFlow> cashflows;
            //! Kinds that were asked for and are not in `results`.
            /*! Named rather than left to be inferred: a client that asked for
                vega and got a map without it cannot tell that from a vega of
                zero. Not a rejection, because an engine that does not publish
                a greek is not a client error and refusing would cost the price
                as well.
            */
            std::vector<quantlib::v2::ResultKind> unavailable;
        };

        //! The live handles one option's engine is assembled from.
        /*! Nothing in here is a value. Every engine registers with the
            process and, when the trade is quanto, with the three FX handles
            as well, so a write to any underlying quote invalidates the
            instrument and the next price recomputes (DESIGN §5).

            Public only so the engine tables in session.cpp can name it; it
            never crosses the wire and nothing outside this file builds one.
        */
        struct EquityGraph {
            QuantLib::ext::shared_ptr<QuantLib::GeneralizedBlackScholesProcess> process;
            QuantLib::Handle<QuantLib::Quote> spot;
            QuantLib::Handle<QuantLib::YieldTermStructure> riskFree;
            QuantLib::Handle<QuantLib::YieldTermStructure> dividend;
            QuantLib::Handle<QuantLib::BlackVolTermStructure> volatility;

            //! Set only when the request carried a Quanto block.
            /*! Quanto is not a product here, it is an adjustment to whatever
                engine the style would otherwise get, so it travels with the
                market rather than with the instrument (DESIGN §6.3).
            */
            bool quanto = false;
            QuantLib::Handle<QuantLib::YieldTermStructure> fxRiskFree;
            QuantLib::Handle<QuantLib::BlackVolTermStructure> fxVol;
            QuantLib::Handle<QuantLib::Quote> correlation;
        };

        //! Called between Monte Carlo batches; returning false aborts.
        /*! The only place this layer can interrupt a calculation, and it works
            only because the worker owns the batching. Inside a single engine
            call QuantLib cannot be stopped (DESIGN §3).
        */
        using ProgressSink = std::function<bool(
            QuantLib::Size done, QuantLib::Size total, QuantLib::Real runningNpv)>;

        explicit Session(const quantlib::v2::OpenSession& msg);

        //! Live forwarding/discount curve by id; throws when unknown.
        /*! An unknown id must not return an empty handle: an index built on
            one prices happily until the first forecast and then fails a long
            way from the cause.
        */
        QuantLib::Handle<QuantLib::YieldTermStructure> curve(const std::string& curveId) const;

        //! Every market id this session built, in the order it built them.
        const std::vector<std::string>& marketIds() const { return marketIds_; }

        //! Applies a batch of quote writes, then commits once.
        /*! The whole batch runs under one UpdateGuard so dependent instruments
            recalculate once rather than once per quote.
        */
        void apply(const quantlib::v2::UpdateMarket& msg);

        PriceOutcome price(const quantlib::v2::PriceRequest& msg, const ProgressSink& progress);

        //! Reads and writes one quote directly, for a scenario sweep.
        /*! A sweep is N prices off one graph, so it cannot go through
            `apply`: that takes an UpdateMarket, and the supervisor records
            every UpdateMarket in the session log. A swept value is a question
            rather than an edit and must not enter the replay (DESIGN §2.1),
            which is why the sweep restores the quote by default.
        */
        double quoteValue(const std::string& quoteId, const std::string& fieldPath) const;
        void writeQuote(const std::string& quoteId, double value, const std::string& fieldPath);

        //! True once a commit failed and the graph is only partly invalidated.
        /*! A dirty Session is not repaired in place: the worker drops it and
            the gateway replays the session log into a fresh one (DESIGN §2.1).
        */
        bool dirty() const { return dirty_; }

        const QuantLib::Date& evaluationDate() const { return evaluationDate_; }

      private:
        // --- market -------------------------------------------------------

        void buildMarket(const quantlib::v2::OpenSession& msg);
        void buildYieldCurve(const std::string& id,
                             const quantlib::v2::YieldCurve& def,
                             const std::string& fieldPath);
        void buildVolatility(const std::string& id,
                             const quantlib::v2::VolatilitySurface& def,
                             const std::string& fieldPath);
        void buildIndex(const std::string& id,
                        const quantlib::v2::Index& def,
                        const std::string& fieldPath);
        void buildCorrelation(const std::string& id,
                              const quantlib::v2::CorrelationMatrix& msg,
                              const std::string& fieldPath);

        //! Everything that makes a square of numbers a correlation matrix.
        /*! Called when the object is built and again on every request that
            uses one, because the entries are live quotes: a matrix that was
            legal when the session opened can be dragged into one that is not.
        */
        void checkCorrelation(const QuantLib::Matrix& m, const std::string& fieldPath) const;
        void applyFixings(const quantlib::v2::FixingSeries& msg, const std::string& fieldPath);

        //! One bootstrap helper from one pillar quote.
        QuantLib::ext::shared_ptr<QuantLib::RateHelper>
        buildHelper(const quantlib::v2::Pillar& pillar,
                    const quantlib::v2::YieldCurve& def,
                    const std::string& fieldPath);

        //! Instantiates the piecewise curve for a traits/interpolator pair.
        /*! `PiecewiseYieldCurve` is a template, so the pair cannot be resolved
            by a registry lookup the way a calendar is: each combination is a
            distinct type and has to be named in source. This is the explicit
            instantiation table, and the reason the schema offers a small fixed
            set rather than an open one (DESIGN §6.1).
        */
        QuantLib::Handle<QuantLib::YieldTermStructure>
        makeCurve(const quantlib::v2::BootstrappedCurve& boot,
                  std::vector<QuantLib::ext::shared_ptr<QuantLib::RateHelper>> helpers,
                  const QuantLib::DayCounter& dayCounter,
                  const std::string& fieldPath);

        // --- resolution ---------------------------------------------------

        //! One curve by id, rejecting an unknown one against the field.
        /*! `curve()` above throws a plain QL_REQUIRE because the registry's
            resolver has no field to name; every caller in this class does.
        */
        QuantLib::Handle<QuantLib::YieldTermStructure>
        curveHandle(const std::string& curveId, const std::string& fieldPath) const;

        //! One live quote by id; throws naming the field when unknown.
        QuantLib::Handle<QuantLib::Quote> quoteHandle(const std::string& quoteId,
                                                      const std::string& fieldPath) const;

        //! A Number, which is either a live quote or a constant.
        /*! A `fixed` becomes a SimpleQuote nobody holds an id for, so the
            graph shape is identical either way and only the bumpability
            differs.
        */
        QuantLib::Handle<QuantLib::Quote> number(const quantlib::v2::Number& msg,
                                                 const std::string& fieldPath) const;

        QuantLib::Handle<QuantLib::BlackVolTermStructure>
        volatility(const std::string& volId, const std::string& fieldPath) const;

        //! Fills PriceOutcome::series from PriceRequest::curve_samples.
        void sampleCurves(const quantlib::v2::PriceRequest& msg, PriceOutcome& out) const;

        //! Fills PriceOutcome::cashflows from a priced swap's legs.
        void fillCashflows(const std::vector<QuantLib::Leg>& legs,
                           const QuantLib::Handle<QuantLib::YieldTermStructure>& discount,
                           PriceOutcome& out) const;

        QuantLib::ext::shared_ptr<QuantLib::IborIndex>
        indexById(const std::string& indexId, const std::string& fieldPath) const;

        //! Parses an expiry and rejects one that is not after the evaluation date.
        /*! An already-expired option is not a pricing failure in QuantLib — it
            reports zero through `setupExpired()` — so it has to be refused
            here or the client gets a confident 0.0 back.
        */
        QuantLib::Date expiryDate(const quantlib::v1::Date& msg,
                                  const std::string& fieldPath) const;

        // --- instruments --------------------------------------------------

        //! Resolves an Underlying, plus the request's Quanto block if any.
        EquityGraph equityGraph(const quantlib::v2::Underlying& msg,
                                const quantlib::v2::Option& option,
                                const std::string& fieldPath) const;

        QuantLib::ext::shared_ptr<QuantLib::StrikedTypePayoff>
        payoff(const quantlib::v2::Payoff& msg, const std::string& fieldPath) const;

        QuantLib::ext::shared_ptr<QuantLib::Exercise>
        exercise(const quantlib::v2::Exercise& msg, const std::string& fieldPath) const;

        //! Prices any of the supported option styles.
        /*! One method rather than one per style, because they differ only in
            the instrument and the inner engine, and every quanto pairing is a
            distinct C++ type. The switch below is an explicit instantiation
            table like `makeCurve`, for the same reason (DESIGN §6.1).
        */
        PriceOutcome priceOption(const quantlib::v2::PriceRequest& msg,
                                 const ProgressSink& progress);

        PriceOutcome priceSwap(const quantlib::v2::PriceRequest& msg);

        //! Builds one leg's cash flows.
        QuantLib::Leg buildLeg(const quantlib::v2::Leg& msg, const std::string& fieldPath);

        QuantLib::Schedule schedule(const quantlib::v2::Schedule& msg,
                                    const std::string& fieldPath);

        //! Runs Monte Carlo in batches so progress can be reported.
        PriceOutcome priceInBatches(
            const quantlib::v2::PriceRequest& msg,
            const QuantLib::ext::shared_ptr<QuantLib::VanillaOption>& option,
            const EquityGraph& graph,
            const ProgressSink& progress);

        /*! Owned, not borrowed, and bound to this session's own curve map:
            the registry resolves forwarding curves while the graph is still
            being built, so it has to see `curves_` as it fills. Market
            definitions therefore arrive in dependency order — with the one
            exception of an index's forwarding curve, which `indexHandles_`
            and `pendingLinks_` below let arrive later.
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
        std::map<std::string, QuantLib::Handle<QuantLib::BlackVolTermStructure>> vols_;
        std::map<std::string, QuantLib::ext::shared_ptr<QuantLib::IborIndex>> indices_;

        //! A correlation matrix, as the labels it indexes on and live entries.
        /*! Entries are `Handle<Quote>` rather than numbers so a correlation
            can be dragged like anything else in the quote bar. Nothing
            observes the handles, though, because the object QuantLib wants is
            a plain `Matrix`: `StochasticProcessArray` takes one by value and
            factorises it in its constructor
            (ql/processes/stochasticprocessarray.cpp). So the matrix is read
            out afresh on every price request, which is the only way a moved
            correlation reaches the answer (DESIGN §5).
        */
        struct Correlation {
            std::vector<std::string> labels;
            std::vector<QuantLib::Handle<QuantLib::Quote>> entries;  // row-major
        };
        std::map<std::string, Correlation> correlations_;

        //! The forwarding handle each index was built on, by index id.
        /*! Relinkable, because an index and the curve it forecasts off
            depend on each other: the curve's pillars name the index for its
            conventions, and the index names the curve to forecast from. One
            of them has to be defined first, so the index takes an empty
            relinkable handle and is linked the moment its curve is built.
            This is how QuantLib's own bootstrap resolves the same cycle
            (`ql/termstructures/yield/ratehelpers.cpp`, the cloned index on a
            helper's own handle), done once at the session level rather than
            once per helper.
        */
        std::map<std::string, QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>
            indexHandles_;

        //! Indices waiting on a curve that has not been built yet.
        /*! curve id -> (index id, field path of the reference). Drained as
            curves are built; anything left at the end of the market is a
            forward reference to a curve that never appeared, and is rejected
            naming the field rather than left as an empty handle that fails
            at the first forecast.
        */
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> pendingLinks_;

        //! Every id in the market namespace, in definition order.
        std::vector<std::string> marketIds_;

        bool dirty_ = false;
    };

}

#endif
