/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "worker.hpp"
#include <limits>
#include <vector>
#include "capabilities.hpp"
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

        for (const auto& series : outcome.series)
            *result.add_series() = series;
        for (const auto& row : outcome.cashflows)
            *result.add_cashflows() = row;
        for (const auto kind : outcome.unavailable)
            result.add_unavailable_results(kind);

        auto err = outcome.results.find("errorEstimate");
        if (err != outcome.results.end()) {
            auto* estimate = result.mutable_error_estimate();
            estimate->set_standard_error(wire(err->second));
            estimate->set_samples(request.engine().mc().samples());
        }
    }


    void Worker::serveScenario(const qlpb::ClientFrame& frame,
                               const Session::ProgressSink& progress) {
        const auto& requested = frame.price().scenarios();
        const std::string base = "scenarios";

        // One axis resolved: the quote it writes, where that quote started, and
        // the values it will take. Resolved for every axis before the first
        // write, so a malformed second axis costs no prices and leaves nothing
        // to put back.
        struct Axis {
            std::string quoteId;
            std::string path;
            double original;
            std::vector<double> points;
            bool keepFinal;
        };

        std::vector<Axis> plan;
        std::size_t total = 1;
        for (int a = 0; a < requested.size(); ++a) {
            const auto& axis = requested[a];
            const std::string path = base + "[" + std::to_string(a) + "]";

            QLS_FIELD_REQUIRE(!axis.quote_id().empty(), qlpb::Error::INVALID_ARGUMENT,
                              path + ".quote_id", "a scenario axis sweeps one named quote");
            for (const auto& earlier : plan)
                QLS_FIELD_REQUIRE(earlier.quoteId != axis.quote_id(),
                                  qlpb::Error::INVALID_ARGUMENT, path + ".quote_id",
                                  "'" + axis.quote_id() +
                                      "' is already an axis of this sweep: the later write "
                                      "would win at every point and the earlier axis would "
                                      "move nothing");
            // The plot belongs to the sweep. Reading it off whichever axis
            // happened to set it would draw a plot of something the client did
            // not ask for, so only the first axis may carry it.
            QLS_FIELD_REQUIRE(a == 0 || axis.plot() == qlpb::RESULT_KIND_UNSPECIFIED,
                              qlpb::Error::INVALID_ARGUMENT, path + ".plot",
                              "plot is a property of the sweep rather than of an axis; set "
                              "it on the first one");

            const double original = session_->quoteValue(axis.quote_id(), path + ".quote_id");

            std::vector<double> points;
            switch (axis.points_case()) {
                case qlpb::Scenario::kExplicit:
                    points.assign(axis.explicit_().values().begin(),
                                  axis.explicit_().values().end());
                    break;
                case qlpb::Scenario::kLinear: {
                    const auto& lin = axis.linear();
                    QLS_FIELD_REQUIRE(lin.steps() >= 2, qlpb::Error::INVALID_ARGUMENT,
                                      path + ".linear.steps",
                                      "a linear sweep needs at least two steps");
                    for (std::uint32_t i = 0; i < lin.steps(); ++i)
                        points.push_back(lin.begin() + (lin.end() - lin.begin()) * i /
                                                          static_cast<double>(lin.steps() - 1));
                    break;
                }
                case qlpb::Scenario::kRelative:
                    for (double f : axis.relative().factors())
                        points.push_back(original * f);
                    break;
                default:
                    QLS_FIELD_FAIL(qlpb::Error::INVALID_ARGUMENT, path,
                                   "a scenario needs explicit, linear or relative points");
            }
            QLS_FIELD_REQUIRE(!points.empty(), qlpb::Error::INVALID_ARGUMENT, path,
                              "a scenario needs at least one point");

            // Checked as the product grows rather than at the end, so the
            // multiplication cannot overflow into a number that looks small.
            total *= points.size();
            QLS_FIELD_REQUIRE(total <= kMaxScenarioPoints, qlpb::Error::INVALID_ARGUMENT, base,
                              "this grid is " + std::to_string(total) + " points across " +
                                  std::to_string(plan.size() + 1) + " axes, over the limit of " +
                                  std::to_string(kMaxScenarioPoints) + "; a product multiplies, "
                                  "so a step count one digit too long is a session-length "
                                  "request rather than a slow one");

            plan.push_back({axis.quote_id(), path, original, std::move(points),
                            axis.keep_final_value()});
        }
        QLS_FIELD_REQUIRE(!plan.empty(), qlpb::Error::INVALID_ARGUMENT, base,
                          "a sweep needs at least one axis");

        qlpb::ScenarioResult out;
        for (const auto& axis : plan) {
            auto* wired = out.add_axes();
            wired->set_quote_id(axis.quoteId);
            for (double point : axis.points)
                wired->add_values(wire(point));
        }

        // Row-major over the axes, last varying fastest: the flat index of a
        // point decomposes into one coordinate per axis by dividing through the
        // strides. Same order DoubleMatrix uses, so the surface needs no
        // rearranging afterwards.
        std::vector<std::size_t> stride(plan.size(), 1);
        for (std::size_t a = plan.size(); a-- > 1;)
            stride[a - 1] = stride[a] * plan[a].points.size();

        std::vector<std::size_t> at(plan.size(), 0);

        const auto restore = [&] {
            for (const auto& axis : plan)
                if (!axis.keepFinal)
                    session_->writeQuote(axis.quoteId, axis.original, axis.path + ".quote_id");
        };

        // Restoring on the way out, including when a point throws. A sweep is
        // a question rather than an edit: the session log never saw these
        // writes, so leaving one in place would put the live graph out of step
        // with what a replay would rebuild (DESIGN §2.1).
        try {
            for (std::size_t i = 0; i < total; ++i) {
                // Only the coordinates that moved are written. The outermost
                // axis changes once per row, and a write it does not need is a
                // notification the whole graph would answer.
                for (std::size_t a = 0; a < plan.size(); ++a) {
                    const std::size_t k = (i / stride[a]) % plan[a].points.size();
                    if (i == 0 || k != at[a]) {
                        at[a] = k;
                        session_->writeQuote(plan[a].quoteId, plan[a].points[k],
                                             plan[a].path + ".quote_id");
                    }
                }

                const auto outcome = session_->price(frame.price(), nullptr);
                fillResult(*out.add_prices(), frame.price(), outcome);

                emit(frame.request_id(), false, [&](qlpb::ServerFrame& o) {
                    auto* p = o.mutable_progress();
                    p->set_completed(i + 1);
                    p->set_total(total);
                    p->set_running_npv(wire(outcome.npv));
                    p->set_scenario_point(static_cast<std::uint32_t>(i));
                });

                if (stopRequested_.load(std::memory_order_relaxed))
                    QL_FAIL("cancelled after " << i + 1 << " of " << total
                                               << " scenario points");
            }
        } catch (...) {
            restore();
            throw;
        }

        restore();

        const auto plot = plan.empty() ? qlpb::RESULT_KIND_UNSPECIFIED : requested[0].plot();
        if (plot != qlpb::RESULT_KIND_UNSPECIFIED) {
            // Checked after the sweep rather than before, so the quotes have
            // already been restored; the prices are still returned, only the
            // plot is refused.
            QLS_FIELD_REQUIRE(!resultName(plot).empty(), qlpb::Error::UNSUPPORTED,
                              base + "[0].plot",
                              "this result kind has no single value to plot per point");
            // A line for one axis, a surface for two. Past that a plot would
            // have to choose which axes to show, and prices is the answer.
            if (plan.size() == 1)
                fillSeries(*out.mutable_series(), out, plot);
            else if (plan.size() == 2)
                fillSurface(*out.mutable_surface(), out, plot);
        }

        emit(frame.request_id(), true,
             [&](qlpb::ServerFrame& o) { *o.mutable_scenario_result() = out; });

        // The progress lambda is unused on this path: a sweep reports its own
        // progress per point, and each point is a single engine call that
        // nothing can interrupt from inside.
        (void)progress;
    }


    void Worker::serveBatch(const qlpb::ClientFrame& frame) {
        const auto& requests = frame.batch().requests();
        QLS_FIELD_REQUIRE(!requests.empty(), qlpb::Error::INVALID_ARGUMENT, "batch.requests",
                          "a batch prices at least one trade");

        qlpb::BatchResult out;

        for (int at = 0; at < requests.size(); ++at) {
            const std::string path = "batch.requests[" + std::to_string(at) + "]";
            auto* entry = out.add_entries();

            // Refused per entry rather than up front: the rest of the book is
            // still priced, and the client learns which row it was.
            if (!requests[at].scenarios().empty()) {
                auto* error = entry->mutable_error();
                error->set_code(qlpb::Error::UNSUPPORTED);
                error->set_field_path(path + ".scenarios");
                error->set_message(
                    "a sweep inside a batch is a product with no honest progress "
                    "stream; send the sweep as its own request");
                continue;
            }

            try {
                const auto outcome = session_->price(requests[at], nullptr);
                fillResult(*entry->mutable_price(), requests[at], outcome);
            } catch (const FieldError& e) {
                // The rejection this entry would have been sent on its own, so
                // a blotter can put the complaint on the row it belongs to.
                // The path is rewritten to name the entry: "instrument.option"
                // is ambiguous across forty trades.
                auto* error = entry->mutable_error();
                error->set_code(e.code());
                error->set_message(e.what());
                error->set_field_path(e.fieldPath().empty() ? path : path + "." + e.fieldPath());
            } catch (const Error& e) {
                auto* error = entry->mutable_error();
                error->set_code(stopRequested_.load(std::memory_order_relaxed)
                                    ? qlpb::Error::CANCELLED
                                    : qlpb::Error::CALCULATION_FAILED);
                error->set_message(e.what());
                error->set_field_path(path);
            }

            emit(frame.request_id(), false, [&](qlpb::ServerFrame& o) {
                auto* p = o.mutable_progress();
                p->set_completed(at + 1);
                p->set_total(requests.size());
                p->set_running_npv(entry->has_price() ? entry->price().npv() : 0.0);
            });

            // A dirtied graph is only partly invalidated, so every later price
            // would be computed against something no longer coherent. The rest
            // of the book is answered with what stopped it rather than with
            // numbers nobody should trust; the supervisor replays the session
            // into a fresh worker afterwards (DESIGN §2.1).
            if (session_->dirty()) {
                out.set_abandoned_after(static_cast<std::uint32_t>(at + 1));
                break;
            }
            if (stopRequested_.load(std::memory_order_relaxed)) {
                out.set_abandoned_after(static_cast<std::uint32_t>(at + 1));
                break;
            }
        }

        const bool wasCancelled = out.abandoned_after() > 0 && !session_->dirty();
        for (int at = out.entries_size(); at < requests.size(); ++at) {
            auto* error = out.add_entries()->mutable_error();
            error->set_code(wasCancelled ? qlpb::Error::CANCELLED
                                         : qlpb::Error::CALCULATION_FAILED);
            error->set_field_path("batch.requests[" + std::to_string(at) + "]");
            error->set_message(wasCancelled
                                   ? "cancelled after " + std::to_string(out.abandoned_after()) +
                                         " of " + std::to_string(requests.size()) + " trades"
                                   : "not priced: an earlier trade in this batch left the graph "
                                     "partly invalidated, and a price off it would not be one");
        }

        emit(frame.request_id(), true,
             [&](qlpb::ServerFrame& o) { *o.mutable_batch_result() = out; });

        // Dropped here rather than in the catch that usually does it: this one
        // did not throw, so nothing else will notice.
        if (session_->dirty())
            session_.reset();
    }


    void Worker::fillSeries(qlpb::Series& series,
                            const qlpb::ScenarioResult& scenario,
                            qlpb::ResultKind kind) {
        series.set_name(resultName(kind));
        series.set_x_axis(qlpb::Series::AXIS_SPOT);
        series.set_y_axis(kind == qlpb::RESULT_KIND_NPV ? qlpb::Series::AXIS_NPV
                                                        : qlpb::Series::AXIS_GREEK);
        for (int i = 0; i < scenario.prices_size(); ++i) {
            series.add_x(scenario.axes(0).values(i));
            series.add_y(pointValue(scenario.prices(i), kind));
        }
    }


    double Worker::pointValue(const qlpb::PriceResult& price, qlpb::ResultKind kind) {
        if (kind == qlpb::RESULT_KIND_NPV)
            return price.npv();
        const auto& results = price.results();
        auto it = results.find(resultName(kind));
        // A point the engine could not supply breaks the line rather than
        // shifting it: NaN is what a plotting library renders as a gap, and
        // dropping the point would silently misalign the value with its axis.
        return it == results.end() ? std::numeric_limits<double>::quiet_NaN()
                                   : it->second.scalar();
    }


    void Worker::fillSurface(qlpb::DoubleMatrix& surface,
                             const qlpb::ScenarioResult& scenario,
                             qlpb::ResultKind kind) {
        const auto& rows = scenario.axes(0).values();
        const auto& columns = scenario.axes(1).values();
        surface.set_rows(static_cast<std::uint32_t>(rows.size()));
        surface.set_columns(static_cast<std::uint32_t>(columns.size()));
        // The axis values travel as the labels, so a heat map or a family of
        // lines can place every cell without being told the axes separately.
        for (double value : rows)
            surface.add_row_labels(value);
        for (double value : columns)
            surface.add_column_labels(value);
        // prices is already row-major over the axes, which is the order
        // DoubleMatrix documents, so this is a copy rather than a transpose.
        for (int i = 0; i < scenario.prices_size(); ++i)
            surface.add_values(pointValue(scenario.prices(i), kind));
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

                    if (!frame.price().scenarios().empty()) {
                        serveScenario(frame, progress);
                        break;
                    }

                    const auto outcome = session_->price(frame.price(), progress);

                    emit(requestId, true, [&](qlpb::ServerFrame& out) {
                        fillResult(*out.mutable_price_result(), frame.price(), outcome);
                    });
                    break;
                }

                case qlpb::ClientFrame::kBatch: {
                    requireSessionOpen();
                    serveBatch(frame);
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
