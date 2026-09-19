/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "registry.hpp"
#include "quantlib/v2/envelope.pb.h"
#include "errors/fielderror.hpp"
#include <ql/indexes/ibor/estr.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/ibor/gbplibor.hpp>
#include <ql/indexes/ibor/sofr.hpp>
#include <ql/indexes/ibor/sonia.hpp>
#include <ql/indexes/ibor/usdlibor.hpp>
#include <ql/time/calendars/australia.hpp>
#include <ql/time/calendars/canada.hpp>
#include <ql/time/calendars/japan.hpp>
#include <ql/time/calendars/jointcalendar.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/calendars/switzerland.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedkingdom.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/time/daycounters/business252.hpp>
#include <ql/time/daycounters/one.hpp>
#include <ql/time/daycounters/simpledaycounter.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/utilities/dataparsers.hpp>
#include <utility>
#include <vector>

using namespace QuantLib;
namespace qlpb = quantlib::v1;

namespace qlbackend {

    namespace {

        //! Rejects a zero-valued enum, naming the field the frontend must fix.
        /*! Called first in every mapping. Proto3 gives an unset enum the value
            zero, so without this a frame that omitted the field would resolve
            to whatever convention sits at the head of the list.
        */
        void requireSet(bool isSet, const std::string& fieldPath, const char* what) {
            QLS_FIELD_REQUIRE(isSet, quantlib::v2::Error::UNSPECIFIED_ENUM, fieldPath,
                              "unspecified " << what << " at '" << fieldPath
                                             << "'; the field must be set explicitly");
        }

    }

    ConventionRegistry::ConventionRegistry(CurveResolver curves) : curves_(std::move(curves)) {
        QL_REQUIRE(curves_, "a curve resolver is required");
    }


