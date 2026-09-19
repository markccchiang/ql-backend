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
#include <ql/instruments/basketoption.hpp>
#include <ql/instruments/cliquetoption.hpp>
#include <ql/instruments/complexchooseroption.hpp>
#include <ql/instruments/compoundoption.hpp>
#include <ql/instruments/doublebarrieroption.hpp>
#include <ql/instruments/forwardvanillaoption.hpp>
#include <ql/instruments/lookbackoption.hpp>
#include <ql/instruments/quantobarrieroption.hpp>
#include <ql/instruments/quantoforwardvanillaoption.hpp>
#include <ql/instruments/quantovanillaoption.hpp>
#include <ql/instruments/simplechooseroption.hpp>
#include <ql/instruments/swap.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/matrixutilities/symmetricschurdecomposition.hpp>
#include <ql/math/interpolations/linearinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/methods/finitedifferences/solvers/fdmbackwardsolver.hpp>
#include <ql/methods/lattices/binomialtree.hpp>
#include <ql/patterns/lazyobject.hpp>
#include <ql/pricingengines/asian/analytic_cont_geom_av_price.hpp>
#include <ql/pricingengines/asian/analytic_discr_geom_av_price.hpp>
#include <ql/pricingengines/asian/mc_discr_arith_av_price.hpp>
#include <ql/pricingengines/barrier/analyticbarrierengine.hpp>
#include <ql/pricingengines/barrier/analyticbinarybarrierengine.hpp>
#include <ql/pricingengines/barrier/analyticdoublebarrierengine.hpp>
#include <ql/pricingengines/barrier/binomialbarrierengine.hpp>
#include <ql/pricingengines/barrier/fdblackscholesbarrierengine.hpp>
#include <ql/pricingengines/barrier/mcbarrierengine.hpp>
#include <ql/pricingengines/basket/kirkengine.hpp>
#include <ql/pricingengines/basket/mceuropeanbasketengine.hpp>
#include <ql/pricingengines/basket/stulzengine.hpp>
#include <ql/pricingengines/cliquet/analyticcliquetengine.hpp>
#include <ql/pricingengines/cliquet/analyticperformanceengine.hpp>
#include <ql/pricingengines/cliquet/mcperformanceengine.hpp>
#include <ql/pricingengines/exotic/analyticcomplexchooserengine.hpp>
#include <ql/pricingengines/exotic/analyticcompoundoptionengine.hpp>
#include <ql/pricingengines/exotic/analyticsimplechooserengine.hpp>
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
#include <ql/processes/stochasticprocessarray.hpp>
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
#include "capabilities.hpp"
#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <set>
#include <type_traits>
#include <utility>

using namespace QuantLib;
namespace qlpb = quantlib::v2;

namespace qlbackend {

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

        //! The literal a Number holds where only a literal can be read.
        /*! Refuses a quote id, which would never be observed (`why` says
            what copies it), and an unset source, which `.fixed()` would
            otherwise read as 0.
        */
        Real fixedNumber(const qlpb::Number& n, const std::string& path, const char* why) {
            QLS_FIELD_REQUIRE(n.source_case() != qlpb::Number::kQuoteId, qlpb::Error::UNSUPPORTED,
                              path + ".quote_id", why);
            QLS_FIELD_REQUIRE(n.source_case() == qlpb::Number::kFixed,
                              qlpb::Error::INVALID_ARGUMENT, path, "a value is required");
            return n.fixed();
        }

