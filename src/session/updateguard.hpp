/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file updateguard.hpp
    \brief RAII batching of observable notifications
*/

#ifndef qlbackend_session_updateguard_hpp
#define qlbackend_session_updateguard_hpp

#include <ql/patterns/observable.hpp>
#include <string>

namespace qlbackend {

    //! Defers observer notifications until the batch is committed.
    /*! Wraps a run of quote writes so the dependent instruments recalculate
        once instead of once per quote.

        \warning It must be `disableUpdates(true)`. The default argument is
                 `false`, which does not defer notifications — it *drops* them:
                 `Observable::notifyObservers()` hands the observers to
                 `registerDeferredObservers()`, which keeps them only when
                 `updatesDeferred()` is set (`ql/patterns/observable.hpp:165`).
                 With the drop form, `LazyObject::calculated_` is never
                 cleared, so the next `NPV()` returns the pre-update price and
                 nothing anywhere reports an error. The one in-tree use of the
                 drop form (`blackswaptionengine.hpp:247`) wants exactly that:
                 it is suppressing a cascade from a temporary engine swap, not
                 batching.

        Committing runs every deferred `update()`. If any observer throws,
        `enableUpdates()` raises after having tried them all
        (`ql/patterns/observable.cpp`), so a failure here means the graph is
        partly invalidated — the session must be treated as dirty and its
        cached results discarded.
    */
    class UpdateGuard {
      public:
        UpdateGuard() { QuantLib::ObservableSettings::instance().disableUpdates(true); }

        //! Runs the deferred notifications; may throw.
        /*! Call this explicitly. The destructor is only a safety net for the
            unwound case, and cannot report what went wrong.
        */
        void commit() {
            if (!committed_) {
                committed_ = true;
                QuantLib::ObservableSettings::instance().enableUpdates();
            }
        }

        ~UpdateGuard() {
            if (!committed_) {
                try {
                    QuantLib::ObservableSettings::instance().enableUpdates();
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // Already unwinding, or commit() was never reached. The
                    // caller marks the session dirty on the original error;
                    // rethrowing here would terminate the worker.
                }
            }
        }

        UpdateGuard(const UpdateGuard&) = delete;
        UpdateGuard& operator=(const UpdateGuard&) = delete;
        UpdateGuard(UpdateGuard&&) = delete;
        UpdateGuard& operator=(UpdateGuard&&) = delete;

      private:
        bool committed_ = false;
    };

}

#endif
