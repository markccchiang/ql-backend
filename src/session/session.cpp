/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "session.hpp"
#include "errors/fielderror.hpp"
#include "updateguard.hpp"
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/currency.hpp>
#include <ql/exercise.hpp>
#include <ql/experimental/barrieroption/quantodoublebarrieroption.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/instruments/asianoption.hpp>
#include <ql/instruments/barrieroption.hpp>
#include <ql/instruments/doublebarrieroption.hpp>
#include <ql/instruments/forwardvanillaoption.hpp>
#include <ql/instruments/lookbackoption.hpp>
#include <ql/instruments/quantobarrieroption.hpp>
#include <ql/instruments/quantoforwardvanillaoption.hpp>
#include <ql/instruments/quantovanillaoption.hpp>
#include <ql/instruments/swap.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/linearinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/methods/lattices/binomialtree.hpp>
#include <ql/patterns/lazyobject.hpp>
#include <ql/pricingengines/asian/analytic_cont_geom_av_price.hpp>
#include <ql/pricingengines/asian/analytic_discr_geom_av_price.hpp>
#include <ql/pricingengines/asian/mc_discr_arith_av_price.hpp>
#include <ql/pricingengines/barrier/analyticbarrierengine.hpp>
#include <ql/pricingengines/barrier/analyticdoublebarrierengine.hpp>
#include <ql/pricingengines/barrier/binomialbarrierengine.hpp>
#include <ql/pricingengines/barrier/fdblackscholesbarrierengine.hpp>
#include <ql/pricingengines/barrier/mcbarrierengine.hpp>
#include <ql/pricingengines/forward/forwardengine.hpp>
#include <ql/pricingengines/forward/forwardperformanceengine.hpp>
#include <ql/pricingengines/lookback/analyticcontinuousfixedlookback.hpp>
#include <ql/pricingengines/lookback/analyticcontinuousfloatinglookback.hpp>
#include <ql/pricingengines/quanto/quantoengine.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/pricingengines/vanilla/analyticdigitalamericanengine.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/pricingengines/vanilla/baroneadesiwhaleyengine.hpp>
#include <ql/pricingengines/vanilla/binomialengine.hpp>
#include <ql/pricingengines/vanilla/bjerksundstenslandengine.hpp>
#include <ql/pricingengines/vanilla/fdblackscholesvanillaengine.hpp>
#include <ql/pricingengines/vanilla/integralengine.hpp>
#include <ql/pricingengines/vanilla/juquadraticengine.hpp>
#include <ql/pricingengines/vanilla/mceuropeanengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancecurve.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancesurface.hpp>
#include <ql/termstructures/yield/bootstraptraits.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <type_traits>
#include <utility>

using namespace QuantLib;
namespace qlpb = quantlib::v2;

namespace qlservice {

    namespace {

        //! Writes a QuantLib date into the wire's ISO form.
        void writeDate(quantlib::v1::Date& out, const Date& date) {
            std::ostringstream iso;
            iso << std::setfill('0') << std::setw(4) << static_cast<int>(date.year()) << '-'
                << std::setw(2) << static_cast<int>(date.month()) << '-' << std::setw(2)
                << static_cast<int>(date.dayOfMonth());
            out.set_iso(iso.str());
        }

        //! The name a sampled quantity is reported under.
        const char* quantityName(qlpb::CurveSample::Quantity quantity) {
            switch (quantity) {
                case qlpb::CurveSample::QUANTITY_ZERO_RATE:
                    return "zeroRate";
                case qlpb::CurveSample::QUANTITY_DISCOUNT_FACTOR:
                    return "discountFactor";
                case qlpb::CurveSample::QUANTITY_FORWARD_RATE:
                    return "forwardRate";
                case qlpb::CurveSample::QUANTITY_BLACK_VOLATILITY:
                    return "blackVolatility";
                default:
                    return "unknown";
            }
        }

        double seconds(std::chrono::steady_clock::time_point from) {
            const auto elapsed = std::chrono::steady_clock::now() - from;
            return std::chrono::duration<double>(elapsed).count();
        }

        //! Reads a Flag, which has no safe default by construction.
        /*! A `bool` would price on `false` for a field the client forgot,
            silently. That is the hole `*_UNSPECIFIED` closes for enums and
            proto3 leaves open for bools, so every flag that moves a price or
            its sign is one of these (DESIGN §6.3).
        */
        bool flag(qlpb::Flag f, const std::string& fieldPath) {
            switch (f) {
                case qlpb::FLAG_TRUE:
                    return true;
                case qlpb::FLAG_FALSE:
                    return false;
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath,
                           "unspecified flag at '"
                               << fieldPath
                               << "'; it changes the price, so there is no safe default");
        }

        template <class Traits, class Interpolator>
        Handle<YieldTermStructure> piecewise(const Date& reference,
                                             std::vector<ext::shared_ptr<RateHelper>> helpers,
                                             const DayCounter& dayCounter) {
            return Handle<YieldTermStructure>(
                ext::make_shared<PiecewiseYieldCurve<Traits, Interpolator>>(
                    reference, std::move(helpers), dayCounter));
        }

        DateGeneration::Rule dateGeneration(qlpb::Schedule_DateGeneration rule,
                                            const std::string& fieldPath) {
            switch (rule) {
                case qlpb::Schedule_DateGeneration_DATE_GENERATION_BACKWARD:
                    return DateGeneration::Backward;
                case qlpb::Schedule_DateGeneration_DATE_GENERATION_FORWARD:
                    return DateGeneration::Forward;
                case qlpb::Schedule_DateGeneration_DATE_GENERATION_ZERO:
                    return DateGeneration::Zero;
                case qlpb::Schedule_DateGeneration_DATE_GENERATION_THIRD_WEDNESDAY:
                    return DateGeneration::ThirdWednesday;
                case qlpb::Schedule_DateGeneration_DATE_GENERATION_TWENTIETH:
                    return DateGeneration::Twentieth;
                case qlpb::Schedule_DateGeneration_DATE_GENERATION_TWENTIETH_IMM:
                    return DateGeneration::TwentiethIMM;
                case qlpb::Schedule_DateGeneration_DATE_GENERATION_CDS2015:
                    return DateGeneration::CDS2015;
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath,
                           "unspecified date generation rule at '" << fieldPath << "'");
        }

        Option::Type optionType(qlpb::Payoff_OptionType msg, const std::string& fieldPath) {
            switch (msg) {
                case qlpb::Payoff_OptionType_OPTION_TYPE_CALL:
                    return Option::Call;
                case qlpb::Payoff_OptionType_OPTION_TYPE_PUT:
                    return Option::Put;
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath,
                           "unspecified option type at '" << fieldPath << "'");
        }

        Barrier::Type barrierType(qlpb::Barrier_Type msg, const std::string& fieldPath) {
            switch (msg) {
                case qlpb::Barrier_Type_TYPE_DOWN_IN:
                    return Barrier::DownIn;
                case qlpb::Barrier_Type_TYPE_UP_IN:
                    return Barrier::UpIn;
                case qlpb::Barrier_Type_TYPE_DOWN_OUT:
                    return Barrier::DownOut;
                case qlpb::Barrier_Type_TYPE_UP_OUT:
                    return Barrier::UpOut;
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath,
                           "unspecified barrier type at '" << fieldPath << "'");
        }

        DoubleBarrier::Type doubleBarrierType(qlpb::DoubleBarrier_Type msg,
                                              const std::string& fieldPath) {
            switch (msg) {
                case qlpb::DoubleBarrier_Type_TYPE_KNOCK_IN:
                    return DoubleBarrier::KnockIn;
                case qlpb::DoubleBarrier_Type_TYPE_KNOCK_OUT:
                    return DoubleBarrier::KnockOut;
                case qlpb::DoubleBarrier_Type_TYPE_KIKO:
                    return DoubleBarrier::KIKO;
                case qlpb::DoubleBarrier_Type_TYPE_KOKI:
                    return DoubleBarrier::KOKI;
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath,
                           "unspecified double barrier type at '" << fieldPath << "'");
        }

        Average::Type averageType(qlpb::Asian_Averaging msg, const std::string& fieldPath) {
            switch (msg) {
                case qlpb::Asian_Averaging_AVERAGING_ARITHMETIC:
                    return Average::Arithmetic;
                case qlpb::Asian_Averaging_AVERAGING_GEOMETRIC:
                    return Average::Geometric;
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath,
                           "unspecified averaging type at '" << fieldPath << "'");
        }

        // -------------------------------------------------------------------
        // Engine assembly
        // -------------------------------------------------------------------

        //! An FD engine with its grid fixed at compile time.
        /*! `QuantoEngine` builds its inner engine as
            `make_shared<Engine>(quantoProcess)`
            (ql/pricingengines/quanto/quantoengine.hpp:114), so there is no
            seam through which an FD engine's own tGrid and xGrid can be
            passed. Baking them into the type is the only way to vary them,
            and that is what turns the grid into a menu on the quanto paths
            while an ordinary FD request can name any pair it likes.
        */
        template <class Base, std::size_t TGrid, std::size_t XGrid>
        class FixedGrid : public Base {
          public:
            explicit FixedGrid(ext::shared_ptr<GeneralizedBlackScholesProcess> process)
            : Base(std::move(process), TGrid, XGrid) {}
        };

        //! The engine for a shape, wrapped in QuantoEngine when the trade is one.
        /*! Quanto composes here rather than in the instrument table, which is
            why v2 needs no quanto-specific product messages: the pairing
            (style, quanto) is resolved once, in one place, for every style
            whose engine is constructible from a process alone (DESIGN §6.3).
        */
        template <class Instr, class Eng>
        ext::shared_ptr<PricingEngine> engineFor(const Session::EquityGraph& g) {
            if (g.quanto)
                return ext::make_shared<QuantoEngine<Instr, Eng>>(g.process, g.fxRiskFree, g.fxVol,
                                                                  g.correlation);
            return ext::make_shared<Eng>(g.process);
        }

        std::pair<Size, Size> presetGrid(qlpb::FdParameters_Preset preset,
                                         const std::string& fieldPath) {
            switch (preset) {
                case qlpb::FdParameters_Preset_PRESET_COARSE:
                    return {100, 100};
                case qlpb::FdParameters_Preset_PRESET_STANDARD:
                    return {400, 200};
                case qlpb::FdParameters_Preset_PRESET_FINE:
                    return {2000, 800};
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath,
                           "a finite-difference request needs an explicit grid at '"
                               << fieldPath << "': two grids are two different prices for the "
                                               "same trade, so there is no safe default");
        }

        //! FD engine: a plain number when plain, a compiled grid when quanto.
        template <class Instr, class FdBase>
        ext::shared_ptr<PricingEngine> fdEngineFor(const qlpb::FdParameters& fd,
                                                   const Session::EquityGraph& g,
                                                   const std::string& fieldPath) {
            if (!g.quanto) {
                if (fd.has_custom()) {
                    const auto& c = fd.custom();
                    QLS_FIELD_REQUIRE(c.time_steps() > 0 && c.asset_steps() > 0,
                                      qlpb::Error::INVALID_ARGUMENT, fieldPath + ".custom",
                                      "a finite-difference grid needs both dimensions");
                    return ext::make_shared<FdBase>(g.process, c.time_steps(), c.asset_steps());
                }
                const auto grid = presetGrid(fd.preset(), fieldPath + ".preset");
                return ext::make_shared<FdBase>(g.process, grid.first, grid.second);
            }

            // The quanto side cannot take a number, only one of the three
            // compiled-in grids. Rejected loudly rather than rounded to the
            // nearest preset: a parameter the client can see us take and
            // cannot see us ignore is the defect this schema exists to avoid.
            QLS_FIELD_REQUIRE(!fd.has_custom(), qlpb::Error::UNSUPPORTED, fieldPath + ".custom",
                              "a quanto finite-difference request takes one of the grid presets, "
                              "not an explicit size: QuantoEngine constructs its inner engine as "
                              "make_shared<Engine>(process) and leaves no seam to pass a grid "
                              "through, so each grid is a separate compiled type");

            switch (fd.preset()) {
                case qlpb::FdParameters_Preset_PRESET_COARSE:
                    return ext::make_shared<QuantoEngine<Instr, FixedGrid<FdBase, 100, 100>>>(
                        g.process, g.fxRiskFree, g.fxVol, g.correlation);
                case qlpb::FdParameters_Preset_PRESET_STANDARD:
                    return ext::make_shared<QuantoEngine<Instr, FixedGrid<FdBase, 400, 200>>>(
                        g.process, g.fxRiskFree, g.fxVol, g.correlation);
                case qlpb::FdParameters_Preset_PRESET_FINE:
                    return ext::make_shared<QuantoEngine<Instr, FixedGrid<FdBase, 2000, 800>>>(
                        g.process, g.fxRiskFree, g.fxVol, g.correlation);
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath + ".preset",
                           "a finite-difference request needs an explicit grid at '"
                               << fieldPath << ".preset'");
        }