        //! Refuses the variance-reduction switches an MC engine does not take.
        /*! Each MakeMC* builder takes a different subset; the rest were
            accepted, echoed back in the result, and never applied.
        */
        void refuseUnreadVariates(const qlpb::McParameters& mc, bool takesAntithetic,
                                  bool takesBrownianBridge, bool takesControlVariate) {
            QLS_FIELD_REQUIRE(takesAntithetic || !mc.antithetic_variate(),
                              qlpb::Error::UNSUPPORTED, "engine.mc.antithetic_variate",
                              "this Monte Carlo engine does not take antithetic variates");
            QLS_FIELD_REQUIRE(takesBrownianBridge || !mc.brownian_bridge(),
                              qlpb::Error::UNSUPPORTED, "engine.mc.brownian_bridge",
                              "this Monte Carlo engine does not take a Brownian bridge");
            QLS_FIELD_REQUIRE(takesControlVariate || !mc.control_variate(),
                              qlpb::Error::UNSUPPORTED, "engine.mc.control_variate",
                              "this Monte Carlo engine does not take a control variate");
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

        //! The finite-difference time-stepping scheme, as a QuantLib desc.
        /*! `SCHEME_UNSPECIFIED` is refused for the reason the grid beside it is
            refused: two schemes are two different prices for the same trade, so
            there is no safe default to pick on the client's behalf. Douglas is
            what QuantLib defaults to and what a client with no opinion should
            send -- but it should send it.

            The five that build are worth knowing apart. Measured on a
            half-year at-the-money vanilla over 400 x 200: Craig-Sneyd returns
            Douglas's number to the last bit and Crank-Nicolson to within one,
            because in one dimension there are no directions to alternate;
            Hundsdorfer carries a different theta and differs in the seventh
            digit; implicit Euler is first order and differs in the third.

            Explicit Euler is refused. It is stable only while the time step is
            small against the square of the asset step, and the asset step is a
            property of the mesher QuantLib builds inside the engine rather than
            of anything on this frame -- the service cannot tell a client which
            of its grids are stable without duplicating that mesher. What an
            unstable run returns is not an error: at 100 x 200 this build
            answered 2.4e140, and at 400 x 200 a NaN. Implicit Euler is the same
            first-order accuracy with no such condition.
        */
        FdmSchemeDesc fdSchemeFor(qlpb::FdParameters_Explicit_Scheme scheme,
                                  const std::string& fieldPath) {
            switch (scheme) {
                case qlpb::FdParameters_Explicit_Scheme_SCHEME_DOUGLAS:
                    return FdmSchemeDesc::Douglas();
                case qlpb::FdParameters_Explicit_Scheme_SCHEME_CRANK_NICOLSON:
                    return FdmSchemeDesc::CrankNicolson();
                case qlpb::FdParameters_Explicit_Scheme_SCHEME_IMPLICIT_EULER:
                    return FdmSchemeDesc::ImplicitEuler();
                case qlpb::FdParameters_Explicit_Scheme_SCHEME_EXPLICIT_EULER:
                    QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, fieldPath,
                                   "the explicit scheme is stable only while the time step is "
                                   "small against the square of the asset step, which depends on "
                                   "the grid QuantLib builds inside the engine; an unstable run "
                                   "answers with a number rather than an error, so this build "
                                   "does not offer it -- implicit Euler is first order too, and "
                                   "unconditionally stable");
                case qlpb::FdParameters_Explicit_Scheme_SCHEME_CRAIG_SNEYD:
                    return FdmSchemeDesc::CraigSneyd();
                case qlpb::FdParameters_Explicit_Scheme_SCHEME_HUNDSDORFER:
                    return FdmSchemeDesc::Hundsdorfer();
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath,
                           "a custom finite-difference grid needs an explicit scheme at '"
                               << fieldPath
                               << "': two schemes are two different prices for the same trade, and "
                                  "Douglas is a choice rather than an absence of one");
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
                    // Damping steps are Rannacher's fix for the oscillation a
                    // Crank-Nicolson-family scheme shows against a kinked
                    // payoff or a barrier: the first few steps are taken
                    // fully implicit. They are counted in addition to
                    // time_steps rather than out of them.
                    QLS_FIELD_REQUIRE(c.damping_steps() < c.time_steps(),
                                      qlpb::Error::INVALID_ARGUMENT,
                                      fieldPath + ".custom.damping_steps",
                                      "damping steps are the first few of the time steps taken "
                                      "fully implicit, so there have to be more time steps than "
                                      "damping steps");
                    return ext::make_shared<FdBase>(g.process, c.time_steps(), c.asset_steps(),
                                                    c.damping_steps(),
                                                    fdSchemeFor(c.scheme(),
                                                                fieldPath + ".custom.scheme"));
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

        //! The five greeks a multi-asset option does not have.
        /*! `OneAssetOption` declares deltaForward, elasticity, thetaPerDay,
            strikeSensitivity and itmCashProbability together
            (ql/instruments/oneassetoption.hpp:47-56); `MultiAssetOption` stops
            at dividendRho and declares none of them. They arrive as a set, so
            one trait covers all five, and without it `run<BasketOption>` does
            not compile rather than merely reporting them absent.
        */
        template <class T, class = void>
        struct HasMoreGreeks : std::false_type {};

        template <class T>
        struct HasMoreGreeks<T, std::void_t<decltype(std::declval<T&>().itmCashProbability())>>
        : std::true_type {};

        //! Prices one instrument and collects the results asked for.
        template <class Instrument>
        Session::PriceOutcome run(const ext::shared_ptr<Instrument>& option,
                                  const ext::shared_ptr<PricingEngine>& engine,
                                  const qlpb::PriceRequest& msg,
                                  const ext::shared_ptr<GeneralizedBlackScholesProcess>& process =
                                      nullptr,
                                  const std::set<int>& publishedAsZero = {}) {
            option->setPricingEngine(engine);

            const auto start = std::chrono::steady_clock::now();

            Session::PriceOutcome out;
            out.npv = option->NPV();

            // Always for a Monte Carlo, never on request: a Monte Carlo price
            // without its standard error is not comparable to another price,
            // and a client that has to know to ask for it will compare them
            // anyway. Asked of the Monte Carlo engines only: every other
            // engine answers errorEstimate() by throwing, and a sweep of a
            // hundred thousand analytic points would unwind that many times
            // in its hot loop to learn nothing.
            if (msg.engine().method() == qlpb::Engine::METHOD_MONTE_CARLO) {
                try {
                    out.results["errorEstimate"] = option->errorEstimate();
                } catch (const Error&) {
                }
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

                // A greek an engine fills with a literal 0.0 rather than
                // computing. `results_.gamma += 0.0` in the cliquet engines
                // (analyticcliquetengine.cpp:88) publishes a number the fetch
                // below would find and report, and a client cannot tell that
                // zero from a computed one -- which is the whole reason
                // `unavailable` exists. Reported absent instead.
                if (publishedAsZero.count(kind) != 0) {
                    out.unavailable.push_back(static_cast<qlpb::ResultKind>(kind));
                    continue;
                }

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
                            if constexpr (HasMoreGreeks<Instrument>::value)
                                out.results["thetaPerDay"] = option->thetaPerDay();
                            break;
                        case qlpb::RESULT_KIND_RHO:
                            out.results["rho"] = option->rho();
                            break;
                        case qlpb::RESULT_KIND_DIVIDEND_RHO:
                            out.results["dividendRho"] = option->dividendRho();
                            break;
                        case qlpb::RESULT_KIND_DELTA_FORWARD:
                            if constexpr (HasMoreGreeks<Instrument>::value)
                                out.results["deltaForward"] = option->deltaForward();
                            break;
                        case qlpb::RESULT_KIND_ELASTICITY:
                            if constexpr (HasMoreGreeks<Instrument>::value)
                                out.results["elasticity"] = option->elasticity();
                            break;
                        case qlpb::RESULT_KIND_STRIKE_SENSITIVITY:
                            if constexpr (HasMoreGreeks<Instrument>::value)
                                out.results["strikeSensitivity"] = option->strikeSensitivity();
                            break;
                        case qlpb::RESULT_KIND_ITM_CASH_PROBABILITY:
                            if constexpr (HasMoreGreeks<Instrument>::value)
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
            // share an id and leave a reference ambiguous. marketIds_ is the
            // whole namespace, fixings included -- a fixings object lives in
            // no map here, it writes into the IndexManager, and without this
            // two of them could share an id or take a quote's.
            const bool taken = quotes_.count(obj.id()) != 0 || curves_.count(obj.id()) != 0 ||
                               vols_.count(obj.id()) != 0 || indices_.count(obj.id()) != 0 ||
                               correlations_.count(obj.id()) != 0 ||
                               std::find(marketIds_.begin(), marketIds_.end(), obj.id()) !=
                                   marketIds_.end();
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
                case qlpb::MarketObject::kCorrelation:
                    buildCorrelation(obj.id(), obj.correlation(), path + ".correlation");
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
            return fixedNumber(n, path,
                               "an interpolated curve copies its nodes at construction and never "
                               "observes them, so a quote id here would never move the curve; use "
                               "'fixed', or a bootstrapped or flat curve for live pillars");
        };
        // Each shape is one compiled interpolation. Named otherwise, the
        // interpolator was read by nothing and the curve interpolated its own
        // way anyway.
        auto requireInterpolator = [](const qlpb::InterpolatedCurve& ic, qlpb::Interpolator built,
                                      const std::string& path) {
            QLS_FIELD_REQUIRE(ic.interpolator() == qlpb::INTERPOLATOR_UNSPECIFIED ||
                                  ic.interpolator() == built,
                              qlpb::Error::UNSUPPORTED, path + ".interpolator",
                              "this curve shape interpolates " << qlpb::Interpolator_Name(built)
                                                               << " and nothing else; "
                                                               << qlpb::Interpolator_Name(
                                                                      ic.interpolator())
                                                               << " would be read by nothing");
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
                requireInterpolator(ic, qlpb::INTERPOLATOR_LINEAR, path);
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
                requireInterpolator(ic, qlpb::INTERPOLATOR_LOG_LINEAR, path);
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
        // Dual-curve bootstrapping: none of the helpers below is given a
        // discounting curve, so each discounts on the curve being built, and a
        // dual-curve request came back single-curve with nothing to say so.
        QLS_FIELD_REQUIRE(pillar.discount_curve_id().empty(), qlpb::Error::UNSUPPORTED,
                          fieldPath + ".discount_curve_id",
                          "dual-curve bootstrapping is in the schema and not built: each pillar "
                          "discounts on the curve it is building");

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
                    vols.push_back(fixedNumber(vc.nodes(i).volatility(), p + ".volatility",
                                               "BlackVarianceCurve copies its volatilities at "
                                               "construction and never observes them; use "
                                               "'fixed'"));
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
                // Both axes bounded before either is read: the size check
                // below multiplies them, and an unbounded product is what let
                // a surface with no volatilities through (kMaxSurfaceAxisPoints).
                QLS_FIELD_REQUIRE(vs.expiries_size() <= kMaxSurfaceAxisPoints,
                                  qlpb::Error::INVALID_ARGUMENT, path + ".expiries",
                                  "a variance surface takes at most "
                                      << kMaxSurfaceAxisPoints << " expiries, got "
                                      << vs.expiries_size());
                QLS_FIELD_REQUIRE(vs.strikes_size() <= kMaxSurfaceAxisPoints,
                                  qlpb::Error::INVALID_ARGUMENT, path + ".strikes",
                                  "a variance surface takes at most "
                                      << kMaxSurfaceAxisPoints << " strikes, got "
                                      << vs.strikes_size());

                // BlackVarianceSurface interpolates bilinearly, extends across
                // strikes by its interpolator and not at all past the last
                // expiry. Anything else asked for here was read by nothing.
                QLS_FIELD_REQUIRE(vs.interpolator() == qlpb::INTERPOLATOR_UNSPECIFIED ||
                                      vs.interpolator() == qlpb::INTERPOLATOR_LINEAR,
                                  qlpb::Error::UNSUPPORTED, path + ".interpolator",
                                  "a variance surface interpolates bilinearly and nothing else");
                QLS_FIELD_REQUIRE(
                    vs.strike_extrapolation() == qlpb::VarianceSurface::EXTRAPOLATION_UNSPECIFIED ||
                        vs.strike_extrapolation() ==
                            qlpb::VarianceSurface::EXTRAPOLATION_INTERPOLATOR,
                    qlpb::Error::UNSUPPORTED, path + ".strike_extrapolation",
                    "a variance surface extends across strikes by its interpolator and no "
                    "other way");
                QLS_FIELD_REQUIRE(
                    vs.time_extrapolation() == qlpb::VarianceSurface::EXTRAPOLATION_UNSPECIFIED,
                    qlpb::Error::UNSUPPORTED, path + ".time_extrapolation",
                    "a variance surface does not extend past its last expiry");

                std::vector<Date> expiries;
                for (int i = 0; i < vs.expiries_size(); ++i)
                    expiries.push_back(registry_.date(
                        vs.expiries(i), path + ".expiries[" + std::to_string(i) + "]"));
                std::vector<Real> strikes(vs.strikes().begin(), vs.strikes().end());

                // In 64 bits, so the product cannot wrap whatever the caps are.
                QLS_FIELD_REQUIRE(
                    static_cast<std::uint64_t>(vs.volatilities_size()) ==
                        static_cast<std::uint64_t>(expiries.size()) * strikes.size(),
                    qlpb::Error::INVALID_ARGUMENT, path + ".volatilities",
                    "a variance surface needs expiries x strikes volatilities, got "
                        << vs.volatilities_size() << " for " << expiries.size() << " x "
                        << strikes.size());

                // Row-major over (expiry, strike) on the wire; QuantLib's
                // Matrix here is strikes x expiries, so this transposes rather
                // than reshapes. Getting it the wrong way round produces a
                // surface that prices without complaint.
                // Read as literals, like the variance curve's: a quote id here
                // used to read as a volatility of 0, and price at intrinsic.
                Matrix v(strikes.size(), expiries.size());
                for (Size i = 0; i < expiries.size(); ++i)
                    for (Size j = 0; j < strikes.size(); ++j) {
                        const auto at = static_cast<int>(i * strikes.size() + j);
                        v[j][i] = fixedNumber(vs.volatilities(at),
                                              path + ".volatilities[" + std::to_string(at) + "]",
                                              "BlackVarianceSurface copies its volatilities at "
                                              "construction and never observes them; use 'fixed'");
                    }

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


    //! A correlation matrix, checked here because no engine checks it.
    /*! `StulzEngine` takes rho as a bare `Real` and asks nothing of it, and
        `StochasticProcessArray` is worse than permissive: it factorises with
        `SalvagingAlgorithm::Spectral`
        (ql/processes/stochasticprocessarray.cpp:31), which *repairs* a matrix
        that is not positive semi-definite by zeroing the negative eigenvalues
        and renormalising. An impossible market is therefore not refused and
        not even mispriced -- it is quietly replaced with the nearest possible
        one and priced correctly for that. Every property is checked here,
        before an engine sees the matrix.
    */
    void Session::buildCorrelation(const std::string& id, const qlpb::CorrelationMatrix& msg,
                                   const std::string& fieldPath) {
        const int n = msg.labels_size();
        QLS_FIELD_REQUIRE(n > 1, qlpb::Error::INVALID_ARGUMENT, fieldPath + ".labels",
                          "a correlation matrix needs at least two labels, got " << n);
        QLS_FIELD_REQUIRE(n <= kMaxCorrelationLabels, qlpb::Error::INVALID_ARGUMENT,
                          fieldPath + ".labels",
                          "a correlation matrix takes at most " << kMaxCorrelationLabels
                                                                << " labels, got " << n);
        // In 64 bits: n * n in an int is undefined past 46,340 labels, and
        // wrapped into a count a client could send.
        QLS_FIELD_REQUIRE(static_cast<std::int64_t>(msg.values_size()) ==
                              static_cast<std::int64_t>(n) * n,
                          qlpb::Error::INVALID_ARGUMENT,
                          fieldPath + ".values",
                          "a " << n << "-label correlation matrix needs " << n * n
                               << " row-major entries, got " << msg.values_size());

        std::set<std::string> seen;
        for (int i = 0; i < n; ++i) {
            const std::string at = fieldPath + ".labels[" + std::to_string(i) + "]";
            QLS_FIELD_REQUIRE(!msg.labels(i).empty(), qlpb::Error::INVALID_ARGUMENT, at,
                              "a correlation label cannot be empty: it is what an underlying "
                              "names to find its row");
            QLS_FIELD_REQUIRE(seen.insert(msg.labels(i)).second, qlpb::Error::INVALID_ARGUMENT, at,
                              "duplicate correlation label '" << msg.labels(i) << "'");
        }

        Correlation c;
        c.labels.assign(msg.labels().begin(), msg.labels().end());
        Matrix m(n, n);
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const std::string at =
                    fieldPath + ".values[" + std::to_string(i * n + j) + "]";
                auto entry = number(msg.values(i * n + j), at);
                const Real v = entry->value();
                QLS_FIELD_REQUIRE(v >= -1.0 && v <= 1.0, qlpb::Error::INVALID_ARGUMENT, at,
                                  "correlation " << v << " at [" << i << "][" << j
                                                 << "] is outside [-1, 1]");
                if (i == j)
                    QLS_FIELD_REQUIRE(close_enough(v, 1.0), qlpb::Error::INVALID_ARGUMENT, at,
                                      "the diagonal of a correlation matrix is 1, and ["
                                          << i << "][" << i << "] is " << v);
                c.entries.push_back(entry);
                m[i][j] = v;
            }
        }

        checkCorrelation(m, fieldPath + ".values");
        correlations_[id] = std::move(c);
    }


    void Session::checkCorrelation(const Matrix& m, const std::string& fieldPath) const {
        const int n = static_cast<int>(m.rows());
        for (int i = 0; i < n; ++i) {
            QLS_FIELD_REQUIRE(close_enough(m[i][i], 1.0), qlpb::Error::INVALID_ARGUMENT, fieldPath,
                              "the diagonal of a correlation matrix is 1, and ["
                                  << i << "][" << i << "] is " << m[i][i]);
            for (int j = 0; j < n; ++j) {
                QLS_FIELD_REQUIRE(m[i][j] >= -1.0 && m[i][j] <= 1.0,
                                  qlpb::Error::INVALID_ARGUMENT, fieldPath,
                                  "correlation " << m[i][j] << " at [" << i << "][" << j
                                                 << "] is outside [-1, 1]");
                QLS_FIELD_REQUIRE(close_enough(m[i][j], m[j][i]), qlpb::Error::INVALID_ARGUMENT,
                                  fieldPath,
                                  "a correlation matrix is symmetric: ["
                                      << i << "][" << j << "] is " << m[i][j] << " and [" << j
                                      << "][" << i << "] is " << m[j][i]);
            }
        }

        // Positive semi-definiteness is the one a client cannot check by eye
        // past two assets. Three pairwise correlations can each be legal and
        // jointly impossible -- 0.9, 0.9, -0.9 is the standard example -- and
        // what comes back from an engine given one is a number, not an error:
        // StochasticProcessArray repairs the matrix instead of refusing it.
        const Array eigenvalues = SymmetricSchurDecomposition(m).eigenvalues();
        const Real smallest = *std::min_element(eigenvalues.begin(), eigenvalues.end());
        QLS_FIELD_REQUIRE(smallest > -1.0e-10, qlpb::Error::INVALID_ARGUMENT, fieldPath,
                          "this correlation matrix is not positive semi-definite -- its smallest "
                          "eigenvalue is "
                              << smallest
                              << " -- so no set of assets has it: some combination of them would "
                                 "have negative variance");
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

        // Every name checked before anything is written. A typo'd quote id
        // used to be found mid-batch, inside the guard whose failure marks the
        // session dirty -- so one wrong id failed the next Price as well, and
        // cost a full replay, for a write that never touched the graph.
        for (int i = 0; i < msg.quotes_size(); ++i)
            QLS_FIELD_REQUIRE(quotes_.count(msg.quotes(i).quote_id()) != 0,
                              qlpb::Error::UNKNOWN_ID,
                              "quotes[" + std::to_string(i) + "].quote_id",
                              "unknown quote '" << msg.quotes(i).quote_id() << "'");
        for (int i = 0; i < msg.fixings_size(); ++i)
            QLS_FIELD_REQUIRE(indices_.count(msg.fixings(i).index_id()) != 0,
                              qlpb::Error::UNKNOWN_ID,
                              "fixings[" + std::to_string(i) + "].index_id",
                              "unknown index '" << msg.fixings(i).index_id() << "'");

        // One guard around the whole batch: N quote writes, one recalculation.
        UpdateGuard guard;
        try {
            for (const auto& u : msg.quotes())
                quotes_.find(u.quote_id())->second->setValue(u.value());
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
            //
            // After the commit, so a bootstrap that fails here does so with
            // the writes already in the graph. The client is told the update
            // failed and the log drops it, so the graph and the log would
            // disagree: the session is marked dirty and rebuilt from the log.
            try {
                for (auto& entry : curves_) {
                    if (auto lazy =
                            ext::dynamic_pointer_cast<LazyObject>(entry.second.currentLink()))
                        lazy->recalculate();
                }
            } catch (...) {
                dirty_ = true;
                throw;
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

    namespace {

        //! Refuses an engine whose size is a typo or a hostile frame rather
        //! than a request. The limits, and why each exists, are in
        //! capabilities.hpp; this is the one place they are enforced.
        void checkEngineLimits(const qlpb::PriceRequest& msg) {
            const auto& engine = msg.engine();

            if (engine.has_lattice()) {
                const auto steps = engine.lattice().steps();
                QLS_FIELD_REQUIRE(steps <= kMaxLatticeSteps, qlpb::Error::INVALID_ARGUMENT,
                                  "engine.lattice.steps",
                                  "a lattice takes at most " << kMaxLatticeSteps
                                                             << " steps, got " << steps);
            }

            if (engine.has_fd() && engine.fd().has_custom()) {
                const auto& grid = engine.fd().custom();
                QLS_FIELD_REQUIRE(grid.time_steps() <= kMaxFdTimeSteps,
                                  qlpb::Error::INVALID_ARGUMENT, "engine.fd.custom.time_steps",
                                  "a finite-difference grid takes at most "
                                      << kMaxFdTimeSteps << " time steps, got "
                                      << grid.time_steps());
                QLS_FIELD_REQUIRE(grid.asset_steps() <= kMaxFdAssetSteps,
                                  qlpb::Error::INVALID_ARGUMENT, "engine.fd.custom.asset_steps",
                                  "a finite-difference grid takes at most "
                                      << kMaxFdAssetSteps << " asset steps, got "
                                      << grid.asset_steps());
            }

            if (engine.has_mc()) {
                const auto& mc = engine.mc();
                QLS_FIELD_REQUIRE(mc.samples() <= kMaxMcSamples, qlpb::Error::INVALID_ARGUMENT,
                                  "engine.mc.samples",
                                  "a Monte Carlo draws at most " << kMaxMcSamples
                                                                 << " paths, got "
                                                                 << mc.samples());
                QLS_FIELD_REQUIRE(mc.time_steps_per_year() <= kMaxMcTimeStepsPerYear,
                                  qlpb::Error::INVALID_ARGUMENT, "engine.mc.time_steps_per_year",
                                  "a Monte Carlo path takes at most "
                                      << kMaxMcTimeStepsPerYear << " time steps a year, got "
                                      << mc.time_steps_per_year());
                if (mc.progress_every_paths() > 0) {
                    // Divided rather than rounded up by adding, which is how
                    // priceInBatches used to overflow on a sample count near
                    // the top of uint64.
                    const auto batch = mc.progress_every_paths();
                    const auto batches = mc.samples() / batch + (mc.samples() % batch != 0 ? 1 : 0);
                    QLS_FIELD_REQUIRE(batches <= kMaxMcBatches, qlpb::Error::INVALID_ARGUMENT,
                                      "engine.mc.progress_every_paths",
                                      "batches of " << batch << " paths split " << mc.samples()
                                                    << " samples into " << batches
                                                    << " batches, and the limit is "
                                                    << kMaxMcBatches
                                                    << ": each batch builds an engine and "
                                                       "sends a progress frame");
                }
            }

            if (msg.has_implied_volatility()) {
                const auto evaluations = msg.implied_volatility().max_evaluations();
                QLS_FIELD_REQUIRE(evaluations <= kMaxImpliedVolatilityEvaluations,
                                  qlpb::Error::INVALID_ARGUMENT,
                                  "implied_volatility.max_evaluations",
                                  "an implied volatility takes at most "
                                      << kMaxImpliedVolatilityEvaluations
                                      << " evaluations, got " << evaluations);
            }
        }

    }


    namespace {

        //! Refuses engine fields this build takes over the wire and never reads.
        /*! Each was accepted and priced without: a model other than
            Black-Scholes came back Black-Scholes, and a Sobol request came
            back pseudo-random -- while the result's engine echo, copied from
            the request, said Sobol. A parameter a client can see us take and
            cannot see us ignore is the defect this schema exists to avoid.
            Unset and the one value that is built stay accepted.
        */
        void checkEngineRead(const qlpb::PriceRequest& msg) {
            const auto& engine = msg.engine();
            const auto model = engine.model();
            QLS_FIELD_REQUIRE(model == qlpb::Engine::MODEL_UNSPECIFIED ||
                                  model == qlpb::Engine::MODEL_BLACK_SCHOLES,
                              qlpb::Error::UNSUPPORTED, "engine.model",
                              qlpb::Engine::Model_Name(model)
                                  << " is in the schema and not built: every price here is "
                                     "Black-Scholes, with the process the underlying names");
            if (engine.has_mc())
                QLS_FIELD_REQUIRE(engine.mc().rng() != qlpb::McParameters::RNG_LOW_DISCREPANCY,
                                  qlpb::Error::UNSUPPORTED, "engine.mc.rng",
                                  "every Monte Carlo here draws pseudo-random numbers (Mersenne "
                                  "twister); low-discrepancy sequences are in the schema and "
                                  "not built");
        }

    }


    Session::PriceOutcome Session::price(const qlpb::PriceRequest& msg,
                                         const ProgressSink& progress) {
        QL_REQUIRE(!dirty_, "session is dirty and must be replayed before pricing");

        // Before anything is built: every other path below trusts these sizes,
        // and none of them reads the fields refused here.
        checkEngineLimits(msg);
        checkEngineRead(msg);

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

        // Basket is the one style that takes more than one, and it is the
        // reason equityGraph is called per underlying rather than once.
        const bool multiAsset = opt.style_case() == qlpb::Option::kBasket;
        QLS_FIELD_REQUIRE(opt.underlyings_size() >= 1, qlpb::Error::INVALID_ARGUMENT,
                          base + ".underlyings", "an option needs an underlying");
        QLS_FIELD_REQUIRE(multiAsset || opt.underlyings_size() == 1,
                          qlpb::Error::INVALID_ARGUMENT, base + ".underlyings",
                          "this option takes exactly one underlying, got "
                              << opt.underlyings_size()
                              << "; basket is the style that takes more");
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
                        refuseUnreadVariates(mc, false, false, false);
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

                // A binary payoff on a barrier is a knock digital, and
                // AnalyticBinaryBarrierEngine is what prices it. There is no
                // QuantLib instrument for one -- this shape is the product --
                // which is why `message Digital` is a description of a barrier
                // rather than a style of its own, and why the arm below refuses
                // it by pointing here.
                const bool binaryPayoff =
                    opt.payoff().kind_case() == qlpb::Payoff::kCashOrNothing ||
                    opt.payoff().kind_case() == qlpb::Payoff::kAssetOrNothing;

                if (graph.quanto) {
                    // QuantoEngine would wrap AnalyticBarrierEngine, which
                    // wants a plain payoff and would fail from inside the
                    // engine. There is no quanto binary barrier engine to wrap
                    // instead.
                    QLS_FIELD_REQUIRE(!binaryPayoff, qlpb::Error::UNSUPPORTED, base + ".quanto",
                                      "there is no quanto binary barrier engine in QuantLib");
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

                if (binaryPayoff) {
                    // Every one of these is something the engine would throw on
                    // from the inside, arriving as CALCULATION_FAILED with no
                    // field on it -- or, for the rebate, not throw on at all.
                    QLS_FIELD_REQUIRE(eng.method() == qlpb::Engine_Method_METHOD_ANALYTIC,
                                      qlpb::Error::UNSUPPORTED, "engine.method",
                                      "a binary payoff on a barrier takes METHOD_ANALYTIC: "
                                      "AnalyticBinaryBarrierEngine is the only engine here that "
                                      "reads one");
                    QLS_FIELD_REQUIRE(opt.exercise().type() == qlpb::Exercise_Type_TYPE_AMERICAN,
                                      qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                      "a knock digital is written on an American exercise: "
                                      "AnalyticBinaryBarrierEngine casts to one "
                                      "(analyticbinarybarrierengine.cpp:65)");
                    QLS_FIELD_REQUIRE(
                        flag(opt.exercise().payoff_at_expiry(),
                             base + ".exercise.payoff_at_expiry"),
                        qlpb::Error::INVALID_ARGUMENT, base + ".exercise.payoff_at_expiry",
                        "a knock digital settles at expiry rather than on touch, so "
                        "payoff_at_expiry must be true (analyticbinarybarrierengine.cpp:66)");
                    QLS_FIELD_REQUIRE(ex->dates().front() <= evaluationDate_,
                                      qlpb::Error::UNSUPPORTED, base + ".exercise.earliest_date",
                                      "the barrier is live from the evaluation date: QuantLib has "
                                      "no window exercise here "
                                      "(analyticbinarybarrierengine.cpp:67)");
                    // The engine never reads arguments_.rebate. A rebate sent
                    // here would be dropped and a number returned for a
                    // different trade, which is the one failure this service
                    // exists to make impossible.
                    QLS_FIELD_REQUIRE(b.rebate() == 0.0, qlpb::Error::UNSUPPORTED,
                                      path + ".rebate",
                                      "AnalyticBinaryBarrierEngine has no rebate; it would be "
                                      "taken and never read");
                    return run(option,
                               ext::make_shared<AnalyticBinaryBarrierEngine>(graph.process), msg,
                               graph.process);
                }

                switch (eng.method()) {
                    case qlpb::Engine_Method_METHOD_ANALYTIC:
                        QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED,
                                          base + ".exercise.type",
                                          "AnalyticBarrierEngine is European only; an American "
                                          "barrier takes METHOD_LATTICE or "
                                          "METHOD_FINITE_DIFFERENCE");
                        // "non-plain payoff given" (analyticbarrierengine.cpp:40)
                        // otherwise, from inside the engine and naming nothing.
                        QLS_FIELD_REQUIRE(opt.payoff().kind_case() == qlpb::Payoff::kPlain,
                                          qlpb::Error::UNSUPPORTED, base + ".payoff",
                                          "AnalyticBarrierEngine takes a plain payoff; a binary "
                                          "one is a knock digital and prices without an engine "
                                          "method of its own");
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
                        refuseUnreadVariates(mc, false, false, false);
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
                        refuseUnreadVariates(mc, false, false, true);
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

            // -- compound --------------------------------------------------
            case qlpb::Option::kCompound: {
                const std::string path = base + ".compound";
                const auto& c = opt.compound();

                // The mother is the option's own payoff and exercise.
                // CompoundOption derives from OneAssetOption and passes those
                // two straight to it (ql/instruments/compoundoption.cpp:31), so
                // Compound.mother_payoff and .mother_exercise re-declare fields
                // every other style already takes. A request setting both would
                // give the same question two answers; these are refused by name
                // rather than merged or, worse, ignored.
                QLS_FIELD_REQUIRE(!c.has_mother_payoff(), qlpb::Error::UNSUPPORTED,
                                  path + ".mother_payoff",
                                  "the mother option is the option's own payoff: set "
                                  "instrument.option.payoff rather than this");
                QLS_FIELD_REQUIRE(!c.has_mother_exercise(), qlpb::Error::UNSUPPORTED,
                                  path + ".mother_exercise",
                                  "the mother option is the option's own exercise: set "
                                  "instrument.option.exercise rather than this");

                QLS_FIELD_REQUIRE(!graph.quanto, qlpb::Error::UNSUPPORTED, base + ".quanto",
                                  "there is no quanto compound engine in QuantLib");
                QLS_FIELD_REQUIRE(eng.method() == qlpb::Engine_Method_METHOD_ANALYTIC,
                                  qlpb::Error::UNSUPPORTED, "engine.method",
                                  "compound options take METHOD_ANALYTIC: QuantLib has one "
                                  "compound engine and it is the Wystup closed form");
                QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                  "AnalyticCompoundOptionEngine is European only");
                QLS_FIELD_REQUIRE(c.daughter_exercise().type() == qlpb::Exercise_Type_TYPE_EUROPEAN,
                                  qlpb::Error::UNSUPPORTED, path + ".daughter_exercise.type",
                                  "the option written on is European only, for the same reason");

                // Plain on both legs. The engine casts each payoff back to a
                // PlainVanillaPayoff and QL_FAILs "non-plain payoff given"
                // (analyticcompoundoptionengine.cpp:205,213) -- which arrives as
                // CALCULATION_FAILED with no field to blame, so it is caught
                // here where there is one.
                QLS_FIELD_REQUIRE(opt.payoff().kind_case() == qlpb::Payoff::kPlain,
                                  qlpb::Error::UNSUPPORTED, base + ".payoff",
                                  "a compound option takes a plain payoff on each leg");
                QLS_FIELD_REQUIRE(c.daughter_payoff().kind_case() == qlpb::Payoff::kPlain,
                                  qlpb::Error::UNSUPPORTED, path + ".daughter_payoff",
                                  "a compound option takes a plain payoff on each leg");

                const auto daughterEx =
                    exercise(c.daughter_exercise(), path + ".daughter_exercise");

                // CompoundOption::arguments::validate() throws on this
                // (ql/instruments/compoundoption.cpp:52) and an exception out of
                // the engine names no field. The option on the option has to
                // expire first, or there is nothing left to exercise into.
                QLS_FIELD_REQUIRE(ex->lastDate() <= daughterEx->lastDate(),
                                  qlpb::Error::INVALID_ARGUMENT,
                                  path + ".daughter_exercise.dates",
                                  "the compound expires "
                                      << ex->lastDate()
                                      << ", after the option it is written on, which expires "
                                      << daughterEx->lastDate());

                return run(ext::make_shared<CompoundOption>(
                               payoff(opt.payoff(), base + ".payoff"), ex,
                               payoff(c.daughter_payoff(), path + ".daughter_payoff"), daughterEx),
                           ext::make_shared<AnalyticCompoundOptionEngine>(graph.process), msg);
            }



            // -- basket ----------------------------------------------------
            case qlpb::Option::kBasket: {
                const std::string path = base + ".basket";
                const auto& bk = opt.basket();
                const int n = opt.underlyings_size();

                QLS_FIELD_REQUIRE(n >= 2, qlpb::Error::INVALID_ARGUMENT, base + ".underlyings",
                                  "a basket needs at least two underlyings, got "
                                      << n << "; with one it is whatever style that one asset is");
                QLS_FIELD_REQUIRE(!graph.quanto, qlpb::Error::UNSUPPORTED, base + ".quanto",
                                  "there is no quanto basket engine in QuantLib");
                QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                  "the basket engines built here are European only: an American "
                                  "basket is MCAmericanBasketEngine, which is Longstaff-Schwartz "
                                  "and takes a basis-function choice this schema cannot carry");
                QLS_FIELD_REQUIRE(opt.payoff().kind_case() == qlpb::Payoff::kPlain,
                                  qlpb::Error::UNSUPPORTED, base + ".payoff",
                                  "a basket wraps a plain payoff: BasketPayoff accumulates the "
                                  "assets to one number and hands that to the payoff underneath");

                // One graph per asset. equityGraph is per-underlying already,
                // so this is the same construction the single-asset styles get,
                // n times -- including the process each Underlying.process asks
                // for, which a spread wants to be PROCESS_BLACK.
                std::vector<Session::EquityGraph> graphs{graph};
                std::vector<ext::shared_ptr<StochasticProcess1D>> processes{graph.process};
                for (int i = 1; i < n; ++i) {
                    graphs.push_back(equityGraph(opt.underlyings(i), opt,
                                                 base + ".underlyings[" + std::to_string(i) + "]"));
                    processes.push_back(graphs.back().process);
                }

                // The correlation matrix, indexed by the labels the underlyings
                // carry. Read out afresh here rather than held: what QuantLib
                // takes is a plain Matrix, factorised once at construction, so
                // a correlation that moved between requests only reaches the
                // price because this runs again (DESIGN §5).
                QLS_FIELD_REQUIRE(!bk.correlation_id().empty(), qlpb::Error::INVALID_ARGUMENT,
                                  path + ".correlation_id",
                                  "a basket needs a correlation matrix: with n assets there are "
                                  "n(n-1)/2 numbers and no default for any of them");
                const auto found = correlations_.find(bk.correlation_id());
                QLS_FIELD_REQUIRE(found != correlations_.end(), qlpb::Error::UNKNOWN_ID,
                                  path + ".correlation_id",
                                  "unknown correlation matrix '" << bk.correlation_id() << "'");
                const auto& cm = found->second;

                std::vector<int> row;
                for (int i = 0; i < n; ++i) {
                    const std::string at =
                        base + ".underlyings[" + std::to_string(i) + "].label";
                    const std::string& label = opt.underlyings(i).label();
                    QLS_FIELD_REQUIRE(!label.empty(), qlpb::Error::INVALID_ARGUMENT, at,
                                      "every underlying in a basket needs a label: the "
                                      "correlation matrix indexes on it, and position would be "
                                      "a second answer to the same question");
                    const auto it = std::find(cm.labels.begin(), cm.labels.end(), label);
                    QLS_FIELD_REQUIRE(it != cm.labels.end(), qlpb::Error::UNKNOWN_ID, at,
                                      "correlation matrix '"
                                          << bk.correlation_id() << "' has no row for label '"
                                          << label << "'");
                    row.push_back(static_cast<int>(it - cm.labels.begin()));
                }

                const int size = static_cast<int>(cm.labels.size());
                Matrix correlation(n, n);
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j)
                        correlation[i][j] = cm.entries[row[i] * size + row[j]]->value();

                // Checked again, not only when the object was built: the
                // entries are quotes, so a matrix that was a correlation
                // matrix when the session opened can be dragged into one that
                // is not, and the engines would take it.
                checkCorrelation(correlation, path + ".correlation_id");

                // Weights are read by AverageBasketPayoff and by nothing else
                // (ql/instruments/basketoption.hpp:71), so on any other kind
                // they would be taken and dropped.
                const bool average = bk.kind() == qlpb::Basket_Kind_KIND_AVERAGE;
                QLS_FIELD_REQUIRE(average || bk.weights_size() == 0, qlpb::Error::UNSUPPORTED,
                                  path + ".weights",
                                  "only KIND_AVERAGE reads weights: a minimum, a maximum and a "
                                  "spread are not weighted sums, and AverageBasketPayoff is the "
                                  "only payoff that looks at them");
                QLS_FIELD_REQUIRE(bk.weights_size() == 0 || bk.weights_size() == n,
                                  qlpb::Error::INVALID_ARGUMENT, path + ".weights",
                                  "a weight per underlying, or none for equal weights: got "
                                      << bk.weights_size() << " for " << n << " assets");

                const auto plain = payoff(opt.payoff(), base + ".payoff");
                ext::shared_ptr<BasketPayoff> basketPayoff;
                switch (bk.kind()) {
                    case qlpb::Basket_Kind_KIND_MIN:
                        basketPayoff = ext::make_shared<MinBasketPayoff>(plain);
                        break;
                    case qlpb::Basket_Kind_KIND_MAX:
                        basketPayoff = ext::make_shared<MaxBasketPayoff>(plain);
                        break;
                    case qlpb::Basket_Kind_KIND_SPREAD:
                        QLS_FIELD_REQUIRE(n == 2, qlpb::Error::INVALID_ARGUMENT,
                                          base + ".underlyings",
                                          "a spread is the difference of two assets, and "
                                          "SpreadBasketPayoff refuses any other count; got " << n);
                        basketPayoff = ext::make_shared<SpreadBasketPayoff>(plain);
                        break;
                    case qlpb::Basket_Kind_KIND_AVERAGE:
                        if (bk.weights_size() == 0) {
                            basketPayoff = ext::make_shared<AverageBasketPayoff>(
                                plain, static_cast<Size>(n));
                        } else {
                            Array weights(static_cast<Size>(n));
                            for (int i = 0; i < n; ++i)
                                weights[i] = bk.weights(i);
                            basketPayoff = ext::make_shared<AverageBasketPayoff>(plain, weights);
                        }
                        break;
                    default:
                        QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, path + ".kind",
                                       "unspecified basket kind at '" << path << ".kind'");
                }

                auto option = ext::make_shared<BasketOption>(basketPayoff, ex);

                switch (eng.method()) {
                    case qlpb::Engine_Method_METHOD_ANALYTIC: {
                        QLS_FIELD_REQUIRE(n == 2, qlpb::Error::UNSUPPORTED, base + ".underlyings",
                                          "the closed forms are two-asset: StulzEngine takes two "
                                          "processes and a rho, and so does Kirk. Past two "
                                          "assets a basket takes METHOD_MONTE_CARLO");
                        if (bk.kind() == qlpb::Basket_Kind_KIND_SPREAD)
                            return run(option,
                                       ext::make_shared<KirkEngine>(graphs[0].process,
                                                                    graphs[1].process,
                                                                    correlation[0][1]),
                                       msg);
                        QLS_FIELD_REQUIRE(!average, qlpb::Error::UNSUPPORTED, path + ".kind",
                                          "there is no closed form here for an average basket: "
                                          "Stulz prices the minimum or the maximum of two assets "
                                          "and Kirk the difference. An average takes "
                                          "METHOD_MONTE_CARLO");
                        return run(option,
                                   ext::make_shared<StulzEngine>(graphs[0].process,
                                                                 graphs[1].process,
                                                                 correlation[0][1]),
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
                        refuseUnreadVariates(mc, true, true, false);
                        return run(option,
                                   MakeMCEuropeanBasketEngine<PseudoRandom>(
                                       ext::make_shared<StochasticProcessArray>(processes,
                                                                                correlation))
                                       .withStepsPerYear(mc.time_steps_per_year() > 0
                                                             ? mc.time_steps_per_year()
                                                             : 1)
                                       .withSamples(mc.samples())
                                       .withSeed(mc.seed())
                                       .withBrownianBridge(mc.brownian_bridge())
                                       .withAntitheticVariate(mc.antithetic_variate()),
                                   msg);
                    }

                    case qlpb::Engine_Method_METHOD_FINITE_DIFFERENCE:
                        // Fd2dBlackScholesVanillaEngine exists and would price
                        // two assets, but it takes two space grids and a time
                        // grid where FdParameters carries one space dimension
                        // (market.proto). Picking the second here would be the
                        // default nobody chose that every other grid in this
                        // service refuses.
                        QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, "engine.fd",
                                       "a two-asset finite-difference grid needs a second space "
                                       "dimension, and FdParameters describes one; send "
                                       "METHOD_ANALYTIC or METHOD_MONTE_CARLO");

                    default:
                        break;
                }
                QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, "engine.method",
                               "basket options take METHOD_ANALYTIC on two assets, or "
                               "METHOD_MONTE_CARLO on any number");
            }

            // -- spread ----------------------------------------------------
            case qlpb::Option::kSpread:
                // Not built, and no longer a separate product. `message
                // Spread`'s comment says it is kept apart from Basket because
                // QuantLib prices it with Kirk rather than through the basket
                // engines; that was true when it was written and is false
                // against QuantLib 1.43. ql/experimental/exoticoptions/
                // spreadoption.hpp and kirkspreadoptionengine.hpp are empty
                // stubs that announce their own removal, and KirkEngine now
                // derives from SpreadBlackScholesVanillaEngine, which is a
                // BasketOption::engine. So a spread is a basket kind, and the
                // basket arm above prices it.
                QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, base + ".spread",
                               "a spread is a basket in QuantLib 1.43: KirkEngine derives from "
                               "BasketOption::engine and the standalone SpreadOption is an empty "
                               "deprecated stub. Send instrument.option.basket with "
                               "KIND_SPREAD and two underlyings");

