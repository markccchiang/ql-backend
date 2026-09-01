"""Drives the quanto schema with QuantLib's own reference data.

Every table below is copied from QuantLib's test-suite/quantooption.cpp — the
same rows, the same expected values, the same tolerances. Each row is checked
twice:

  1. the ClientFrame built from it is serialized, parsed back, and compared
     field by field against the row, so the protobuf provably carries the
     test's inputs and nothing else;
  2. the PriceResult that comes back over the WebSocket is compared against
     the row's expected value at the row's tolerance.

The C++ test builds its market by hand with flatRate()/flatVol() and moves
SimpleQuotes between rows. This does the same over the wire: one session, one
object graph, an UpdateMarket per row.
"""

import asyncio, math, sys, time
from datetime import date, timedelta

sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else "pb")
import websockets
from quantlib.v1 import envelope_pb2 as E
from quantlib.v1 import conventions_pb2 as C

URL = "ws://127.0.0.1:9111"  # matches ./build/qlserviced --port 9111

# The C++ test uses Date::todaysDate(). Any date does: every term structure is
# Actual/360 and anchored at the evaluation date, so a maturity of exactly
# timeToDays(t) days is t years whatever "today" is. Pinning it makes the run
# reproducible.
TODAY = date(2026, 9, 1)

CALL, PUT = E.VanillaOption.CALL, E.VanillaOption.PUT


def time_to_days(t, days_per_year=360):
    """test-suite/utilities.hpp:141 — Integer(std::lround(t * daysPerYear))."""
    return int(math.floor(t * days_per_year + 0.5))


def maturity(t):
    return TODAY + timedelta(days=time_to_days(t))


# ---------------------------------------------------------------------------
# The reference data, verbatim from test-suite/quantooption.cpp
# ---------------------------------------------------------------------------

# testValues, "Option pricing formulas", E.G. Haug, McGraw-Hill 1998, pag 105-106
QUANTO_VALUES = [
    # type, strike, spot, q, r, t, vol, fxr, fxv, corr, result, tol
    dict(type=CALL, strike=105.0, s=100.0, q=0.04, r=0.08, t=0.5, v=0.2,
         fxr=0.05, fxv=0.10, corr=0.3, result=5.3280 / 1.5, tol=1.0e-4),
    dict(type=PUT, strike=105.0, s=100.0, q=0.04, r=0.08, t=0.5, v=0.2,
         fxr=0.05, fxv=0.10, corr=0.3, result=8.1636, tol=1.0e-4),
]

# testForwardValues. reset == 0 degrades to a plain quanto option and must
# reproduce the two values above; reset != 0 was checked against FinCAD 7.
FORWARD_VALUES = [
    dict(type=CALL, moneyness=1.05, s=100.0, q=0.04, r=0.08, start=0.00, t=0.5, v=0.20,
         fxr=0.05, fxv=0.10, corr=0.3, result=5.3280 / 1.5, tol=1.0e-4),
    dict(type=PUT, moneyness=1.05, s=100.0, q=0.04, r=0.08, start=0.00, t=0.5, v=0.20,
         fxr=0.05, fxv=0.10, corr=0.3, result=8.1636, tol=1.0e-4),
    dict(type=CALL, moneyness=1.05, s=100.0, q=0.04, r=0.08, start=0.25, t=0.5, v=0.20,
         fxr=0.05, fxv=0.10, corr=0.3, result=2.0171, tol=1.0e-4),
    dict(type=PUT, moneyness=1.05, s=100.0, q=0.04, r=0.08, start=0.25, t=0.5, v=0.20,
         fxr=0.05, fxv=0.10, corr=0.3, result=6.7296, tol=1.0e-4),
]

