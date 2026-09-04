/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file capabilities.cpp
    \brief what this build prices, as data
*/

#include "session/capabilities.hpp"

#include <ql/version.hpp>
#include <quantlib/v2/envelope.pb.h>

namespace qlservice {

    namespace qlpb = quantlib::v2;

    void fillCapabilities(qlpb::Capabilities& out) {
        out.set_max_scenario_points(kMaxScenarioPoints);
        out.set_build("ql-backend");
        out.set_quantlib_version(QL_VERSION);

        // Two of the nine Instrument.kind arms are dispatched
        // (Session::priceOption, Session::priceSwap).
        out.add_instruments("option");
        out.add_instruments("swap");

        // Six of the twelve Option.style arms are built.
        for (const char* style :
             {"vanilla", "barrier", "double_barrier", "asian", "lookback", "forward_start"})
            out.add_option_styles(style);

        // Seven payoffs build. `floating` is valid on a lookback only, which is
        // a combination rule and therefore the client's to know.
        for (const char* payoff : {"plain", "percentage_strike", "asset_or_nothing",
                                   "cash_or_nothing", "gap", "super_fund", "super_share",
                                   "floating"})
            out.add_payoffs(payoff);

        for (const auto exercise : {qlpb::Exercise_Type_TYPE_EUROPEAN,
                                    qlpb::Exercise_Type_TYPE_AMERICAN,
                                    qlpb::Exercise_Type_TYPE_BERMUDAN})
            out.add_exercises(exercise);

        for (const auto process : {qlpb::Underlying_Process_PROCESS_BLACK_SCHOLES_MERTON,
                                   qlpb::Underlying_Process_PROCESS_BLACK_SCHOLES,
                                   qlpb::Underlying_Process_PROCESS_BLACK})
            out.add_processes(process);

        for (const auto method :
             {qlpb::Engine_Method_METHOD_ANALYTIC, qlpb::Engine_Method_METHOD_LATTICE,
              qlpb::Engine_Method_METHOD_FINITE_DIFFERENCE,
              qlpb::Engine_Method_METHOD_MONTE_CARLO, qlpb::Engine_Method_METHOD_INTEGRAL,
              qlpb::Engine_Method_METHOD_DISCOUNTING})
            out.add_engine_methods(method);

        // Seven trees compile for a vanilla. A barrier takes Cox-Ross-
        // Rubinstein only, which is again a combination rule.
        for (const auto tree : {qlpb::LatticeParameters_Tree_TREE_COX_ROSS_RUBINSTEIN,
                                qlpb::LatticeParameters_Tree_TREE_JARROW_RUDD,
                                qlpb::LatticeParameters_Tree_TREE_ADDITIVE_EQUIPROBABILITIES,
                                qlpb::LatticeParameters_Tree_TREE_TRIGEORGIS,
                                qlpb::LatticeParameters_Tree_TREE_TIAN,
                                qlpb::LatticeParameters_Tree_TREE_LEISEN_REIMER,
                                qlpb::LatticeParameters_Tree_TREE_JOSHI4})
            out.add_lattice_trees(tree);

        for (const auto approximation :
             {qlpb::AnalyticParameters_Approximation_APPROXIMATION_BARONE_ADESI_WHALEY,
              qlpb::AnalyticParameters_Approximation_APPROXIMATION_BJERKSUND_STENSLAND,
              qlpb::AnalyticParameters_Approximation_APPROXIMATION_JU_QUADRATIC})
            out.add_approximations(approximation);

        for (const auto preset : {qlpb::FdParameters_Preset_PRESET_COARSE,
                                  qlpb::FdParameters_Preset_PRESET_STANDARD,
                                  qlpb::FdParameters_Preset_PRESET_FINE})
            out.add_fd_presets(preset);

        // The sixteen kinds Worker::fillResult and Session::priceSwap map.
        for (const auto kind :
             {qlpb::RESULT_KIND_NPV, qlpb::RESULT_KIND_DELTA, qlpb::RESULT_KIND_GAMMA,
              qlpb::RESULT_KIND_THETA, qlpb::RESULT_KIND_VEGA, qlpb::RESULT_KIND_RHO,
              qlpb::RESULT_KIND_DIVIDEND_RHO, qlpb::RESULT_KIND_THETA_PER_DAY,
              qlpb::RESULT_KIND_DELTA_FORWARD, qlpb::RESULT_KIND_ELASTICITY,
              qlpb::RESULT_KIND_STRIKE_SENSITIVITY, qlpb::RESULT_KIND_ITM_CASH_PROBABILITY,
              qlpb::RESULT_KIND_IMPLIED_VOLATILITY, qlpb::RESULT_KIND_QRHO, qlpb::RESULT_KIND_QVEGA, qlpb::RESULT_KIND_QLAMBDA,
              qlpb::RESULT_KIND_FAIR_RATE, qlpb::RESULT_KIND_LEG_NPV,
              qlpb::RESULT_KIND_LEG_BPS})
            out.add_result_kinds(kind);

        // Five of the eight MarketObject.kind arms are built.
        for (const char* kind : {"quote", "yield_curve", "volatility", "index", "fixings"})
            out.add_market_kinds(kind);

        // Four of the six curve shapes; zero and discount take fixed nodes only,
        // which is a rule about the nodes rather than about the shape.
        for (const char* shape : {"flat", "zero", "discount", "bootstrap"})
            out.add_yield_curve_shapes(shape);

        for (const char* shape : {"constant", "variance_curve", "variance_surface"})
            out.add_volatility_shapes(shape);

        for (const auto family : {qlpb::Index_Family_FAMILY_IBOR, qlpb::Index_Family_FAMILY_OVERNIGHT})
            out.add_index_families(family);

        for (const auto kind : {qlpb::Pillar_Kind_KIND_DEPOSIT, qlpb::Pillar_Kind_KIND_SWAP,
                                qlpb::Pillar_Kind_KIND_OIS})
            out.add_pillar_kinds(kind);

        // Three traits by three interpolators: nine compiled types, and the
        // menu is the schema (DESIGN §6.1).
        for (const auto traits : {qlpb::BootstrappedCurve_Traits_TRAITS_DISCOUNT,
                                  qlpb::BootstrappedCurve_Traits_TRAITS_ZERO_YIELD,
                                  qlpb::BootstrappedCurve_Traits_TRAITS_FORWARD_RATE})
            out.add_bootstrap_traits(traits);

        for (const auto interpolator :
             {qlpb::INTERPOLATOR_LINEAR, qlpb::INTERPOLATOR_LOG_LINEAR, qlpb::INTERPOLATOR_CUBIC})
            out.add_bootstrap_interpolators(interpolator);

        for (const auto kind : {qlpb::Leg_Kind_KIND_FIXED, qlpb::Leg_Kind_KIND_IBOR})
            out.add_leg_kinds(kind);

        // The ClientFrame arms Worker::serve dispatches. Listed because a
        // batch is a frame rather than a PriceRequest option, and a client
        // should not have to send one to find out whether it is served.
        for (const char* frame : {"open_session", "close_session", "update_market", "price",
                                  "cancel", "hello", "batch"})
            out.add_frames(frame);

        out.add_price_request_options("include_additional_results");
        out.add_price_request_options("curve_samples");
        out.add_price_request_options("include_cashflows");
    }

}
