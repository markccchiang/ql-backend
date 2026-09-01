/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file fielderror.hpp
    \brief a rejected frame, carrying the wire code and the field that caused it
*/

#ifndef qlservice_errors_fielderror_hpp
#define qlservice_errors_fielderror_hpp

#include "quantlib/v2/envelope.pb.h"
#include <ql/errors.hpp>
#include <sstream>
#include <string>
#include <utility>

namespace qlservice {

    //! An error that names the proto field responsible for it.
    /*! `QuantLib::Error` carries only a message, so a rejection thrown with
        `QL_FAIL` reaches the socket as prose: the frontend can show it, but it
        cannot highlight the input that caused it. Everything below the worker
        already threads a `fieldPath` through for exactly that purpose, and
        this is what carries it — plus the `Error::Code` the frame should use —
        the rest of the way out.

        `Worker::serve` catches this ahead of `QuantLib::Error` and copies both
        onto `Error.field_path` and `Error.code`. An error thrown with plain
        `QL_FAIL` still works; it simply arrives as `CALCULATION_FAILED` with
        no attributable field, which is the right answer for a failure that
        genuinely comes from the maths rather than from the request.

        Deriving from `QuantLib::Error` is what keeps that fallback true: code
        that only catches `QuantLib::Error` still catches these.
    */
    class FieldError : public QuantLib::Error {
      public:
        FieldError(quantlib::v2::Error::Code code,
                   std::string fieldPath,
                   const std::string& message,
                   const std::string& file,
                   long line,
                   const std::string& function)
        : QuantLib::Error(file, line, function, message), code_(code),
          fieldPath_(std::move(fieldPath)) {}

        quantlib::v2::Error::Code code() const { return code_; }

        //! Proto field path, e.g. "instrument.vanilla_swap.fixed_leg.frequency".
        const std::string& fieldPath() const { return fieldPath_; }

      private:
        quantlib::v2::Error::Code code_;
        std::string fieldPath_;
    };

}

/*! \def QLS_FIELD_FAIL
    \brief throw a FieldError naming a wire code and a proto field path

    Mirrors `QL_FAIL`, with the code and the path in front of the streamed
    message. Use `UNSPECIFIED_ENUM` when a proto3 enum was left at its zero
    value and `INVALID_ARGUMENT` when the frame was well formed but the value
    is unusable.
*/
#define QLS_FIELD_FAIL(code, path, message)                                                  \
    QL_MULTILINE_FAILURE_BEGIN                                                               \
    std::ostringstream _qls_msg_stream;                                                      \
    _qls_msg_stream << message;                                                              \
    throw ::qlservice::FieldError((code), (path), _qls_msg_stream.str(), __FILE__, __LINE__, \
                                  BOOST_CURRENT_FUNCTION);                                   \
    QL_MULTILINE_FAILURE_END

/*! \def QLS_FIELD_REQUIRE
    \brief throw a FieldError unless the condition holds
*/
#define QLS_FIELD_REQUIRE(condition, code, path, message) \
    QL_MULTILINE_ASSERTION_BEGIN                          \
    if (!(condition)) {                                   \
        QLS_FIELD_FAIL(code, path, message);              \
    }                                                     \
    QL_MULTILINE_ASSERTION_END

#endif