# testForwardPerformanceValues — roughly one hundredth of the above, because
# the payoff is divided by the spot at reset.
FORWARD_PERFORMANCE_VALUES = [
    dict(type=CALL, moneyness=1.05, s=100.0, q=0.04, r=0.08, start=0.00, t=0.5, v=0.20,
         fxr=0.05, fxv=0.10, corr=0.3, result=5.3280 / 150, tol=1.0e-4),
    dict(type=PUT, moneyness=1.05, s=100.0, q=0.04, r=0.08, start=0.00, t=0.5, v=0.20,
         fxr=0.05, fxv=0.10, corr=0.3, result=0.0816, tol=1.0e-4),
    dict(type=CALL, moneyness=1.05, s=100.0, q=0.04, r=0.08, start=0.25, t=0.5, v=0.20,
         fxr=0.05, fxv=0.10, corr=0.3, result=0.0201, tol=1.0e-4),
    dict(type=PUT, moneyness=1.05, s=100.0, q=0.04, r=0.08, start=0.25, t=0.5, v=0.20,
         fxr=0.05, fxv=0.10, corr=0.3, result=0.0672, tol=1.0e-4),
]

# testBarrierValues. The C++ test carries a "TODO: bench against an existing
# prop calculator" and a tolerance of 0.5 to match; both are reproduced here
# rather than tightened, so this checks the wiring, not the analytics.
BARRIER_VALUES = [
    dict(barrier_type=E.DOWN_OUT, barrier=95.0, rebate=3.0, type=CALL, s=100.0, strike=90.0,
         q=0.04, r=0.0212, t=0.50, v=0.25, fxr=0.05, fxv=0.2, corr=0.3, result=8.247, tol=0.5),
    dict(barrier_type=E.DOWN_OUT, barrier=95.0, rebate=3.0, type=PUT, s=100.0, strike=90.0,
         q=0.04, r=0.0212, t=0.50, v=0.25, fxr=0.05, fxv=0.2, corr=0.3, result=2.274, tol=0.5),
    dict(barrier_type=E.DOWN_IN, barrier=95.0, rebate=0.0, type=PUT, s=100.0, strike=90.0,
         q=0.04, r=0.0212, t=0.50, v=0.25, fxr=0.05, fxv=0.2, corr=0.3, result=2.85, tol=0.5),
]

# testDoubleBarrierValues
DOUBLE_BARRIER_VALUES = [
    dict(barrier_type=E.KNOCK_OUT, lo=50.0, hi=150.0, rebate=0.0, type=CALL, s=100.0, strike=100.0,
         q=0.00, r=0.1, t=0.25, v=0.15, fxr=0.05, fxv=0.2, corr=0.3, result=3.4623, tol=1.0e-4),
    dict(barrier_type=E.KNOCK_OUT, lo=90.0, hi=110.0, rebate=0.0, type=CALL, s=100.0, strike=100.0,
         q=0.00, r=0.1, t=0.50, v=0.15, fxr=0.05, fxv=0.2, corr=0.3, result=0.5236, tol=1.0e-4),
    dict(barrier_type=E.KNOCK_OUT, lo=90.0, hi=110.0, rebate=0.0, type=PUT, s=100.0, strike=100.0,
         q=0.00, r=0.1, t=0.25, v=0.15, fxr=0.05, fxv=0.2, corr=0.3, result=1.1320, tol=1.0e-4),
    dict(barrier_type=E.KNOCK_IN, lo=80.0, hi=120.0, rebate=0.0, type=CALL, s=100.0, strike=102.0,
         q=0.00, r=0.1, t=0.25, v=0.25, fxr=0.05, fxv=0.2, corr=0.3, result=2.6313, tol=1.0e-4),
    dict(barrier_type=E.KNOCK_IN, lo=80.0, hi=120.0, rebate=0.0, type=CALL, s=100.0, strike=102.0,
         q=0.00, r=0.1, t=0.50, v=0.15, fxr=0.05, fxv=0.2, corr=0.3, result=1.9305, tol=1.0e-4),
]

# ---------------------------------------------------------------------------
# Frames
# ---------------------------------------------------------------------------

# The seven quotes the C++ test holds as SimpleQuotes and moves per row.
QUOTES = ["SPOT", "VOL", "Q", "R", "FXR", "FXVOL", "CORR"]

# Every result the quanto engines can produce, so one round trip carries the
# whole panel a frontend would plot.
ALL_RESULTS = [E.RESULT_KIND_DELTA, E.RESULT_KIND_GAMMA, E.RESULT_KIND_VEGA,
               E.RESULT_KIND_THETA, E.RESULT_KIND_RHO, E.RESULT_KIND_DIVIDEND_RHO,
               E.RESULT_KIND_QRHO, E.RESULT_KIND_QVEGA, E.RESULT_KIND_QLAMBDA]

