/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "worker.hpp"
#include "errors/fielderror.hpp"
#include "session.hpp"
#include <ql/errors.hpp>
#include <chrono>
#include <utility>

using namespace QuantLib;
namespace qlpb = quantlib::v1;

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

                    const auto outcome = session_->price(frame.price(), progress);

                    emit(requestId, true, [&](qlpb::ServerFrame& out) {
                        auto* result = out.mutable_price_result();
                        result->set_npv(wire(outcome.npv));
                        result->set_calculation_seconds(outcome.calculationSeconds);

                        // All three, not just the seed. A batched Monte Carlo runs
                        // independent batches with derived seeds and averages them,
                        // which partitions the RNG stream differently from one run
                        // of the same total, so two requests differing only in
                        // progress_every_paths return different numbers (DESIGN
                        // §4). A client comparing two prices has to be able to see
                        // that from the results alone.
                        result->set_seed(frame.price().engine().seed());
                        result->set_samples(frame.price().engine().samples());
                        result->set_progress_every_paths(frame.price().progress_every_paths());
                        for (const auto& [name, v] : outcome.results)
                            (*result->mutable_results())[name] = wire(v);
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
