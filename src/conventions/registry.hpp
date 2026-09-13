/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file registry.hpp
    \brief translation of protobuf convention messages into QuantLib objects
*/

#ifndef qlbackend_conventions_registry_hpp
#define qlbackend_conventions_registry_hpp

#include "quantlib/v1/conventions.pb.h"
// Error::Code is v2's; the conventions above are shared (DESIGN 6.3).
#include "quantlib/v2/envelope.pb.h"
#include <ql/compounding.hpp>
#include <ql/errors.hpp>
#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/shared_ptr.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/frequency.hpp>
#include <ql/time/period.hpp>
#include <functional>
#include <string>

namespace qlbackend {

    //! Translates protobuf convention messages into QuantLib objects.
    /*! QuantLib has no reflection and no string-to-object factory, so this
        mapping is written by hand and must be extended whenever a convention
        is added to the schema.

        Three rules hold throughout:

        - **Zero is rejected.** Proto3 cannot distinguish an unset enum from
          its first value, so every `*_UNSPECIFIED` raises rather than
          resolving to a real convention. A forgotten field must fail loudly,
          not price on Actual/360 by accident.

        - **Every mapping is a `switch`, never a lookup table.** The compiler
          then flags an unhandled value (`-Werror=switch`) the moment the
          schema gains an entry, which is the only mechanism that keeps this
          layer honest as the protocol grows. A `std::map` would compile
          cleanly and throw in production instead.

        - **Errors name the field.** Each method takes a `fieldPath` used in
          the failure message, so a rejected frame tells the frontend which
          field to fix. `Error.field_path` carries it back over the wire.

        Instances are cheap and hold no mutable state beyond the forwarding
        curve lookup, so one per session thread is fine. Nothing here is
        thread-safe by design: a registry belongs to the worker thread that
        owns the session's object graph.
    */
    class ConventionRegistry {
      public:
        //! Resolves a curve_id to a live forwarding curve handle.
        /*! Supplied by the session, which owns the bootstrapped curves. An
            unknown id must throw, not return an empty handle: an index built
            on an empty handle prices happily until the first forecast and
            then fails somewhere far from the cause.
        */
        using CurveResolver =
            std::function<QuantLib::Handle<QuantLib::YieldTermStructure>(const std::string&)>;

        explicit ConventionRegistry(CurveResolver curves);

        QuantLib::Date date(const quantlib::v1::Date& msg, const std::string& fieldPath) const;

        //! Parses QuantLib period text ("6M", "10Y") via PeriodParser.
        QuantLib::Period period(const std::string& text, const std::string& fieldPath) const;

        QuantLib::BusinessDayConvention
        businessDayConvention(quantlib::v1::BusinessDayConvention msg,
                              const std::string& fieldPath) const;

        QuantLib::Frequency frequency(quantlib::v1::Frequency msg,
                                      const std::string& fieldPath) const;

        QuantLib::Compounding compounding(quantlib::v1::Compounding msg,
                                          const std::string& fieldPath) const;

        QuantLib::DayCounter dayCounter(const quantlib::v1::DayCounter& msg,
                                        const std::string& fieldPath) const;

        QuantLib::Calendar calendar(const quantlib::v1::Calendar& msg,
                                    const std::string& fieldPath) const;

        QuantLib::ext::shared_ptr<QuantLib::IborIndex> index(const quantlib::v1::IborIndex& msg,
                                                             const std::string& fieldPath) const;

      private:
        //! Guards against a malformed frame nesting joint calendars forever.
        static constexpr QuantLib::Size maxJointCalendarDepth_ = 4;

        QuantLib::Calendar calendar(const quantlib::v1::Calendar& msg,
                                    const std::string& fieldPath,
                                    QuantLib::Size depth) const;

        CurveResolver curves_;
    };

}

#endif