rid = 0


def next_id():
    global rid
    rid += 1
    return rid


def flat_curve(open_session, curve_id, quote_id):
    """flatRate(today, quote, Actual360()) — FlatForward, continuous, annual."""
    c = open_session.curves.add()
    c.curve_id = curve_id
    c.day_counter.family = C.DayCounter.ACTUAL_360
    c.flat.quote_id = quote_id
    c.flat.compounding = C.CONTINUOUS
    c.flat.frequency = C.ANNUAL


def open_session():
    f = E.ClientFrame(request_id=next_id())
    o = f.open_session
    o.evaluation_date.iso = TODAY.isoformat()
    # Values are irrelevant: every row overwrites all seven before pricing.
    for qid in QUOTES:
        o.quotes.add(quote_id=qid, value=0.0)
    flat_curve(o, "DIV", "Q")
    flat_curve(o, "RF", "R")
    flat_curve(o, "FXRF", "FXR")
    return f


def fill_market(market):
    market.spot_quote_id = "SPOT"
    market.vol_quote_id = "VOL"
    market.dividend_curve_id = "DIV"
    market.risk_free_curve_id = "RF"
    market.fx_risk_free_curve_id = "FXRF"
    market.fx_vol_quote_id = "FXVOL"
    market.correlation_quote_id = "CORR"
    # flatVol(today, vol, Actual360()) in the C++ test.
    market.vol_day_counter.family = C.DayCounter.ACTUAL_360


def set_market(session, row):
    """One UpdateMarket standing in for the row's seven setValue() calls."""
    f = E.ClientFrame(request_id=next_id(), session_id=session)
    for qid, value in [("SPOT", row["s"]), ("VOL", row["v"]), ("Q", row["q"]),
                       ("R", row["r"]), ("FXR", row["fxr"]), ("FXVOL", row["fxv"]),
                       ("CORR", row["corr"])]:
        f.update_market.updates.add(quote_id=qid, value=value)
    return f


def quanto_frame(session, row):
    f = E.ClientFrame(request_id=next_id(), session_id=session)
    o = f.price.instrument.quanto_vanilla_option
    o.type = row["type"]
    o.strike = row["strike"]
    o.expiry.iso = maturity(row["t"]).isoformat()
    fill_market(o.market)
    f.price.engine.kind = E.Engine.ANALYTIC
    f.price.results.extend(ALL_RESULTS)
    return f


def forward_frame(session, row, performance):
    f = E.ClientFrame(request_id=next_id(), session_id=session)
    o = f.price.instrument.quanto_forward_vanilla_option
    o.type = row["type"]
    o.moneyness = row["moneyness"]
    o.reset.iso = maturity(row["start"]).isoformat()
    o.expiry.iso = maturity(row["t"]).isoformat()
    o.performance = performance
    fill_market(o.market)
    f.price.engine.kind = E.Engine.ANALYTIC
    f.price.results.extend(ALL_RESULTS)
    return f


def barrier_frame(session, row):
    f = E.ClientFrame(request_id=next_id(), session_id=session)
    o = f.price.instrument.quanto_barrier_option
    o.type = row["type"]
    o.strike = row["strike"]
    o.expiry.iso = maturity(row["t"]).isoformat()
    o.barrier_type = row["barrier_type"]
    o.barrier = row["barrier"]
    o.rebate = row["rebate"]
    fill_market(o.market)
    f.price.engine.kind = E.Engine.ANALYTIC
    f.price.results.extend(ALL_RESULTS)
    return f


def double_barrier_frame(session, row):
    f = E.ClientFrame(request_id=next_id(), session_id=session)
    o = f.price.instrument.quanto_double_barrier_option
    o.type = row["type"]
    o.strike = row["strike"]
    o.expiry.iso = maturity(row["t"]).isoformat()
    o.barrier_type = row["barrier_type"]
    o.barrier_low = row["lo"]
    o.barrier_high = row["hi"]
    o.rebate = row["rebate"]
    fill_market(o.market)
    f.price.engine.kind = E.Engine.ANALYTIC
    f.price.results.extend(ALL_RESULTS)
    return f