            // -- cliquet ---------------------------------------------------
            case qlpb::Option::kCliquet: {
                const std::string path = base + ".cliquet";
                const auto& cl = opt.cliquet();

                // The four fields QuantLib cannot carry. This is not one
                // engine's limitation: CliquetOption::setupArguments copies the
                // reset dates and stops -- the comment above the line says "set
                // accrued coupon, last fixing, caps, floors" and the line does
                // not (ql/instruments/cliquetoption.cpp:32) -- so a cap sent
                // here reaches no engine at all. Every engine then finds the
                // argument still Null and prices the uncapped ratchet
                // (analyticcliquetengine.cpp:38-42). Refused by name, because
                // the alternative is a price for a trade nobody described.
                for (const auto& [set, field] :
                     {std::pair{cl.local_cap() != 0.0, "local_cap"},
                      std::pair{cl.local_floor() != 0.0, "local_floor"},
                      std::pair{cl.global_cap() != 0.0, "global_cap"},
                      std::pair{cl.global_floor() != 0.0, "global_floor"}}) {
                    QLS_FIELD_REQUIRE(!set, qlpb::Error::UNSUPPORTED, path + "." + field,
                                      "QuantLib carries no cap or floor on a cliquet: "
                                      "CliquetOption::setupArguments never copies this field, so "
                                      "the engine would price the uncapped ratchet and report "
                                      "nothing amiss");
                }

                QLS_FIELD_REQUIRE(!graph.quanto, qlpb::Error::UNSUPPORTED, base + ".quanto",
                                  "there is no quanto cliquet engine in QuantLib");
                QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                  "the cliquet engines are European only");

