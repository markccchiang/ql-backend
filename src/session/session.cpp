/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "session.hpp"
#include "errors/fielderror.hpp"
#include "updateguard.hpp"
#include <ql/exercise.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/linearinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/patterns/lazyobject.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/pricingengines/vanilla/mceuropeanengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/bootstraptraits.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <chrono>
#include <cmath>
#include <utility>

using namespace QuantLib;
namespace qlpb = quantlib::v1;

namespace qlservice {

    namespace {

        double seconds(std::chrono::steady_clock::time_point from) {
            const auto elapsed = std::chrono::steady_clock::now() - from;
            return std::chrono::duration<double>(elapsed).count();
        }

        template <class Traits, class Interpolator>
        Handle<YieldTermStructure> piecewise(const Date& reference,
                                             std::vector<ext::shared_ptr<RateHelper>> helpers,
                                             const DayCounter& dayCounter) {
            return Handle<YieldTermStructure>(
                ext::make_shared<PiecewiseYieldCurve<Traits, Interpolator>>(
                    reference, std::move(helpers), dayCounter));
        }

        DateGeneration::Rule dateGeneration(qlpb::SwapLeg::DateGeneration rule,
                                            const std::string& fieldPath) {
            switch (rule) {
                case qlpb::SwapLeg::BACKWARD:
                    return DateGeneration::Backward;
                case qlpb::SwapLeg::FORWARD:
                    return DateGeneration::Forward;
                case qlpb::SwapLeg::ZERO:
                    return DateGeneration::Zero;
                case qlpb::SwapLeg::THIRD_WEDNESDAY:
                    return DateGeneration::ThirdWednesday;
                default:
                    break;
            }
            QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath,
                           "unspecified date generation rule at '" << fieldPath << "'");
        }

    }


    Session::Session(const qlpb::OpenSession& msg)
    : registry_([this](const std::string& curveId) { return curve(curveId); }) {

        evaluationDate_ = registry_.date(msg.evaluation_date(), "evaluation_date");

        // Thread-local under QL_ENABLE_SESSIONS, so this sets the date for
        // this session alone. In a default build it would set it for every
        // session in the process (ql/patterns/singleton.hpp:104).
        Settings::instance().evaluationDate() = evaluationDate_;

        for (const auto& q : msg.quotes()) {
            QLS_FIELD_REQUIRE(quotes_.find(q.quote_id()) == quotes_.end(),
                              qlpb::Error::INVALID_ARGUMENT, "quotes",
                              "duplicate quote id '" << q.quote_id() << "'");
            quotes_[q.quote_id()] = ext::make_shared<SimpleQuote>(q.value());
        }

        buildCurves(msg);
    }


    Handle<YieldTermStructure> Session::curve(const std::string& curveId) const {
        auto it = curves_.find(curveId);
        QL_REQUIRE(it != curves_.end(), "unknown curve '" << curveId << "'");
        return it->second;
    }


    // -----------------------------------------------------------------------
    // Bootstrapping
    // -----------------------------------------------------------------------

    void Session::buildCurves(const qlpb::OpenSession& msg) {
        for (int c = 0; c < msg.curves_size(); ++c) {
            const auto& def = msg.curves(c);
            const std::string path = "curves[" + std::to_string(c) + "]";

            QLS_FIELD_REQUIRE(!def.curve_id().empty(), qlpb::Error::INVALID_ARGUMENT,
                              path + ".curve_id", "curve at '" << path << "' has no id");
            QLS_FIELD_REQUIRE(curves_.find(def.curve_id()) == curves_.end(),
                              qlpb::Error::INVALID_ARGUMENT, path + ".curve_id",
                              "duplicate curve id '" << def.curve_id() << "'");
            QLS_FIELD_REQUIRE(def.pillars_size() > 0, qlpb::Error::INVALID_ARGUMENT,
                              path + ".pillars", "curve '" << def.curve_id() << "' has no pillars");

            std::vector<ext::shared_ptr<RateHelper>> helpers;
            helpers.reserve(def.pillars_size());
            for (int p = 0; p < def.pillars_size(); ++p)
                helpers.push_back(
                    buildHelper(def.pillars(p), def, path + ".pillars[" + std::to_string(p) + "]"));

            // Curves go in as they are built, so a later curve's index can
            // forward off an earlier one. A forward reference fails in the
            // resolver rather than producing an empty handle.
            curves_[def.curve_id()] = makeCurve(def, std::move(helpers), path);
        }
    }


    ext::shared_ptr<RateHelper> Session::buildHelper(const qlpb::CurvePillar& pillar,
                                                     const qlpb::CurveDefinition& def,
                                                     const std::string& fieldPath) {
        auto quote = quotes_.find(pillar.quote_id());
        QLS_FIELD_REQUIRE(quote != quotes_.end(), qlpb::Error::INVALID_ARGUMENT,
                          fieldPath + ".quote_id",
                          "pillar at '" << fieldPath << "' refers to unknown quote '"
                                        << pillar.quote_id() << "'");

        // The helper takes the handle, not the value: a quote write then moves
        // the curve, every instrument discounting off it, and nothing else.
        // That propagation is the whole reason the session is stateful.
        const Handle<Quote> rate(quote->second);

        // An index with no forwarding curve is the single-curve case: the
        // bootstrap hands the helper the term structure it is building. A
        // forwarding_curve_id is only for a genuinely exogenous forecast
        // curve, and resolves against curves built earlier in this session.
        const auto index = registry_.index(pillar.index(), fieldPath + ".index");

        switch (pillar.kind()) {

            case qlpb::CurvePillar::DEPOSIT:
                // The index carries the deposit's own tenor and conventions.
                return ext::make_shared<DepositRateHelper>(rate, index);

            case qlpb::CurvePillar::SWAP: {
                QLS_FIELD_REQUIRE(
                    pillar.has_fixed_leg(), qlpb::Error::INVALID_ARGUMENT, fieldPath + ".fixed_leg",
                    "swap pillar at '" << fieldPath << "' needs fixed-leg conventions");
                const auto& fixed = pillar.fixed_leg();
                const std::string fixedPath = fieldPath + ".fixed_leg";

                return ext::make_shared<SwapRateHelper>(
                    rate, registry_.period(pillar.tenor(), fieldPath + ".tenor"),
                    registry_.calendar(fixed.calendar(), fixedPath + ".calendar"),
                    registry_.frequency(fixed.frequency(), fixedPath + ".frequency"),
                    registry_.businessDayConvention(fixed.convention(), fixedPath + ".convention"),
                    registry_.dayCounter(fixed.day_counter(), fixedPath + ".day_counter"), index);
            }

            case qlpb::CurvePillar::OIS: {
                QLS_FIELD_REQUIRE(
                    pillar.has_fixed_leg(), qlpb::Error::INVALID_ARGUMENT, fieldPath + ".fixed_leg",
                    "OIS pillar at '" << fieldPath << "' needs fixed-leg conventions");

                // The registry hands back an IborIndex; an OIS helper needs the
                // overnight subtype. Sending SOFR here and EURIBOR there is an
                // easy client mistake and the cast is what catches it.
                const auto overnight = ext::dynamic_pointer_cast<OvernightIndex>(index);
                QLS_FIELD_REQUIRE(
                    overnight != nullptr, qlpb::Error::INVALID_ARGUMENT, fieldPath + ".index",
                    "OIS pillar at '" << fieldPath << "' needs an overnight index, got '"
                                      << index->name() << "'");

                return ext::make_shared<OISRateHelper>(
                    def.settlement_days(), registry_.period(pillar.tenor(), fieldPath + ".tenor"),
                    rate, overnight);
            }

            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::UNSPECIFIED_ENUM, fieldPath + ".kind",
                       "unspecified pillar kind at '" << fieldPath << ".kind'");
    }


    Handle<YieldTermStructure> Session::makeCurve(const qlpb::CurveDefinition& def,
                                                  std::vector<ext::shared_ptr<RateHelper>> helpers,
                                                  const std::string& fieldPath) {
        const auto dc = registry_.dayCounter(def.day_counter(), fieldPath + ".day_counter");
        const auto& boot = def.bootstrap();

        QLS_FIELD_REQUIRE(boot.traits() != qlpb::CurveBootstrap::TRAITS_UNSPECIFIED,
                          qlpb::Error::UNSPECIFIED_ENUM, fieldPath + ".bootstrap.traits",
                          "unspecified bootstrap traits at '" << fieldPath << ".bootstrap.traits'");
        QLS_FIELD_REQUIRE(boot.interpolator() != qlpb::CurveBootstrap::INTERPOLATOR_UNSPECIFIED,
                          qlpb::Error::UNSPECIFIED_ENUM, fieldPath + ".bootstrap.interpolator",
                          "unspecified interpolator at '" << fieldPath
                                                          << ".bootstrap.interpolator'");

        // Each pair below is a distinct C++ type, so this table is the schema:
        // a combination absent here cannot be requested over the wire, and
        // adding one means adding a line of source and recompiling. That is
        // the price of PiecewiseYieldCurve being a template, and the reason
        // the enum is deliberately small.
        switch (boot.traits()) {
            case qlpb::CurveBootstrap::DISCOUNT:
                switch (boot.interpolator()) {
                    case qlpb::CurveBootstrap::LINEAR:
                        return piecewise<Discount, Linear>(evaluationDate_, std::move(helpers), dc);
                    case qlpb::CurveBootstrap::LOG_LINEAR:
                        return piecewise<Discount, LogLinear>(evaluationDate_, std::move(helpers),
                                                              dc);
                    case qlpb::CurveBootstrap::CUBIC:
                        return piecewise<Discount, Cubic>(evaluationDate_, std::move(helpers), dc);
                    default:
                        break;
                }
                break;

            case qlpb::CurveBootstrap::ZERO_YIELD:
                switch (boot.interpolator()) {
                    case qlpb::CurveBootstrap::LINEAR:
                        return piecewise<ZeroYield, Linear>(evaluationDate_, std::move(helpers),
                                                            dc);
                    case qlpb::CurveBootstrap::LOG_LINEAR:
                        return piecewise<ZeroYield, LogLinear>(evaluationDate_, std::move(helpers),
                                                               dc);
                    case qlpb::CurveBootstrap::CUBIC:
                        return piecewise<ZeroYield, Cubic>(evaluationDate_, std::move(helpers), dc);
                    default:
                        break;
                }
                break;

            case qlpb::CurveBootstrap::FORWARD_RATE:
                switch (boot.interpolator()) {
                    case qlpb::CurveBootstrap::LINEAR:
                        return piecewise<ForwardRate, Linear>(evaluationDate_, std::move(helpers),
                                                              dc);
                    case qlpb::CurveBootstrap::LOG_LINEAR:
                        return piecewise<ForwardRate, LogLinear>(evaluationDate_,
                                                                 std::move(helpers), dc);
                    case qlpb::CurveBootstrap::CUBIC:
                        return piecewise<ForwardRate, Cubic>(evaluationDate_, std::move(helpers),
                                                             dc);
                    default:
                        break;
                }
                break;

            default:
                break;
        }
        QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, fieldPath + ".bootstrap",
                       "unsupported traits/interpolator pair at '" << fieldPath << ".bootstrap'");
    }


    // -----------------------------------------------------------------------
    // Market updates
    // -----------------------------------------------------------------------

    void Session::apply(const qlpb::UpdateMarket& msg) {
        QL_REQUIRE(!dirty_, "session is dirty and must be replayed, not updated");
        QLS_FIELD_REQUIRE(msg.updates_size() > 0, qlpb::Error::INVALID_ARGUMENT, "updates",
                          "empty market update");

        // One guard around the whole batch: N quote writes, one recalculation.
        UpdateGuard guard;
        try {
            for (const auto& u : msg.updates()) {
                auto it = quotes_.find(u.quote_id());
                QLS_FIELD_REQUIRE(it != quotes_.end(), qlpb::Error::INVALID_ARGUMENT, "updates",
                                  "unknown quote '" << u.quote_id() << "'");
                it->second->setValue(u.value());
            }
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
            for (auto& [id, handle] : curves_) {
                if (auto lazy = ext::dynamic_pointer_cast<LazyObject>(handle.currentLink()))
                    lazy->recalculate();
            }
        }
    }


    // -----------------------------------------------------------------------
    // Pricing
    // -----------------------------------------------------------------------

    Session::PriceOutcome Session::price(const qlpb::PriceRequest& msg,
                                         const ProgressSink& progress) {
        QL_REQUIRE(!dirty_, "session is dirty and must be replayed before pricing");

        switch (msg.instrument().kind_case()) {
            case qlpb::Instrument::kVanillaOption:
                return priceOption(msg, progress);
            case qlpb::Instrument::kVanillaSwap:
                return priceSwap(msg);
            case qlpb::Instrument::KIND_NOT_SET:
                QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, "instrument",
                               "no instrument set at 'instrument'");
        }
        QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, "instrument",
                       "unhandled instrument at 'instrument'");
    }


    Schedule Session::schedule(const qlpb::SwapLeg& leg,
                               const Date& start,
                               const Date& maturity,
                               const std::string& fieldPath) {
        const auto calendar = registry_.calendar(leg.calendar(), fieldPath + ".calendar");
        const auto convention = registry_.businessDayConvention(
            leg.business_day_convention(), fieldPath + ".business_day_convention");
        const auto frequency = registry_.frequency(leg.frequency(), fieldPath + ".frequency");
        const auto rule = dateGeneration(leg.date_generation(), fieldPath + ".date_generation");

        // end_of_month is business-day aware in QuantLib, not raw month end
        // (AGENTS.md §5.4); it is passed through rather than interpreted here.
        return Schedule(start, maturity, Period(frequency), calendar, convention, convention, rule,
                        leg.end_of_month());
    }


    Session::PriceOutcome Session::priceSwap(const qlpb::PriceRequest& msg) {
        const auto& swapMsg = msg.instrument().vanilla_swap();

        QLS_FIELD_REQUIRE(swapMsg.type() != qlpb::VanillaSwap::SWAP_TYPE_UNSPECIFIED,
                          qlpb::Error::UNSPECIFIED_ENUM, "instrument.vanilla_swap.type",
                          "unspecified swap type at 'instrument.vanilla_swap.type'; every result "
                          "changes sign with it");
        QLS_FIELD_REQUIRE(
            swapMsg.notional() > 0.0, qlpb::Error::INVALID_ARGUMENT,
            "instrument.vanilla_swap.notional",
            "notional must be positive; direction is carried by 'type', not by its sign");

        const Date start = registry_.date(swapMsg.start(), "instrument.vanilla_swap.start");
        const Date maturity =
            registry_.date(swapMsg.maturity(), "instrument.vanilla_swap.maturity");
        QLS_FIELD_REQUIRE(maturity > start, qlpb::Error::INVALID_ARGUMENT,
                          "instrument.vanilla_swap.maturity",
                          "maturity " << maturity << " is not after start " << start);

        const auto discount = curve(swapMsg.discount_curve_id());

        const auto& fixedLeg = swapMsg.fixed_leg();
        const auto& floatLeg = swapMsg.floating_leg();

        // The floating index carries its own forwarding curve handle, which
        // may differ from the discount curve. Both are live handles, so a
        // quote write on either curve reprices this swap and nothing else.
        const auto index =
            registry_.index(floatLeg.index(), "instrument.vanilla_swap.floating_leg.index");
        QLS_FIELD_REQUIRE(!index->forwardingTermStructure().empty(), qlpb::Error::INVALID_ARGUMENT,
                          "instrument.vanilla_swap.floating_leg.index",
                          "the floating leg index needs a forwarding curve: pricing a swap off an "
                          "index with an empty handle fails at the first forecast");

        auto swap = ext::make_shared<VanillaSwap>(
            swapMsg.type() == qlpb::VanillaSwap::PAYER ? Swap::Payer : Swap::Receiver,
            swapMsg.notional(),
            schedule(fixedLeg, start, maturity, "instrument.vanilla_swap.fixed_leg"),
            swapMsg.fixed_rate(),
            registry_.dayCounter(fixedLeg.day_counter(),
                                 "instrument.vanilla_swap.fixed_leg.day_counter"),
            schedule(floatLeg, start, maturity, "instrument.vanilla_swap.floating_leg"), index,
            swapMsg.floating_spread(),
            registry_.dayCounter(floatLeg.day_counter(),
                                 "instrument.vanilla_swap.floating_leg.day_counter"));

        swap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(discount));

        const auto t0 = std::chrono::steady_clock::now();

        PriceOutcome out;
        out.npv = swap->NPV();

        for (const auto kind : msg.results()) {
            try {
                switch (kind) {
                    case qlpb::RESULT_KIND_FAIR_RATE:
                        out.results["fairRate"] = swap->fairRate();
                        break;
                    case qlpb::RESULT_KIND_FAIR_SPREAD:
                        out.results["fairSpread"] = swap->fairSpread();
                        break;
                    case qlpb::RESULT_KIND_FIXED_LEG_BPS:
                        out.results["fixedLegBPS"] = swap->legBPS(0);
                        break;
                    case qlpb::RESULT_KIND_FLOATING_LEG_BPS:
                        out.results["floatingLegBPS"] = swap->legBPS(1);
                        break;
                    default:
                        // Option greeks asked of a swap: not an error, just absent.
                        break;
                }
            } catch (const Error&) {
                // not provided by this engine
            }
        }

        out.calculationSeconds = seconds(t0);
        return out;
    }


    Session::PriceOutcome Session::priceOption(const qlpb::PriceRequest& msg,
                                               const ProgressSink& progress) {
        const auto& opt = msg.instrument().vanilla_option();
        const auto& engineMsg = msg.engine();

        QLS_FIELD_REQUIRE(opt.type() != qlpb::VanillaOption::OPTION_TYPE_UNSPECIFIED,
                          qlpb::Error::UNSPECIFIED_ENUM, "instrument.vanilla_option.type",
                          "unspecified option type at 'instrument.vanilla_option.type'");

        auto spot = quotes_.find(opt.spot_quote_id());
        QLS_FIELD_REQUIRE(spot != quotes_.end(), qlpb::Error::INVALID_ARGUMENT,
                          "instrument.vanilla_option.spot_quote_id",
                          "unknown spot quote '" << opt.spot_quote_id() << "'");
        auto vol = quotes_.find(opt.vol_quote_id());
        QLS_FIELD_REQUIRE(vol != quotes_.end(), qlpb::Error::INVALID_ARGUMENT,
                          "instrument.vanilla_option.vol_quote_id",
                          "unknown vol quote '" << opt.vol_quote_id() << "'");
        const auto discount = curve(opt.discount_curve_id());

        const Date expiry = registry_.date(opt.expiry(), "instrument.vanilla_option.expiry");
        QLS_FIELD_REQUIRE(expiry > evaluationDate_, qlpb::Error::INVALID_ARGUMENT,
                          "instrument.vanilla_option.expiry",
                          "expiry " << expiry << " is not after the evaluation date "
                                    << evaluationDate_);

        const auto payoff = ext::make_shared<PlainVanillaPayoff>(
            opt.type() == qlpb::VanillaOption::CALL ? Option::Call : Option::Put, opt.strike());
        const auto exercise = ext::make_shared<EuropeanExercise>(expiry);
        auto instrument = ext::make_shared<VanillaOption>(payoff, exercise);

        // Both market inputs enter as handles, so moving either quote
        // invalidates this instrument and the next price recomputes. Passing
        // spot->second->value() here instead would give a graph that never
        // reacts to the UI (AGENTS.md §5.2).
        const auto volTS = ext::make_shared<BlackConstantVol>(
            evaluationDate_, NullCalendar(), Handle<Quote>(vol->second), Actual365Fixed());
        const auto process = ext::make_shared<BlackScholesMertonProcess>(
            Handle<Quote>(spot->second), discount, discount, Handle<BlackVolTermStructure>(volTS));

        if (engineMsg.kind() == qlpb::Engine::MONTE_CARLO && msg.progress_every_paths() > 0)
            return priceInBatches(msg, instrument, process, progress);

        switch (engineMsg.kind()) {
            case qlpb::Engine::ANALYTIC:
                instrument->setPricingEngine(ext::make_shared<AnalyticEuropeanEngine>(process));
                break;
            case qlpb::Engine::MONTE_CARLO: {
                QLS_FIELD_REQUIRE(
                    engineMsg.seed() != 0, qlpb::Error::INVALID_ARGUMENT, "engine.seed",
                    "a Monte Carlo request needs an explicit seed: QuantLib seeds from the "
                    "clock by default (ql/math/randomnumbers/seedgenerator.cpp:36) and the "
                    "same inputs would price differently on every request");
                QLS_FIELD_REQUIRE(engineMsg.samples() > 0, qlpb::Error::INVALID_ARGUMENT,
                                  "engine.samples", "a Monte Carlo request needs samples");
                instrument->setPricingEngine(
                    MakeMCEuropeanEngine<PseudoRandom>(process)
                        .withSteps(engineMsg.time_steps() > 0 ? engineMsg.time_steps() : 1)
                        .withSamples(engineMsg.samples())
                        .withSeed(engineMsg.seed()));
                break;
            }
            default:
                QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, "engine.kind",
                               "engine kind " << engineMsg.kind()
                                              << " is not wired up for options");
        }

        const auto start = std::chrono::steady_clock::now();

        PriceOutcome out;
        out.npv = instrument->NPV();

        for (const auto kind : msg.results()) {
            // Results are fetched by name and QuantLib throws when the engine
            // did not produce one (ql/instrument.hpp:193). A missing greek is
            // reported as an absent map entry, not as a failed request: the
            // analytic engine has delta, the MC one does not, and the client
            // asks the same question of both.
            try {
                switch (kind) {
                    case qlpb::RESULT_KIND_DELTA:
                        out.results["delta"] = instrument->delta();
                        break;
                    case qlpb::RESULT_KIND_GAMMA:
                        out.results["gamma"] = instrument->gamma();
                        break;
                    case qlpb::RESULT_KIND_VEGA:
                        out.results["vega"] = instrument->vega();
                        break;
                    case qlpb::RESULT_KIND_THETA:
                        out.results["theta"] = instrument->theta();
                        break;
                    case qlpb::RESULT_KIND_ERROR_ESTIMATE:
                        out.results["errorEstimate"] = instrument->errorEstimate();
                        break;
                    default:
                        break;
                }
            } catch (const Error&) {
                // not provided by this engine
            }
        }

        out.calculationSeconds = seconds(start);
        return out;
    }


    Session::PriceOutcome
    Session::priceInBatches(const qlpb::PriceRequest& msg,
                            const ext::shared_ptr<VanillaOption>& option,
                            const ext::shared_ptr<GeneralizedBlackScholesProcess>& process,
                            const ProgressSink& progress) {
        const auto& engineMsg = msg.engine();
        QLS_FIELD_REQUIRE(engineMsg.seed() != 0, qlpb::Error::INVALID_ARGUMENT, "engine.seed",
                          "a Monte Carlo request needs an explicit seed");
        QLS_FIELD_REQUIRE(engineMsg.samples() > 0, qlpb::Error::INVALID_ARGUMENT, "engine.samples",
                          "a Monte Carlo request needs samples");

        const Size batch = msg.progress_every_paths();
        const Size batches = (engineMsg.samples() + batch - 1) / batch;

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
            const Size thisBatch =
                std::min(batch, static_cast<Size>(engineMsg.samples()) - i * batch);

            // Derived, not random: replaying the same request reproduces the
            // same batch seeds.
            const BigNatural batchSeed = static_cast<BigNatural>(engineMsg.seed() + i);

            option->setPricingEngine(
                MakeMCEuropeanEngine<PseudoRandom>(process)
                    .withSteps(engineMsg.time_steps() > 0 ? engineMsg.time_steps() : 1)
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
            if (progress && !progress(done, engineMsg.samples(), sum / static_cast<Real>(done))) {
                // Cooperative stop between batches. Inside a batch nothing can
                // interrupt the engine, which is why a hard cancel still has
                // to kill the process (DESIGN §3).
                QL_FAIL("cancelled after " << done << " of " << engineMsg.samples() << " paths");
            }
        }

        PriceOutcome out;
        out.npv = sum / static_cast<Real>(engineMsg.samples());
        if (sumOfSquaredErrors > 0.0) {
            // Independent batches, so the errors add in quadrature.
            out.results["errorEstimate"] =
                std::sqrt(sumOfSquaredErrors) / static_cast<Real>(batches);
        }
        out.calculationSeconds = seconds(start);
        return out;
    }

}