        //! The binomial tree table. Each tree is a distinct type (DESIGN §6.1).
        ext::shared_ptr<PricingEngine> latticeVanillaEngine(const qlpb::LatticeParameters& lat,
                                                            const Session::EquityGraph& g,
                                                            const std::string& fieldPath) {
            QLS_FIELD_REQUIRE(lat.steps() > 0, qlpb::Error::INVALID_ARGUMENT, fieldPath + ".steps",
                              "a lattice request needs a step count");
            QLS_FIELD_REQUIRE(!g.quanto, qlpb::Error::UNSUPPORTED, "engine.method",
                              "there is no quanto binomial engine: QuantoEngine wraps an engine "
                              "constructed from a process alone, and a tree also needs its steps");
            const Size n = lat.steps();
            switch (lat.tree()) {
                case qlpb::LatticeParameters_Tree_TREE_COX_ROSS_RUBINSTEIN:
                    return ext::make_shared<BinomialVanillaEngine<CoxRossRubinstein>>(g.process, n);
                case qlpb::LatticeParameters_Tree_TREE_JARROW_RUDD:
                    return ext::make_shared<BinomialVanillaEngine<JarrowRudd>>(g.process, n);
                case qlpb::LatticeParameters_Tree_TREE_ADDITIVE_EQUIPROBABILITIES:
                    return ext::make_shared<BinomialVanillaEngine<AdditiveEQPBinomialTree>>(
                        g.process, n);
                case qlpb::LatticeParameters_Tree_TREE_TRIGEORGIS:
                    return ext::make_shared<BinomialVanillaEngine<Trigeorgis>>(g.process, n);
                case qlpb::LatticeParameters_Tree_TREE_TIAN:
                    return ext::make_shared<BinomialVanillaEngine<Tian>>(g.process, n);
                case qlpb::LatticeParameters_Tree_TREE_LEISEN_REIMER:
                    return ext::make_shared<BinomialVanillaEngine<LeisenReimer>>(g.process, n);
                case qlpb::LatticeParameters_Tree_TREE_JOSHI4:
                    return ext::make_shared<BinomialVanillaEngine<Joshi4>>(g.process, n);
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath + ".tree",
                           "unspecified binomial tree at '" << fieldPath << ".tree'");
        }

        //! Detects the quanto greeks, which are mixed in per instrument.
        /*! `QuantoOptionResults` is a mixin
            (ql/instruments/quantovanillaoption.hpp:34), so `qrho()` is found
            by name on each quanto instrument and by inheritance on none.
            There is no base class to dispatch on, hence a trait.
        */
        template <class T, class = void>
        struct HasQuantoGreeks : std::false_type {};

        template <class T>
        struct HasQuantoGreeks<T, std::void_t<decltype(std::declval<T&>().qrho())>>
        : std::true_type {};

        //! Whether QuantLib can invert a price on this instrument.
        /*! Declared on VanillaOption, BarrierOption and DoubleBarrierOption
            and on no base they share, so it is a trait for the same reason
            HasQuantoGreeks is.
        */
        template <class T, class = void>
        struct HasImpliedVolatility : std::false_type {};

        template <class T>
        struct HasImpliedVolatility<
            T,
            std::void_t<decltype(std::declval<T&>().impliedVolatility(
                Real(), ext::shared_ptr<GeneralizedBlackScholesProcess>()))>> : std::true_type {};

        //! Prices one instrument and collects the results asked for.
        template <class Instrument>
        Session::PriceOutcome run(const ext::shared_ptr<Instrument>& option,
                                  const ext::shared_ptr<PricingEngine>& engine,
                                  const qlpb::PriceRequest& msg,
                                  const ext::shared_ptr<GeneralizedBlackScholesProcess>& process =
                                      nullptr) {
            option->setPricingEngine(engine);

            const auto start = std::chrono::steady_clock::now();

            Session::PriceOutcome out;
            out.npv = option->NPV();

            // Always, never on request: a Monte Carlo price without its
            // standard error is not comparable to another price, and a client
            // that has to know to ask for it will compare them anyway. Engines
            // that do not produce one simply have no entry.
            try {
                out.results["errorEstimate"] = option->errorEstimate();
            } catch (const Error&) {
            }

            if (msg.include_additional_results()) {
                // What the engine published on its own account, scalars only:
                // everything this layer reports is a Real, and the vector and
                // matrix results some engines add are a Value the worker does
                // not map yet. Same key namespace as the named greeks on
                // purpose -- QuantLib's engines already use "delta" for delta.
                for (const auto& entry : option->additionalResults())
                    if (const auto* x = std::any_cast<Real>(&entry.second))
                        out.results[entry.first] = *x;
            }

            for (const auto kind : msg.results()) {
                // Results are fetched by name and QuantLib throws when the
                // engine did not produce one (ql/instrument.hpp:193). A
                // missing greek is not a failed request: AnalyticEuropeanEngine
                // has vega, the binomial one does not, and the frontend asks
                // both the same question. What it must not be is silent, so
                // whatever this loop does not produce is named below.
                const auto produced = out.results.size();
                try {
                    switch (kind) {
                        case qlpb::RESULT_KIND_IMPLIED_VOLATILITY:
                            // A root find rather than a published result, so
                            // it needs the process the engine was built from
                            // and a price to look for. Where QuantLib cannot
                            // invert this instrument, nothing is produced and
                            // the kind is named absent like any other.
                            if constexpr (HasImpliedVolatility<Instrument>::value) {
                                if (process) {
                                    const auto& iv = msg.implied_volatility();
                                    out.results["impliedVolatility"] = option->impliedVolatility(
                                        iv.target_price(), process,
                                        iv.accuracy() > 0.0 ? iv.accuracy() : 1.0e-4,
                                        iv.max_evaluations() > 0
                                            ? static_cast<Size>(iv.max_evaluations())
                                            : 100,
                                        iv.min_volatility() > 0.0 ? iv.min_volatility() : 1.0e-7,
                                        iv.max_volatility() > 0.0 ? iv.max_volatility() : 4.0);
                                }
                            }
                            break;
                        case qlpb::RESULT_KIND_DELTA:
                            out.results["delta"] = option->delta();
                            break;
                        case qlpb::RESULT_KIND_GAMMA:
                            out.results["gamma"] = option->gamma();
                            break;
                        case qlpb::RESULT_KIND_VEGA:
                            out.results["vega"] = option->vega();
                            break;
                        case qlpb::RESULT_KIND_THETA:
                            out.results["theta"] = option->theta();
                            break;
                        case qlpb::RESULT_KIND_THETA_PER_DAY:
                            out.results["thetaPerDay"] = option->thetaPerDay();
                            break;
                        case qlpb::RESULT_KIND_RHO:
                            out.results["rho"] = option->rho();
                            break;
                        case qlpb::RESULT_KIND_DIVIDEND_RHO:
                            out.results["dividendRho"] = option->dividendRho();
                            break;
                        case qlpb::RESULT_KIND_DELTA_FORWARD:
                            out.results["deltaForward"] = option->deltaForward();
                            break;
                        case qlpb::RESULT_KIND_ELASTICITY:
                            out.results["elasticity"] = option->elasticity();
                            break;
                        case qlpb::RESULT_KIND_STRIKE_SENSITIVITY:
                            out.results["strikeSensitivity"] = option->strikeSensitivity();
                            break;
                        case qlpb::RESULT_KIND_ITM_CASH_PROBABILITY:
                            out.results["itmCashProbability"] = option->itmCashProbability();
                            break;
                        case qlpb::RESULT_KIND_QRHO:
                            if constexpr (HasQuantoGreeks<Instrument>::value)
                                out.results["qrho"] = option->qrho();
                            break;
                        case qlpb::RESULT_KIND_QVEGA:
                            if constexpr (HasQuantoGreeks<Instrument>::value)
                                out.results["qvega"] = option->qvega();
                            break;
                        case qlpb::RESULT_KIND_QLAMBDA:
                            if constexpr (HasQuantoGreeks<Instrument>::value)
                                out.results["qlambda"] = option->qlambda();
                            break;
                        default:
                            // A swap result asked of an option: absent, not an error.
                            break;
                    }
                } catch (const Error&) {
                    // Not published by this engine; named just below.
                }

                // NPV is the result's own field rather than a map entry, so it
                // is never missing and never reported so.
                if (out.results.size() == produced && kind != qlpb::RESULT_KIND_NPV)
                    out.unavailable.push_back(static_cast<qlpb::ResultKind>(kind));
            }

            out.calculationSeconds = seconds(start);
            return out;
        }

    }


    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    Session::Session(const qlpb::OpenSession& msg)
    : registry_([this](const std::string& curveId) { return curve(curveId); }) {

        evaluationDate_ = registry_.date(msg.evaluation_date(), "evaluation_date");

        // Thread-local under QL_ENABLE_SESSIONS, so this sets the date for
        // this session alone. In a default build it would set it for every
        // session in the process (ql/patterns/singleton.hpp:104).
        Settings::instance().evaluationDate() = evaluationDate_;

        buildMarket(msg);
    }


    Handle<YieldTermStructure> Session::curve(const std::string& curveId) const {
        auto it = curves_.find(curveId);
        QL_REQUIRE(it != curves_.end(), "unknown curve '" << curveId << "'");
        return it->second;
    }


    Handle<YieldTermStructure> Session::curveHandle(const std::string& curveId,
                                                    const std::string& fieldPath) const {
        auto it = curves_.find(curveId);
        QLS_FIELD_REQUIRE(it != curves_.end(), qlpb::Error::UNKNOWN_ID, fieldPath,
                          "unknown curve '" << curveId << "' at '" << fieldPath << "'");
        return it->second;
    }


    Handle<Quote> Session::quoteHandle(const std::string& quoteId,
                                       const std::string& fieldPath) const {
        auto it = quotes_.find(quoteId);
        QLS_FIELD_REQUIRE(it != quotes_.end(), qlpb::Error::UNKNOWN_ID, fieldPath,
                          "unknown quote '" << quoteId << "' at '" << fieldPath << "'");
        // The handle, never the value: an instrument built on the number
        // would price once and then ignore every UpdateMarket (DESIGN §5).
        return Handle<Quote>(it->second);
    }


    Handle<Quote> Session::number(const qlpb::Number& msg, const std::string& fieldPath) const {
        switch (msg.source_case()) {
            case qlpb::Number::kQuoteId:
                return quoteHandle(msg.quote_id(), fieldPath + ".quote_id");
            case qlpb::Number::kFixed:
                // Still a handle, so the graph shape does not depend on which
                // arm was used; it simply names a quote nobody can write.
                return Handle<Quote>(ext::make_shared<SimpleQuote>(msg.fixed()));
            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, fieldPath,
                       "no value set at '" << fieldPath
                                           << "'; a Number is either a quote id or a constant");
    }


    Handle<BlackVolTermStructure> Session::volatility(const std::string& volId,
                                                      const std::string& fieldPath) const {
        auto it = vols_.find(volId);
        QLS_FIELD_REQUIRE(it != vols_.end(), qlpb::Error::UNKNOWN_ID, fieldPath,
                          "unknown volatility '" << volId << "' at '" << fieldPath << "'");
        return it->second;
    }


    ext::shared_ptr<IborIndex> Session::indexById(const std::string& indexId,
                                                  const std::string& fieldPath) const {
        auto it = indices_.find(indexId);
        QLS_FIELD_REQUIRE(it != indices_.end(), qlpb::Error::UNKNOWN_ID, fieldPath,
                          "unknown index '" << indexId << "' at '" << fieldPath << "'");
        return it->second;
    }


    Date Session::expiryDate(const quantlib::v1::Date& msg, const std::string& fieldPath) const {
        const Date expiry = registry_.date(msg, fieldPath);
        QLS_FIELD_REQUIRE(expiry > evaluationDate_, qlpb::Error::INVALID_ARGUMENT, fieldPath,
                          "expiry " << expiry << " is not after the evaluation date "
                                    << evaluationDate_);
        return expiry;
    }


    // -----------------------------------------------------------------------
    // The market namespace
    // -----------------------------------------------------------------------