    Date ConventionRegistry::date(const qlpb::Date& msg, const std::string& fieldPath) const {
        switch (msg.form_case()) {
            case qlpb::Date::kSerialNumber:
                QLS_FIELD_REQUIRE(msg.serial_number() >= Date::minDate().serialNumber() &&
                                      msg.serial_number() <= Date::maxDate().serialNumber(),
                                  quantlib::v2::Error::INVALID_ARGUMENT, fieldPath,
                                  "serial number " << msg.serial_number() << " at '" << fieldPath
                                                   << "' is outside QuantLib's date range");
                return Date(static_cast<Date::serial_type>(msg.serial_number()));
            case qlpb::Date::kIso:
                // QuantLib's parser throws "stoi" on a malformed field and its
                // own error on an impossible date, neither naming where; the
                // request is what is wrong, so it says so, and says where.
                try {
                    return DateParser::parseISO(msg.iso());
                } catch (const std::exception& e) {
                    QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath,
                                   "'" << msg.iso() << "' at '" << fieldPath
                                       << "' is not a date in YYYY-MM-DD form (" << e.what()
                                       << ")");
                }
            case qlpb::Date::FORM_NOT_SET:
                QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath,
                               "no date form set at '" << fieldPath << "'");
        }
        QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath,
                       "unhandled date form at '" << fieldPath << "'");
    }


    Period ConventionRegistry::period(const std::string& text, const std::string& fieldPath) const {
        QLS_FIELD_REQUIRE(!text.empty(), quantlib::v2::Error::INVALID_ARGUMENT, fieldPath,
                          "empty period at '" << fieldPath << "'");
        // PeriodParser covers the whole tenor space, so this is the one
        // convention that does not need a registry entry per value. Its
        // "invalid format" says neither what nor where.
        try {
            return PeriodParser::parse(text);
        } catch (const std::exception& e) {
            QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath,
                           "'" << text << "' at '" << fieldPath
                               << "' is not a tenor such as 3M, 1Y or 2W (" << e.what() << ")");
        }
    }


    BusinessDayConvention
    ConventionRegistry::businessDayConvention(qlpb::BusinessDayConvention msg,
                                              const std::string& fieldPath) const {
        requireSet(msg != qlpb::BUSINESS_DAY_CONVENTION_UNSPECIFIED, fieldPath,
                   "business day convention");
        switch (msg) {
            case qlpb::FOLLOWING:
                return Following;
            case qlpb::MODIFIED_FOLLOWING:
                return ModifiedFollowing;
            case qlpb::PRECEDING:
                return Preceding;
            case qlpb::MODIFIED_PRECEDING:
                return ModifiedPreceding;
            case qlpb::UNADJUSTED:
                return Unadjusted;
            case qlpb::HALF_MONTH_MODIFIED_FOLLOWING:
                return HalfMonthModifiedFollowing;
            case qlpb::NEAREST:
                return Nearest;
            default:
                break;
        }
        QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath,
                       "unhandled business day convention " << msg << " at '" << fieldPath << "'");
    }


    Frequency ConventionRegistry::frequency(qlpb::Frequency msg,
                                            const std::string& fieldPath) const {
        requireSet(msg != qlpb::FREQUENCY_UNSPECIFIED, fieldPath, "frequency");
        // Explicit, not a cast: QuantLib's Frequency values are payments per
        // year, which puts NoFrequency at -1 and Once at 0
        // (ql/time/frequency.hpp:37-38). Casting the proto value here would
        // shift every frequency by one.
        switch (msg) {
            case qlpb::NO_FREQUENCY:
                return NoFrequency;
            case qlpb::ONCE:
                return Once;
            case qlpb::ANNUAL:
                return Annual;
            case qlpb::SEMIANNUAL:
                return Semiannual;
            case qlpb::EVERY_FOURTH_MONTH:
                return EveryFourthMonth;
            case qlpb::QUARTERLY:
                return Quarterly;
            case qlpb::BIMONTHLY:
                return Bimonthly;
            case qlpb::MONTHLY:
                return Monthly;
            case qlpb::EVERY_FOURTH_WEEK:
                return EveryFourthWeek;
            case qlpb::BIWEEKLY:
                return Biweekly;
            case qlpb::WEEKLY:
                return Weekly;
            case qlpb::DAILY:
                return Daily;
            default:
                break;
        }
        QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath,
                       "unhandled frequency " << msg << " at '" << fieldPath << "'");
    }


    Compounding ConventionRegistry::compounding(qlpb::Compounding msg,
                                                const std::string& fieldPath) const {
        requireSet(msg != qlpb::COMPOUNDING_UNSPECIFIED, fieldPath, "compounding");
        switch (msg) {
            case qlpb::SIMPLE:
                return Simple;
            case qlpb::COMPOUNDED:
                return Compounded;
            case qlpb::CONTINUOUS:
                return Continuous;
            case qlpb::SIMPLE_THEN_COMPOUNDED:
                return SimpleThenCompounded;
            case qlpb::COMPOUNDED_THEN_SIMPLE:
                return CompoundedThenSimple;
            default:
                break;
        }
        QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath,
                       "unhandled compounding " << msg << " at '" << fieldPath << "'");
    }


    DayCounter ConventionRegistry::dayCounter(const qlpb::DayCounter& msg,
                                              const std::string& fieldPath) const {
        requireSet(msg.family() != qlpb::DayCounter::FAMILY_UNSPECIFIED, fieldPath,
                   "day counter family");

        switch (msg.family()) {
            case qlpb::DayCounter::ACTUAL_360:
                return Actual360();
            case qlpb::DayCounter::ACTUAL_365_FIXED:
                return Actual365Fixed();
            case qlpb::DayCounter::ACTUAL_365_NO_LEAP:
                return Actual365Fixed(Actual365Fixed::NoLeap);
            case qlpb::DayCounter::ONE_DAY_COUNTER:
                return OneDayCounter();
            case qlpb::DayCounter::SIMPLE_DAY_COUNTER:
                return SimpleDayCounter();

            case qlpb::DayCounter::BUSINESS_252:
                // Business/252 counts business days, so it carries a calendar.
                // QuantLib defaults it to Brazil() (business252.hpp:50); defaulting
                // it here would price a EUR leg on Brazilian holidays.
                QLS_FIELD_REQUIRE(msg.has_business_252_calendar(), quantlib::v2::Error::INVALID_ARGUMENT,
                                  fieldPath + ".business_252_calendar",
                                  "Business/252 needs a calendar at '" << fieldPath << "'");
                return Business252(
                    calendar(msg.business_252_calendar(), fieldPath + ".business_252_calendar"));

            case qlpb::DayCounter::THIRTY_360: {
                requireSet(msg.thirty_360() != qlpb::DayCounter::THIRTY_360_CONVENTION_UNSPECIFIED,
                           fieldPath + ".thirty_360", "30/360 convention");
                // The variants differ materially in the day count (AGENTS.md
                // §5.4), so there is no defensible default.
                switch (msg.thirty_360()) {
                    case qlpb::DayCounter::USA:
                        return Thirty360(Thirty360::USA);
                    case qlpb::DayCounter::BOND_BASIS:
                        return Thirty360(Thirty360::BondBasis);
                    case qlpb::DayCounter::EUROPEAN:
                        return Thirty360(Thirty360::European);
                    case qlpb::DayCounter::EUROBOND_BASIS:
                        return Thirty360(Thirty360::EurobondBasis);
                    case qlpb::DayCounter::ITALIAN:
                        return Thirty360(Thirty360::Italian);
                    case qlpb::DayCounter::GERMAN:
                        return Thirty360(Thirty360::German);
                    case qlpb::DayCounter::ISMA:
                        return Thirty360(Thirty360::ISMA);
                    case qlpb::DayCounter::ISDA:
                        return Thirty360(Thirty360::ISDA);
                    case qlpb::DayCounter::NASD:
                        return Thirty360(Thirty360::NASD);
                    default:
                        break;
                }
                QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath + ".thirty_360",
                               "unhandled 30/360 convention at '" << fieldPath << ".thirty_360'");
            }

            case qlpb::DayCounter::ACTUAL_ACTUAL: {
                requireSet(msg.actual_actual() !=
                               qlpb::DayCounter::ACTUAL_ACTUAL_CONVENTION_UNSPECIFIED,
                           fieldPath + ".actual_actual", "act/act convention");
                switch (msg.actual_actual()) {
                    case qlpb::DayCounter::AA_ISDA:
                        return ActualActual(ActualActual::ISDA);
                    case qlpb::DayCounter::AA_HISTORICAL:
                        return ActualActual(ActualActual::Historical);
                    case qlpb::DayCounter::AA_ACTUAL_365:
                        return ActualActual(ActualActual::Actual365);
                    case qlpb::DayCounter::AA_AFB:
                        return ActualActual(ActualActual::AFB);
                    case qlpb::DayCounter::AA_EURO:
                        return ActualActual(ActualActual::Euro);
                    case qlpb::DayCounter::AA_ISMA:
                    case qlpb::DayCounter::AA_BOND:
                        // ISMA/Bond need the coupon schedule for irregular periods and
                        // are therefore built by the leg builder, which has it. The
                        // schedule-free form silently gives a different year fraction
                        // on a stub, so it is refused rather than approximated.
                        QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath + ".actual_actual",
                                       "Act/Act (ISMA) at '"
                                           << fieldPath
                                           << "' must be built with a schedule by the leg builder");
                    default:
                        break;
                }
                QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath + ".actual_actual",
                               "unhandled act/act convention at '" << fieldPath
                                                                   << ".actual_actual'");
            }

            default:
                break;
        }
        QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath + ".family",
                       "unhandled day counter family at '" << fieldPath << "'");
    }


    Calendar ConventionRegistry::calendar(const qlpb::Calendar& msg,
                                          const std::string& fieldPath) const {
        return calendar(msg, fieldPath, 0);
    }


    Calendar ConventionRegistry::calendar(const qlpb::Calendar& msg,
                                          const std::string& fieldPath,
                                          Size depth) const {
        QLS_FIELD_REQUIRE(depth <= maxJointCalendarDepth_, quantlib::v2::Error::INVALID_ARGUMENT,
                          fieldPath + ".joint",
                          "joint calendars nested more than " << maxJointCalendarDepth_
                                                              << " deep at '" << fieldPath << "'");
        requireSet(msg.name() != qlpb::Calendar::NAME_UNSPECIFIED, fieldPath, "calendar");

        switch (msg.name()) {
            case qlpb::Calendar::NULL_CALENDAR:
                return NullCalendar();
            case qlpb::Calendar::TARGET:
                return QuantLib::TARGET();
            case qlpb::Calendar::JAPAN:
                return Japan();
            case qlpb::Calendar::SWITZERLAND:
                return Switzerland();
            case qlpb::Calendar::CANADA:
                return Canada();
            case qlpb::Calendar::AUSTRALIA:
                return Australia();

            case qlpb::Calendar::UNITED_STATES: {
                requireSet(msg.united_states_market() !=
                               qlpb::Calendar::UNITED_STATES_MARKET_UNSPECIFIED,
                           fieldPath + ".united_states_market", "US calendar market");
                switch (msg.united_states_market()) {
                    case qlpb::Calendar::US_SETTLEMENT:
                        return UnitedStates(UnitedStates::Settlement);
                    case qlpb::Calendar::US_NYSE:
                        return UnitedStates(UnitedStates::NYSE);
                    case qlpb::Calendar::US_GOVERNMENT_BOND:
                        return UnitedStates(UnitedStates::GovernmentBond);
                    case qlpb::Calendar::US_NERC:
                        return UnitedStates(UnitedStates::NERC);
                    case qlpb::Calendar::US_LIBOR_IMPACT:
                        return UnitedStates(UnitedStates::LiborImpact);
                    case qlpb::Calendar::US_FEDERAL_RESERVE:
                        return UnitedStates(UnitedStates::FederalReserve);
                    case qlpb::Calendar::US_SOFR:
                        return UnitedStates(UnitedStates::SOFR);
                    default:
                        break;
                }
                QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath + ".united_states_market",
                               "unhandled US market at '" << fieldPath << ".united_states_market'");
            }

            case qlpb::Calendar::UNITED_KINGDOM: {
                requireSet(msg.united_kingdom_market() !=
                               qlpb::Calendar::UNITED_KINGDOM_MARKET_UNSPECIFIED,
                           fieldPath + ".united_kingdom_market", "UK calendar market");
                switch (msg.united_kingdom_market()) {
                    case qlpb::Calendar::UK_SETTLEMENT:
                        return UnitedKingdom(UnitedKingdom::Settlement);
                    case qlpb::Calendar::UK_EXCHANGE:
                        return UnitedKingdom(UnitedKingdom::Exchange);
                    case qlpb::Calendar::UK_METALS:
                        return UnitedKingdom(UnitedKingdom::Metals);
                    default:
                        break;
                }
                QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath + ".united_kingdom_market",
                               "unhandled UK market at '" << fieldPath
                                                          << ".united_kingdom_market'");
            }

            case qlpb::Calendar::JOINT: {
                QLS_FIELD_REQUIRE(
                    msg.joint_size() >= 2, quantlib::v2::Error::INVALID_ARGUMENT, fieldPath + ".joint",
                    "a joint calendar at '" << fieldPath << "' needs at least two members, got "
                                            << msg.joint_size());
                std::vector<Calendar> members;
                members.reserve(msg.joint_size());
                for (int i = 0; i < msg.joint_size(); ++i)
                    members.push_back(calendar(
                        msg.joint(i), fieldPath + ".joint[" + std::to_string(i) + "]", depth + 1));
                return JointCalendar(members);
            }

            default:
                break;
        }
        QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath + ".name",
                       "unhandled calendar at '" << fieldPath << "'");
    }


    ext::shared_ptr<IborIndex> ConventionRegistry::index(const qlpb::IborIndex& msg,
                                                         const std::string& fieldPath) const {
        requireSet(msg.family() != qlpb::IborIndex::FAMILY_UNSPECIFIED, fieldPath, "index family");

        // An empty curve id means fixings-only use; an unknown one throws in
        // the resolver rather than yielding an empty handle, which would price
        // fine until the first forecast and then fail far from the cause.
        Handle<YieldTermStructure> forwarding;
        if (!msg.forwarding_curve_id().empty())
            forwarding = curves_(msg.forwarding_curve_id());

        const bool overnight = msg.family() == qlpb::IborIndex::SOFR ||
                               msg.family() == qlpb::IborIndex::ESTR ||
                               msg.family() == qlpb::IborIndex::SONIA;
        QLS_FIELD_REQUIRE(!(overnight && !msg.tenor().empty()), quantlib::v2::Error::INVALID_ARGUMENT,
                          fieldPath + ".tenor",
                          "overnight index at '" << fieldPath << "' cannot take tenor '"
                                                 << msg.tenor() << "'");
        QLS_FIELD_REQUIRE(overnight || !msg.tenor().empty(), quantlib::v2::Error::INVALID_ARGUMENT,
                          fieldPath + ".tenor", "index at '" << fieldPath << "' needs a tenor");

        switch (msg.family()) {
            case qlpb::IborIndex::EURIBOR:
                return ext::make_shared<Euribor>(period(msg.tenor(), fieldPath + ".tenor"),
                                                 forwarding);
            case qlpb::IborIndex::USD_LIBOR:
                return ext::make_shared<USDLibor>(period(msg.tenor(), fieldPath + ".tenor"),
                                                  forwarding);
            case qlpb::IborIndex::GBP_LIBOR:
                return ext::make_shared<GBPLibor>(period(msg.tenor(), fieldPath + ".tenor"),
                                                  forwarding);
            case qlpb::IborIndex::SOFR:
                return ext::make_shared<Sofr>(forwarding);
            case qlpb::IborIndex::ESTR:
                return ext::make_shared<Estr>(forwarding);
            case qlpb::IborIndex::SONIA:
                return ext::make_shared<Sonia>(forwarding);
            default:
                break;
        }
        QLS_FIELD_FAIL(quantlib::v2::Error::INVALID_ARGUMENT, fieldPath + ".family",
                       "unhandled index family at '" << fieldPath << "'");
    }

}