# ---------------------------------------------------------------------------
# Input verification: what the wire carries must be what the table says
# ---------------------------------------------------------------------------

def roundtrip(frame):
    """Serialize and parse back, so the comparison is against wire bytes."""
    out = E.ClientFrame()
    out.ParseFromString(frame.SerializeToString())
    return out


def market_inputs(update_frame):
    """The seven quote writes, keyed by id."""
    return {u.quote_id: u.value for u in update_frame.update_market.updates}


def expected_market(row):
    return {"SPOT": row["s"], "VOL": row["v"], "Q": row["q"], "R": row["r"],
            "FXR": row["fxr"], "FXVOL": row["fxv"], "CORR": row["corr"]}


def instrument_inputs(price_frame):
    """Everything the instrument message says, in the table's own vocabulary."""
    i = price_frame.price.instrument
    which = i.WhichOneof("kind")
    o = getattr(i, which)
    got = {"type": o.type, "expiry": o.expiry.iso}
    if which == "quanto_vanilla_option":
        got["strike"] = o.strike
    elif which == "quanto_forward_vanilla_option":
        got.update(moneyness=o.moneyness, reset=o.reset.iso, performance=o.performance)
    elif which == "quanto_barrier_option":
        got.update(strike=o.strike, barrier_type=o.barrier_type, barrier=o.barrier,
                   rebate=o.rebate)
    elif which == "quanto_double_barrier_option":
        got.update(strike=o.strike, barrier_type=o.barrier_type, lo=o.barrier_low,
                   hi=o.barrier_high, rebate=o.rebate)
    return got


def expected_instrument(row, kind, performance=None):
    exp = {"type": row["type"], "expiry": maturity(row["t"]).isoformat()}
    if kind == "quanto_vanilla_option":
        exp["strike"] = row["strike"]
    elif kind == "quanto_forward_vanilla_option":
        exp.update(moneyness=row["moneyness"], reset=maturity(row["start"]).isoformat(),
                   performance=performance)
    elif kind == "quanto_barrier_option":
        exp.update(strike=row["strike"], barrier_type=row["barrier_type"],
                   barrier=row["barrier"], rebate=row["rebate"])
    elif kind == "quanto_double_barrier_option":
        exp.update(strike=row["strike"], barrier_type=row["barrier_type"], lo=row["lo"],
                   hi=row["hi"], rebate=row["rebate"])
    return exp


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------

async def collect(ws, want, timeout=30.0):
    deadline = time.time() + timeout
    while True:
        raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.time()))
        f = E.ServerFrame()
        f.ParseFromString(raw)
        if f.HasField("progress"):
            continue
        if f.request_id == want and f.terminal:
            return f


async def send(ws, frame):
    await ws.send(frame.SerializeToString())
    return await collect(ws, frame.request_id)