    void Session::buildMarket(const qlpb::OpenSession& msg) {
        for (int i = 0; i < msg.market_size(); ++i) {
            const auto& obj = msg.market(i);
            const std::string path = "market[" + std::to_string(i) + "]";

            QLS_FIELD_REQUIRE(!obj.id().empty(), qlpb::Error::INVALID_ARGUMENT, path + ".id",
                              "market object at '" << path << "' has no id");

            // One namespace across every kind, so a curve and a quote cannot
            // share an id and leave a reference ambiguous.
            const bool taken = quotes_.count(obj.id()) != 0 || curves_.count(obj.id()) != 0 ||
                               vols_.count(obj.id()) != 0 || indices_.count(obj.id()) != 0;
            QLS_FIELD_REQUIRE(!taken, qlpb::Error::INVALID_ARGUMENT, path + ".id",
                              "duplicate market id '" << obj.id() << "'");

            switch (obj.kind_case()) {
                case qlpb::MarketObject::kQuote:
                    quotes_[obj.id()] = ext::make_shared<SimpleQuote>(obj.quote().value());
                    break;
                case qlpb::MarketObject::kYieldCurve:
                    buildYieldCurve(obj.id(), obj.yield_curve(), path + ".yield_curve");
                    break;
                case qlpb::MarketObject::kVolatility:
                    buildVolatility(obj.id(), obj.volatility(), path + ".volatility");
                    break;
                case qlpb::MarketObject::kIndex:
                    buildIndex(obj.id(), obj.index(), path + ".index");
                    break;
                case qlpb::MarketObject::kFixings:
                    applyFixings(obj.fixings(), path + ".fixings");
                    break;
                default:
                    QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, path,
                                   "market object kind at '"
                                       << path << "' is in the schema but not implemented");
            }
            marketIds_.push_back(obj.id());

            // A curve that just appeared may be the one an earlier index was
            // waiting for.
            if (obj.has_yield_curve()) {
                auto waiting = pendingLinks_.find(obj.id());
                if (waiting != pendingLinks_.end()) {
                    for (const auto& [indexId, refPath] : waiting->second)
                        indexHandles_.at(indexId).linkTo(curves_.at(obj.id()).currentLink());
                    pendingLinks_.erase(waiting);
                }
            }
        }