                // Same payoff as a forward start, and for the same reason: each
                // period is struck at a fraction of the spot when it opens.
                // CliquetOption's constructor takes a PercentageStrikePayoff by
                // type (ql/instruments/cliquetoption.cpp:26).
                QLS_FIELD_REQUIRE(
                    opt.payoff().kind_case() == qlpb::Payoff::kPercentageStrike,
                    qlpb::Error::INVALID_ARGUMENT, base + ".payoff.percentage_strike",
                    "a cliquet resets its strike to a fraction of the spot at each reset, so it "
                    "takes a percentage_strike payoff");
                const Real moneyness = opt.payoff().percentage_strike().moneyness();
                QLS_FIELD_REQUIRE(moneyness > 0.0, qlpb::Error::INVALID_ARGUMENT,
                                  base + ".payoff.percentage_strike.moneyness",
                                  "moneyness must be positive");

                QLS_FIELD_REQUIRE(cl.reset_dates_size() > 0, qlpb::Error::INVALID_ARGUMENT,
                                  path + ".reset_dates",
                                  "a cliquet needs the dates its strike resets on");
                std::vector<Date> resets;
                for (int i = 0; i < cl.reset_dates_size(); ++i) {
                    const std::string at =
                        path + ".reset_dates[" + std::to_string(i) + "]";
                    const Date reset = registry_.date(cl.reset_dates(i), at);

                    // The engines discount to each reset in turn
                    // (analyticcliquetengine.cpp:70), and a curve throws rather
                    // than extrapolates behind its reference date -- "date
                    // before reference date", with no field on it.
                    QLS_FIELD_REQUIRE(reset >= evaluationDate_, qlpb::Error::INVALID_ARGUMENT, at,
                                      "reset " << reset << " is before the evaluation date "
                                               << evaluationDate_);
                    QLS_FIELD_REQUIRE(reset < ex->lastDate(), qlpb::Error::INVALID_ARGUMENT, at,
                                      "reset " << reset << " is not before the expiry "
                                               << ex->lastDate());
                    QLS_FIELD_REQUIRE(resets.empty() || reset > resets.back(),
                                      qlpb::Error::INVALID_ARGUMENT, at,
                                      "reset dates must be in order and distinct: "
                                          << reset << " does not follow " << resets.back());
                    resets.push_back(reset);
                }

