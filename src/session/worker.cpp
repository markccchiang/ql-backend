/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "worker.hpp"
#include <limits>
#include <vector>
#include "errors/fielderror.hpp"
#include "session.hpp"
#include <ql/errors.hpp>
#include <chrono>
#include <utility>

using namespace QuantLib;
namespace qlpb = quantlib::v2;

namespace qlservice {

    namespace {

        //! Converts a QuantLib number for the wire.
        /*! `Real` is `QL_REAL` and only `double` by default, so every number
            crossing into a protobuf message goes through here rather than
            being assigned directly (DESIGN §6).

            The overload pair is the mechanism. In a stock build `Real` is
            `double`, the non-template is an exact match, and this costs
            nothing. Against an AD type only the template is viable and it
            resolves `value(x)` by argument-dependent lookup, which is how such
            a type exposes its underlying double — the convention QuantLib's
            own test suite relies on (`test-suite/utilities.hpp:53-58`). That
            helper lives in the test suite and is not installed with the
            library, so it cannot simply be called from here.
        */
        double wire(double x) {
            return x;
        }

        template <class T>
        double wire(const T& x) {
            return value(x);
        }

        //! The map key a ResultKind is reported under.
        /*! The same names QuantLib's engines use in additionalResults, so a
            client asking for RESULT_KIND_DELTA and a client dumping the
            engine's own map read the same key (results.proto).
        */
        std::string resultName(qlpb::ResultKind kind) {
            switch (kind) {
                case qlpb::RESULT_KIND_NPV:
                    return "npv";
                case qlpb::RESULT_KIND_DELTA:
                    return "delta";
                case qlpb::RESULT_KIND_GAMMA:
                    return "gamma";
                case qlpb::RESULT_KIND_THETA:
                    return "theta";
                case qlpb::RESULT_KIND_VEGA:
                    return "vega";
                case qlpb::RESULT_KIND_RHO:
                    return "rho";
                case qlpb::RESULT_KIND_DIVIDEND_RHO:
                    return "dividendRho";
                case qlpb::RESULT_KIND_THETA_PER_DAY:
                    return "thetaPerDay";
                case qlpb::RESULT_KIND_DELTA_FORWARD:
                    return "deltaForward";
                case qlpb::RESULT_KIND_ELASTICITY:
                    return "elasticity";
                case qlpb::RESULT_KIND_STRIKE_SENSITIVITY:
                    return "strikeSensitivity";
                case qlpb::RESULT_KIND_ITM_CASH_PROBABILITY:
                    return "itmCashProbability";
                case qlpb::RESULT_KIND_QRHO:
                    return "qrho";
                case qlpb::RESULT_KIND_QVEGA:
                    return "qvega";
                case qlpb::RESULT_KIND_QLAMBDA:
                    return "qlambda";
                case qlpb::RESULT_KIND_FAIR_RATE:
                    return "fairRate";
                default:
                    break;
            }
            // Per-leg results (legNPV.0, legNPV.1 ...) and the kinds no path
            // computes have no single key to plot; the caller rejects them.
            return "";
        }

    }


    Worker::Worker(std::string sessionId, FrameSink sink)
    : sessionId_(std::move(sessionId)), sink_(std::move(sink)) {
        QL_REQUIRE(sink_, "a frame sink is required");
    }


    Worker::~Worker() {
        shutdown();
    }


    void Worker::start() {
        QL_REQUIRE(!thread_.joinable(), "worker already started");
        thread_ = std::thread([this] { run(); });
    }


