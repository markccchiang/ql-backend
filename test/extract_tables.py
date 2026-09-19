#!/usr/bin/env python3
"""Extracts QuantLib's own reference tables into reference_tables.py.

The tables in test-suite/*.cpp are the published values this service is
checked against. Transcribing them by hand is how a benchmark quietly stops
benchmarking: a digit changes and the test still passes, against the wrong
number. So they are parsed out of the C++ instead, and the parser is checked
in beside the result.

Usage:
    python3 test/extract_tables.py ~/CLionProjects/QuantLib > test/reference_tables.py

The output is committed. Re-run it when the QuantLib checkout moves, and the
diff is then the library's change rather than ours.
"""

import re
import sys
from pathlib import Path

# (module name, source file, C++ declaration, field names)
#
# Field names follow the C++ struct member for member, including the ones we
# do not use, so a row can be read against the source without counting commas.
TABLES = [
    ("EUROPEAN", "europeanoption.cpp", "EuropeanOptionData values[]",
     "type strike s q r t v result tol"),
    ("AMERICAN_BAW", "americanoption.cpp", "AmericanOptionData values[]",
     "type strike s q r t v result", 0),
    ("AMERICAN_BS", "americanoption.cpp", "AmericanOptionData values[]",
     "type strike s q r t v result", 1),
    ("AMERICAN_JU", "americanoption.cpp", "AmericanOptionData juValues[]",
     "type strike s q r t v result"),
    ("BARRIER", "barrieroption.cpp", "NewBarrierOptionData values[]",
     "barrier_type barrier rebate type exercise strike s q r t v result tol"),
    ("FORWARD", "forwardoption.cpp", "ForwardOptionData values[]",
     "type moneyness s q r start t v result tol"),
    ("QUANTO", "quantooption.cpp", "QuantoOptionData values[]",
     "type strike s q r t v fxr fxv corr result tol"),
    ("QUANTO_FORWARD", "quantooption.cpp", "QuantoForwardOptionData values[]",
     "type moneyness s q r start t v fxr fxv corr result tol"),
    ("QUANTO_BARRIER", "quantooption.cpp", "QuantoBarrierOptionData values[]",
     "barrier_type barrier rebate type s strike q r t v fxr fxv corr result tol"),
    ("QUANTO_DOUBLE_BARRIER", "quantooption.cpp", "QuantoDoubleBarrierOptionData values[]",
     "barrier_type barrier_lo barrier_hi rebate type s strike q r t v fxr fxv corr result tol"),
    # Haug p.180, cases 13-28. Two tables, one per binary payoff: the engine
    # reads the cash payoff off a CashOrNothingPayoff and the forward off an
    # AssetOrNothingPayoff, so they are two products rather than one table with
    # a flag. `cash` is 0.00 throughout the second and is not read there.
    ("BINARY_CASH", "binaryoption.cpp", "BinaryOptionData values[]",
     "barrierType barrier cash type strike s q r t v result tol", 0),
    ("BINARY_ASSET", "binaryoption.cpp", "BinaryOptionData values[]",
     "barrierType barrier cash type strike s q r t v result tol", 1),
    # 56 rows over three engines: StulzEngine for the minimum and maximum,
    # KirkEngine for the spread, and MCEuropeanBasketEngine for all of them.
    # The other tables in the file are American (MCAmericanBasketEngine, which
    # is Longstaff-Schwartz), largely commented out, or written in months
    # rather than years -- so this is the one that extracts.
    ("BASKET", "basketoption.cpp", "BasketOptionTwoData values[]",
     "basketType type strike s1 s2 q1 q2 r t v1 v2 rho result tol"),
    # Occurrence 1: the first table in the file is the put-call parity one,
    # which carries no published price -- it checks a relation rather than a
    # number, so there is nothing in it to price against.
    ("COMPOUND", "compoundoption.cpp", "CompoundOptionData values[]",
     "typeMother typeDaughter strikeMother strikeDaughter s q r tMother tDaughter v"
     " npv tol delta gamma vega theta", 1),
]