        // Anything still waiting named a curve that never appeared. Rejected
        // here rather than left as an empty handle: an index built on one
        // prices happily until the first forecast and then fails a long way
        // from the cause, with no field to blame.
        if (!pendingLinks_.empty()) {
            const auto& [curveId, refs] = *pendingLinks_.begin();
            QLS_FIELD_FAIL(qlpb::Error::UNKNOWN_ID, refs.front().second,
                           "index '" << refs.front().first << "' forecasts off curve '" << curveId
                                     << "', which is not defined anywhere in this market");
        }
    }


    void Session::buildYieldCurve(const std::string& id,
                                  const qlpb::YieldCurve& def,
                                  const std::string& fieldPath) {
        const auto dc = registry_.dayCounter(def.day_counter(), fieldPath + ".day_counter");

        // Interpolated shapes take vectors of numbers, not handles: QuantLib's
        // InterpolatedZeroCurve copies its rates at construction and observes
        // nothing. Accepting a quote id there would look live and never move,
        // which is exactly the failure DESIGN §5 exists to prevent, so those
        // nodes must be `fixed` and a quote id is refused by name.
        auto nodeValue = [](const qlpb::Number& n, const std::string& path) {
            QLS_FIELD_REQUIRE(n.source_case() != qlpb::Number::kQuoteId, qlpb::Error::UNSUPPORTED,
                              path + ".quote_id",
                              "an interpolated curve copies its nodes at construction and never "
                              "observes them, so a quote id here would never move the curve; use "
                              "'fixed', or a bootstrapped or flat curve for live pillars");
            return n.fixed();
        };

        auto nodeDates = [&](const qlpb::InterpolatedCurve& ic, const std::string& path) {
            std::vector<Date> dates;
            QLS_FIELD_REQUIRE(ic.nodes_size() > 0, qlpb::Error::INVALID_ARGUMENT, path + ".nodes",
                              "an interpolated curve needs nodes");
            for (int i = 0; i < ic.nodes_size(); ++i) {
                const auto& n = ic.nodes(i);
                const std::string p = path + ".nodes[" + std::to_string(i) + "]";
                switch (n.when_case()) {
                    case qlpb::InterpolatedCurve_Node::kDate:
                        dates.push_back(registry_.date(n.date(), p + ".date"));
                        break;
                    case qlpb::InterpolatedCurve_Node::kTenor:
                        dates.push_back(evaluationDate_ + registry_.period(n.tenor(), p + ".tenor"));
                        break;
                    default:
                        QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, p,
                                       "node at '" << p << "' has neither a date nor a tenor");
                }
            }
            // The first node is the curve's reference date: QuantLib takes it
            // from dates[0], so a curve whose first pillar is 6M out starts
            // six months from now and discounts nothing before then. It has
            // to be today, said explicitly, rather than inferred.
            QLS_FIELD_REQUIRE(dates.front() == evaluationDate_, qlpb::Error::INVALID_ARGUMENT,
                              path + ".nodes[0]",
                              "the first node of an interpolated curve must sit on the "
                              "evaluation date " << evaluationDate_ << " (tenor \"0D\"), got "
                                                 << dates.front()
                                                 << "; the curve's reference date is taken from it");
            return dates;
        };

        switch (def.shape_case()) {

            case qlpb::YieldCurve::kFlat: {
                const std::string path = fieldPath + ".flat";
                const auto& flat = def.flat();
                // Reference date, not settlement days: this curve has no
                // market instrument to imply anything from, which is the whole
                // point of the shape.
                curves_[id] = Handle<YieldTermStructure>(ext::make_shared<FlatForward>(
                    evaluationDate_, number(flat.rate(), path + ".rate"), dc,
                    registry_.compounding(flat.compounding(), path + ".compounding"),
                    registry_.frequency(flat.frequency(), path + ".frequency")));
                return;
            }

            case qlpb::YieldCurve::kZero: {
                const std::string path = fieldPath + ".zero";
                const auto& ic = def.zero();
                auto dates = nodeDates(ic, path);
                std::vector<Rate> rates;
                for (int i = 0; i < ic.nodes_size(); ++i)
                    rates.push_back(nodeValue(ic.nodes(i).value(),
                                              path + ".nodes[" + std::to_string(i) + "].value"));
                QLS_FIELD_REQUIRE(dates.size() >= 2, qlpb::Error::INVALID_ARGUMENT, path + ".nodes",
                                  "a zero curve needs at least two nodes");
                curves_[id] = Handle<YieldTermStructure>(
                    ext::make_shared<InterpolatedZeroCurve<Linear>>(
                        dates, rates, dc,
                        registry_.calendar(def.calendar(), fieldPath + ".calendar"), Linear(),
                        registry_.compounding(ic.compounding(), path + ".compounding"),
                        registry_.frequency(ic.frequency(), path + ".frequency")));
                return;
            }

            case qlpb::YieldCurve::kDiscount: {
                const std::string path = fieldPath + ".discount";
                const auto& ic = def.discount();
                auto dates = nodeDates(ic, path);
                std::vector<DiscountFactor> dfs;
                for (int i = 0; i < ic.nodes_size(); ++i)
                    dfs.push_back(nodeValue(ic.nodes(i).value(),
                                            path + ".nodes[" + std::to_string(i) + "].value"));
                QLS_FIELD_REQUIRE(dates.size() >= 2, qlpb::Error::INVALID_ARGUMENT, path + ".nodes",
                                  "a discount curve needs at least two nodes");
                curves_[id] = Handle<YieldTermStructure>(
                    ext::make_shared<InterpolatedDiscountCurve<LogLinear>>(dates, dfs, dc));
                return;
            }

            case qlpb::YieldCurve::kBootstrap: {
                const std::string path = fieldPath + ".bootstrap";
                const auto& boot = def.bootstrap();
                QLS_FIELD_REQUIRE(boot.pillars_size() > 0, qlpb::Error::INVALID_ARGUMENT,
                                  path + ".pillars", "curve '" << id << "' has no pillars");
                std::vector<ext::shared_ptr<RateHelper>> helpers;
                helpers.reserve(boot.pillars_size());
                for (int p = 0; p < boot.pillars_size(); ++p)
                    helpers.push_back(buildHelper(boot.pillars(p), def,
                                                  path + ".pillars[" + std::to_string(p) + "]"));
                // Curves go in as they are built, so a later curve's index can
                // forward off an earlier one. A forward reference fails in the
                // resolver rather than producing an empty handle.
                curves_[id] = makeCurve(boot, std::move(helpers), dc, path);
                return;
            }

            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, fieldPath,
                       "curve shape at '" << fieldPath << "' is in the schema but not implemented");
    }


    ext::shared_ptr<RateHelper> Session::buildHelper(const qlpb::Pillar& pillar,
                                                     const qlpb::YieldCurve& def,
                                                     const std::string& fieldPath) {
        // The helper takes the handle, not the value: a quote write then moves
        // the curve, every instrument discounting off it, and nothing else.
        // That propagation is the whole reason the session is stateful.
        const Handle<Quote> rate = quoteHandle(pillar.quote_id(), fieldPath + ".quote_id");
        const auto index = indexById(pillar.index_id(), fieldPath + ".index_id");

        switch (pillar.kind()) {

            case qlpb::Pillar_Kind_KIND_DEPOSIT:
                // Tenor from the pillar, conventions from the index. The
                // two-argument DepositRateHelper(rate, index) takes both from
                // the index, which makes every deposit on a curve the same
                // tenor as the one index its pillars share -- four pillars all
                // at 6M, and a bootstrap that fails on duplicate dates at
                // price time with no field to blame.
                return ext::make_shared<DepositRateHelper>(
                    rate, registry_.period(pillar.tenor(), fieldPath + ".tenor"),
                    index->fixingDays(), index->fixingCalendar(), index->businessDayConvention(),
                    index->endOfMonth(), index->dayCounter());

            case qlpb::Pillar_Kind_KIND_SWAP:
                return ext::make_shared<SwapRateHelper>(
                    rate, registry_.period(pillar.tenor(), fieldPath + ".tenor"),
                    registry_.calendar(pillar.calendar(), fieldPath + ".calendar"),
                    registry_.frequency(pillar.fixed_frequency(), fieldPath + ".fixed_frequency"),
                    registry_.businessDayConvention(pillar.fixed_convention(),
                                                    fieldPath + ".fixed_convention"),
                    registry_.dayCounter(pillar.fixed_day_counter(),
                                         fieldPath + ".fixed_day_counter"),
                    index);

            case qlpb::Pillar_Kind_KIND_OIS: {
                // The map holds an IborIndex; an OIS helper needs the
                // overnight subtype. Sending SOFR here and EURIBOR there is an
                // easy client mistake and the cast is what catches it.
                const auto overnight = ext::dynamic_pointer_cast<OvernightIndex>(index);
                QLS_FIELD_REQUIRE(overnight != nullptr, qlpb::Error::INVALID_ARGUMENT,
                                  fieldPath + ".index_id",
                                  "OIS pillar at '" << fieldPath
                                                    << "' needs an overnight index, got '"
                                                    << index->name() << "'");
                return ext::make_shared<OISRateHelper>(
                    def.settlement_days(), registry_.period(pillar.tenor(), fieldPath + ".tenor"),
                    rate, overnight);
            }

            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath + ".kind",
                       "unspecified or unimplemented pillar kind at '" << fieldPath << ".kind'");
    }


    Handle<YieldTermStructure> Session::makeCurve(const qlpb::BootstrappedCurve& boot,
                                                  std::vector<ext::shared_ptr<RateHelper>> helpers,
                                                  const DayCounter& dc,
                                                  const std::string& fieldPath) {
        QLS_FIELD_REQUIRE(boot.traits() != qlpb::BootstrappedCurve_Traits_TRAITS_UNSPECIFIED,
                          qlpb::Error::UNSPECIFIED_ENUM, fieldPath + ".traits",
                          "unspecified bootstrap traits at '" << fieldPath << ".traits'");

        // Each pair below is a distinct C++ type, so this table is the schema:
        // a combination absent here cannot be requested over the wire, and
        // adding one means adding a line of source and recompiling. That is
        // the price of PiecewiseYieldCurve being a template, and the reason
        // the enum is deliberately small (DESIGN §6.1).
        switch (boot.traits()) {
            case qlpb::BootstrappedCurve_Traits_TRAITS_DISCOUNT:
                switch (boot.interpolator()) {
                    case qlpb::INTERPOLATOR_LINEAR:
                        return piecewise<Discount, Linear>(evaluationDate_, std::move(helpers), dc);
                    case qlpb::INTERPOLATOR_LOG_LINEAR:
                        return piecewise<Discount, LogLinear>(evaluationDate_, std::move(helpers),
                                                              dc);
                    case qlpb::INTERPOLATOR_CUBIC:
                        return piecewise<Discount, Cubic>(evaluationDate_, std::move(helpers), dc);
                    default:
                        break;
                }
                break;

            case qlpb::BootstrappedCurve_Traits_TRAITS_ZERO_YIELD:
                switch (boot.interpolator()) {
                    case qlpb::INTERPOLATOR_LINEAR:
                        return piecewise<ZeroYield, Linear>(evaluationDate_, std::move(helpers), dc);
                    case qlpb::INTERPOLATOR_LOG_LINEAR:
                        return piecewise<ZeroYield, LogLinear>(evaluationDate_, std::move(helpers),
                                                               dc);
                    case qlpb::INTERPOLATOR_CUBIC:
                        return piecewise<ZeroYield, Cubic>(evaluationDate_, std::move(helpers), dc);
                    default:
                        break;
                }
                break;

            case qlpb::BootstrappedCurve_Traits_TRAITS_FORWARD_RATE:
                switch (boot.interpolator()) {
                    case qlpb::INTERPOLATOR_LINEAR:
                        return piecewise<ForwardRate, Linear>(evaluationDate_, std::move(helpers),
                                                              dc);
                    case qlpb::INTERPOLATOR_LOG_LINEAR:
                        return piecewise<ForwardRate, LogLinear>(evaluationDate_,
                                                                 std::move(helpers), dc);
                    case qlpb::INTERPOLATOR_CUBIC:
                        return piecewise<ForwardRate, Cubic>(evaluationDate_, std::move(helpers),
                                                             dc);
                    default:
                        break;
                }
                break;

            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, fieldPath,
                       "unsupported traits/interpolator pair at '" << fieldPath << "'");
    }


    void Session::buildVolatility(const std::string& id,
                                  const qlpb::VolatilitySurface& def,
                                  const std::string& fieldPath) {
        const auto dc = registry_.dayCounter(def.day_counter(), fieldPath + ".day_counter");
        // NullCalendar is the honest default for a surface with no schedule to
        // roll on; a client that wants business-day variance says so.
        const Calendar cal = def.has_calendar()
                                 ? registry_.calendar(def.calendar(), fieldPath + ".calendar")
                                 : Calendar(NullCalendar());

        switch (def.shape_case()) {

            case qlpb::VolatilitySurface::kConstant: {
                const std::string path = fieldPath + ".constant";
                // Live, and the only vol shape that is: BlackConstantVol takes
                // a Handle<Quote> and observes it, so the frontend's vol
                // slider moves every option built on this surface.
                vols_[id] = Handle<BlackVolTermStructure>(ext::make_shared<BlackConstantVol>(
                    evaluationDate_, cal, number(def.constant().volatility(), path + ".volatility"),
                    dc));
                return;
            }

            case qlpb::VolatilitySurface::kVarianceCurve: {
                const std::string path = fieldPath + ".variance_curve";
                const auto& vc = def.variance_curve();
                std::vector<Date> dates;
                std::vector<Volatility> vols;
                for (int i = 0; i < vc.nodes_size(); ++i) {
                    const std::string p = path + ".nodes[" + std::to_string(i) + "]";
                    dates.push_back(registry_.date(vc.nodes(i).expiry(), p + ".expiry"));
                    QLS_FIELD_REQUIRE(
                        vc.nodes(i).volatility().source_case() != qlpb::Number::kQuoteId,
                        qlpb::Error::UNSUPPORTED, p + ".volatility.quote_id",
                        "BlackVarianceCurve copies its volatilities at construction and never "
                        "observes them; use 'fixed'");
                    vols.push_back(vc.nodes(i).volatility().fixed());
                }
                QLS_FIELD_REQUIRE(!dates.empty(), qlpb::Error::INVALID_ARGUMENT, path + ".nodes",
                                  "a variance curve needs at least one node");
                auto curve = ext::make_shared<BlackVarianceCurve>(evaluationDate_, dates, vols, dc);
                if (vc.extrapolate())
                    curve->enableExtrapolation();
                vols_[id] = Handle<BlackVolTermStructure>(curve);
                return;
            }

            case qlpb::VolatilitySurface::kVarianceSurface: {
                const std::string path = fieldPath + ".variance_surface";
                const auto& vs = def.variance_surface();
                std::vector<Date> expiries;
                for (int i = 0; i < vs.expiries_size(); ++i)
                    expiries.push_back(registry_.date(
                        vs.expiries(i), path + ".expiries[" + std::to_string(i) + "]"));
                std::vector<Real> strikes(vs.strikes().begin(), vs.strikes().end());

                QLS_FIELD_REQUIRE(
                    vs.volatilities_size() == static_cast<int>(expiries.size() * strikes.size()),
                    qlpb::Error::INVALID_ARGUMENT, path + ".volatilities",
                    "a variance surface needs expiries x strikes volatilities, got "
                        << vs.volatilities_size() << " for " << expiries.size() << " x "
                        << strikes.size());

                // Row-major over (expiry, strike) on the wire; QuantLib's
                // Matrix here is strikes x expiries, so this transposes rather
                // than reshapes. Getting it the wrong way round produces a
                // surface that prices without complaint.
                Matrix v(strikes.size(), expiries.size());
                for (Size i = 0; i < expiries.size(); ++i)
                    for (Size j = 0; j < strikes.size(); ++j)
                        v[j][i] = vs.volatilities(static_cast<int>(i * strikes.size() + j)).fixed();

                vols_[id] = Handle<BlackVolTermStructure>(ext::make_shared<BlackVarianceSurface>(
                    evaluationDate_, cal, expiries, strikes, v, dc));
                return;
            }

            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, fieldPath,
                       "volatility shape at '" << fieldPath
                                               << "' is in the schema but not implemented");
    }


    void Session::buildIndex(const std::string& id,
                             const qlpb::Index& def,
                             const std::string& fieldPath) {
        const auto dc = registry_.dayCounter(def.day_counter(), fieldPath + ".day_counter");
        const auto cal = registry_.calendar(def.fixing_calendar(), fieldPath + ".fixing_calendar");

        // Empty is legal and means "past fixings only": an index used by a leg
        // whose periods have all fixed needs no forecast curve, and demanding
        // one would force a client to invent it.
        //
        // Otherwise the handle is relinkable and may be linked later: the
        // curve this index forecasts off is usually bootstrapped from pillars
        // that name this very index for their conventions, so it cannot
        // exist yet. A forward reference is legal here and resolved when the
        // curve is built; one that never resolves is rejected at the end of
        // the market (buildMarket), by field.
        RelinkableHandle<YieldTermStructure> forwarding;
        if (!def.forwarding_curve_id().empty()) {
            auto it = curves_.find(def.forwarding_curve_id());
            if (it != curves_.end())
                forwarding.linkTo(it->second.currentLink());
            else
                pendingLinks_[def.forwarding_curve_id()].emplace_back(
                    id, fieldPath + ".forwarding_curve_id");
        }
        indexHandles_[id] = forwarding;

        QLS_FIELD_REQUIRE(!def.name().empty(), qlpb::Error::INVALID_ARGUMENT, fieldPath + ".name",
                          "an index needs a family name");

        switch (def.family()) {
            case qlpb::Index_Family_FAMILY_IBOR:
                indices_[id] = ext::make_shared<IborIndex>(
                    def.name(), registry_.period(def.tenor(), fieldPath + ".tenor"),
                    def.fixing_days(), Currency(), cal,
                    registry_.businessDayConvention(def.convention(), fieldPath + ".convention"),
                    flag(def.end_of_month(), fieldPath + ".end_of_month"), dc, forwarding);
                return;
            case qlpb::Index_Family_FAMILY_OVERNIGHT:
                indices_[id] = ext::make_shared<OvernightIndex>(def.name(), def.fixing_days(),
                                                                Currency(), cal, dc, forwarding);
                return;
            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, fieldPath + ".family",
                       "index family at '" << fieldPath
                                           << ".family' is unspecified or not implemented");
    }


    void Session::applyFixings(const qlpb::FixingSeries& msg, const std::string& fieldPath) {
        const auto index = indexById(msg.index_id(), fieldPath + ".index_id");
        for (int i = 0; i < msg.fixings_size(); ++i) {
            const std::string p = fieldPath + ".fixings[" + std::to_string(i) + "]";
            // forceOverwrite, because a replayed session log re-adds the same
            // fixings to a thread-local IndexManager that may already hold
            // them from the session that died (DESIGN §2.1).
            index->addFixing(registry_.date(msg.fixings(i).date(), p + ".date"),
                             msg.fixings(i).value(), true);
        }
    }


    // -----------------------------------------------------------------------
    // Market updates
    // -----------------------------------------------------------------------

    void Session::apply(const qlpb::UpdateMarket& msg) {
        QL_REQUIRE(!dirty_, "session is dirty and must be replayed, not updated");
        QLS_FIELD_REQUIRE(msg.quotes_size() > 0 || msg.fixings_size() > 0,
                          qlpb::Error::INVALID_ARGUMENT, "quotes", "empty market update");

        // One guard around the whole batch: N quote writes, one recalculation.
        UpdateGuard guard;
        try {
            for (const auto& u : msg.quotes()) {
                auto it = quotes_.find(u.quote_id());
                QLS_FIELD_REQUIRE(it != quotes_.end(), qlpb::Error::UNKNOWN_ID, "quotes",
                                  "unknown quote '" << u.quote_id() << "'");
                it->second->setValue(u.value());
            }
            for (int i = 0; i < msg.fixings_size(); ++i)
                applyFixings(msg.fixings(i), "fixings[" + std::to_string(i) + "]");
        } catch (...) {
            // Some writes landed and some did not. Commit anyway so the graph
            // sees the ones that did, then refuse further use of the session.
            dirty_ = true;
            throw;
        }

        try {
            guard.commit();
        } catch (...) {
            // enableUpdates() tries every deferred observer and raises
            // afterwards, so an exception here means part of the graph was
            // invalidated and part was not. Nothing cached can be trusted.
            dirty_ = true;
            throw;
        }

        if (msg.force_rebootstrap()) {
            // Normally unnecessary: a piecewise curve is a LazyObject that
            // observes its helpers' quotes, so the writes above already
            // invalidated it and the next NPV() re-runs the bootstrap. This
            // exists only for inputs that are not observable — a fixing added
            // to IndexManager, say — where nothing notified the curve.
            for (auto& entry : curves_) {
                if (auto lazy = ext::dynamic_pointer_cast<LazyObject>(entry.second.currentLink()))
                    lazy->recalculate();
            }
        }
    }


    double Session::quoteValue(const std::string& quoteId, const std::string& fieldPath) const {
        auto it = quotes_.find(quoteId);
        QLS_FIELD_REQUIRE(it != quotes_.end(), qlpb::Error::UNKNOWN_ID, fieldPath,
                          "unknown quote '" << quoteId << "' at '" << fieldPath << "'");
        return it->second->value();
    }


    void Session::writeQuote(const std::string& quoteId,
                             double value,
                             const std::string& fieldPath) {
        auto it = quotes_.find(quoteId);
        QLS_FIELD_REQUIRE(it != quotes_.end(), qlpb::Error::UNKNOWN_ID, fieldPath,
                          "unknown quote '" << quoteId << "' at '" << fieldPath << "'");
        UpdateGuard guard;
        it->second->setValue(value);
        try {
            guard.commit();
        } catch (...) {
            // Same contract as apply(): a commit that raised has invalidated
            // part of the graph and not the rest, and nothing cached can be
            // trusted. The worker drops a dirty session for replay.
            dirty_ = true;
            throw;
        }
    }


    // -----------------------------------------------------------------------
    // Pricing
    // -----------------------------------------------------------------------

    Session::PriceOutcome Session::price(const qlpb::PriceRequest& msg,
                                         const ProgressSink& progress) {
        QL_REQUIRE(!dirty_, "session is dirty and must be replayed before pricing");

        // Request options the schema offers and this build does not serve.
        // Rejected, not dropped: a client that asked for a cash-flow table
        // and got a price without one cannot tell that from an instrument
        // with no cash flows.
        // Inverting a price needs a price to invert, and the one this request
        // is about to compute is not it: that would return the volatility the
        // client sent in.
        const bool wantsImplied =
            std::find(msg.results().begin(), msg.results().end(),
                      qlpb::RESULT_KIND_IMPLIED_VOLATILITY) != msg.results().end();
        QLS_FIELD_REQUIRE(!wantsImplied || msg.implied_volatility().target_price() > 0.0,
                          qlpb::Error::INVALID_ARGUMENT, "implied_volatility.target_price",
                          "an implied volatility needs the price to invert; inverting the one "
                          "this request computes would return the volatility you sent");

        // A cash-flow table is a property of a cash-flow instrument. An option
        // has no coupons, and answering with an empty table would read as one
        // that has none rather than one that was never going to have any.
        QLS_FIELD_REQUIRE(!msg.include_cashflows() ||
                              msg.instrument().kind_case() == qlpb::Instrument::kSwap,
                          qlpb::Error::UNSUPPORTED, "include_cashflows",
                          "cash-flow tables are for cash-flow instruments; this one has none");

        switch (msg.instrument().kind_case()) {
            case qlpb::Instrument::kOption: {
                auto out = priceOption(msg, progress);
                sampleCurves(msg, out);
                return out;
            }
            case qlpb::Instrument::kSwap: {
                auto out = priceSwap(msg);
                sampleCurves(msg, out);
                return out;
            }
            case qlpb::Instrument::KIND_NOT_SET:
                QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, "instrument",
                               "no instrument set at 'instrument'");
            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, "instrument",
                       "this instrument is in the schema but not implemented");
    }


    Session::EquityGraph Session::equityGraph(const qlpb::Underlying& msg,
                                              const qlpb::Option& option,
                                              const std::string& fieldPath) const {
        EquityGraph g;
        g.spot = quoteHandle(msg.spot_quote_id(), fieldPath + ".spot_quote_id");
        g.riskFree = curveHandle(msg.discount_curve_id(), fieldPath + ".discount_curve_id");
        // An omitted dividend curve is a zero yield, not the risk-free one.
        // Defaulting to risk-free would silently make the cost of carry zero
        // — a futures-like asset — and price a plain stock option wrong by
        // several percent with nothing in the request to show for it. Zero is
        // what BlackScholesProcess means by having no dividend curve at all.
        g.dividend = msg.dividend_curve_id().empty()
                         ? Handle<YieldTermStructure>(ext::make_shared<FlatForward>(
                               evaluationDate_, 0.0, Actual365Fixed()))
                         : curveHandle(msg.dividend_curve_id(), fieldPath + ".dividend_curve_id");
        g.volatility = volatility(msg.volatility_id(), fieldPath + ".volatility_id");

        switch (msg.process()) {
            case qlpb::Underlying_Process_PROCESS_UNSPECIFIED:
            case qlpb::Underlying_Process_PROCESS_BLACK_SCHOLES_MERTON:
            case qlpb::Underlying_Process_PROCESS_GARMAN_KOHLHAGEN:
                // BlackScholesMertonProcess, so the dividend yield is a curve
                // of its own rather than folded into the risk-free rate.
                // QuantoEngine reads process->dividendYield() back out and
                // offsets it, so collapsing the two would change the price
                // rather than just the bookkeeping. Garman-Kohlhagen is the
                // same process with the foreign curve in the dividend slot.
                g.process = ext::make_shared<BlackScholesMertonProcess>(g.spot, g.dividend,
                                                                        g.riskFree, g.volatility);
                break;
            case qlpb::Underlying_Process_PROCESS_BLACK_SCHOLES:
                QLS_FIELD_REQUIRE(msg.dividend_curve_id().empty(), qlpb::Error::INVALID_ARGUMENT,
                                  fieldPath + ".dividend_curve_id",
                                  "PROCESS_BLACK_SCHOLES has no dividend yield; use "
                                  "PROCESS_BLACK_SCHOLES_MERTON to give it one");
                g.process = ext::make_shared<BlackScholesProcess>(g.spot, g.riskFree, g.volatility);
                break;
            case qlpb::Underlying_Process_PROCESS_BLACK:
                g.process = ext::make_shared<BlackProcess>(g.spot, g.riskFree, g.volatility);
                break;
            default:
                QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, fieldPath + ".process",
                               "process at '" << fieldPath
                                              << ".process' is in the schema but not implemented");
        }

        if (!option.has_quanto())
            return g;

        const std::string path = "instrument.option.quanto";
        const auto& q = option.quanto();
        g.quanto = true;
        g.fxRiskFree = curveHandle(q.fx_risk_free_curve_id(), path + ".fx_risk_free_curve_id");
        g.fxVol = volatility(q.fx_volatility_id(), path + ".fx_volatility_id");
        g.correlation = quoteHandle(q.correlation_id(), path + ".correlation_id");

        // Checked here rather than left to QuantLib. The correlation reaches
        // QuantoTermStructure as a coefficient on a cross-variance term
        // (ql/termstructures/yield/quantotermstructure.hpp), where a value
        // outside [-1, 1] is not rejected: it produces a plausible-looking
        // number for an impossible market.
        QLS_FIELD_REQUIRE(g.correlation->value() >= -1.0 && g.correlation->value() <= 1.0,
                          qlpb::Error::INVALID_ARGUMENT, path + ".correlation_id",
                          "correlation " << g.correlation->value() << " from quote '"
                                         << q.correlation_id() << "' is outside [-1, 1]");
        return g;
    }


    ext::shared_ptr<StrikedTypePayoff> Session::payoff(const qlpb::Payoff& msg,
                                                       const std::string& fieldPath) const {
        const Option::Type type = optionType(msg.type(), fieldPath + ".type");

        switch (msg.kind_case()) {
            case qlpb::Payoff::kPlain:
                QLS_FIELD_REQUIRE(msg.plain().strike() > 0.0, qlpb::Error::INVALID_ARGUMENT,
                                  fieldPath + ".plain.strike", "strike must be positive");
                return ext::make_shared<PlainVanillaPayoff>(type, msg.plain().strike());

            case qlpb::Payoff::kPercentageStrike:
                QLS_FIELD_REQUIRE(msg.percentage_strike().moneyness() > 0.0,
                                  qlpb::Error::INVALID_ARGUMENT,
                                  fieldPath + ".percentage_strike.moneyness",
                                  "moneyness must be positive");
                return ext::make_shared<PercentageStrikePayoff>(
                    type, msg.percentage_strike().moneyness());

            case qlpb::Payoff::kAssetOrNothing:
                return ext::make_shared<AssetOrNothingPayoff>(type, msg.asset_or_nothing().strike());

            case qlpb::Payoff::kCashOrNothing:
                return ext::make_shared<CashOrNothingPayoff>(type, msg.cash_or_nothing().strike(),
                                                             msg.cash_or_nothing().cash_payoff());

            case qlpb::Payoff::kGap:
                return ext::make_shared<GapPayoff>(type, msg.gap().strike(),
                                                   msg.gap().second_strike());

            case qlpb::Payoff::kSuperFund:
                return ext::make_shared<SuperFundPayoff>(msg.super_fund().strike(),
                                                         msg.super_fund().second_strike());

            case qlpb::Payoff::kSuperShare:
                return ext::make_shared<SuperSharePayoff>(msg.super_share().strike(),
                                                          msg.super_share().second_strike(),
                                                          msg.super_share().cash_payoff());
            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, fieldPath,
                       "no struck payoff set at '"
                           << fieldPath << "'; a floating payoff is only valid on a lookback");
    }


    ext::shared_ptr<Exercise> Session::exercise(const qlpb::Exercise& msg,
                                                const std::string& fieldPath) const {
        switch (msg.type()) {

            case qlpb::Exercise_Type_TYPE_EUROPEAN:
                QLS_FIELD_REQUIRE(msg.dates_size() == 1, qlpb::Error::INVALID_ARGUMENT,
                                  fieldPath + ".dates",
                                  "a European exercise needs exactly one date, got "
                                      << msg.dates_size());
                return ext::make_shared<EuropeanExercise>(
                    expiryDate(msg.dates(0), fieldPath + ".dates[0]"));

            case qlpb::Exercise_Type_TYPE_AMERICAN: {
                QLS_FIELD_REQUIRE(msg.dates_size() == 1, qlpb::Error::INVALID_ARGUMENT,
                                  fieldPath + ".dates",
                                  "an American exercise needs exactly one expiry, got "
                                      << msg.dates_size());
                const Date expiry = expiryDate(msg.dates(0), fieldPath + ".dates[0]");
                const Date earliest =
                    msg.has_earliest_date()
                        ? registry_.date(msg.earliest_date(), fieldPath + ".earliest_date")
                        : evaluationDate_;
                return ext::make_shared<AmericanExercise>(
                    earliest, expiry, flag(msg.payoff_at_expiry(), fieldPath + ".payoff_at_expiry"));
            }

            case qlpb::Exercise_Type_TYPE_BERMUDAN: {
                QLS_FIELD_REQUIRE(msg.dates_size() >= 1, qlpb::Error::INVALID_ARGUMENT,
                                  fieldPath + ".dates", "a Bermudan exercise needs dates");
                std::vector<Date> dates;
                for (int i = 0; i < msg.dates_size(); ++i)
                    dates.push_back(registry_.date(msg.dates(i),
                                                   fieldPath + ".dates[" + std::to_string(i) + "]"));
                QLS_FIELD_REQUIRE(dates.back() > evaluationDate_, qlpb::Error::INVALID_ARGUMENT,
                                  fieldPath + ".dates",
                                  "the last Bermudan date "
                                      << dates.back() << " is not after the evaluation date");
                return ext::make_shared<BermudanExercise>(
                    dates, flag(msg.payoff_at_expiry(), fieldPath + ".payoff_at_expiry"));
            }

            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath + ".type",
                       "unspecified exercise type at '" << fieldPath << ".type'");
    }


    Session::PriceOutcome Session::priceOption(const qlpb::PriceRequest& msg,
                                               const ProgressSink& progress) {
        const auto& opt = msg.instrument().option();
        const auto& eng = msg.engine();
        const std::string base = "instrument.option";

        QLS_FIELD_REQUIRE(opt.underlyings_size() == 1, qlpb::Error::INVALID_ARGUMENT,
                          base + ".underlyings",
                          "this option takes exactly one underlying, got "
                              << opt.underlyings_size());
        QLS_FIELD_REQUIRE(opt.dividends_size() == 0, qlpb::Error::UNSUPPORTED, base + ".dividends",
                          "discrete dividends are in the schema but not implemented");

        // The engine is checked before the market is resolved, so a frame that
        // is wrong in two ways is blamed on the engine rather than on
        // whichever quote happened to fail first. The cheaper, more structural
        // rejection is the more useful one.
        QLS_FIELD_REQUIRE(eng.method() != qlpb::Engine_Method_METHOD_UNSPECIFIED,
                          qlpb::Error::UNSPECIFIED_ENUM, "engine.method",
                          "unspecified engine method at 'engine.method'");

        const auto graph = equityGraph(opt.underlyings(0), opt, base + ".underlyings[0]");
        const auto ex = exercise(opt.exercise(), base + ".exercise");
        const bool european = opt.exercise().type() == qlpb::Exercise_Type_TYPE_EUROPEAN;

        switch (opt.style_case()) {

            // -- vanilla ---------------------------------------------------
            case qlpb::Option::kVanilla: {
                const auto po = payoff(opt.payoff(), base + ".payoff");

                // A binary payoff on an American exercise is a one-touch, and
                // AnalyticDigitalAmericanEngine is what prices it; that is the
                // shape digitaloption.cpp tests.
                const bool digitalPayoff =
                    opt.payoff().kind_case() == qlpb::Payoff::kCashOrNothing ||
                    opt.payoff().kind_case() == qlpb::Payoff::kAssetOrNothing;

                if (graph.quanto) {
                    QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                      "quanto options are European only: QuantoEngine wraps an "
                                      "engine built from a process alone");
                    auto option = ext::make_shared<QuantoVanillaOption>(po, ex);
                    if (eng.method() == qlpb::Engine_Method_METHOD_FINITE_DIFFERENCE)
                        return run(option,
                                   fdEngineFor<VanillaOption, FdBlackScholesVanillaEngine>(
                                       eng.fd(), graph, "engine.fd"),
                                   msg);
                    QLS_FIELD_REQUIRE(eng.method() == qlpb::Engine_Method_METHOD_ANALYTIC,
                                      qlpb::Error::UNSUPPORTED, "engine.method",
                                      "a quanto vanilla option takes METHOD_ANALYTIC or "
                                      "METHOD_FINITE_DIFFERENCE");
                    return run(option, engineFor<VanillaOption, AnalyticEuropeanEngine>(graph), msg, graph.process);
                }

                auto option = ext::make_shared<VanillaOption>(po, ex);

                switch (eng.method()) {
                    case qlpb::Engine_Method_METHOD_ANALYTIC: {
                        if (digitalPayoff && !european)
                            return run(option,
                                       ext::make_shared<AnalyticDigitalAmericanEngine>(graph.process),
                                       msg, graph.process);
                        if (european)
                            return run(option,
                                       ext::make_shared<AnalyticEuropeanEngine>(graph.process), msg, graph.process);
                        // American: three published approximations that
                        // disagree in the third decimal, so the client names
                        // one rather than inheriting a default (engine.proto).
                        switch (eng.analytic().approximation()) {
                            case qlpb::AnalyticParameters_Approximation_APPROXIMATION_BARONE_ADESI_WHALEY:
                                return run(option,
                                           ext::make_shared<BaroneAdesiWhaleyApproximationEngine>(
                                               graph.process),
                                           msg);
                            case qlpb::AnalyticParameters_Approximation_APPROXIMATION_BJERKSUND_STENSLAND:
                                return run(option,
                                           ext::make_shared<BjerksundStenslandApproximationEngine>(
                                               graph.process),
                                           msg);
                            case qlpb::AnalyticParameters_Approximation_APPROXIMATION_JU_QUADRATIC:
                                return run(option,
                                           ext::make_shared<JuQuadraticApproximationEngine>(
                                               graph.process),
                                           msg);
                            default:
                                break;
                        }
                        QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM,
                                       "engine.analytic.approximation",
                                       "an American analytic price needs an explicit "
                                       "approximation: QuantLib has three and they disagree");
                    }

                    case qlpb::Engine_Method_METHOD_INTEGRAL:
                        QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED,
                                          base + ".exercise.type",
                                          "the integral engine is European only");
                        return run(option, ext::make_shared<IntegralEngine>(graph.process), msg);

                    case qlpb::Engine_Method_METHOD_LATTICE:
                        return run(option,
                                   latticeVanillaEngine(eng.lattice(), graph, "engine.lattice"),
                                   msg);

                    case qlpb::Engine_Method_METHOD_FINITE_DIFFERENCE:
                        return run(option,
                                   fdEngineFor<VanillaOption, FdBlackScholesVanillaEngine>(
                                       eng.fd(), graph, "engine.fd"),
                                   msg);

                    case qlpb::Engine_Method_METHOD_MONTE_CARLO: {
                        QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED,
                                          base + ".exercise.type",
                                          "MCEuropeanEngine is European only");
                        const auto& mc = eng.mc();
                        if (mc.progress_every_paths() > 0)
                            return priceInBatches(msg, option, graph, progress);
                        QLS_FIELD_REQUIRE(
                            mc.seed() != 0, qlpb::Error::INVALID_ARGUMENT, "engine.mc.seed",
                            "a Monte Carlo request needs an explicit seed: QuantLib seeds from "
                            "the clock by default "
                            "(ql/math/randomnumbers/seedgenerator.cpp:36) and the same inputs "
                            "would price differently on every request");
                        QLS_FIELD_REQUIRE(mc.samples() > 0, qlpb::Error::INVALID_ARGUMENT,
                                          "engine.mc.samples",
                                          "a Monte Carlo request needs samples");
                        return run(option,
                                   MakeMCEuropeanEngine<PseudoRandom>(graph.process)
                                       .withSteps(mc.time_steps_per_year() > 0
                                                      ? mc.time_steps_per_year()
                                                      : 1)
                                       .withSamples(mc.samples())
                                       .withSeed(mc.seed()),
                                   msg);
                    }

                    default:
                        break;
                }
                QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, "engine.method",
                               "this engine method is not wired up for vanilla options");
            }

            // -- barrier ---------------------------------------------------
            case qlpb::Option::kBarrier: {
                const std::string path = base + ".barrier";
                const auto& b = opt.barrier();
                const auto po = payoff(opt.payoff(), base + ".payoff");

                QLS_FIELD_REQUIRE(b.level() > 0.0, qlpb::Error::INVALID_ARGUMENT, path + ".level",
                                  "barrier must be positive");
                QLS_FIELD_REQUIRE(b.monitoring_dates_size() == 0, qlpb::Error::UNSUPPORTED,
                                  path + ".monitoring_dates",
                                  "discretely monitored barriers are in the schema but not "
                                  "implemented; the engines here assume continuous monitoring");
                QLS_FIELD_REQUIRE(!b.has_window_start() && !b.has_window_end(),
                                  qlpb::Error::UNSUPPORTED, path + ".window_start",
                                  "partial-time barriers are in the schema but not implemented");

                const auto type = barrierType(b.type(), path + ".type");

                if (graph.quanto) {
                    QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                      "quanto barrier options are European only");
                    auto option =
                        ext::make_shared<QuantoBarrierOption>(type, b.level(), b.rebate(), po, ex);
                    if (eng.method() == qlpb::Engine_Method_METHOD_FINITE_DIFFERENCE)
                        return run(option,
                                   fdEngineFor<BarrierOption, FdBlackScholesBarrierEngine>(
                                       eng.fd(), graph, "engine.fd"),
                                   msg);
                    QLS_FIELD_REQUIRE(eng.method() == qlpb::Engine_Method_METHOD_ANALYTIC,
                                      qlpb::Error::UNSUPPORTED, "engine.method",
                                      "a quanto barrier option takes METHOD_ANALYTIC or "
                                      "METHOD_FINITE_DIFFERENCE");
                    return run(option, engineFor<BarrierOption, AnalyticBarrierEngine>(graph), msg, graph.process);
                }

                auto option = ext::make_shared<BarrierOption>(type, b.level(), b.rebate(), po, ex);

                switch (eng.method()) {
                    case qlpb::Engine_Method_METHOD_ANALYTIC:
                        QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED,
                                          base + ".exercise.type",
                                          "AnalyticBarrierEngine is European only; an American "
                                          "barrier takes METHOD_LATTICE or "
                                          "METHOD_FINITE_DIFFERENCE");
                        return run(option, ext::make_shared<AnalyticBarrierEngine>(graph.process),
                                   msg, graph.process);

                    case qlpb::Engine_Method_METHOD_FINITE_DIFFERENCE:
                        return run(option,
                                   fdEngineFor<BarrierOption, FdBlackScholesBarrierEngine>(
                                       eng.fd(), graph, "engine.fd"),
                                   msg);

                    case qlpb::Engine_Method_METHOD_LATTICE: {
                        QLS_FIELD_REQUIRE(eng.lattice().steps() > 0, qlpb::Error::INVALID_ARGUMENT,
                                          "engine.lattice.steps",
                                          "a lattice request needs a step count");
                        // One tree only: BinomialBarrierEngine takes a second
                        // template argument for the discretisation, so the
                        // table would be trees x discretisations. CRR with the
                        // Derman-Kani correction is what barrieroption.cpp
                        // benchmarks against.
                        QLS_FIELD_REQUIRE(
                            eng.lattice().tree() ==
                                qlpb::LatticeParameters_Tree_TREE_COX_ROSS_RUBINSTEIN,
                            qlpb::Error::UNSUPPORTED, "engine.lattice.tree",
                            "the barrier lattice is Cox-Ross-Rubinstein only");
                        return run(option,
                                   ext::make_shared<BinomialBarrierEngine<
                                       CoxRossRubinstein, DiscretizedDermanKaniBarrierOption>>(
                                       graph.process, eng.lattice().steps()),
                                   msg);
                    }

                    case qlpb::Engine_Method_METHOD_MONTE_CARLO: {
                        const auto& mc = eng.mc();
                        QLS_FIELD_REQUIRE(mc.seed() != 0, qlpb::Error::INVALID_ARGUMENT,
                                          "engine.mc.seed",
                                          "a Monte Carlo request needs an explicit seed");
                        QLS_FIELD_REQUIRE(mc.samples() > 0, qlpb::Error::INVALID_ARGUMENT,
                                          "engine.mc.samples",
                                          "a Monte Carlo request needs samples");
                        return run(option,
                                   MakeMCBarrierEngine<PseudoRandom>(graph.process)
                                       .withStepsPerYear(mc.time_steps_per_year() > 0
                                                             ? mc.time_steps_per_year()
                                                             : 100)
                                       .withSamples(mc.samples())
                                       .withSeed(mc.seed()),
                                   msg);
                    }

                    default:
                        break;
                }
                QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, "engine.method",
                               "this engine method is not wired up for barrier options");
            }

            // -- double barrier --------------------------------------------
            case qlpb::Option::kDoubleBarrier: {
                const std::string path = base + ".double_barrier";
                const auto& b = opt.double_barrier();
                const auto po = payoff(opt.payoff(), base + ".payoff");

                QLS_FIELD_REQUIRE(b.lower() > 0.0 && b.upper() > b.lower(),
                                  qlpb::Error::INVALID_ARGUMENT, path + ".lower",
                                  "need 0 < lower < upper, got " << b.lower() << " and "
                                                                 << b.upper());
                QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                  "AnalyticDoubleBarrierEngine is European only");
                QLS_FIELD_REQUIRE(eng.method() == qlpb::Engine_Method_METHOD_ANALYTIC,
                                  qlpb::Error::UNSUPPORTED, "engine.method",
                                  "a double-barrier option takes METHOD_ANALYTIC: the only FD "
                                  "double-barrier engine QuantLib has is Heston, which takes a "
                                  "calibrated model rather than a process");

                const auto type = doubleBarrierType(b.type(), path + ".type");

                if (graph.quanto)
                    return run(ext::make_shared<QuantoDoubleBarrierOption>(
                                   type, b.lower(), b.upper(), b.rebate(), po, ex),
                               engineFor<DoubleBarrierOption, AnalyticDoubleBarrierEngine>(graph),
                               msg);

                return run(ext::make_shared<DoubleBarrierOption>(type, b.lower(), b.upper(),
                                                                 b.rebate(), po, ex),
                           ext::make_shared<AnalyticDoubleBarrierEngine>(graph.process), msg, graph.process);
            }

            // -- forward start ---------------------------------------------
            case qlpb::Option::kForwardStart: {
                const std::string path = base + ".forward_start";
                const auto& fs = opt.forward_start();

                QLS_FIELD_REQUIRE(
                    opt.payoff().kind_case() == qlpb::Payoff::kPercentageStrike,
                    qlpb::Error::INVALID_ARGUMENT, base + ".payoff.percentage_strike",
                    "a forward-start option is struck as a fraction of the spot at reset, so it "
                    "takes a percentage_strike payoff");
                QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                  "the forward-start engines are European only");
                QLS_FIELD_REQUIRE(eng.method() == qlpb::Engine_Method_METHOD_ANALYTIC,
                                  qlpb::Error::UNSUPPORTED, "engine.method",
                                  "forward-start options take METHOD_ANALYTIC: there is no FD "
                                  "forward-start engine");

                const Real moneyness = opt.payoff().percentage_strike().moneyness();
                QLS_FIELD_REQUIRE(moneyness > 0.0, qlpb::Error::INVALID_ARGUMENT,
                                  base + ".payoff.percentage_strike.moneyness",
                                  "moneyness must be positive");

                const Date reset = registry_.date(fs.reset(), path + ".reset");
                QLS_FIELD_REQUIRE(reset >= evaluationDate_, qlpb::Error::INVALID_ARGUMENT,
                                  path + ".reset",
                                  "reset " << reset << " is before the evaluation date "
                                           << evaluationDate_);
                QLS_FIELD_REQUIRE(reset <= ex->lastDate(), qlpb::Error::INVALID_ARGUMENT,
                                  path + ".reset",
                                  "reset " << reset << " is after the expiry " << ex->lastDate());

                // Zero strike, deliberately: the strike is `moneyness` times
                // the spot at reset and is filled in by the forward engine
                // (ql/pricingengines/forward/forwardengine.hpp). A struck
                // payoff here would be silently overwritten.
                const auto po = ext::make_shared<PlainVanillaPayoff>(
                    optionType(opt.payoff().type(), base + ".payoff.type"), 0.0);

                // The performance variant pays the return rather than the
                // amount, which is a different price for the same trade
                // description — hence a Flag, not a bool (DESIGN §6.3).
                const bool performance = flag(fs.performance(), path + ".performance");

                if (graph.quanto) {
                    auto option =
                        ext::make_shared<QuantoForwardVanillaOption>(moneyness, reset, po, ex);
                    if (performance)
                        return run(option,
                                   engineFor<ForwardVanillaOption,
                                             ForwardPerformanceVanillaEngine<
                                                 AnalyticEuropeanEngine>>(graph),
                                   msg);
                    return run(option,
                               engineFor<ForwardVanillaOption,
                                         ForwardVanillaEngine<AnalyticEuropeanEngine>>(graph),
                               msg);
                }

                auto option = ext::make_shared<ForwardVanillaOption>(moneyness, reset, po, ex);
                if (performance)
                    return run(
                        option,
                        ext::make_shared<ForwardPerformanceVanillaEngine<AnalyticEuropeanEngine>>(
                            graph.process),
                        msg);
                return run(option,
                           ext::make_shared<ForwardVanillaEngine<AnalyticEuropeanEngine>>(
                               graph.process),
                           msg);
            }

            // -- asian -----------------------------------------------------
            case qlpb::Option::kAsian: {
                const std::string path = base + ".asian";
                const auto& a = opt.asian();
                const auto po = payoff(opt.payoff(), base + ".payoff");
                const auto avg = averageType(a.averaging(), path + ".averaging");

                QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                  "the Asian engines here are European only");
                QLS_FIELD_REQUIRE(!graph.quanto, qlpb::Error::UNSUPPORTED, base + ".quanto",
                                  "there is no quanto Asian engine in QuantLib");

                if (a.fixing_dates_size() == 0) {
                    QLS_FIELD_REQUIRE(avg == Average::Geometric, qlpb::Error::UNSUPPORTED,
                                      path + ".averaging",
                                      "a continuously averaged Asian option has a closed form "
                                      "for the geometric average only");
                    QLS_FIELD_REQUIRE(eng.method() == qlpb::Engine_Method_METHOD_ANALYTIC,
                                      qlpb::Error::UNSUPPORTED, "engine.method",
                                      "a continuous Asian option takes METHOD_ANALYTIC");
                    return run(ext::make_shared<ContinuousAveragingAsianOption>(avg, po, ex),
                               ext::make_shared<AnalyticContinuousGeometricAveragePriceAsianEngine>(
                                   graph.process),
                               msg);
                }

                std::vector<Date> fixings;
                for (int i = 0; i < a.fixing_dates_size(); ++i)
                    fixings.push_back(registry_.date(
                        a.fixing_dates(i), path + ".fixing_dates[" + std::to_string(i) + "]"));

                auto option = ext::make_shared<DiscreteAveragingAsianOption>(
                    avg, a.running_average(), a.past_fixings(), fixings, po, ex);

                switch (eng.method()) {
                    case qlpb::Engine_Method_METHOD_ANALYTIC:
                        QLS_FIELD_REQUIRE(avg == Average::Geometric, qlpb::Error::UNSUPPORTED,
                                          path + ".averaging",
                                          "the discrete Asian closed form is geometric only; an "
                                          "arithmetic average takes METHOD_MONTE_CARLO");
                        return run(option,
                                   ext::make_shared<
                                       AnalyticDiscreteGeometricAveragePriceAsianEngine>(
                                       graph.process),
                                   msg);

                    case qlpb::Engine_Method_METHOD_MONTE_CARLO: {
                        QLS_FIELD_REQUIRE(avg == Average::Arithmetic, qlpb::Error::UNSUPPORTED,
                                          path + ".averaging",
                                          "this Monte Carlo engine averages arithmetically");
                        const auto& mc = eng.mc();
                        QLS_FIELD_REQUIRE(mc.seed() != 0, qlpb::Error::INVALID_ARGUMENT,
                                          "engine.mc.seed",
                                          "a Monte Carlo request needs an explicit seed");
                        QLS_FIELD_REQUIRE(mc.samples() > 0, qlpb::Error::INVALID_ARGUMENT,
                                          "engine.mc.samples",
                                          "a Monte Carlo request needs samples");
                        return run(option,
                                   MakeMCDiscreteArithmeticAPEngine<PseudoRandom>(graph.process)
                                       .withSamples(mc.samples())
                                       .withSeed(mc.seed())
                                       .withControlVariate(mc.control_variate()),
                                   msg);
                    }

                    default:
                        break;
                }
                QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, "engine.method",
                               "this engine method is not wired up for Asian options");
            }

            // -- lookback --------------------------------------------------
            case qlpb::Option::kLookback: {
                const std::string path = base + ".lookback";
                const auto& lb = opt.lookback();

                QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                  "the continuous lookback engines are European only");
                // Without this the request is priced as a plain lookback. The
                // engines below take graph.process directly and nothing on
                // this path consults graph.quanto, so the adjustment would be
                // dropped and a number returned for a different trade -- the
                // one failure mode DESIGN §6 exists to prevent. QuantLib has
                // no quanto lookback instrument to carry the results and the
                // test suite has no reference value for one, so this is
                // refused by name rather than answered on a substitute.
                // HANDLERS.md already says quanto is unavailable here; this is
                // the code agreeing with it.
                QLS_FIELD_REQUIRE(!graph.quanto, qlpb::Error::UNSUPPORTED, base + ".quanto",
                                  "there is no quanto lookback engine in QuantLib");
                QLS_FIELD_REQUIRE(eng.method() == qlpb::Engine_Method_METHOD_ANALYTIC,
                                  qlpb::Error::UNSUPPORTED, "engine.method",
                                  "lookback options take METHOD_ANALYTIC");
                QLS_FIELD_REQUIRE(!lb.has_window_start(), qlpb::Error::UNSUPPORTED,
                                  path + ".window_start",
                                  "partial-time lookbacks are in the schema but not implemented");
                QLS_FIELD_REQUIRE(lb.running_extremum() > 0.0, qlpb::Error::INVALID_ARGUMENT,
                                  path + ".running_extremum",
                                  "a lookback needs the extremum realised so far; an option "
                                  "already running whose extremum is dropped prices as if it had "
                                  "just started");

                // The floating-strike form is the one with no strike, so the
                // payoff arm chooses the instrument rather than a flag doing
                // it (ql/instruments/lookbackoption.hpp:37,54).
                if (opt.payoff().kind_case() == qlpb::Payoff::kFloating)
                    return run(ext::make_shared<ContinuousFloatingLookbackOption>(
                                   lb.running_extremum(),
                                   ext::make_shared<FloatingTypePayoff>(
                                       optionType(opt.payoff().type(), base + ".payoff.type")),
                                   ex),
                               ext::make_shared<AnalyticContinuousFloatingLookbackEngine>(
                                   graph.process),
                               msg);

                return run(ext::make_shared<ContinuousFixedLookbackOption>(
                               lb.running_extremum(), payoff(opt.payoff(), base + ".payoff"), ex),
                           ext::make_shared<AnalyticContinuousFixedLookbackEngine>(graph.process),
                           msg);
            }

            case qlpb::Option::STYLE_NOT_SET:
                QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, base + ".style",
                               "no option style set at '" << base << "'");

            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, base,
                       "this option style is in the schema but not implemented");
    }


    void Session::fillCashflows(const std::vector<Leg>& legs,
                                const Handle<YieldTermStructure>& discount,
                                PriceOutcome& out) const {
        for (std::size_t i = 0; i < legs.size(); ++i) {
            for (const auto& flow : legs[i]) {
                if (flow->hasOccurred(evaluationDate_))
                    continue;

                qlpb::CashFlow row;
                row.set_leg(static_cast<std::uint32_t>(i));
                writeDate(*row.mutable_payment_date(), flow->date());
                row.set_amount(flow->amount());

                // The discount the engine used, not one recomputed from a
                // different curve: the sum of the present-value column has to
                // come to the NPV or the table is decoration.
                const DiscountFactor df = discount->discount(flow->date());
                row.set_discount(df);
                row.set_present_value(flow->amount() * df);

                if (const auto coupon = ext::dynamic_pointer_cast<Coupon>(flow)) {
                    writeDate(*row.mutable_accrual_start(), coupon->accrualStartDate());
                    writeDate(*row.mutable_accrual_end(), coupon->accrualEndDate());
                    row.set_accrual_period(coupon->accrualPeriod());
                    row.set_notional(coupon->nominal());
                    row.set_rate(coupon->rate());
                }

                if (const auto floating = ext::dynamic_pointer_cast<FloatingRateCoupon>(flow)) {
                    const Date fixing = floating->fixingDate();
                    writeDate(*row.mutable_fixing_date(), fixing);
                    row.set_spread(floating->spread());
                    row.set_gearing(floating->gearing());
                    // A fixing on or before the evaluation date came from
                    // IndexManager; a later one is a forecast, and a client
                    // reading a column of rates should be able to tell which
                    // it is looking at.
                    row.set_is_past_fixing(fixing <= evaluationDate_);
                    try {
                        row.set_index_fixing(floating->indexFixing());
                    } catch (const Error&) {
                        // A past fixing that was never supplied. The row is
                        // still worth showing; the rate is what is missing.
                    }
                }

                out.cashflows.push_back(std::move(row));
            }
        }
    }


    void Session::sampleCurves(const qlpb::PriceRequest& msg, PriceOutcome& out) const {
        for (int i = 0; i < msg.curve_samples_size(); ++i) {
            const auto& sample = msg.curve_samples(i);
            const std::string path = "curve_samples[" + std::to_string(i) + "]";

            const bool byDate = sample.dates_size() > 0;
            const bool byTime = sample.times_size() > 0;
            QLS_FIELD_REQUIRE(byDate != byTime, qlpb::Error::INVALID_ARGUMENT, path,
                              "a curve sample takes dates or times, one of the two");

            // Rate quantities need to say what a rate means; a discount factor
            // does not, and a compounding set beside one would be a field
            // taken and ignored.
            const auto quantity = sample.quantity();
            const bool isRate = quantity == qlpb::CurveSample::QUANTITY_ZERO_RATE ||
                                quantity == qlpb::CurveSample::QUANTITY_FORWARD_RATE;
            const bool isVol = quantity == qlpb::CurveSample::QUANTITY_BLACK_VOLATILITY;

            qlpb::Series series;
            series.set_x_axis(byDate ? qlpb::Series::AXIS_DATE : qlpb::Series::AXIS_TIME_YEARS);

            if (isRate || quantity == qlpb::CurveSample::QUANTITY_DISCOUNT_FACTOR) {
                const auto curve = curveHandle(sample.market_id(), path + ".market_id");
                Compounding compounding = Continuous;
                Frequency frequency = Annual;
                DayCounter dc;
                if (isRate) {
                    compounding = registry_.compounding(sample.compounding(), path + ".compounding");
                    frequency = registry_.frequency(sample.frequency(), path + ".frequency");
                    if (byDate)
                        dc = registry_.dayCounter(sample.day_counter(), path + ".day_counter");
                } else {
                    QLS_FIELD_REQUIRE(
                        sample.compounding() == quantlib::v1::COMPOUNDING_UNSPECIFIED,
                        qlpb::Error::INVALID_ARGUMENT, path + ".compounding",
                        "a discount factor has no compounding; it is the number a rate "
                        "compounds to");
                }

                series.set_y_axis(isRate ? qlpb::Series::AXIS_RATE
                                         : qlpb::Series::AXIS_DISCOUNT_FACTOR);
                series.set_name(sample.market_id() + "." + quantityName(quantity));

                for (int n = 0; n < (byDate ? sample.dates_size() : sample.times_size()); ++n) {
                    Real value = 0.0;
                    if (byDate) {
                        const Date d = registry_.date(sample.dates(n),
                                                      path + ".dates[" + std::to_string(n) + "]");
                        value = quantity == qlpb::CurveSample::QUANTITY_ZERO_RATE
                                    ? curve->zeroRate(d, dc, compounding, frequency).rate()
                                : quantity == qlpb::CurveSample::QUANTITY_FORWARD_RATE
                                    ? curve->forwardRate(d, d, dc, compounding, frequency).rate()
                                    : curve->discount(d);
                        *series.add_x_dates() = sample.dates(n);
                    } else {
                        const Time t = sample.times(n);
                        value = quantity == qlpb::CurveSample::QUANTITY_ZERO_RATE
                                    ? curve->zeroRate(t, compounding, frequency).rate()
                                : quantity == qlpb::CurveSample::QUANTITY_FORWARD_RATE
                                    ? curve->forwardRate(t, t, compounding, frequency).rate()
                                    : curve->discount(t);
                        series.add_x(t);
                    }
                    series.add_y(value);
                }
            } else if (isVol) {
                // One strike gives a line. Several would give a surface, and a
                // surface is a matrix rather than a series; that is a shape
                // this reply cannot carry, so it is refused rather than
                // flattened into the first strike.
                QLS_FIELD_REQUIRE(sample.strikes_size() == 1, qlpb::Error::UNSUPPORTED,
                                  path + ".strikes",
                                  "sampling a surface takes exactly one strike: several would be "
                                  "a matrix, which this reply does not carry");
                const auto surface = volatility(sample.market_id(), path + ".market_id");
                const Real strike = sample.strikes(0);

                series.set_y_axis(qlpb::Series::AXIS_VOLATILITY);
                series.set_name(sample.market_id() + "." + quantityName(quantity));

                for (int n = 0; n < (byDate ? sample.dates_size() : sample.times_size()); ++n) {
                    if (byDate) {
                        const Date d = registry_.date(sample.dates(n),
                                                      path + ".dates[" + std::to_string(n) + "]");
                        series.add_y(surface->blackVol(d, strike));
                        *series.add_x_dates() = sample.dates(n);
                    } else {
                        series.add_y(surface->blackVol(sample.times(n), strike));
                        series.add_x(sample.times(n));
                    }
                }
            } else {
                // Survival, hazard and local volatility all need a term
                // structure this build does not construct.
                QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, path + ".quantity",
                               "curve quantity at '" << path
                                                     << ".quantity' is unspecified or not "
                                                        "implemented");
            }

            out.series.push_back(std::move(series));
        }
    }


    Session::PriceOutcome Session::priceInBatches(const qlpb::PriceRequest& msg,
                                                  const ext::shared_ptr<VanillaOption>& option,
                                                  const EquityGraph& graph,
                                                  const ProgressSink& progress) {
        const auto& mc = msg.engine().mc();
        QLS_FIELD_REQUIRE(mc.seed() != 0, qlpb::Error::INVALID_ARGUMENT, "engine.mc.seed",
                          "a Monte Carlo request needs an explicit seed");
        QLS_FIELD_REQUIRE(mc.samples() > 0, qlpb::Error::INVALID_ARGUMENT, "engine.mc.samples",
                          "a Monte Carlo request needs samples");

        const Size batch = mc.progress_every_paths();
        const Size batches = (mc.samples() + batch - 1) / batch;

        // The batched result is NOT the single-shot result.
        //
        // QuantLib cannot resume an engine, so progress is produced by running
        // `batches` independent simulations with derived seeds and averaging
        // their means. That is a valid estimator of the same quantity, but it
        // partitions the RNG stream differently and therefore returns a
        // different number than one run of `samples` paths at the same seed.
        //
        // Reproducibility therefore keys on (seed, samples, progress_every_paths),
        // all three of which PriceResult echoes. Two "identical" requests that
        // differ only in whether the user watched a progress bar do not agree.
        const auto start = std::chrono::steady_clock::now();

        Real sum = 0.0;
        Real sumOfSquaredErrors = 0.0;

        for (Size i = 0; i < batches; ++i) {
            // Explicit Size: samples() is uint64 and Size is size_t, which are
            // distinct types where both are 64 bits, so std::min cannot deduce.
            const Size thisBatch = std::min(batch, static_cast<Size>(mc.samples()) - i * batch);

            // Derived, not random: replaying the same request reproduces the
            // same batch seeds.
            const BigNatural batchSeed = static_cast<BigNatural>(mc.seed() + i);

            option->setPricingEngine(
                MakeMCEuropeanEngine<PseudoRandom>(graph.process)
                    .withSteps(mc.time_steps_per_year() > 0 ? mc.time_steps_per_year() : 1)
                    .withSamples(thisBatch)
                    .withSeed(batchSeed));

            const Real batchNpv = option->NPV();
            sum += batchNpv * static_cast<Real>(thisBatch);

            try {
                const Real err = option->errorEstimate();
                sumOfSquaredErrors += err * err;
            } catch (const Error&) {
                // engine did not provide one; the combined estimate is dropped
            }

            const Size done = i * batch + thisBatch;
            if (progress && !progress(done, mc.samples(), sum / static_cast<Real>(done))) {
                // Cooperative stop between batches. Inside a batch nothing can
                // interrupt the engine, which is why a hard cancel still has
                // to kill the process (DESIGN §3).
                QL_FAIL("cancelled after " << done << " of " << mc.samples() << " paths");
            }
        }

        PriceOutcome out;
        out.npv = sum / static_cast<Real>(mc.samples());
        if (sumOfSquaredErrors > 0.0) {
            // Independent batches, so the errors add in quadrature.
            out.results["errorEstimate"] = std::sqrt(sumOfSquaredErrors) / static_cast<Real>(batches);
        }
        out.calculationSeconds = seconds(start);
        return out;
    }


    // -----------------------------------------------------------------------
    // Swaps
    // -----------------------------------------------------------------------

    Schedule Session::schedule(const qlpb::Schedule& msg, const std::string& fieldPath) {
        const Date start = registry_.date(msg.start(), fieldPath + ".start");
        const Date maturity = registry_.date(msg.maturity(), fieldPath + ".maturity");
        QLS_FIELD_REQUIRE(maturity > start, qlpb::Error::INVALID_ARGUMENT, fieldPath + ".maturity",
                          "maturity " << maturity << " is not after start " << start);

        const auto calendar = registry_.calendar(msg.calendar(), fieldPath + ".calendar");
        const auto convention =
            registry_.businessDayConvention(msg.convention(), fieldPath + ".convention");
        const auto termination =
            msg.termination_convention() == quantlib::v1::BUSINESS_DAY_CONVENTION_UNSPECIFIED
                ? convention
                : registry_.businessDayConvention(msg.termination_convention(),
                                                  fieldPath + ".termination_convention");

        // end_of_month is business-day aware in QuantLib, not raw month end
        // (AGENTS.md §5.4); it is passed through rather than interpreted here.
        return Schedule(start, maturity,
                        Period(registry_.frequency(msg.frequency(), fieldPath + ".frequency")),
                        calendar, convention, termination,
                        dateGeneration(msg.date_generation(), fieldPath + ".date_generation"),
                        flag(msg.end_of_month(), fieldPath + ".end_of_month"));
    }


    Leg Session::buildLeg(const qlpb::Leg& msg, const std::string& fieldPath) {
        const auto sched = schedule(msg.schedule(), fieldPath + ".schedule");
        const auto dc = registry_.dayCounter(msg.day_counter(), fieldPath + ".day_counter");

        QLS_FIELD_REQUIRE(msg.notionals_size() > 0, qlpb::Error::INVALID_ARGUMENT,
                          fieldPath + ".notionals", "a leg needs at least one notional");
        const std::vector<Real> notionals(msg.notionals().begin(), msg.notionals().end());

        switch (msg.kind()) {
            case qlpb::Leg_Kind_KIND_FIXED: {
                // FixedRateLeg takes a rate, not a handle: its coupons are
                // built once and observe nothing. A fixed rate is a term of
                // the trade rather than market data, so freezing it is
                // correct — but it does mean a client bumping this quote has
                // to send a new PriceRequest, not an UpdateMarket.
                const Handle<Quote> rate =
                    quoteHandle(msg.rate_quote_id(), fieldPath + ".rate_quote_id");
                return FixedRateLeg(sched).withNotionals(notionals).withCouponRates(rate->value(),
                                                                                    dc);
            }

            case qlpb::Leg_Kind_KIND_IBOR: {
                const auto index = indexById(msg.index_id(), fieldPath + ".index_id");
                QLS_FIELD_REQUIRE(!index->forwardingTermStructure().empty(),
                                  qlpb::Error::INVALID_ARGUMENT, fieldPath + ".index_id",
                                  "the floating leg index needs a forwarding curve: pricing off "
                                  "an index with an empty handle fails at the first forecast");
                // A capped or floored coupon needs an optionlet volatility
                // and a coupon pricer, neither of which the market carries
                // yet; a per-leg discount curve or currency needs the
                // cross-currency engine. All four rejected rather than dropped.
                QLS_FIELD_REQUIRE(msg.caps_size() == 0 && msg.floors_size() == 0,
                                  qlpb::Error::UNSUPPORTED, fieldPath + ".caps",
                                  "capped and floored coupons are in the schema but not "
                                  "implemented: they need an optionlet volatility surface");
                QLS_FIELD_REQUIRE(msg.discount_curve_id().empty() && msg.currency().empty(),
                                  qlpb::Error::UNSUPPORTED, fieldPath + ".discount_curve_id",
                                  "per-leg discounting and currencies are in the schema but not "
                                  "implemented");
                auto leg = IborLeg(sched, index)
                               .withNotionals(notionals)
                               .withPaymentDayCounter(dc)
                               .withFixingDays(msg.fixing_days())
                               .inArrears(flag(msg.in_arrears(), fieldPath + ".in_arrears"));
                if (msg.spreads_size() > 0)
                    leg = leg.withSpreads(
                        std::vector<Real>(msg.spreads().begin(), msg.spreads().end()));
                if (msg.gearings_size() > 0)
                    leg = leg.withGearings(
                        std::vector<Real>(msg.gearings().begin(), msg.gearings().end()));
                return leg;
            }

            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, fieldPath + ".kind",
                       "leg kind at '" << fieldPath << ".kind' is unspecified or not implemented");
    }


    Session::PriceOutcome Session::priceSwap(const qlpb::PriceRequest& msg) {
        const auto& swapMsg = msg.instrument().swap();
        const std::string base = "instrument.swap";

        // Checked, unlike v1: Session::priceSwap there always used
        // DiscountingSwapEngine and never read the field, so a swap priced
        // with an unset engine succeeded silently. That was the one place the
        // "zero is rejected" rule of DESIGN §6 was not enforced.
        QLS_FIELD_REQUIRE(msg.engine().method() == qlpb::Engine_Method_METHOD_DISCOUNTING,
                          msg.engine().method() == qlpb::Engine_Method_METHOD_UNSPECIFIED
                              ? qlpb::Error::UNSPECIFIED_ENUM
                              : qlpb::Error::UNSUPPORTED,
                          "engine.method", "a swap takes METHOD_DISCOUNTING");

        QLS_FIELD_REQUIRE(swapMsg.legs_size() >= 2, qlpb::Error::INVALID_ARGUMENT, base + ".legs",
                          "a swap needs at least two legs, got " << swapMsg.legs_size());

        std::vector<Leg> legs;
        std::vector<bool> payers;
        for (int i = 0; i < swapMsg.legs_size(); ++i) {
            const std::string path = base + ".legs[" + std::to_string(i) + "]";
            legs.push_back(buildLeg(swapMsg.legs(i), path));
            payers.push_back(flag(swapMsg.legs(i).pays(), path + ".pays"));
        }

        const bool allSame = std::all_of(payers.begin(), payers.end(),
                                         [&](bool p) { return p == payers.front(); });
        QLS_FIELD_REQUIRE(!allSame, qlpb::Error::INVALID_ARGUMENT, base + ".legs",
                          "every leg of this swap has the same direction; that is a portfolio, "
                          "not a swap");

        // Checked here and not inside the results loop below, whose catch
        // swallows QuantLib::Error -- and FieldError is one.
        const bool wantsFairRate = std::find(msg.results().begin(), msg.results().end(),
                                             qlpb::RESULT_KIND_FAIR_RATE) != msg.results().end();
        QLS_FIELD_REQUIRE(!wantsFairRate ||
                              (legs.size() == 2 &&
                               swapMsg.legs(0).kind() == qlpb::Leg_Kind_KIND_FIXED &&
                               swapMsg.legs(1).kind() == qlpb::Leg_Kind_KIND_IBOR),
                          qlpb::Error::UNSUPPORTED, base + ".legs",
                          "a fair rate is computed for a two-leg swap with the fixed leg first "
                          "and the floating leg second; the formula assumes that order and would "
                          "return a wrong number silently for any other");

        auto swap = ext::make_shared<Swap>(legs, payers);
        swap->setPricingEngine(
            ext::make_shared<DiscountingSwapEngine>(
                curveHandle(swapMsg.discount_curve_id(), base + ".discount_curve_id")));

        const auto discount = curveHandle(swapMsg.discount_curve_id(), base + ".discount_curve_id");
        const auto t0 = std::chrono::steady_clock::now();

        PriceOutcome out;
        out.npv = swap->NPV();

        if (msg.include_cashflows())
            fillCashflows(legs, discount, out);

        for (const auto kind : msg.results()) {
            const auto produced = out.results.size();
            try {
                switch (kind) {
                    case qlpb::RESULT_KIND_LEG_NPV:
                        for (Size i = 0; i < legs.size(); ++i)
                            out.results["legNPV." + std::to_string(i)] = swap->legNPV(i);
                        break;
                    case qlpb::RESULT_KIND_LEG_BPS:
                        for (Size i = 0; i < legs.size(); ++i)
                            out.results["legBPS." + std::to_string(i)] = swap->legBPS(i);
                        break;
                    case qlpb::RESULT_KIND_FAIR_RATE: {
                        // The par rate of a two-leg swap: minus the floating
                        // NPV over the fixed leg's annuity. Computed here
                        // rather than taken from VanillaSwap, because this
                        // path builds the general n-leg Swap and VanillaSwap's
                        // own accessor is not available on it. The leg order
                        // it assumes was checked above, outside this try:
                        // a FieldError thrown in here would be swallowed as
                        // "not provided by this engine".
                        const Real annuity = swap->legBPS(0) / 1.0e-4;
                        QL_REQUIRE(std::fabs(annuity) > 0.0, "the fixed leg has no annuity");
                        out.results["fairRate"] = -swap->legNPV(1) / annuity;
                        break;
                    }
                    default:
                        // An option greek asked of a swap. Absent, and named
                        // as absent rather than left to be guessed at.
                        break;
                }
            } catch (const Error&) {
                // Not published by this engine; named just below.
            }

            if (out.results.size() == produced && kind != qlpb::RESULT_KIND_NPV)
                out.unavailable.push_back(static_cast<qlpb::ResultKind>(kind));
        }

        out.calculationSeconds = seconds(t0);
        return out;
    }

}