                // A different engine rather than a scaling of the same number,
                // exactly as on a forward start -- hence a Flag with no default
                // (DESIGN §6.3).
                const bool performance = flag(cl.performance(), path + ".performance");

                auto option = ext::make_shared<CliquetOption>(
                    ext::make_shared<PercentageStrikePayoff>(
                        optionType(opt.payoff().type(), base + ".payoff.type"), moneyness),
                    ext::dynamic_pointer_cast<EuropeanExercise>(ex), resets);

                // Both closed forms write `results_.gamma += 0.0` and the
                // performance one does the same to delta
                // (analyticperformanceengine.cpp:60). Those are placeholders,
                // not values, and they are reported absent rather than as a
                // zero a client would have no way to distinguish.
                const std::set<int> zeroed =
                    performance ? std::set<int>{qlpb::RESULT_KIND_DELTA, qlpb::RESULT_KIND_GAMMA}
                                : std::set<int>{qlpb::RESULT_KIND_GAMMA};

                switch (eng.method()) {
                    case qlpb::Engine_Method_METHOD_ANALYTIC:
                        if (performance)
                            return run(option,
                                       ext::make_shared<AnalyticPerformanceEngine>(graph.process),
                                       msg, nullptr, zeroed);
                        return run(option,
                                   ext::make_shared<AnalyticCliquetEngine>(graph.process), msg,
                                   nullptr, zeroed);

                    case qlpb::Engine_Method_METHOD_MONTE_CARLO: {
                        // The one Monte Carlo cliquet engine QuantLib has is
                        // the performance one; there is no ratchet path pricer
                        // to pair with it.
                        QLS_FIELD_REQUIRE(performance, qlpb::Error::UNSUPPORTED,
                                          path + ".performance",
                                          "QuantLib's only Monte Carlo cliquet engine is the "
                                          "performance one: send performance true, or price the "
                                          "ratchet with METHOD_ANALYTIC");
                        const auto& mc = eng.mc();
                        QLS_FIELD_REQUIRE(mc.seed() != 0, qlpb::Error::INVALID_ARGUMENT,
                                          "engine.mc.seed",
                                          "a Monte Carlo request needs an explicit seed");
                        QLS_FIELD_REQUIRE(mc.samples() > 0, qlpb::Error::INVALID_ARGUMENT,
                                          "engine.mc.samples",
                                          "a Monte Carlo request needs samples");
                        refuseUnreadVariates(mc, true, true, false);
                        return run(option,
                                   MakeMCPerformanceEngine<PseudoRandom>(graph.process)
                                       .withSamples(mc.samples())
                                       .withSeed(mc.seed())
                                       .withBrownianBridge(mc.brownian_bridge())
                                       .withAntitheticVariate(mc.antithetic_variate()),
                                   msg, nullptr, zeroed);
                    }

                    default:
                        break;
                }
                QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, "engine.method",
                               "cliquet options take METHOD_ANALYTIC, or METHOD_MONTE_CARLO for "
                               "the performance form");
            }

            // -- chooser ---------------------------------------------------
            case qlpb::Option::kChooser: {
                const std::string path = base + ".chooser";
                const auto& ch = opt.chooser();

                // Both chooser instruments derive from OneAssetOption and hand
                // it a PlainVanillaPayoff struck at the strike -- the call
                // strike, when the two sides differ -- together with the (call)
                // exercise (simplechooseroption.cpp:30,
                // complexchooseroption.cpp:34). So those live where every other
                // style puts them, and Chooser.call_strike and .call_expiry
                // re-declare them. Refused by name, as Compound.mother_payoff
                // is, rather than merged or ignored.
                QLS_FIELD_REQUIRE(ch.call_strike() == 0.0, qlpb::Error::UNSUPPORTED,
                                  path + ".call_strike",
                                  "the strike is the option's own payoff: set "
                                  "instrument.option.payoff.plain.strike rather than this");
                QLS_FIELD_REQUIRE(!ch.has_call_expiry(), qlpb::Error::UNSUPPORTED,
                                  path + ".call_expiry",
                                  "the expiry is the option's own exercise: set "
                                  "instrument.option.exercise rather than this");

                QLS_FIELD_REQUIRE(!graph.quanto, qlpb::Error::UNSUPPORTED, base + ".quanto",
                                  "there is no quanto chooser engine in QuantLib");
                QLS_FIELD_REQUIRE(eng.method() == qlpb::Engine_Method_METHOD_ANALYTIC,
                                  qlpb::Error::UNSUPPORTED, "engine.method",
                                  "chooser options take METHOD_ANALYTIC: QuantLib has one engine "
                                  "per chooser and both are closed forms");

                // Neither engine reads the exercise type. Both take
                // exercise->lastDate() and value a European option at it
                // (analyticsimplechooserengine.cpp:52,
                // analyticcomplexchooserengine.cpp:135), so an American
                // exercise would price as if it were European and the early
                // exercise would be dropped without a word.
                QLS_FIELD_REQUIRE(european, qlpb::Error::UNSUPPORTED, base + ".exercise.type",
                                  "the chooser engines are European only -- and neither checks: "
                                  "an American exercise would be priced as if it were European");

                // The instrument builds its own PlainVanillaPayoff and forces
                // the type to Call, so payoff.type says nothing here: which
                // side this becomes is what the holder chooses. Every other arm
                // requires it, so leaving it set would be the one place in the
                // schema where a field is read and discarded.
                QLS_FIELD_REQUIRE(opt.payoff().kind_case() == qlpb::Payoff::kPlain,
                                  qlpb::Error::UNSUPPORTED, base + ".payoff",
                                  "a chooser is struck on a plain payoff: the instrument builds "
                                  "the PlainVanillaPayoff itself and takes only a strike");
                QLS_FIELD_REQUIRE(opt.payoff().type() ==
                                      qlpb::Payoff_OptionType_OPTION_TYPE_UNSPECIFIED,
                                  qlpb::Error::INVALID_ARGUMENT, base + ".payoff.type",
                                  "a chooser has no option type until the choice date: that is "
                                  "the thing being chosen. Leave payoff.type unset");
                const Real strike = opt.payoff().plain().strike();
                QLS_FIELD_REQUIRE(strike > 0.0, qlpb::Error::INVALID_ARGUMENT,
                                  base + ".payoff.plain.strike", "strike must be positive");

                // One time axis. AnalyticSimpleChooserEngine requires the three
                // day counters to be equal and QL_REQUIREs if they are not
                // (analyticsimplechooserengine.cpp:36-42), which arrives as
                // CALCULATION_FAILED with nothing to blame. The complex engine
                // makes the same assumption and does not check: it takes every
                // time off the risk-free counter (blackscholesprocess.cpp:150)
                // and then reads the dividend curve and the vol surface at that
                // number, which is only the same instant if they agree.
                const DayCounter rfdc = graph.riskFree->dayCounter();
                QLS_FIELD_REQUIRE(graph.dividend->dayCounter() == rfdc,
                                  qlpb::Error::INVALID_ARGUMENT,
                                  base + ".underlyings[0].dividend_curve_id",
                                  "a chooser is priced on one time axis: the dividend curve counts "
                                  "days as " << graph.dividend->dayCounter().name()
                                             << " and the discount curve as " << rfdc.name());
                QLS_FIELD_REQUIRE(graph.volatility->dayCounter() == rfdc,
                                  qlpb::Error::INVALID_ARGUMENT,
                                  base + ".underlyings[0].volatility_id",
                                  "a chooser is priced on one time axis: the volatility counts "
                                  "days as " << graph.volatility->dayCounter().name()
                                             << " and the discount curve as " << rfdc.name());

                QLS_FIELD_REQUIRE(ch.has_choice_date(), qlpb::Error::INVALID_ARGUMENT,
                                  path + ".choice_date",
                                  "a chooser needs the date the choice is made");
                const Date choice = registry_.date(ch.choice_date(), path + ".choice_date");
                QLS_FIELD_REQUIRE(choice > evaluationDate_, qlpb::Error::INVALID_ARGUMENT,
                                  path + ".choice_date",
                                  "the choice date " << choice << " is not after the evaluation "
                                                     << "date " << evaluationDate_
                                                     << ": the choice has been made already, and "
                                                        "what is left is a vanilla option");
                QLS_FIELD_REQUIRE(choice < ex->lastDate(), qlpb::Error::INVALID_ARGUMENT,
                                  path + ".choice_date",
                                  "the choice date " << choice << " is not before the expiry "
                                                     << ex->lastDate());

                // Simple or complex is read off the put leg rather than
                // declared: SimpleChooserOption is the one that shares a strike
                // and an expiry between the two sides, and it takes exactly one
                // of each, so a put expiry is what there is no room for in it.
                if (!ch.has_put_expiry()) {
                    QLS_FIELD_REQUIRE(ch.put_strike() == 0.0, qlpb::Error::INVALID_ARGUMENT,
                                      path + ".put_expiry",
                                      "a put strike of its own needs a put expiry beside it: "
                                      "SimpleChooserOption shares one strike and one expiry "
                                      "between the two sides and takes no second pair");
                    return run(ext::make_shared<SimpleChooserOption>(choice, strike, ex),
                               ext::make_shared<AnalyticSimpleChooserEngine>(graph.process), msg);
                }

                QLS_FIELD_REQUIRE(ch.put_strike() > 0.0, qlpb::Error::INVALID_ARGUMENT,
                                  path + ".put_strike", "strike must be positive");
                const ext::shared_ptr<Exercise> putEx = ext::make_shared<EuropeanExercise>(
                    expiryDate(ch.put_expiry(), path + ".put_expiry"));
                QLS_FIELD_REQUIRE(choice < putEx->lastDate(), qlpb::Error::INVALID_ARGUMENT,
                                  path + ".choice_date",
                                  "the choice date " << choice << " is not before the put expiry "
                                                     << putEx->lastDate());

                // AnalyticComplexChooserEngine finds the critical spot with a
                // Black-Scholes calculator run to (maturity - 2 x choice time)
                // rather than (maturity - choice time)
                // (analyticcomplexchooserengine.cpp:91,99). A leg expiring
                // before twice the choice date leaves that negative, and the
                // volatility surface then throws "negative time" from inside
                // the Newton-Raphson: CALCULATION_FAILED, no field. The bound
                // is the engine's, not the product's.
                const Time tChoice = graph.riskFree->timeFromReference(choice);
                QLS_FIELD_REQUIRE(graph.riskFree->timeFromReference(ex->lastDate()) > 2.0 * tChoice,
                                  qlpb::Error::UNSUPPORTED, base + ".exercise.dates",
                                  "AnalyticComplexChooserEngine needs the expiry more than twice "
                                  "the choice time out, and " << ex->lastDate() << " is not: it "
                                  "prices the choice off (expiry - 2 x choice time)");
                QLS_FIELD_REQUIRE(graph.riskFree->timeFromReference(putEx->lastDate()) >
                                      2.0 * tChoice,
                                  qlpb::Error::UNSUPPORTED, path + ".put_expiry",
                                  "AnalyticComplexChooserEngine needs the put expiry more than "
                                  "twice the choice time out, and " << putEx->lastDate()
                                                                    << " is not");

                return run(ext::make_shared<ComplexChooserOption>(choice, strike, ch.put_strike(),
                                                                  ex, putEx),
                           ext::make_shared<AnalyticComplexChooserEngine>(graph.process), msg);
            }

            // -- digital ---------------------------------------------------
            case qlpb::Option::kDigital:
                // Not built, and not a gap in the build. QuantLib has no
                // digital-knock instrument: the product is a BarrierOption
                // carrying a binary payoff, which the barrier arm above now
                // prices. `message Digital`'s three fields re-declare
                // Barrier.type, Barrier.level and CashOrNothingPayoff.cash_payoff,
                // all of which this request could already carry, so the arm is
                // a tag to reserve rather than a style to implement -- and this
                // says where to send it instead of leaving the generic refusal
                // to imply there is an engine missing.
                QLS_FIELD_FAIL(qlpb::Error::UNSUPPORTED, base + ".digital",
                               "a knock digital is a barrier carrying a binary payoff, not a "
                               "style of its own: send instrument.option.barrier with a "
                               "cash_or_nothing or asset_or_nothing payoff on an American "
                               "exercise");

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
            const int points = byDate ? sample.dates_size() : sample.times_size();
            QLS_FIELD_REQUIRE(points <= kMaxCurveSamplePoints, qlpb::Error::INVALID_ARGUMENT,
                              path + (byDate ? ".dates" : ".times"),
                              "this sample asks for " << points << " points, over the limit of "
                                                      << kMaxCurveSamplePoints
                                                      << "; a chart shows hundreds");

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
        // Not (samples + batch - 1) / batch, which wraps to zero batches, and
        // an NPV of zero with no error, for a sample count near the top of
        // uint64. checkEngineLimits refuses those now; this stays exact anyway.
        const Size batches = mc.samples() / batch + (mc.samples() % batch != 0 ? 1 : 0);

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
        Real sumOfSquaredWeightedErrors = 0.0;

        for (Size i = 0; i < batches; ++i) {
            // Explicit Size: samples() is uint64 and Size is size_t, which are
            // distinct types where both are 64 bits, so std::min cannot deduce.
            const Size thisBatch = std::min(batch, static_cast<Size>(mc.samples()) - i * batch);

            // Derived, not random: replaying the same request reproduces the
            // same batch seeds. Never zero, which the engine reads as "seed
            // from the clock" and which the sum can wrap to from a seed near
            // the top of the range.
            BigNatural batchSeed = static_cast<BigNatural>(mc.seed() + i);
            if (batchSeed == 0)
                batchSeed = 1;

            option->setPricingEngine(
                MakeMCEuropeanEngine<PseudoRandom>(graph.process)
                    .withSteps(mc.time_steps_per_year() > 0 ? mc.time_steps_per_year() : 1)
                    .withSamples(thisBatch)
                    .withSeed(batchSeed));

            const Real batchNpv = option->NPV();
            sum += batchNpv * static_cast<Real>(thisBatch);

            try {
                // The mean below weights each batch by its paths, so its
                // error carries the same weight: the last batch is usually
                // short, and counting it as a full one would overstate it.
                const Real weighted = option->errorEstimate() * static_cast<Real>(thisBatch);
                sumOfSquaredWeightedErrors += weighted * weighted;
            } catch (const Error&) {
                // engine did not provide one; the combined estimate is dropped
            }

            const Size done = i * batch + thisBatch;
            if (progress && !progress(done, mc.samples(), sum / static_cast<Real>(done))) {
                // Cooperative stop between batches. Inside a batch nothing can
                // interrupt the engine, which is why a hard cancel still has
                // to kill the process (DESIGN §3).
                throw Cancelled("cancelled after " + std::to_string(done) + " of " +
                                std::to_string(mc.samples()) + " paths");
            }
        }

        PriceOutcome out;
        out.npv = sum / static_cast<Real>(mc.samples());
        if (sumOfSquaredWeightedErrors > 0.0) {
            // Independent batches, so the errors add in quadrature, each
            // scaled by its share of the paths.
            out.results["errorEstimate"] =
                std::sqrt(sumOfSquaredWeightedErrors) / static_cast<Real>(mc.samples());
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