# Chooser has no table. Its two published values sit in the body of a
# BOOST_AUTO_TEST_CASE as local variables, and transcribing two numbers is
# still transcribing -- 6.1071 typed as 6.1017 is a benchmark that passes
# against the wrong number, which is the failure this file exists to prevent.
# So they are parsed too, by naming the C++ variable each field is read from.
# There is no struct to follow here, so the pattern carries the provenance;
# a pattern that stops matching, or starts matching twice, is a hard failure
# rather than a stale constant frozen into reference_tables.py.
#
# The dates are kept as the day offsets the C++ writes -- `today + 180`,
# `choosingDate + 210` -- rather than converted to year fractions here.
# Arithmetic on an extracted number is the same transcription by another name.
#
# (module name, source file, test case, [(field, pattern with one group)])
N = r"[-+0-9.eE]+"
# Two spellings of the same line: `spot = ext::make_shared<SimpleQuote>(50.0)`
# and the older `spot(new SimpleQuote(60.0))`. Both appear in the files below.
QUOTE = (r"\b{}\s*(?:=\s*ext::make_shared<SimpleQuote>|\(\s*new\s+SimpleQuote)"
         r"\(\s*({})\s*\)")
REAL = r"\bReal\s+{}\s*=\s*({})\s*;"
OFFSET = r"\bDate\s+{}\s*=\s*{}\s*\+\s*(\d+)\s*;"

SCALARS = [
    ("SIMPLE_CHOOSER", "chooseroption.cpp", "testAnalyticSimpleChooserEngine", [
        ("s", QUOTE.format("spot", N)),
        ("q", QUOTE.format("qRate", N)),
        ("r", QUOTE.format("rRate", N)),
        ("v", QUOTE.format("vol", N)),
        ("strike", REAL.format("strike", N)),
        ("choosing_days", OFFSET.format("choosingDate", "today")),
        ("exercise_days", OFFSET.format("exerciseDate", "today")),
        ("result", REAL.format("expected", N)),
        ("tol", REAL.format("tolerance", N)),
    ]),
    ("CLIQUET", "cliquetoption.cpp", "testValues", [
        ("s", QUOTE.format("spot", N)),
        ("q", QUOTE.format("qRate", N)),
        ("r", QUOTE.format("rRate", N)),
        ("v", QUOTE.format("vol", N)),
        ("type", r"Option::Type\s+type\s*=\s*(Option::\w+)\s*;"),
        ("moneyness", REAL.format("moneyness", N)),
        # A push_back rather than a named Date, so it gets its own pattern.
        ("reset_days", r"reset\.push_back\(\s*today\s*\+\s*(\d+)\s*\)"),
        ("maturity_days", OFFSET.format("maturity", "today")),
        ("result", REAL.format("expected", N)),
        ("tol", REAL.format("tolerance", N)),
    ]),
    ("COMPLEX_CHOOSER", "chooseroption.cpp", "testAnalyticComplexChooserEngine", [
        ("s", QUOTE.format("spot", N)),
        ("q", QUOTE.format("qRate", N)),
        ("r", QUOTE.format("rRate", N)),
        ("v", QUOTE.format("vol", N)),
        ("call_strike", REAL.format("callStrike", N)),
        ("put_strike", REAL.format("putStrike", N)),
        ("choosing_days", OFFSET.format("choosingDate", "today")),
        ("call_days", OFFSET.format("callExerciseDate", "choosingDate")),
        ("put_days", OFFSET.format("putExerciseDate", "choosingDate")),
        ("result", REAL.format("expected", N)),
        ("tol", REAL.format("tolerance", N)),
    ]),
]