    bool Worker::submit(const qlpb::ClientFrame& frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_)
                return false;
            queue_.push_back(frame);
        }
        cv_.notify_one();
        return true;
    }


    void Worker::shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shuttingDown_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }


    void Worker::run() {
        // Everything QuantLib touches lives on this thread from here on: the
        // session, its registry, and every thread_local singleton they use.
        for (;;) {
            qlpb::ClientFrame frame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return shuttingDown_ || !queue_.empty(); });
                if (shuttingDown_ && queue_.empty())
                    break;
                frame = std::move(queue_.front());
                queue_.pop_front();
            }

            stopRequested_.store(false, std::memory_order_relaxed);
            serve(frame);
        }

        // Destroyed on the owning thread, so the thread_local singletons it
        // registered with are still alive.
        session_.reset();
    }


    void Worker::requireSessionOpen() const {
        QLS_FIELD_REQUIRE(session_ != nullptr, qlpb::Error::SESSION_NOT_FOUND, "session_id",
                          "no session is open on this worker");
    }


    void Worker::fillResult(qlpb::PriceResult& result,
                            const qlpb::PriceRequest& request,
                            const Session::PriceOutcome& outcome) const {
        result.set_npv(wire(outcome.npv));
        result.set_calculation_seconds(outcome.calculationSeconds);

        // The engine as it actually ran, echoed rather than assumed. A
        // batched Monte Carlo runs independent batches with derived seeds and
        // averages them, which partitions the RNG stream differently from one
        // run of the same total, so two requests differing only in
        // progress_every_paths return different numbers (DESIGN §4). An FD
        // price depends on its grid the same way. A client comparing two
        // prices has to be able to see which is which from the results alone,
        // and copying the whole Engine message is the form of that promise
        // that cannot fall behind the schema.
        *result.mutable_engine() = request.engine();

        for (const auto& entry : outcome.results) {
            // Scalars only: everything this layer computes is a number. The
            // Value variant carries the vectors and matrices an engine can
            // publish, and nothing here produces one yet.
            (*result.mutable_results())[entry.first].set_scalar(wire(entry.second));
        }

        auto err = outcome.results.find("errorEstimate");
        if (err != outcome.results.end()) {
            auto* estimate = result.mutable_error_estimate();
            estimate->set_standard_error(wire(err->second));
            estimate->set_samples(request.engine().mc().samples());
        }
    }


    void Worker::serveScenario(const qlpb::ClientFrame& frame,
                               const Session::ProgressSink& progress) {
        const auto& scenario = frame.price().scenario();
        const std::string path = "scenario";

        QLS_FIELD_REQUIRE(!scenario.quote_id().empty(), qlpb::Error::INVALID_ARGUMENT,
                          path + ".quote_id", "a scenario sweeps one named quote");

        const double original = session_->quoteValue(scenario.quote_id(), path + ".quote_id");

        std::vector<double> points;
        switch (scenario.points_case()) {
            case qlpb::Scenario::kExplicit:
                points.assign(scenario.explicit_().values().begin(),
                              scenario.explicit_().values().end());
                break;
            case qlpb::Scenario::kLinear: {
                const auto& lin = scenario.linear();
                QLS_FIELD_REQUIRE(lin.steps() >= 2, qlpb::Error::INVALID_ARGUMENT,
                                  path + ".linear.steps",
                                  "a linear sweep needs at least two steps");
                for (std::uint32_t i = 0; i < lin.steps(); ++i)
                    points.push_back(lin.begin() + (lin.end() - lin.begin()) * i /
                                                      static_cast<double>(lin.steps() - 1));
                break;
            }
            case qlpb::Scenario::kRelative:
                for (double f : scenario.relative().factors())
                    points.push_back(original * f);
                break;
            default:
                QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, path,
                               "a scenario needs explicit, linear or relative points");
        }
        QLS_FIELD_REQUIRE(!points.empty(), qlpb::Error::INVALID_ARGUMENT, path,
                          "a scenario needs at least one point");

        qlpb::ScenarioResult out;
        out.set_quote_id(scenario.quote_id());

        // Restoring on the way out, including when a point throws. A sweep is
        // a question rather than an edit: the session log never saw these
        // writes, so leaving one in place would put the live graph out of step
        // with what a replay would rebuild (DESIGN §2.1).
        try {
            for (std::size_t i = 0; i < points.size(); ++i) {
                session_->writeQuote(scenario.quote_id(), points[i], path + ".quote_id");
                const auto outcome = session_->price(frame.price(), nullptr);

                out.add_values(points[i]);
                fillResult(*out.add_prices(), frame.price(), outcome);

                emit(frame.request_id(), false, [&](qlpb::ServerFrame& o) {
                    auto* p = o.mutable_progress();
                    p->set_completed(i + 1);
                    p->set_total(points.size());
                    p->set_running_npv(wire(outcome.npv));
                    p->set_scenario_point(static_cast<std::uint32_t>(i));
                });

                if (stopRequested_.load(std::memory_order_relaxed))
                    QL_FAIL("cancelled after " << i + 1 << " of " << points.size()
                                               << " scenario points");
            }
        } catch (...) {
            if (!scenario.keep_final_value())
                session_->writeQuote(scenario.quote_id(), original, path + ".quote_id");
            throw;
        }

        if (!scenario.keep_final_value())
            session_->writeQuote(scenario.quote_id(), original, path + ".quote_id");

        if (scenario.plot() != qlpb::RESULT_KIND_UNSPECIFIED) {
            // Checked after the sweep rather than before, so the quote has
            // already been restored; the prices are still returned, only the
            // series is refused.
            QLS_FIELD_REQUIRE(!resultName(scenario.plot()).empty(), qlpb::Error::UNSUPPORTED,
                              path + ".plot",
                              "this result kind has no single value to plot per point");
            fillSeries(*out.mutable_series(), out, scenario.plot());
        }

        emit(frame.request_id(), true,
             [&](qlpb::ServerFrame& o) { *o.mutable_scenario_result() = out; });

        // The progress lambda is unused on this path: a sweep reports its own
        // progress per point, and each point is a single engine call that
        // nothing can interrupt from inside.
        (void)progress;
    }


    void Worker::fillSeries(qlpb::Series& series,
                            const qlpb::ScenarioResult& scenario,
                            qlpb::ResultKind kind) {
        series.set_name(resultName(kind));
        series.set_x_axis(qlpb::Series::AXIS_SPOT);
        series.set_y_axis(kind == qlpb::RESULT_KIND_NPV ? qlpb::Series::AXIS_NPV
                                                        : qlpb::Series::AXIS_GREEK);
        for (int i = 0; i < scenario.prices_size(); ++i) {
            series.add_x(scenario.values(i));
            if (kind == qlpb::RESULT_KIND_NPV) {
                series.add_y(scenario.prices(i).npv());
                continue;
            }
            const auto& results = scenario.prices(i).results();
            auto it = results.find(resultName(kind));
            // A point the engine could not supply breaks the line rather than
            // shifting it: NaN is what a plotting library renders as a gap,
            // and dropping the point would silently misalign x and y.
            series.add_y(it == results.end() ? std::numeric_limits<double>::quiet_NaN()
                                             : it->second.scalar());
        }
    }


    void Worker::serve(const qlpb::ClientFrame& frame) {
        try {
            switch (frame.payload_case()) {

                case qlpb::ClientFrame::kOpenSession: {
                    QLS_FIELD_REQUIRE(session_ == nullptr, qlpb::Error::INVALID_ARGUMENT,
                                      "session_id", "a session is already open on this worker");
                    const auto start = std::chrono::steady_clock::now();
                    session_ = std::make_unique<Session>(frame.open_session());
                    const double elapsed =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                            .count();

                    emit(frame.request_id(), true, [&](qlpb::ServerFrame& out) {
                        auto* opened = out.mutable_session_opened();
                        opened->set_session_id(sessionId_);
                        opened->set_bootstrap_seconds(elapsed);
                        // What was actually built, in build order: a client
                        // that posted forty objects and got thirty-nine can
                        // see which one is missing without diffing its own
                        // request.
                        for (const auto& id : session_->marketIds())
                            opened->add_market_ids(id);
                    });
                    break;
                }

                case qlpb::ClientFrame::kUpdateMarket: {
                    requireSessionOpen();
                    session_->apply(frame.update_market());
                    emit(frame.request_id(), true,
                         [](qlpb::ServerFrame& out) { out.mutable_ack(); });
                    break;
                }

                case qlpb::ClientFrame::kPrice: {
                    requireSessionOpen();

                    const auto requestId = frame.request_id();
                    auto progress = [&](Size done, Size total, Real runningNpv) {
                        emit(requestId, false, [&](qlpb::ServerFrame& out) {
                            auto* p = out.mutable_progress();
                            p->set_completed(done);
                            p->set_total(total);
                            p->set_running_npv(wire(runningNpv));
                        });
                        return !stopRequested_.load(std::memory_order_relaxed);
                    };

                    if (frame.price().has_scenario()) {
                        serveScenario(frame, progress);
                        break;
                    }

                    const auto outcome = session_->price(frame.price(), progress);

                    emit(requestId, true, [&](qlpb::ServerFrame& out) {
                        fillResult(*out.mutable_price_result(), frame.price(), outcome);
                    });
                    break;
                }

                case qlpb::ClientFrame::kCloseSession:
                    session_.reset();
                    emit(frame.request_id(), true,
                         [](qlpb::ServerFrame& out) { out.mutable_ack(); });
                    break;

                case qlpb::ClientFrame::kCancel:
                    // A cancel that reached the queue is already too late for the
                    // request it targets: this worker is busy with that request,
                    // so nothing here runs until it finishes. Real cancellation is
                    // Worker::requestStop() from the supervisor's thread, or the
                    // process kill behind it (DESIGN §2.1, §3).
                    emit(frame.request_id(), true,
                         [](qlpb::ServerFrame& out) { out.mutable_ack(); });
                    break;

                default:
                    emitError(frame.request_id(), qlpb::Error::INVALID_ARGUMENT,
                              "unhandled frame payload");
                    break;
            }

        } catch (const FieldError& e) {
            // Caught ahead of QuantLib::Error, which it derives from: a
            // rejection that names a field carries its own wire code and the
            // path the frontend should highlight (DESIGN §6).
            emitError(frame.request_id(), e.code(), e.what(), e.fieldPath());

        } catch (const Error& e) {
            // A stop taken at a batch boundary unwinds through here as well,
            // and it is not a calculation failure: the client asked for it,
            // and the protocol promises one code for a cancel however it was
            // served (envelope.proto, CancelRequest). Reporting
            // CALCULATION_FAILED would tell the user their trade did not
            // price.
            const bool stopped = stopRequested_.load(std::memory_order_relaxed);

            // Otherwise QuantLib's message names the failing quantity and is
            // worth showing to the user verbatim.
            emitError(frame.request_id(),
                      stopped ? qlpb::Error::CANCELLED : qlpb::Error::CALCULATION_FAILED, e.what());

            // A dirty session cannot be repaired here: the supervisor replays
            // the session log into a fresh worker (DESIGN §2.1).
            if (session_ != nullptr && session_->dirty())
                session_.reset();

        } catch (const std::exception& e) {
            emitError(frame.request_id(), qlpb::Error::INVALID_ARGUMENT, e.what());
        }
    }


    void Worker::emit(std::uint64_t requestId,
                      bool terminal,
                      const std::function<void(qlpb::ServerFrame&)>& fill) {
        qlpb::ServerFrame out;
        out.set_request_id(requestId);
        // Every frame names its session. A terminal error can arrive when the
        // client has several sessions in flight on one socket, and request_id
        // alone does not say which graph it concerns.
        out.set_session_id(sessionId_);
        out.set_terminal(terminal);
        fill(out);
        sink_(out);
    }


    void Worker::emitError(std::uint64_t requestId,
                           qlpb::Error::Code code,
                           const std::string& message,
                           const std::string& fieldPath) {
        emit(requestId, true, [&](qlpb::ServerFrame& out) {
            auto* err = out.mutable_error();
            err->set_code(code);
            err->set_message(message);
            if (!fieldPath.empty())
                err->set_field_path(fieldPath);
        });
    }

}