async def main():
    failures = []

    def check(name, ok, detail=""):
        print(("  PASS  " if ok else "  FAIL  ") + name + ("  " + detail if detail else ""))
        if not ok:
            failures.append(name)

    async with websockets.connect(URL, max_size=8 << 20) as ws:
        f = open_session()
        r = await send(ws, f)
        if not r.HasField("session_opened"):
            print("open failed:", r)
            return 1
        sid = r.session_id
        check("OpenSession with three flat curves", True, f"session={sid}")

        async def run(label, rows, build, kind, performance=None):
            for n, row in enumerate(rows):
                tag = f"{label}[{n}]"

                update = set_market(sid, row)
                got, exp = market_inputs(roundtrip(update)), expected_market(row)
                check(f"{tag} market on the wire", got == exp,
                      "" if got == exp else f"{got} != {exp}")

                frame = build(sid, row) if performance is None else build(sid, row, performance)
                got = instrument_inputs(roundtrip(frame))
                exp = expected_instrument(row, kind, performance)
                check(f"{tag} instrument on the wire", got == exp,
                      "" if got == exp else f"{got} != {exp}")

                r = await send(ws, update)
                if not r.HasField("ack"):
                    check(f"{tag} market accepted", False, str(r.error))
                    continue

                r = await send(ws, frame)
                if not r.HasField("price_result"):
                    check(f"{tag} priced", False, f"{r.error.code} {r.error.message!r} "
                                                  f"field={r.error.field_path!r}")
                    continue

                npv = r.price_result.npv
                err = abs(npv - row["result"])
                greeks = " ".join(f"{k}={v:.4g}" for k, v in
                                  sorted(r.price_result.results.items()))
                check(f"{tag} npv", err <= row["tol"],
                      f"npv={npv:.6f} expected={row['result']:.6f} err={err:.2e} "
                      f"tol={row['tol']:g}" + (f" | {greeks}" if greeks else ""))

        await run("quanto", QUANTO_VALUES, quanto_frame, "quanto_vanilla_option")
        await run("forward", FORWARD_VALUES, forward_frame,
                  "quanto_forward_vanilla_option", performance=False)
        await run("forward-performance", FORWARD_PERFORMANCE_VALUES, forward_frame,
                  "quanto_forward_vanilla_option", performance=True)
        await run("barrier", BARRIER_VALUES, barrier_frame, "quanto_barrier_option")
        await run("double-barrier", DOUBLE_BARRIER_VALUES, double_barrier_frame,
                  "quanto_double_barrier_option")

        # The quanto greeks are the point of the wrapper: if the analytic
        # engine is reached at all, all three are present. Their absence would
        # mean a plain engine had been substituted.
        r = await send(ws, set_market(sid, QUANTO_VALUES[0]))
        r = await send(ws, quanto_frame(sid, QUANTO_VALUES[0]))
        got = r.price_result.results
        check("quanto greeks returned", all(k in got for k in ("qrho", "qvega", "qlambda")),
              " ".join(f"{k}={got[k]:.6f}" for k in ("qrho", "qvega", "qlambda") if k in got))
        base = r.price_result.npv

        # Correlation reaches the engine as a Handle<Quote>, so moving it alone
        # must reprice without rebuilding anything. A price that did not move
        # would mean the value had been copied at construction (DESIGN §5).
        f = E.ClientFrame(request_id=next_id(), session_id=sid)
        f.update_market.updates.add(quote_id="CORR", value=-0.9)
        await send(ws, f)
        r = await send(ws, quanto_frame(sid, QUANTO_VALUES[0]))
        moved = r.price_result.npv
        check("correlation is live", abs(moved - base) > 1.0e-6,
              f"npv={moved:.6f} (was {base:.6f} at corr=0.3)")

        # And it is bounded: outside [-1, 1] there is no such market.
        f = E.ClientFrame(request_id=next_id(), session_id=sid)
        f.update_market.updates.add(quote_id="CORR", value=1.5)
        await send(ws, f)
        r = await send(ws, quanto_frame(sid, QUANTO_VALUES[0]))
        check("correlation outside [-1, 1] rejected",
              r.HasField("error") and r.error.code == E.Error.INVALID_ARGUMENT,
              f"field={r.error.field_path!r}")

        # Engine kinds other than ANALYTIC are refused rather than defaulted.
        f = quanto_frame(sid, QUANTO_VALUES[0])
        f.price.engine.kind = E.Engine.MONTE_CARLO
        r = await send(ws, f)
        check("non-analytic engine rejected",
              r.HasField("error") and r.error.field_path == "engine.kind",
              f"code={r.error.code} field={r.error.field_path!r}")

        f = quanto_frame(sid, QUANTO_VALUES[0])
        f.price.engine.ClearField("kind")
        r = await send(ws, f)
        check("unset engine kind rejected",
              r.HasField("error") and r.error.code == E.Error.UNSPECIFIED_ENUM,
              f"code={r.error.code} field={r.error.field_path!r}")

        f = E.ClientFrame(request_id=next_id(), session_id=sid)
        f.close_session.SetInParent()
        r = await send(ws, f)
        check("CloseSession ack", r.HasField("ack"))

    print("\n" + ("ALL PASS" if not failures else
                  f"{len(failures)} FAILURES: {failures}"))
    return 1 if failures else 0


sys.exit(asyncio.run(main()))