# C++ spellings that are not numbers. Mapped to the strings the smoke scripts
# use, never to the proto enum values: those are deliberately not QuantLib's
# own numbers, and a table that hardcoded them would silently follow a
# renumbering (conventions.proto).
WORDS = {
    "Option::Call": "'call'",
    "Option::Put": "'put'",
    "Barrier::DownIn": "'down_in'",
    "Barrier::UpIn": "'up_in'",
    "Barrier::DownOut": "'down_out'",
    "Barrier::UpOut": "'up_out'",
    "DoubleBarrier::KnockIn": "'knock_in'",
    "DoubleBarrier::KnockOut": "'knock_out'",
    "DoubleBarrier::KIKO": "'kiko'",
    "DoubleBarrier::KOKI": "'koki'",
    "european": "'european'",
    "american": "'american'",
    "Exercise::European": "'european'",
    "Exercise::American": "'american'",
    "MinBasket": "'min'",
    "MaxBasket": "'max'",
    "SpreadBasket": "'spread'",
    "AverageBasket": "'average'",
    "true": "True",
    "false": "False",
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def balanced(text, i):
    """The text from `i` to the `}` that closes the `{` just before it."""
    depth = 1
    out = []
    while depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        out.append(c)
        i += 1
    return "".join(out)


def find_table(text, declaration, occurrence=0):
    """The brace-balanced body of the nth `declaration = { ... };`."""
    starts = [m.end() for m in re.finditer(re.escape(declaration) + r"\s*=\s*\{",
                                           text)]
    if len(starts) <= occurrence:
        raise SystemExit(f"no occurrence {occurrence} of {declaration!r}")
    return balanced(text, starts[occurrence])


def find_case(text, case):
    """The brace-balanced body of a BOOST_AUTO_TEST_CASE."""
    m = re.search(r"BOOST_AUTO_TEST_CASE\s*\(\s*" + re.escape(case)
                  + r"\s*\)\s*\{", text)
    if not m:
        raise SystemExit(f"no test case {case!r}")
    return balanced(text, m.end())


def parse_rows(body, fields):
    rows = []
    for match in re.finditer(r"\{([^{}]*)\}", body):
        cells = [c.strip() for c in match.group(1).split(",")]
        cells = [c for c in cells if c != ""]
        if len(cells) != len(fields):
            raise SystemExit(
                f"row has {len(cells)} cells, expected {len(fields)}: {cells}")
        rows.append([WORDS.get(c, c) for c in cells])
    return rows


def parse_scalars(body, fields):
    """One row, read variable by variable out of a test case body."""
    row = []
    for name, pattern in fields:
        found = re.findall(pattern, body)
        if len(found) != 1:
            raise SystemExit(
                f"{name}: {len(found)} matches for {pattern!r}, expected 1")
        row.append(WORDS.get(found[0], found[0]))
    return [row]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    root = Path(sys.argv[1]).expanduser() / "test-suite"

    print('"""QuantLib\'s own reference tables, extracted from its test suite.')
    print()
    print("GENERATED by test/extract_tables.py -- do not edit by hand.")
    print()
    print("Each row is a dict keyed by the C++ struct's own member names, so a")
    print("row can be read against the source without counting commas. `result`")
    print("is the published value and `tol` the tolerance the C++ test uses;")
    print("neither is tightened here, because those numbers are what is under")
    print("test. The chooser cases have no struct: they are single rows read")
    print("variable by variable out of the test case body, and the field names")
    print('are this benchmark\'s own.')
    print('"""')
    print()

    for spec in TABLES:
        name, filename, declaration, fields = spec[:4]
        occurrence = spec[4] if len(spec) > 4 else 0
        fields = fields.split()

        text = strip_comments((root / filename).read_text())
        rows = parse_rows(find_table(text, declaration, occurrence), fields)
        if not rows:
            # Comments are stripped first, so a table upstream has commented
            # out parses as an empty one -- and an empty table is a benchmark
            # that checks nothing and passes.
            raise SystemExit(f"{name}: {declaration} in {filename} has no rows")

        print(f"# {filename}: {declaration}"
              + (f" [{occurrence}]" if occurrence else ""))
        print(f"{name} = [")
        for row in rows:
            pairs = ", ".join(f"{f}={v}" for f, v in zip(fields, row))
            print(f"    dict({pairs}),")
        print("]")
        print()

    for name, filename, case, fields in SCALARS:
        text = strip_comments((root / filename).read_text())
        rows = parse_scalars(find_case(text, case), fields)
        if not rows:
            raise SystemExit(f"{name}: {case}() in {filename} has no rows")

        print(f"# {filename}: {case}()")
        print(f"{name} = [")
        for row in rows:
            pairs = ", ".join(f"{f}={v}" for (f, _), v in zip(fields, row))
            print(f"    dict({pairs}),")
        print("]")
        print()

    print("TABLES = {")
    for spec in TABLES + SCALARS:
        print(f"    {spec[0]!r}: {spec[0]},")
    print("}")


if __name__ == "__main__":
    main()
