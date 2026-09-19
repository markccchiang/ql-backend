"""End-to-end checks for the v2 schema, against QuantLib's own reference tables.

Every number checked here comes from `reference_tables.py`, which is generated
from QuantLib's test-suite sources by `extract_tables.py` rather than typed in.
The tolerances are the ones the C++ tests use, and are not tightened: those
published values are what is under test.

One session, one live graph. Each row is a quote write plus a price, which is
the shape a frontend actually drives — and the reason the backend is stateful
at all.

    ./build/ql-backend --port 9111 &
    /tmp/qlvenv/bin/python test/smoke_v2.py /tmp/qlpb2
"""

import asyncio
import json
import math
import os
import sys
import time
import urllib.request
from datetime import date, timedelta

sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else "pb")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import websockets
from quantlib.v1 import conventions_pb2 as C
from quantlib.v2 import engine_pb2 as EN
from quantlib.v2 import envelope_pb2 as E
from quantlib.v2 import instrument_pb2 as I
from quantlib.v2 import market_pb2 as M
from quantlib.v2 import results_pb2 as R

import reference_tables as T

URL = "ws://127.0.0.1:9111"

# The service may have been started with --token-file, in which case every
# upgrade has to present the secret. A header rather than a subprotocol: only
# a browser is unable to set one, and this is not a browser.
TOKEN = os.environ.get("QL_TOKEN", "")


def connect(*args, **kwargs):
    """websockets.connect, carrying the token when there is one."""
    if TOKEN:
        headers = dict(kwargs.pop("additional_headers", None) or {})
        headers["Authorization"] = "Bearer " + TOKEN
        kwargs["additional_headers"] = headers
    return websockets.connect(*args, **kwargs)
HEALTH = "http://127.0.0.1:9111/healthz"

# The C++ tests all price against Actual/360 with the expiry a whole number of
# 360ths of a year away (test-suite/utilities.hpp:141). Nothing here has a
# calendar, so a fixed evaluation date is as good as today's and reproduces.
TODAY = date(2026, 9, 1)

# The engine limits in src/session/capabilities.hpp. A request may reach each
# one; one past it is refused naming the field.
MAX_LATTICE_STEPS = 10000
MAX_FD_TIME_STEPS = 10000
MAX_FD_ASSET_STEPS = 10000
MAX_MC_SAMPLES = 100_000_000
MAX_MC_TIME_STEPS_PER_YEAR = 10000
MAX_MC_BATCHES = 10000
MAX_IMPLIED_VOLATILITY_EVALUATIONS = 1000

# A sweep of a lattice at the step limit, long enough to be running when a
# cancel arrives -- about 3.5 s on the laptop this was measured on. The cancel
# checks use it as the request a cancel aimed elsewhere must not touch: a stop
# that reached it would show, because a sweep stops between points and says
# how far it got.
LONG_SWEEP_POINTS = 200


def days(t):
    """timeToDays(t, 360) — Integer(std::lround(t * 360))."""
    return int(math.floor(t * 360 + 0.5))


def expiry(t):
    return (TODAY + timedelta(days=days(t))).isoformat()


def on_day(n):
    """chooseroption.cpp writes its dates as day offsets, not year fractions."""
    return (TODAY + timedelta(days=n)).isoformat()


rid = 0


def next_id():
    global rid
    rid += 1
    return rid


def act360(msg):
    msg.family = C.DayCounter.ACTUAL_360


# ---------------------------------------------------------------------------
# One market, rewritten per row
# ---------------------------------------------------------------------------

QUOTES = {
    "S": 100.0,   # spot
    "Q": 0.0,     # dividend yield
    "R": 0.05,    # risk-free
    "V": 0.20,    # volatility
    "FXR": 0.05,  # foreign risk-free, quanto only
    "FXV": 0.20,  # FX volatility
    "CORR": 0.30,
    "S2": 100.0,  # the second asset, basket only
    "Q2": 0.0,
    "V2": 0.20,
    "RHO": 0.30,  # the off-diagonal of CORRM, live so a row can write it
}


def open_session():
    f = E.ClientFrame(request_id=next_id())
    o = f.open_session
    o.evaluation_date.iso = TODAY.isoformat()
    o.client_label = "smoke_v2"

    for qid, v in QUOTES.items():
        m = o.market.add()
        m.id = qid
        m.quote.value = v
        m.quote.unit = M.Quote.UNIT_ABSOLUTE if qid == "S" else M.Quote.UNIT_RATE

    # Flat curves on live quotes, which is what makes a bump move the price.
    # flatRate() in the C++ helper is FlatForward(today, handle, dc) with
    # Continuous/Annual defaults (test-suite/utilities.hpp).
    for cid, qid in [("RC", "R"), ("QC", "Q"), ("FXRC", "FXR")]:
        m = o.market.add()
        m.id = cid
        act360(m.yield_curve.day_counter)
        m.yield_curve.flat.rate.quote_id = qid
        m.yield_curve.flat.compounding = C.CONTINUOUS
        m.yield_curve.flat.frequency = C.ANNUAL

    # The same dividend yield counting days the other way. Nothing prices
    # against it; it is here so the chooser's one-time-axis check has something
    # to refuse, which is otherwise unreachable from a market this uniform.
    m = o.market.add()
    m.id = "QC365"
    m.yield_curve.day_counter.family = C.DayCounter.ACTUAL_365_FIXED
    m.yield_curve.flat.rate.quote_id = "Q"
    m.yield_curve.flat.compounding = C.CONTINUOUS
    m.yield_curve.flat.frequency = C.ANNUAL

    for vid, qid in [("VOL", "V"), ("FXVOL", "FXV"), ("VOL2", "V2")]:
        m = o.market.add()
        m.id = vid
        act360(m.volatility.day_counter)
        m.volatility.constant.volatility.quote_id = qid

    # The second asset's dividend curve, and the correlation matrix the two
    # index on by label. The off-diagonal is a quote id so a reference row can
    # write it like any other number; the diagonal is fixed, because a
    # correlation matrix whose diagonal could be dragged off 1 is not one.
    m = o.market.add()
    m.id = "QC2"
    act360(m.yield_curve.day_counter)
    m.yield_curve.flat.rate.quote_id = "Q2"
    m.yield_curve.flat.compounding = C.CONTINUOUS
    m.yield_curve.flat.frequency = C.ANNUAL

    m = o.market.add()
    m.id = "CORRM"
    m.correlation.labels.extend(["A", "B"])
    for i in range(2):
        for j in range(2):
            v = m.correlation.values.add()
            if i == j:
                v.fixed = 1.0
            else:
                v.quote_id = "RHO"

    return f


def set_market(sid, **values):
    f = E.ClientFrame(request_id=next_id(), session_id=sid)
    for qid, v in values.items():
        f.update_market.quotes.add(quote_id=qid, value=v)
    return f


def row_market(row):
    """The quote writes one reference row implies."""
    if "s1" in row:
        # A two-asset row: two spots, two dividend yields, two volatilities and
        # the correlation between them, against one shared risk-free curve --
        # which is how basketoption.cpp builds it.
        return {"S": row["s1"], "S2": row["s2"], "Q": row["q1"], "Q2": row["q2"],
                "R": row["r"], "V": row["v1"], "V2": row["v2"], "RHO": row["rho"]}
    out = {"S": row["s"], "R": row["r"], "V": row["v"]}
    if "q" in row:
        out["Q"] = row["q"]
    if "fxr" in row:
        out.update(FXR=row["fxr"], FXV=row["fxv"], CORR=row["corr"])
    return out


def underlying(opt, quanto=False):
    u = opt.underlyings.add()
    u.spot_quote_id = "S"
    u.discount_curve_id = "RC"
    u.dividend_curve_id = "QC"
    u.volatility_id = "VOL"
    u.process = I.Underlying.PROCESS_BLACK_SCHOLES_MERTON
    if quanto:
        opt.quanto.fx_risk_free_curve_id = "FXRC"
        opt.quanto.fx_volatility_id = "FXVOL"
        opt.quanto.correlation_id = "CORR"
    return u


OPTION_TYPE = {"call": I.Payoff.OPTION_TYPE_CALL, "put": I.Payoff.OPTION_TYPE_PUT}
BARRIER_TYPE = {
    "down_in": I.Barrier.TYPE_DOWN_IN,
    "up_in": I.Barrier.TYPE_UP_IN,
    "down_out": I.Barrier.TYPE_DOWN_OUT,
    "up_out": I.Barrier.TYPE_UP_OUT,
}
DOUBLE_BARRIER_TYPE = {
    "knock_in": I.DoubleBarrier.TYPE_KNOCK_IN,
    "knock_out": I.DoubleBarrier.TYPE_KNOCK_OUT,
    "kiko": I.DoubleBarrier.TYPE_KIKO,
    "koki": I.DoubleBarrier.TYPE_KOKI,
}


def base_frame(sid):
    f = E.ClientFrame(request_id=next_id(), session_id=sid)
    return f, f.price.instrument.option


def set_exercise(opt, kind, t):
    opt.exercise.dates.add().iso = expiry(t)
    if kind == "american":
        opt.exercise.type = I.Exercise.TYPE_AMERICAN
        # payoff_at_expiry is a Flag, not a bool: unset would be indistinguishable
        # from false and it changes the price (DESIGN §6.3).
        opt.exercise.payoff_at_expiry = M.FLAG_FALSE
    else:
        opt.exercise.type = I.Exercise.TYPE_EUROPEAN


def vanilla_frame(sid, row, method=EN.Engine.METHOD_ANALYTIC, exercise="european",
                  approximation=None, steps=0, preset=None, mc=None, quanto=False):
    f, opt = base_frame(sid)
    opt.payoff.type = OPTION_TYPE[row["type"]]
    opt.payoff.plain.strike = row["strike"]
    set_exercise(opt, exercise, row["t"])
    underlying(opt, quanto)
    opt.vanilla.SetInParent()

    eng = f.price.engine
    eng.method = method
    eng.model = EN.Engine.MODEL_BLACK_SCHOLES
    if approximation is not None:
        eng.analytic.approximation = approximation
    if steps:
        eng.lattice.tree = EN.LatticeParameters.TREE_COX_ROSS_RUBINSTEIN
        eng.lattice.steps = steps
    if preset is not None:
        eng.fd.preset = preset
    if mc is not None:
        eng.mc.seed, eng.mc.samples = mc
        eng.mc.rng = EN.McParameters.RNG_PSEUDO_RANDOM
    return f


def barrier_frame(sid, row, method=EN.Engine.METHOD_ANALYTIC, preset=None, steps=0,
                  quanto=False):
    f, opt = base_frame(sid)
    opt.payoff.type = OPTION_TYPE[row["type"]]
    opt.payoff.plain.strike = row["strike"]
    set_exercise(opt, row.get("exercise", "european"), row["t"])
    underlying(opt, quanto)
    opt.barrier.type = BARRIER_TYPE[row["barrier_type"]]
    opt.barrier.level = row["barrier"]
    opt.barrier.rebate = row["rebate"]

    eng = f.price.engine
    eng.method = method
    eng.model = EN.Engine.MODEL_BLACK_SCHOLES
    if preset is not None:
        eng.fd.preset = preset
    if steps:
        eng.lattice.tree = EN.LatticeParameters.TREE_COX_ROSS_RUBINSTEIN
        eng.lattice.steps = steps
    return f


def binary_frame(sid, row, payoff="cash_or_nothing", rebate=0.0,
                 exercise="american", at_expiry=True,
                 method=EN.Engine.METHOD_ANALYTIC):
    """A knock digital: a barrier carrying a binary payoff.

    There is no digital-knock instrument in QuantLib; this shape is the
    product. AnalyticBinaryBarrierEngine casts the exercise to an
    AmericanExercise and requires payoffAtExpiry, which is what
    binaryoption.cpp builds.
    """
    f, opt = base_frame(sid)
    opt.payoff.type = OPTION_TYPE[row["type"]]
    if payoff == "cash_or_nothing":
        opt.payoff.cash_or_nothing.strike = row["strike"]
        opt.payoff.cash_or_nothing.cash_payoff = row["cash"]
    else:
        opt.payoff.asset_or_nothing.strike = row["strike"]

    opt.exercise.dates.add().iso = expiry(row["t"])
    if exercise == "american":
        opt.exercise.type = I.Exercise.TYPE_AMERICAN
        opt.exercise.payoff_at_expiry = M.FLAG_TRUE if at_expiry else M.FLAG_FALSE
    else:
        opt.exercise.type = I.Exercise.TYPE_EUROPEAN

    underlying(opt)
    opt.barrier.type = BARRIER_TYPE[row["barrierType"]]
    opt.barrier.level = row["barrier"]
    opt.barrier.rebate = rebate
    f.price.engine.method = method
    if method == EN.Engine.METHOD_LATTICE:
        eng = f.price.engine
        eng.lattice.tree = EN.LatticeParameters.TREE_COX_ROSS_RUBINSTEIN
        eng.lattice.steps = 200
    return f


def double_barrier_frame(sid, row, quanto=False):
    f, opt = base_frame(sid)
    opt.payoff.type = OPTION_TYPE[row["type"]]
    opt.payoff.plain.strike = row["strike"]
    set_exercise(opt, "european", row["t"])
    underlying(opt, quanto)
    opt.double_barrier.type = DOUBLE_BARRIER_TYPE[row["barrier_type"]]
    opt.double_barrier.lower = row["barrier_lo"]
    opt.double_barrier.upper = row["barrier_hi"]
    opt.double_barrier.rebate = row["rebate"]
    f.price.engine.method = EN.Engine.METHOD_ANALYTIC
    return f


def compound_frame(sid, row, daughter_payoff="plain", mother_fields=False):
    """An option on an option.

    The mother is the option's own payoff and exercise -- CompoundOption hands
    those two straight to OneAssetOption -- so they go where every other style
    puts them, and the style block carries only the option written on.
    """
    f, opt = base_frame(sid)
    opt.payoff.type = OPTION_TYPE[row["typeMother"]]
    opt.payoff.plain.strike = row["strikeMother"]
    set_exercise(opt, "european", row["tMother"])
    underlying(opt)

    c = opt.compound
    c.daughter_payoff.type = OPTION_TYPE[row["typeDaughter"]]
    if daughter_payoff == "plain":
        c.daughter_payoff.plain.strike = row["strikeDaughter"]
    else:
        c.daughter_payoff.cash_or_nothing.strike = row["strikeDaughter"]
        c.daughter_payoff.cash_or_nothing.cash_payoff = 1.0
    c.daughter_exercise.type = I.Exercise.TYPE_EUROPEAN
    c.daughter_exercise.dates.add().iso = expiry(row["tDaughter"])
    if mother_fields:
        c.mother_payoff.type = OPTION_TYPE[row["typeMother"]]
        c.mother_payoff.plain.strike = row["strikeMother"]

    f.price.engine.method = EN.Engine.METHOD_ANALYTIC
    return f


def simple_chooser_frame(sid, row, exercise="european", payoff_type=None,
                         call_strike=False, put_strike=0.0, dividend_curve="QC",
                         method=EN.Engine.METHOD_ANALYTIC):
    """One choice date, one strike, one expiry.

    SimpleChooserOption builds its own PlainVanillaPayoff and passes the strike
    and the exercise to OneAssetOption, so those go where every other style
    puts them and the style block carries only the choice date. There is no
    option type: which side this becomes is what is being chosen.
    """
    f, opt = base_frame(sid)
    opt.payoff.plain.strike = row["strike"]
    if payoff_type is not None:
        opt.payoff.type = payoff_type
    opt.exercise.dates.add().iso = on_day(row["exercise_days"])
    if exercise == "american":
        opt.exercise.type = I.Exercise.TYPE_AMERICAN
        opt.exercise.payoff_at_expiry = M.FLAG_FALSE
    else:
        opt.exercise.type = I.Exercise.TYPE_EUROPEAN
    underlying(opt).dividend_curve_id = dividend_curve

    c = opt.chooser
    c.choice_date.iso = on_day(row["choosing_days"])
    if call_strike:
        c.call_strike = row["strike"]
    if put_strike:
        c.put_strike = put_strike

    f.price.engine.method = method
    return f


def complex_chooser_frame(sid, row, call_days=None, put_days=None):
    """Separate strikes and expiries for the two sides.

    ComplexChooserOption hands the call leg to OneAssetOption, so the put leg
    is the only one the style block carries -- and carrying it is what says
    this is the complex chooser rather than the simple one.
    """
    f, opt = base_frame(sid)
    opt.payoff.plain.strike = row["call_strike"]
    opt.exercise.type = I.Exercise.TYPE_EUROPEAN
    opt.exercise.dates.add().iso = on_day(
        row["choosing_days"] + (row["call_days"] if call_days is None else call_days))
    underlying(opt)

    c = opt.chooser
    c.choice_date.iso = on_day(row["choosing_days"])
    c.put_strike = row["put_strike"]
    c.put_expiry.iso = on_day(
        row["choosing_days"] + (row["put_days"] if put_days is None else put_days))

    f.price.engine.method = EN.Engine.METHOD_ANALYTIC
    return f


BASKET_KIND = {
    "min": I.Basket.KIND_MIN,
    "max": I.Basket.KIND_MAX,
    "spread": I.Basket.KIND_SPREAD,
    "average": I.Basket.KIND_AVERAGE,
}


def basket_frame(sid, row, method=EN.Engine.METHOD_ANALYTIC, kind=None, samples=0,
                 weights=None, labels=("A", "B"), correlation_id="CORRM",
                 assets=2, exercise="european"):
    """Two assets, one correlation matrix, one plain payoff underneath.

    basketoption.cpp prices the minimum and maximum off BlackScholesMertonProcess
    and the spread off BlackProcess -- Kirk is a formula on futures -- so the
    process each underlying asks for follows the basket kind.
    """
    f, opt = base_frame(sid)
    opt.payoff.type = OPTION_TYPE[row["type"]]
    opt.payoff.plain.strike = row["strike"]
    set_exercise(opt, exercise, row["t"])

    basketType = kind if kind is not None else row["basketType"]
    black = basketType == "spread"
    for n in range(assets):
        u = opt.underlyings.add()
        u.label = labels[n] if n < len(labels) else f"U{n}"
        u.spot_quote_id = "S" if n == 0 else "S2"
        u.discount_curve_id = "RC"
        u.volatility_id = "VOL" if n == 0 else "VOL2"
        if black:
            u.process = I.Underlying.PROCESS_BLACK
        else:
            u.process = I.Underlying.PROCESS_BLACK_SCHOLES_MERTON
            u.dividend_curve_id = "QC" if n == 0 else "QC2"

    b = opt.basket
    b.kind = BASKET_KIND[basketType]
    b.correlation_id = correlation_id
    if weights:
        b.weights.extend(weights)

    f.price.engine.method = method
    if method == EN.Engine.METHOD_MONTE_CARLO:
        f.price.engine.mc.seed = 42
        f.price.engine.mc.samples = samples or 10000
        f.price.engine.mc.rng = EN.McParameters.RNG_PSEUDO_RANDOM
        f.price.engine.mc.time_steps_per_year = 1
    return f


def cliquet_frame(sid, row, performance=False, method=EN.Engine.METHOD_ANALYTIC,
                  resets=None, caps=None, samples=0):
    """A ratchet: a series of forward starts, each struck at the spot when it
    opens.

    CliquetOption takes a PercentageStrikePayoff and a European exercise, and
    the style block carries the reset dates. The four cap and floor fields are
    in the schema and reach no engine, so they are refused rather than set.
    """
    f, opt = base_frame(sid)
    opt.payoff.type = OPTION_TYPE[row["type"]]
    opt.payoff.percentage_strike.moneyness = row["moneyness"]
    opt.exercise.type = I.Exercise.TYPE_EUROPEAN
    opt.exercise.dates.add().iso = on_day(row["maturity_days"])
    underlying(opt)

    c = opt.cliquet
    for n in resets if resets is not None else [row["reset_days"]]:
        c.reset_dates.add().iso = on_day(n)
    c.performance = M.FLAG_TRUE if performance else M.FLAG_FALSE
    if caps:
        setattr(c, caps, 0.05)

    f.price.engine.method = method
    if method == EN.Engine.METHOD_MONTE_CARLO:
        f.price.engine.mc.seed = 42
        f.price.engine.mc.samples = samples or 20000
        f.price.engine.mc.rng = EN.McParameters.RNG_PSEUDO_RANDOM
    return f


def forward_frame(sid, row, performance=False, quanto=False):
    f, opt = base_frame(sid)
    opt.payoff.type = OPTION_TYPE[row["type"]]
    opt.payoff.percentage_strike.moneyness = row["moneyness"]
    set_exercise(opt, "european", row["t"])
    underlying(opt, quanto)
    opt.forward_start.reset.iso = expiry(row["start"])
    opt.forward_start.performance = M.FLAG_TRUE if performance else M.FLAG_FALSE
    f.price.engine.method = EN.Engine.METHOD_ANALYTIC
    return f


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------

async def collect(ws, want, timeout=120.0, frames=None):
    progress = 0
    deadline = time.time() + timeout
    while True:
        raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.time()))
        f = E.ServerFrame()
        f.ParseFromString(raw)
        if f.HasField("progress"):
            progress += 1
            if frames is not None and f.request_id == want:
                frames.append(f.progress)
            continue
        if f.request_id == want and f.terminal:
            return f, progress


async def send(ws, frame):
    await ws.send(frame.SerializeToString())
    reply, _ = await collect(ws, frame.request_id)
    return reply


RESULT_NAME = {
    R.RESULT_KIND_DELTA: "delta", R.RESULT_KIND_GAMMA: "gamma",
    R.RESULT_KIND_THETA: "theta", R.RESULT_KIND_VEGA: "vega",
    R.RESULT_KIND_RHO: "rho", R.RESULT_KIND_DIVIDEND_RHO: "dividendRho",
    R.RESULT_KIND_THETA_PER_DAY: "thetaPerDay",
    R.RESULT_KIND_DELTA_FORWARD: "deltaForward",
    R.RESULT_KIND_ELASTICITY: "elasticity",
    R.RESULT_KIND_STRIKE_SENSITIVITY: "strikeSensitivity",
    R.RESULT_KIND_ITM_CASH_PROBABILITY: "itmCashProbability",
    R.RESULT_KIND_QRHO: "qrho", R.RESULT_KIND_QVEGA: "qvega",
    R.RESULT_KIND_QLAMBDA: "qlambda", R.RESULT_KIND_FAIR_RATE: "fairRate",
    R.RESULT_KIND_LEG_NPV: "legNPV", R.RESULT_KIND_LEG_BPS: "legBPS",
    R.RESULT_KIND_IMPLIED_VOLATILITY: "impliedVolatility",
}


async def main():
    failures = []
    checks = 0

    def check(name, ok, detail=""):
        nonlocal checks
        checks += 1
        print(("  PASS  " if ok else "  FAIL  ") + name + ("  " + detail if detail else ""))
        if not ok:
            failures.append(name)

    async def table(ws, sid, label, rows, build, tol=None, price_of=None, value="result"):
        """Prices every row of a reference table and checks it against `value`.

        `value` names the column, because the field names here are the C++
        struct's own: compoundoption.cpp calls its published price `npv` where
        the others call it `result`, and renaming it in the extractor would be
        the transcription this benchmark exists to avoid.
        """
        worst, worst_row = 0.0, None
        if not rows:
            # A table the extractor emptied -- upstream commented its rows out,
            # or a filter here matched nothing -- would otherwise pass with a
            # worst error of zero, as a check of nothing.
            check(label, False, "no rows to check")
            return
        for n, row in enumerate(rows):
            await send(ws, set_market(sid, **row_market(row)))
            reply = await send(ws, build(sid, row))
            if not reply.HasField("price_result"):
                check(f"{label}[{n}]", False,
                      f"{E.Error.Code.Name(reply.error.code)} "
                      f"{reply.error.field_path!r} {reply.error.message!r}")
                return
            got = price_of(reply.price_result) if price_of else reply.price_result.npv
            err = abs(got - row[value])
            limit = tol if tol is not None else row.get("tol", 1.0e-4)
            if err > limit:
                check(f"{label}[{n}]", False,
                      f"expected {row[value]} got {got:.6f} err={err:.2e} tol={limit:g}")
                return
            if err > worst:
                worst, worst_row = err, n
        check(f"{label} ({len(rows)} rows)", True, f"worst err={worst:.2e} at row {worst_row}")

    async with connect(URL, max_size=16 << 20) as ws:

        # -- what this build says it can do ----------------------------------
        #
        # The lists in src/session/capabilities.cpp and the dispatch in
        # session.cpp are the same fact told twice. These checks tie the
        # advertisement to observed behaviour so the two cannot drift apart
        # silently, which is the whole reason the handshake exists.
        print("\n  -- capabilities --")
        f = E.ClientFrame(request_id=next_id())
        f.hello.SetInParent()
        caps = (await send(ws, f)).capabilities
        check("Hello answers with Capabilities",
              bool(caps.option_styles) and bool(caps.engine_methods) and bool(caps.result_kinds),
              f"{len(caps.option_styles)} styles, {len(caps.engine_methods)} methods, "
              f"{len(caps.result_kinds)} result kinds")
        # Built all along -- the same process as Black-Scholes-Merton, the
        # foreign curve in the dividend slot -- and left off the list, so a
        # client gating on the handshake hid it.
        check("it advertises the processes it builds, Garman-Kohlhagen among them",
              I.Underlying.PROCESS_GARMAN_KOHLHAGEN in caps.processes,
              f"processes={[I.Underlying.Process.Name(p) for p in caps.processes]}")
        check("it names the frames it serves, batch among them",
              "batch" in caps.frames and "price" in caps.frames,
              f"frames={list(caps.frames)}")
        check("it advertises the sweep ceiling a grid has to fit under",
              caps.max_scenario_points > 0,
              f"max_scenario_points={caps.max_scenario_points}")
        check("it advertises the batch and curve-sample ceilings beside it",
              caps.max_batch_entries > 0 and caps.max_curve_sample_points > 0,
              f"max_batch_entries={caps.max_batch_entries} "
              f"max_curve_sample_points={caps.max_curve_sample_points}")
        check("it names the build it is",
              bool(caps.build) and bool(caps.quantlib_version),
              f"{caps.build!r} on QuantLib {caps.quantlib_version!r}")

        # -- lifecycle -------------------------------------------------------
        print("\n  -- session lifecycle --")
        f = open_session()
        r = await send(ws, f)
        if not r.HasField("session_opened"):
            print("open failed:", r)
            return 1
        sid = r.session_id
        check("OpenSession", True,
              f"session={sid} objects={len(r.session_opened.market_ids)} "
              f"bootstrap={r.session_opened.bootstrap_seconds:.4f}s")
        check("SessionOpened lists what it built",
              list(r.session_opened.market_ids) ==
              list(QUOTES)
              + ["RC", "QC", "FXRC", "QC365", "VOL", "FXVOL", "VOL2", "QC2", "CORRM"])

        # -- the reference tables --------------------------------------------
        print("\n  -- QuantLib's published values --")

        await table(ws, sid, "european analytic", T.EUROPEAN,
                    lambda s, row: vanilla_frame(s, row))

        # Three American approximations, each against its own table and its
        # own tolerance from the C++ test that owns it.
        await table(ws, sid, "american Barone-Adesi-Whaley", T.AMERICAN_BAW,
                    lambda s, row: vanilla_frame(
                        s, row, exercise="american",
                        approximation=EN.AnalyticParameters.APPROXIMATION_BARONE_ADESI_WHALEY),
                    tol=3.0e-3)
        await table(ws, sid, "american Bjerksund-Stensland", T.AMERICAN_BS,
                    lambda s, row: vanilla_frame(
                        s, row, exercise="american",
                        approximation=EN.AnalyticParameters.APPROXIMATION_BJERKSUND_STENSLAND),
                    tol=5.0e-5)
        await table(ws, sid, "american Ju quadratic", T.AMERICAN_JU,
                    lambda s, row: vanilla_frame(
                        s, row, exercise="american",
                        approximation=EN.AnalyticParameters.APPROXIMATION_JU_QUADRATIC),
                    tol=1.0e-3)

        european_barriers = [r for r in T.BARRIER if r["exercise"] == "european"]
        american_barriers = [r for r in T.BARRIER if r["exercise"] == "american"]
        await table(ws, sid, "barrier analytic", european_barriers,
                    lambda s, row: barrier_frame(s, row))
        # The American rows have no closed form, and FdBlackScholesBarrierEngine
        # refuses a non-European exercise, so these go through the lattice --
        # Cox-Ross-Rubinstein with the Derman-Kani correction at 400 steps,
        # exactly what barrieroption.cpp:567 prices them with. Its tolerance
        # for that pairing is 4e-2, not the 1.1e-2 it uses for the Boyle-Lau
        # discretisation: the same tree with a different barrier correction is
        # a different convergence rate.
        await table(ws, sid, "barrier american lattice", american_barriers,
                    lambda s, row: barrier_frame(
                        s, row, method=EN.Engine.METHOD_LATTICE, steps=400),
                    tol=4.0e-2)

        # The knock digitals, Haug p.180 cases 13-28. Same instrument as the
        # rows above and a different payoff on it, which is the whole of what
        # `message Digital` describes.
        await table(ws, sid, "binary barrier, cash-or-nothing", T.BINARY_CASH,
                    lambda s, row: binary_frame(s, row))
        await table(ws, sid, "binary barrier, asset-or-nothing", T.BINARY_ASSET,
                    lambda s, row: binary_frame(s, row, "asset_or_nothing"))

        await table(ws, sid, "forward start", T.FORWARD,
                    lambda s, row: forward_frame(s, row))

        # Twenty rows against Haug and two independent implementations, at the
        # 1e-3 the C++ test uses: the price is sensitive to which bivariate
        # normal is underneath, and that tolerance is what is published.
        await table(ws, sid, "compound", T.COMPOUND,
                    lambda s, row: compound_frame(s, row), value="npv")

        # Chooser publishes no table -- two values, in the bodies of
        # chooseroption.cpp's two test cases. extract_tables.py reads them out
        # variable by variable rather than letting them be typed here.
        await table(ws, sid, "simple chooser", T.SIMPLE_CHOOSER,
                    lambda s, row: simple_chooser_frame(s, row))
        await table(ws, sid, "complex chooser", T.COMPLEX_CHOOSER,
                    lambda s, row: complex_chooser_frame(s, row))

        # Haug p.37, the one published cliquet value, read out of the test case
        # body the same way the chooser's two are.
        await table(ws, sid, "cliquet", T.CLIQUET,
                    lambda s, row: cliquet_frame(s, row))

        # Two assets and a correlation matrix. The minimum and maximum go to
        # StulzEngine and the spread to Kirk, which is the split the table's
        # own basketType column makes.
        await table(ws, sid, "basket, two-asset closed form", T.BASKET,
                    lambda s, row: basket_frame(s, row))

        await table(ws, sid, "quanto vanilla", T.QUANTO,
                    lambda s, row: vanilla_frame(s, row, quanto=True))
        await table(ws, sid, "quanto forward", T.QUANTO_FORWARD,
                    lambda s, row: forward_frame(s, row, quanto=True))
        await table(ws, sid, "quanto barrier", T.QUANTO_BARRIER,
                    lambda s, row: barrier_frame(s, row, quanto=True))
        await table(ws, sid, "quanto double barrier", T.QUANTO_DOUBLE_BARRIER,
                    lambda s, row: double_barrier_frame(s, row, quanto=True))

        # -- one payoff and exercise, four methods ---------------------------
        #
        # The point of the v2 decomposition: the same trade description reaches
        # four engines because only the engine block changed.
        print("\n  -- the same option through four engines --")
        row = dict(type="call", strike=100.0, s=100.0, q=0.04, r=0.06, t=1.0, v=0.20,
                   result=0.0)
        await send(ws, set_market(sid, **row_market(row)))

        analytic = (await send(ws, vanilla_frame(sid, row))).price_result.npv

        for label, frame, tol in [
            ("lattice CRR 801", vanilla_frame(sid, row, EN.Engine.METHOD_LATTICE, steps=801),
             5.0e-3),
            ("finite difference", vanilla_frame(sid, row, EN.Engine.METHOD_FINITE_DIFFERENCE,
                                                preset=EN.FdParameters.PRESET_FINE), 5.0e-3),
            ("integral", vanilla_frame(sid, row, EN.Engine.METHOD_INTEGRAL), 1.0e-4),
            ("monte carlo", vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO,
                                          mc=(42, 200000)), 5.0e-2),
        ]:
            reply = await send(ws, frame)
            if not reply.HasField("price_result"):
                check(label, False, f"{E.Error.Code.Name(reply.error.code)} "
                                    f"{reply.error.message!r}")
                continue
            err = abs(reply.price_result.npv - analytic)
            check(label, err < tol,
                  f"analytic={analytic:.6f} got={reply.price_result.npv:.6f} err={err:.2e}")

        # The custom grid is not just a pair of numbers: the scheme and the
        # damping steps beside them reach the engine too. Both were carried by
        # the schema and read by nothing for three milestones, which is the
        # defect this section exists to keep out.
        print("\n  -- the custom FD grid, its scheme and its damping steps --")

        def fd_custom(t_steps, x_steps, scheme, damping=0):
            f = vanilla_frame(sid, row, EN.Engine.METHOD_FINITE_DIFFERENCE)
            c = f.price.engine.fd.custom
            c.time_steps, c.asset_steps = t_steps, x_steps
            c.damping_steps, c.scheme = damping, scheme
            return f

        SCHEME = EN.FdParameters.Explicit
        douglas = (await send(ws, fd_custom(400, 200, SCHEME.SCHEME_DOUGLAS))).price_result
        check("custom grid prices", abs(douglas.npv - analytic) < 5.0e-3,
              f"analytic={analytic:.6f} douglas={douglas.npv:.6f}")
        check("and echoes the grid it ran on",
              douglas.engine.fd.custom.time_steps == 400
              and douglas.engine.fd.custom.asset_steps == 200
              and douglas.engine.fd.custom.scheme == SCHEME.SCHEME_DOUGLAS,
              str(douglas.engine.fd.custom).replace("\n", " "))

        # Craig-Sneyd splits a multi-factor operator by direction and this one
        # has a single direction, so it is Douglas here -- which is worth
        # asserting rather than assuming, since it is why the two are both
        # offered.
        craig = (await send(ws, fd_custom(400, 200, SCHEME.SCHEME_CRAIG_SNEYD))).price_result.npv
        check("Craig-Sneyd is Douglas in one dimension", craig == douglas.npv,
              f"douglas={douglas.npv:.12f} craig_sneyd={craig:.12f}")

        # Implicit Euler is first order where Douglas is second, so it has to
        # differ -- and by more than rounding, or the field is still being
        # dropped somewhere.
        implicit = (await send(ws, fd_custom(400, 200,
                                             SCHEME.SCHEME_IMPLICIT_EULER))).price_result.npv
        check("implicit Euler is a different scheme, not a different name",
              abs(implicit - douglas.npv) > 1.0e-4 and abs(implicit - analytic) < 5.0e-3,
              f"douglas={douglas.npv:.6f} implicit={implicit:.6f} "
              f"gap={abs(implicit - douglas.npv):.2e}")

        # Rannacher damping: the first few steps taken fully implicit. It moves
        # the price, which is the whole point of asking for it.
        damped = (await send(ws, fd_custom(400, 200, SCHEME.SCHEME_DOUGLAS,
                                           damping=20))).price_result
        check("damping steps reach the engine",
              abs(damped.npv - douglas.npv) > 1.0e-6 and abs(damped.npv - analytic) < 5.0e-3,
              f"undamped={douglas.npv:.8f} damped={damped.npv:.8f} "
              f"gap={abs(damped.npv - douglas.npv):.2e}")
        check("and are echoed back", damped.engine.fd.custom.damping_steps == 20,
              f"echo={damped.engine.fd.custom.damping_steps}")

        # The engine is echoed whole, so a client can tell two prices apart by
        # what produced them and not by remembering what it asked for.
        reply = await send(ws, vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO,
                                             mc=(7, 20000)))
        echo = reply.price_result.engine
        check("engine echoed on the result",
              echo.method == EN.Engine.METHOD_MONTE_CARLO and echo.mc.seed == 7
              and echo.mc.samples == 20000,
              f"seed={echo.mc.seed} samples={echo.mc.samples}")
        check("monte carlo reports an error estimate",
              reply.price_result.error_estimate.standard_error > 0.0,
              f"stderr={reply.price_result.error_estimate.standard_error:.2e}")

        # -- greeks ----------------------------------------------------------
        print("\n  -- greeks --")
        f = vanilla_frame(sid, row)
        f.price.results.extend([R.RESULT_KIND_DELTA, R.RESULT_KIND_GAMMA, R.RESULT_KIND_VEGA,
                                R.RESULT_KIND_RHO, R.RESULT_KIND_DIVIDEND_RHO,
                                R.RESULT_KIND_THETA])
        reply = await send(ws, f)
        results = reply.price_result.results
        check("analytic engine supplies six greeks",
              all(k in results for k in
                  ("delta", "gamma", "vega", "rho", "dividendRho", "theta")),
              " ".join(f"{k}={results[k].scalar:.4f}" for k in sorted(results)))

        # A greek the engine cannot supply is an absent key, not a failure:
        # the frontend asks every engine the same question.
        f = vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO, mc=(1, 5000))
        f.price.results.append(R.RESULT_KIND_VEGA)
        reply = await send(ws, f)
        check("a greek the MC engine lacks is absent, not an error",
              reply.HasField("price_result") and "vega" not in reply.price_result.results)

        # Garman-Kohlhagen is Black-Scholes-Merton with the foreign rate where
        # the dividend yield goes, so on the same curves it is the same price.
        f = vanilla_frame(sid, row)
        f.price.instrument.option.underlyings[0].process = I.Underlying.PROCESS_GARMAN_KOHLHAGEN
        gk = await send(ws, f)
        bsm = await send(ws, vanilla_frame(sid, row))
        check("Garman-Kohlhagen prices as Black-Scholes-Merton on the same curves",
              gk.HasField("price_result") and gk.price_result.npv == bsm.price_result.npv,
              f"gk={gk.price_result.npv if gk.HasField('price_result') else gk.error.message!r} "
              f"bsm={bsm.price_result.npv}")

        # time_steps_per_year is per year on the vanilla path too. It was read
        # as a total there (and per year on the barrier and the basket), so
        # asking a half-year option for 2 a year gave it two steps rather than
        # one. One step is what an unset field gives, so the two agree exactly.
        half = dict(row, t=0.5)
        await send(ws, set_market(sid, **row_market(half)))
        unset = await send(ws, vanilla_frame(sid, half, EN.Engine.METHOD_MONTE_CARLO, mc=(7, 2000)))
        f = vanilla_frame(sid, half, EN.Engine.METHOD_MONTE_CARLO, mc=(7, 2000))
        f.price.engine.mc.time_steps_per_year = 2
        per_year = await send(ws, f)
        check("steps a year on a vanilla Monte Carlo are per year, as on the others",
              unset.HasField("price_result") and per_year.HasField("price_result")
              and unset.price_result.npv == per_year.price_result.npv,
              f"unset={unset.price_result.npv:.10f} two-a-year={per_year.price_result.npv:.10f}")
        await send(ws, set_market(sid, **row_market(row)))

        # -- a book in one frame ----------------------------------------------
        print("\n  -- batch --")
        # Three trades that price and one that cannot: the point of the shape is
        # that the bad one costs its own row and nothing else.
        f = E.ClientFrame(request_id=next_id(), session_id=sid)
        for strike in (90.0, 100.0, 110.0):
            one = f.batch.requests.add()
            one.CopyFrom(vanilla_frame(sid, row).price)
            one.instrument.option.payoff.plain.strike = strike
        broken = f.batch.requests.add()
        broken.CopyFrom(vanilla_frame(sid, row).price)
        broken.instrument.option.underlyings[0].spot_quote_id = "NOPE"
        await ws.send(f.SerializeToString())
        frames = []
        reply, _ = await collect(ws, f.request_id, frames=frames)

        ok = reply.HasField("batch_result")
        check("a batch returns a BatchResult", ok,
              "" if ok else f"{E.Error.Code.Name(reply.error.code)} {reply.error.message!r}")
        if ok:
            book = reply.batch_result
            check("a batch answers one entry per request, in order",
                  len(book.entries) == 4, f"entries={len(book.entries)}")
            check("a batch prices the trades that price",
                  all(e.HasField("price") for e in book.entries[:3])
                  and book.entries[0].price.npv > book.entries[1].price.npv
                  > book.entries[2].price.npv,
                  " ".join(f"{e.price.npv:.4f}" for e in book.entries[:3]))
            # The middle trade is the session's own, so it must equal the price
            # a single request returns: same graph, same engine, same answer.
            check("a batch entry equals the price sent on its own",
                  abs(book.entries[1].price.npv - analytic) < 1.0e-12,
                  f"direct={analytic:.10f} batched={book.entries[1].price.npv:.10f}")
            bad = book.entries[3]
            check("a failing trade costs its own row and nothing else",
                  bad.HasField("error") and bad.error.code == E.Error.UNKNOWN_ID
                  and bad.error.field_path.startswith("batch.requests[3]"),
                  f"{E.Error.Code.Name(bad.error.code)} {bad.error.field_path!r}")
            check("and the batch was not abandoned", book.abandoned_after == 0,
                  f"abandoned_after={book.abandoned_after}")
            # Progress per entry carries its NPV, and none for the one that
            # failed: it used to read 0.0, which is a price.
            check("a failed entry's progress has no running NPV, rather than 0",
                  len(frames) == 4
                  and all(p.HasField("running_npv") for p in frames[:3])
                  and not frames[3].HasField("running_npv")
                  and not any(p.HasField("scenario_point") for p in frames),
                  " ".join(f"{p.running_npv:.4f}" if p.HasField("running_npv") else "unset"
                           for p in frames))

        # A sweep inside a batch is refused by name rather than served: nesting
        # them is a product with no honest progress stream.
        f = E.ClientFrame(request_id=next_id(), session_id=sid)
        nested = f.batch.requests.add()
        nested.CopyFrom(vanilla_frame(sid, row).price)
        axis = nested.scenarios.add()
        axis.quote_id = "S"
        axis.explicit.values.extend([95.0, 105.0])
        reply = await send(ws, f)
        entry = reply.batch_result.entries[0] if reply.HasField("batch_result") else None
        check("a sweep inside a batch is refused by name",
              entry is not None and entry.HasField("error")
              and entry.error.field_path == "batch.requests[0].scenarios"
              and entry.error.code == E.Error.UNSUPPORTED,
              f"{entry.error.field_path!r}" if entry is not None else "no batch_result")

        # An empty book is the request itself being wrong, so it fails as one.
        f = E.ClientFrame(request_id=next_id(), session_id=sid)
        f.batch.SetInParent()
        reply = await send(ws, f)
        check("an empty batch fails as a request rather than as a row",
              reply.HasField("error") and reply.error.field_path == "batch.requests",
              f"{E.Error.Code.Name(reply.error.code)} {reply.error.field_path!r}"
              if reply.HasField("error") else "returned a BatchResult")

        # The batch left the graph as it found it.
        check("a batch is a question about the market, not an edit",
              abs((await send(ws, vanilla_frame(sid, row))).price_result.npv - analytic) < 1.0e-12)

        # -- the scenario sweep ----------------------------------------------
        print("\n  -- scenario sweep --")
        f = vanilla_frame(sid, row)
        f.price.results.append(R.RESULT_KIND_DELTA)
        axis = f.price.scenarios.add()
        axis.quote_id = "S"
        axis.linear.begin = 80.0
        axis.linear.end = 120.0
        axis.linear.steps = 41
        axis.plot = R.RESULT_KIND_NPV
        await ws.send(f.SerializeToString())
        frames = []
        reply, _ = await collect(ws, f.request_id, frames=frames)
        ok = reply.HasField("scenario_result")
        check("sweep returns a ScenarioResult", ok,
              "" if ok else f"{E.Error.Code.Name(reply.error.code)} {reply.error.message!r}")
        if ok:
            sweep = reply.scenario_result
            check("sweep priced every point", len(sweep.prices) == 41,
                  f"points={len(sweep.prices)}")
            # The first point is point 0, and says so: without presence it
            # read the same as a frame from something that is not a sweep.
            check("a sweep's progress names every point, the first included",
                  [p.scenario_point if p.HasField("scenario_point") else None
                   for p in frames] == list(range(41))
                  and all(p.running_npv == q.npv for p, q in zip(frames, sweep.prices)),
                  f"{len(frames)} frames, first={frames[0] if frames else None}")
            # The midpoint is the spot the session already held, so it must
            # reproduce the single-shot price exactly: same graph, same engine.
            mid = sweep.prices[20].npv
            check("sweep midpoint equals the direct price", abs(mid - analytic) < 1.0e-12,
                  f"direct={analytic:.10f} swept={mid:.10f}")
            check("sweep is monotone in spot for a call",
                  all(a.npv <= b.npv for a, b in zip(sweep.prices, sweep.prices[1:])))
            check("sweep carries a plottable series",
                  len(sweep.series.x) == 41 and len(sweep.series.y) == 41
                  and sweep.series.x_axis == R.Series.AXIS_SPOT)

        # -- and the same sweep in two dimensions ------------------------------
        # The reason this exists: spot x vol used to be five separate sweeps,
        # five round trips and five progress streams against a graph that was
        # already warm.
        f = vanilla_frame(sid, row)
        spot = f.price.scenarios.add()
        spot.quote_id = "S"
        spot.linear.begin = 90.0
        spot.linear.end = 110.0
        spot.linear.steps = 5
        spot.plot = R.RESULT_KIND_NPV
        vol = f.price.scenarios.add()
        vol.quote_id = "V"
        vol.explicit.values.extend([0.1, 0.2, 0.3])
        reply = await send(ws, f)
        grid_ok = reply.HasField("scenario_result")
        check("a grid returns a ScenarioResult", grid_ok,
              "" if grid_ok else f"{E.Error.Code.Name(reply.error.code)} {reply.error.message!r}")
        if grid_ok:
            grid = reply.scenario_result
            check("a grid prices the product of its axes", len(grid.prices) == 15,
                  f"points={len(grid.prices)}")
            check("a grid names both axes",
                  [a.quote_id for a in grid.axes] == ["S", "V"]
                  and len(grid.axes[0].values) == 5 and len(grid.axes[1].values) == 3)
            # Row-major, last axis fastest: prices[i * 3 + j] is spot i at vol j.
            # The point at spot 100 and vol 0.2 is the market the session holds,
            # so it has to reproduce the single-shot price exactly.
            check("a grid is row-major with the last axis fastest",
                  abs(grid.prices[2 * 3 + 1].npv - analytic) < 1.0e-12,
                  f"direct={analytic:.10f} grid={grid.prices[7].npv:.10f}")
            check("a grid rises in vol at every spot",
                  all(grid.prices[i * 3 + j].npv < grid.prices[i * 3 + j + 1].npv
                      for i in range(5) for j in range(2)))
            check("a grid carries a surface, labelled by its axes",
                  grid.surface.rows == 5 and grid.surface.columns == 3
                  and len(grid.surface.values) == 15
                  and list(grid.surface.column_labels) == [0.1, 0.2, 0.3]
                  and abs(grid.surface.values[7] - analytic) < 1.0e-12,
                  f"{grid.surface.rows}x{grid.surface.columns}")
            check("a grid draws no line, having no single x axis",
                  not grid.HasField("series"))

        # Both axes are put back, not only the last one written.
        reply = await send(ws, vanilla_frame(sid, row))
        check("a grid restored every quote it wrote",
              abs(reply.price_result.npv - analytic) < 1.0e-12,
              f"before={analytic:.10f} after={reply.price_result.npv:.10f}")

        # A sweep is a question, not an edit: the quote must be where it was.
        reply = await send(ws, vanilla_frame(sid, row))
        check("sweep restored the quote it wrote",
              abs(reply.price_result.npv - analytic) < 1.0e-12,
              f"before={analytic:.10f} after={reply.price_result.npv:.10f}")

        # Unless the client asked to keep it -- and then the supervisor has to
        # fold the kept value into the session log, or the live graph and the
        # log disagree and a replay silently reverts the price. A
        # finite-difference request forces exactly that replay: it moves the
        # session to a sacrificial worker and rebuilds it from the log
        # (DESIGN §2.1), so the price that comes back afterwards is the log's
        # opinion of the spot, not the worker's.
        f = vanilla_frame(sid, row)
        axis = f.price.scenarios.add()
        axis.quote_id = "S"
        axis.explicit.values.extend([90.0, 110.0])
        axis.keep_final_value = True
        reply = await send(ws, f)
        check("a kept sweep returns", reply.HasField("scenario_result"))
        kept = (await send(ws, vanilla_frame(sid, row))).price_result.npv
        await send(ws, vanilla_frame(sid, row, EN.Engine.METHOD_FINITE_DIFFERENCE,
                                     preset=EN.FdParameters.PRESET_COARSE))
        after_replay = (await send(ws, vanilla_frame(sid, row))).price_result.npv
        check("a kept sweep's final value survives a replay",
              abs(after_replay - kept) < 1.0e-12 and abs(kept - analytic) > 1.0,
              f"kept={kept:.6f} after replay={after_replay:.6f} (unswept={analytic:.6f})")
        await send(ws, set_market(sid, S=row["s"]))

        # -- the live graph --------------------------------------------------
        print("\n  -- the live graph --")
        await send(ws, set_market(sid, S=110.0))
        bumped = (await send(ws, vanilla_frame(sid, row))).price_result.npv
        check("a quote write moves the price", bumped > analytic,
              f"{analytic:.6f} -> {bumped:.6f}")
        await send(ws, set_market(sid, S=100.0))
        restored = (await send(ws, vanilla_frame(sid, row))).price_result.npv
        check("and writing it back restores it exactly",
              abs(restored - analytic) < 1.0e-12)

        # -- rejections ------------------------------------------------------
        #
        # Each of these is a field the client could plausibly leave out, and
        # each would otherwise price on a default nobody chose.
        print("\n  -- rejections --")

        async def rejected(label, frame, field, code=None):
            reply = await send(ws, frame)
            ok = reply.HasField("error") and reply.error.field_path == field
            if ok and code is not None:
                ok = reply.error.code == code
            check(label, ok,
                  f"field={reply.error.field_path!r} "
                  f"code={E.Error.Code.Name(reply.error.code)}" if reply.HasField("error")
                  else "priced instead of failing")

        f = vanilla_frame(sid, row)
        f.price.engine.ClearField("method")
        await rejected("unset engine method", f, "engine.method", E.Error.UNSPECIFIED_ENUM)

        f = vanilla_frame(sid, row, exercise="american")
        await rejected("American analytic with no approximation named", f,
                       "engine.analytic.approximation", E.Error.UNSPECIFIED_ENUM)

        f = vanilla_frame(sid, row, exercise="american",
                          approximation=EN.AnalyticParameters.APPROXIMATION_BARONE_ADESI_WHALEY)
        f.price.instrument.option.exercise.ClearField("payoff_at_expiry")
        await rejected("unset payoff_at_expiry Flag", f,
                       "instrument.option.exercise.payoff_at_expiry", E.Error.UNSPECIFIED_ENUM)

        f = vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO)
        f.price.engine.mc.samples = 1000
        await rejected("Monte Carlo with no seed", f, "engine.mc.seed")

        f = vanilla_frame(sid, row, EN.Engine.METHOD_FINITE_DIFFERENCE)
        await rejected("finite difference with no grid", f, "engine.fd.preset",
                       E.Error.UNSPECIFIED_ENUM)

        # A custom grid names its scheme. Two schemes are two prices for one
        # trade, so an unset one is a question rather than a default.
        f = vanilla_frame(sid, row, EN.Engine.METHOD_FINITE_DIFFERENCE)
        f.price.engine.fd.custom.time_steps = 400
        f.price.engine.fd.custom.asset_steps = 200
        await rejected("custom FD grid with no scheme", f, "engine.fd.custom.scheme",
                       E.Error.UNSPECIFIED_ENUM)

        # Explicit Euler is conditionally stable and QuantLib does not enforce
        # the condition: at 100 x 200 it answers 2.4e140 and at 400 x 200 a
        # NaN. A number that wrong is worse than a rejection.
        f = vanilla_frame(sid, row, EN.Engine.METHOD_FINITE_DIFFERENCE)
        f.price.engine.fd.custom.time_steps = 400
        f.price.engine.fd.custom.asset_steps = 200
        f.price.engine.fd.custom.scheme = EN.FdParameters.Explicit.SCHEME_EXPLICIT_EULER
        await rejected("explicit Euler", f, "engine.fd.custom.scheme", E.Error.UNSUPPORTED)

        # Damping steps run in addition to the time steps: FdmBackwardSolver
        # takes steps + dampingSteps in all. They were refused unless fewer
        # than the time steps, on the reading that they were counted out of
        # them; as many as the time steps prices, and they are bounded by
        # the time-step limit instead.
        f = vanilla_frame(sid, row, EN.Engine.METHOD_FINITE_DIFFERENCE)
        f.price.engine.fd.custom.time_steps = 20
        f.price.engine.fd.custom.asset_steps = 200
        f.price.engine.fd.custom.damping_steps = 20
        f.price.engine.fd.custom.scheme = EN.FdParameters.Explicit.SCHEME_DOUGLAS
        reply = await send(ws, f)
        check("as many damping steps as time steps prices, since they are extra",
              reply.HasField("price_result"),
              f"{reply.error.field_path!r} {reply.error.message[:80]!r}" if reply.HasField("error") else "")
        f.price.engine.fd.custom.damping_steps = MAX_FD_TIME_STEPS + 1
        await rejected("more damping steps than a grid may have time steps", f,
                       "engine.fd.custom.damping_steps", E.Error.INVALID_ARGUMENT)

        # The quanto FD path can only take one of the compiled presets, and
        # says so rather than rounding a custom grid to the nearest one.
        f = barrier_frame(sid, T.QUANTO_BARRIER[0], method=EN.Engine.METHOD_FINITE_DIFFERENCE,
                          quanto=True)
        f.price.engine.fd.custom.time_steps = 500
        f.price.engine.fd.custom.asset_steps = 250
        await rejected("quanto FD with an explicit grid", f, "engine.fd.custom",
                       E.Error.UNSUPPORTED)

        # Engine sizes past the limits in capabilities.hpp. Each is a loop
        # bound or an allocation QuantLib cannot be interrupted in, so a size
        # a few digits too long was a request that ran for days or took the
        # process's memory with it. The largest values a field can hold are
        # sent too, because those are what a hostile frame would carry.
        INVALID = E.Error.INVALID_ARGUMENT
        for steps in (MAX_LATTICE_STEPS + 1, 2**32 - 1):
            f = vanilla_frame(sid, row, EN.Engine.METHOD_LATTICE, steps=steps)
            await rejected(f"a lattice of {steps:,} steps", f, "engine.lattice.steps", INVALID)
        reply = await send(ws, vanilla_frame(sid, row, EN.Engine.METHOD_LATTICE,
                                             steps=MAX_LATTICE_STEPS))
        check("a lattice at the step limit still prices", reply.HasField("price_result"),
              E.Error.Code.Name(reply.error.code) if reply.HasField("error") else "")

        for field, value, limit in (("time_steps", 2**32 - 1, MAX_FD_TIME_STEPS),
                                    ("asset_steps", MAX_FD_ASSET_STEPS + 1, MAX_FD_ASSET_STEPS)):
            f = vanilla_frame(sid, row, EN.Engine.METHOD_FINITE_DIFFERENCE)
            f.price.engine.fd.custom.time_steps = 400
            f.price.engine.fd.custom.asset_steps = 200
            f.price.engine.fd.custom.scheme = EN.FdParameters.Explicit.SCHEME_DOUGLAS
            setattr(f.price.engine.fd.custom, field, value)
            await rejected(f"an FD grid of {value:,} {field.replace('_', ' ')} (limit {limit:,})",
                           f, f"engine.fd.custom.{field}", INVALID)

        # The last of these is the sample count that used to wrap the batch
        # count to zero and answer an NPV of 0 with no error.
        for samples in (MAX_MC_SAMPLES + 1, 10**15, 2**64 - 1):
            f = vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO, mc=(11, samples))
            f.price.engine.mc.progress_every_paths = 2
            await rejected(f"a Monte Carlo of {samples:,} paths", f, "engine.mc.samples", INVALID)

        f = vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO, mc=(11, 1000))
        f.price.engine.mc.time_steps_per_year = MAX_MC_TIME_STEPS_PER_YEAR + 1
        await rejected("a Monte Carlo path of too many steps a year", f,
                       "engine.mc.time_steps_per_year", INVALID)

        f = vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO,
                          mc=(11, MAX_MC_BATCHES * 10 + 1))
        f.price.engine.mc.progress_every_paths = 10   # one batch too many
        await rejected("a Monte Carlo split into more batches than the limit", f,
                       "engine.mc.progress_every_paths", INVALID)

        f = vanilla_frame(sid, row)
        f.price.results.append(R.RESULT_KIND_IMPLIED_VOLATILITY)
        f.price.implied_volatility.target_price = 10.0
        f.price.implied_volatility.max_evaluations = 2**32 - 1
        await rejected("an implied volatility of four billion evaluations", f,
                       "implied_volatility.max_evaluations", INVALID)

        f = vanilla_frame(sid, row)
        f.price.instrument.option.underlyings[0].spot_quote_id = "NOPE"
        await rejected("unknown quote id", f,
                       "instrument.option.underlyings[0].spot_quote_id", E.Error.UNKNOWN_ID)

        # Engine fields taken over the wire and read by nothing. Each used to
        # price -- Black-Scholes, pseudo-random, no variance reduction -- under
        # a request, and a result echo, that said otherwise.
        f = vanilla_frame(sid, row)
        f.price.engine.model = EN.Engine.MODEL_HESTON
        await rejected("a model this build does not have", f, "engine.model",
                       E.Error.UNSUPPORTED)
        f = vanilla_frame(sid, row, method=EN.Engine.METHOD_MONTE_CARLO, mc=(42, 1000))
        f.price.engine.mc.rng = EN.McParameters.RNG_LOW_DISCREPANCY
        await rejected("a Sobol Monte Carlo, which is built pseudo-random", f,
                       "engine.mc.rng", E.Error.UNSUPPORTED)
        for field in ["antithetic_variate", "control_variate", "brownian_bridge"]:
            f = vanilla_frame(sid, row, method=EN.Engine.METHOD_MONTE_CARLO, mc=(42, 1000))
            setattr(f.price.engine.mc, field, True)
            await rejected(f"a vanilla Monte Carlo asked for {field.replace('_', ' ')}", f,
                           f"engine.mc.{field}", E.Error.UNSUPPORTED)

        # A market update naming a quote the session does not have. It used to
        # be found mid-batch, after the writes before it had landed, and dirty
        # the session; it is now refused by position before anything is written.
        before = (await send(ws, vanilla_frame(sid, row))).price_result.npv
        reply = await send(ws, set_market(sid, S=row["s"] * 1.5, NOPE=1.0))
        check("an update naming an unknown quote is refused at its position",
              reply.HasField("error") and reply.error.field_path == "quotes[1].quote_id"
              and reply.error.code == E.Error.UNKNOWN_ID,
              f"{reply.error.field_path!r} {E.Error.Code.Name(reply.error.code)}"
              if reply.HasField("error") else "applied")
        after = await send(ws, vanilla_frame(sid, row))
        check("...and writes nothing: the session prices as before",
              after.HasField("price_result") and abs(after.price_result.npv - before) < 1e-12,
              f"{before:.6f} -> {after.price_result.npv:.6f}"
              if after.HasField("price_result") else after.WhichOneof("payload"))

        f = vanilla_frame(sid, row)
        f.price.instrument.option.underlyings.add().spot_quote_id = "S"
        await rejected("two underlyings on a one-asset option", f,
                       "instrument.option.underlyings")

        f = vanilla_frame(sid, row)
        f.price.instrument.option.underlyings[0].discount_curve_id = "NOPE"
        await rejected("unknown curve id", f,
                       "instrument.option.underlyings[0].discount_curve_id", E.Error.UNKNOWN_ID)

        # Request options the schema offers and this build does not serve are
        # refused, not dropped: a client that asked for a cash-flow table and
        # got a price without one cannot tell that from an instrument with no
        # cash flows.
        # Served for cash-flow instruments and refused for an option, which
        # has no coupons: an empty table would read as an instrument that has
        # none rather than one that was never going to.
        # Inverting a price round-trips: price the option, hand the price back
        # as the target, and the volatility that comes out is the one the
        # market object holds. Checked this way rather than against a constant
        # because the live-graph section above has already moved the quote.
        # The market is put back to the row first: the live-graph section
        # above has been writing quotes, so what the vol is at this point is
        # not something to assume.
        await send(ws, set_market(sid, **row_market(row)))
        priced = (await send(ws, vanilla_frame(sid, row))).price_result.npv
        f = vanilla_frame(sid, row)
        f.price.results.append(R.RESULT_KIND_IMPLIED_VOLATILITY)
        f.price.implied_volatility.target_price = priced
        got = (await send(ws, f)).price_result.results.get("impliedVolatility")
        check("an implied volatility inverts back to the quote it priced with",
              got is not None and abs(got.scalar - row["v"]) < 1e-4,
              f"implied={got.scalar:.6f} quote={row['v']}" if got else "absent")

        # And it is refused without one, rather than inverting the price it is
        # about to compute and handing back the volatility that went in.
        f = vanilla_frame(sid, row)
        f.price.results.append(R.RESULT_KIND_IMPLIED_VOLATILITY)
        await rejected("an implied volatility with no price to invert", f,
                       "implied_volatility.target_price", E.Error.INVALID_ARGUMENT)

        f = vanilla_frame(sid, row)
        f.price.include_cashflows = True
        await rejected("include_cashflows on an option, which has no cash flows", f,
                       "include_cashflows", E.Error.UNSUPPORTED)
        check("and it is advertised, because a swap does have them",
              "include_cashflows" in caps.price_request_options,
              f"options={list(caps.price_request_options)}")

        # Nothing advertised may be refused. The option path ignores a kind it
        # has no answer for rather than failing, so this catches the opposite
        # mistake: a kind advertised that the build rejects outright.
        f = vanilla_frame(sid, row)
        for kind in caps.result_kinds:
            f.price.results.append(kind)
        # One of them needs an input of its own; asking for everything means
        # supplying it rather than dropping the kind from the sweep.
        f.price.implied_volatility.target_price = priced
        reply = await send(ws, f)
        check("every advertised result kind is accepted", reply.HasField("price_result"),
              f"got {reply.WhichOneof('payload')}"
              + (f" {reply.error.field_path}" if reply.HasField("error") else ""))

        # And every one of them either comes back or is named as absent. This
        # is the promise HANDLERS.md used to make and the code used to break:
        # a client that asked for vega and got a map without it could not tell
        # that from a vega of zero.
        asked = set(caps.result_kinds) - {R.RESULT_KIND_NPV}
        answered = set()
        for kind in asked:
            name = RESULT_NAME.get(kind)
            if name and any(k == name or k.startswith(name + ".")
                            for k in reply.price_result.results):
                answered.add(kind)
        named_absent = set(reply.price_result.unavailable_results)
        check("every result asked for is answered or named absent",
              asked == answered | named_absent,
              f"{len(answered)} answered, {len(named_absent)} named absent, "
              f"{len(asked - answered - named_absent)} silently missing")
        # curve_samples used to be refused here. It is served now, so what is
        # checked is that the numbers come off the curve the engine priced
        # with: a flat 5% continuous curve discounts to exp(-0.05 t), and a
        # client that had to re-implement the interpolation in its own language
        # would be drawing something else.
        f = vanilla_frame(sid, row)
        cs = f.price.curve_samples.add()
        cs.market_id = "RC"
        cs.quantity = E.CurveSample.QUANTITY_DISCOUNT_FACTOR
        for t in (0.25, 0.5, 1.0, 2.0):
            cs.times.append(t)
        cs = f.price.curve_samples.add()
        cs.market_id = "RC"
        cs.quantity = E.CurveSample.QUANTITY_ZERO_RATE
        cs.compounding, cs.frequency = C.CONTINUOUS, C.ANNUAL
        for t in (0.5, 1.0, 2.0):
            cs.times.append(t)
        reply = await send(ws, f)
        series = {s.name: list(s.y) for s in reply.price_result.series}
        dfs = series.get("RC.discountFactor", [])
        zeros = series.get("RC.zeroRate", [])
        # Checked against the rate the curve reports rather than the rate it
        # was built with: by this point the live-graph section above has
        # already written that quote, and a sample that did not follow it
        # would be the bug this feature exists to prevent.
        rate = zeros[0] if zeros else float("nan")
        worst_df = max((abs(df - math.exp(-rate * t)) for df, t in
                        zip(dfs, (0.25, 0.5, 1.0, 2.0))), default=1.0)
        check("a sampled discount curve is the curve that priced",
              len(dfs) == 4 and worst_df < 1e-12,
              f"worst err={worst_df:.2e} against a sampled zero of {rate:.4f}")
        check("a sampled zero curve is flat, and at the quote it now holds",
              len(zeros) == 3 and max(abs(z - rate) for z in zeros) < 1e-12
              and abs(rate - 0.06) < 1e-12,
              f"zeros={[round(z, 6) for z in zeros]}")

        # A surface across several strikes is a matrix, and this reply carries
        # series; refused rather than flattened into the first strike.
        f = vanilla_frame(sid, row)
        cs = f.price.curve_samples.add()
        cs.market_id = "VOL"
        cs.quantity = E.CurveSample.QUANTITY_BLACK_VOLATILITY
        cs.times.append(1.0)
        cs.strikes.extend([90.0, 100.0, 110.0])
        await rejected("sampling a surface across strikes", f, "curve_samples[0].strikes",
                       E.Error.UNSUPPORTED)

        f = vanilla_frame(sid, row)
        axis = f.price.scenarios.add()
        axis.quote_id = "S"
        axis.explicit.values.append(100.0)
        axis.plot = R.RESULT_KIND_LEG_NPV
        await rejected("a sweep plotting a result with no single value", f, "scenarios[0].plot",
                       E.Error.UNSUPPORTED)

        # A quote on two axes: the later write wins at every point and the
        # earlier axis moves nothing, which would look like a flat dimension.
        f = vanilla_frame(sid, row)
        for _ in range(2):
            axis = f.price.scenarios.add()
            axis.quote_id = "S"
            axis.explicit.values.extend([95.0, 105.0])
        await rejected("a grid sweeping one quote on two axes", f, "scenarios[1].quote_id")

        # The plot belongs to the sweep, not to an axis.
        f = vanilla_frame(sid, row)
        first = f.price.scenarios.add()
        first.quote_id = "S"
        first.explicit.values.append(100.0)
        second = f.price.scenarios.add()
        second.quote_id = "V"
        second.explicit.values.append(0.2)
        second.plot = R.RESULT_KIND_NPV
        await rejected("a grid asking a later axis to choose the plot", f, "scenarios[1].plot")

        # A product multiplies: three innocent-looking axes are 8 million
        # points, and the ceiling is advertised so a client need not find out.
        f = vanilla_frame(sid, row)
        for quote, mid in (("S", 100.0), ("V", 0.2), ("R", 0.05)):
            axis = f.price.scenarios.add()
            axis.quote_id = quote
            axis.linear.begin = mid * 0.9
            axis.linear.end = mid * 1.1
            axis.linear.steps = 200
        await rejected("a grid larger than the advertised ceiling", f, "scenarios")

        # A quanto lookback was priced as a plain one: the lookback arm builds
        # its engines on the bare process and nothing there consulted the
        # quanto graph, so the FX adjustment was dropped and a number came back
        # for a different trade. HANDLERS.md said quanto was unavailable on a
        # lookback all along; this is the check that the code agrees.
        f, opt = base_frame(sid)
        opt.payoff.type = OPTION_TYPE[row["type"]]
        opt.payoff.plain.strike = row["strike"]
        set_exercise(opt, "european", row["t"])
        underlying(opt, quanto=True)
        opt.lookback.running_extremum = row["strike"]
        f.price.engine.method = EN.Engine.METHOD_ANALYTIC
        await rejected("a quanto lookback, which has no engine", f,
                       "instrument.option.quanto", E.Error.UNSUPPORTED)

        # The same trade without the FX leg still prices, so the refusal is
        # about the quanto rather than about lookbacks.
        f, opt = base_frame(sid)
        opt.payoff.type = OPTION_TYPE[row["type"]]
        opt.payoff.plain.strike = row["strike"]
        set_exercise(opt, "european", row["t"])
        underlying(opt)
        opt.lookback.running_extremum = row["strike"]
        f.price.engine.method = EN.Engine.METHOD_ANALYTIC
        reply = await send(ws, f)
        check("a plain lookback still prices", reply.HasField("price_result"),
              f"got {reply.WhichOneof('payload')}")

        # The knock digital's five refusals. Four are things
        # AnalyticBinaryBarrierEngine would throw on from the inside; the fifth
        # is the one it would not -- a rebate it never reads, which would come
        # back as a price for a different trade.
        brow = T.BINARY_CASH[0]
        await send(ws, set_market(sid, **row_market(brow)))

        await rejected("a knock digital with a rebate the engine never reads",
                       binary_frame(sid, brow, rebate=3.0),
                       "instrument.option.barrier.rebate", E.Error.UNSUPPORTED)
        await rejected("a knock digital on a European exercise",
                       binary_frame(sid, brow, exercise="european"),
                       "instrument.option.exercise.type", E.Error.UNSUPPORTED)
        await rejected("a knock digital settled on touch rather than at expiry",
                       binary_frame(sid, brow, at_expiry=False),
                       "instrument.option.exercise.payoff_at_expiry",
                       E.Error.INVALID_ARGUMENT)
        await rejected("a knock digital on a lattice",
                       binary_frame(sid, brow, method=EN.Engine.METHOD_LATTICE),
                       "engine.method", E.Error.UNSUPPORTED)

        # And the style arm the schema still carries, which says where to send
        # the trade rather than implying an engine is missing.
        f, opt = base_frame(sid)
        opt.payoff.type = OPTION_TYPE[brow["type"]]
        opt.payoff.plain.strike = brow["strike"]
        set_exercise(opt, "european", brow["t"])
        underlying(opt)
        opt.digital.barrier = brow["barrier"]
        opt.digital.cash_payoff = brow["cash"]
        f.price.engine.method = EN.Engine.METHOD_ANALYTIC
        await rejected("the digital style, which is a barrier described twice", f,
                       "instrument.option.digital", E.Error.UNSUPPORTED)

        # The guard that came with it: a gap payoff used to reach
        # AnalyticBarrierEngine and fail as "non-plain payoff given", with no
        # field to blame.
        f, opt = base_frame(sid)
        opt.payoff.type = OPTION_TYPE[brow["type"]]
        opt.payoff.gap.strike = brow["strike"]
        opt.payoff.gap.second_strike = brow["strike"] + 5.0
        set_exercise(opt, "european", brow["t"])
        underlying(opt)
        opt.barrier.type = BARRIER_TYPE[brow["barrierType"]]
        opt.barrier.level = brow["barrier"]
        f.price.engine.method = EN.Engine.METHOD_ANALYTIC
        await rejected("a gap payoff on an analytic barrier", f,
                       "instrument.option.payoff", E.Error.UNSUPPORTED)

        # Three compound refusals. Each is something QuantLib would throw on
        # from inside the engine -- "non-plain payoff given", or the maturity
        # check in CompoundOption::arguments::validate -- which arrives as
        # CALCULATION_FAILED with no field on it. Named here instead.
        crow = T.COMPOUND[0]
        await send(ws, set_market(sid, **row_market(crow)))

        await rejected("a compound naming its mother twice",
                       compound_frame(sid, crow, mother_fields=True),
                       "instrument.option.compound.mother_payoff", E.Error.UNSUPPORTED)

        await rejected("a compound written on a binary payoff",
                       compound_frame(sid, crow, daughter_payoff="cash_or_nothing"),
                       "instrument.option.compound.daughter_payoff", E.Error.UNSUPPORTED)

        await rejected("a compound outliving the option it is written on",
                       compound_frame(sid, dict(crow, tMother=crow["tDaughter"] + 0.25)),
                       "instrument.option.compound.daughter_exercise.dates",
                       E.Error.INVALID_ARGUMENT)

        # Seven chooser refusals. Two are duplicate fields, one is a field the
        # instrument overwrites, and four are things the engines get wrong
        # quietly rather than loudly: an exercise type neither of them reads, a
        # day-counter mismatch only the simple engine checks, and a complex leg
        # inside twice the choice time, where the volatility surface throws
        # "negative time" from inside a Newton-Raphson.
        srow, xrow = T.SIMPLE_CHOOSER[0], T.COMPLEX_CHOOSER[0]
        await send(ws, set_market(sid, **row_market(srow)))

        await rejected("a chooser naming its strike twice",
                       simple_chooser_frame(sid, srow, call_strike=True),
                       "instrument.option.chooser.call_strike", E.Error.UNSUPPORTED)

        await rejected("a chooser told which side it is",
                       simple_chooser_frame(sid, srow, payoff_type=I.Payoff.OPTION_TYPE_CALL),
                       "instrument.option.payoff.type", E.Error.INVALID_ARGUMENT)

        await rejected("a chooser on an American exercise, which neither engine reads",
                       simple_chooser_frame(sid, srow, exercise="american"),
                       "instrument.option.exercise.type", E.Error.UNSUPPORTED)

        await rejected("a chooser on a lattice",
                       simple_chooser_frame(sid, srow, method=EN.Engine.METHOD_LATTICE),
                       "engine.method", E.Error.UNSUPPORTED)

        await rejected("a put strike with no put expiry beside it",
                       simple_chooser_frame(sid, srow, put_strike=srow["strike"] - 2.0),
                       "instrument.option.chooser.put_expiry", E.Error.INVALID_ARGUMENT)

        await rejected("a chooser whose curves count days differently",
                       simple_chooser_frame(sid, srow, dividend_curve="QC365"),
                       "instrument.option.underlyings[0].dividend_curve_id",
                       E.Error.INVALID_ARGUMENT)

        await send(ws, set_market(sid, **row_market(xrow)))
        await rejected("a complex chooser expiring inside twice its choice time",
                       complex_chooser_frame(sid, xrow, call_days=60),
                       "instrument.option.exercise.dates", E.Error.UNSUPPORTED)

        # The cliquet's four dead fields, plus the two rules the instrument
        # itself enforces. The caps are the interesting ones: they are refused
        # here because they reach no engine at all -- setupArguments copies the
        # reset dates and stops -- so the price would be the uncapped ratchet
        # under a capped description.
        krow = T.CLIQUET[0]
        await send(ws, set_market(sid, **row_market(krow)))

        for field in ["local_cap", "local_floor", "global_cap", "global_floor"]:
            await rejected(f"a cliquet with a {field} QuantLib never copies",
                           cliquet_frame(sid, krow, caps=field),
                           f"instrument.option.cliquet.{field}", E.Error.UNSUPPORTED)

        await rejected("a cliquet resetting after it expires",
                       cliquet_frame(sid, krow, resets=[krow["maturity_days"] + 30]),
                       "instrument.option.cliquet.reset_dates[0]", E.Error.INVALID_ARGUMENT)
        await rejected("a cliquet whose resets are out of order",
                       cliquet_frame(sid, krow, resets=[180, 90]),
                       "instrument.option.cliquet.reset_dates[1]", E.Error.INVALID_ARGUMENT)
        f = cliquet_frame(sid, krow, performance=True, method=EN.Engine.METHOD_MONTE_CARLO)
        f.price.engine.mc.control_variate = True
        await rejected("a performance Monte Carlo asked for a control variate", f,
                       "engine.mc.control_variate", E.Error.UNSUPPORTED)
        await rejected("a ratchet on the Monte Carlo engine, which only does performance",
                       cliquet_frame(sid, krow, method=EN.Engine.METHOD_MONTE_CARLO),
                       "instrument.option.cliquet.performance", E.Error.UNSUPPORTED)

        # The gamma both closed forms publish as a literal 0.0. Reported absent
        # rather than as a zero a client could not tell from a computed one.
        f = cliquet_frame(sid, krow)
        f.price.results.extend([R.RESULT_KIND_DELTA, R.RESULT_KIND_GAMMA, R.RESULT_KIND_VEGA])
        reply = await send(ws, f)
        got = set(reply.price_result.results)
        absent = set(reply.price_result.unavailable_results)
        check("a cliquet reports the gamma its engine never computes as absent",
              reply.HasField("price_result") and R.RESULT_KIND_GAMMA in absent
              and "delta" in got and "vega" in got,
              f"got={sorted(got)} absent={[R.ResultKind.Name(k) for k in absent]}")

        # The basket's refusals. Two are the schema being narrower than the
        # build, two are QuantLib being permissive where it should not be.
        brow2 = next(r for r in T.BASKET if r["basketType"] == "min")
        await send(ws, set_market(sid, **row_market(brow2)))

        await rejected("a basket with one underlying",
                       basket_frame(sid, brow2, assets=1),
                       "instrument.option.underlyings", E.Error.INVALID_ARGUMENT)
        await rejected("a basket on an American exercise",
                       basket_frame(sid, brow2, exercise="american"),
                       "instrument.option.exercise.type", E.Error.UNSUPPORTED)
        await rejected("weights on a basket that is not an average",
                       basket_frame(sid, brow2, weights=[0.5, 0.5]),
                       "instrument.option.basket.weights", E.Error.UNSUPPORTED)
        await rejected("an average basket asked for a closed form",
                       basket_frame(sid, brow2, kind="average"),
                       "instrument.option.basket.kind", E.Error.UNSUPPORTED)
        await rejected("a two-asset finite-difference grid the schema cannot describe",
                       basket_frame(sid, brow2, method=EN.Engine.METHOD_FINITE_DIFFERENCE),
                       "engine.fd", E.Error.UNSUPPORTED)
        f = basket_frame(sid, brow2, kind="average", method=EN.Engine.METHOD_MONTE_CARLO)
        f.price.engine.mc.control_variate = True
        await rejected("a basket Monte Carlo asked for a control variate", f,
                       "engine.mc.control_variate", E.Error.UNSUPPORTED)
        await rejected("an underlying with no label for the correlation to index on",
                       basket_frame(sid, brow2, labels=("", "B")),
                       "instrument.option.underlyings[0].label", E.Error.INVALID_ARGUMENT)
        await rejected("a correlation matrix that is not in this market",
                       basket_frame(sid, brow2, correlation_id="NOPE"),
                       "instrument.option.basket.correlation_id", E.Error.UNKNOWN_ID)

        # The one QuantLib would not have raised: StochasticProcessArray
        # factorises with SalvagingAlgorithm::Spectral, which repairs an
        # impossible correlation rather than refusing it, so a matrix dragged
        # out of range between requests would have been quietly replaced.
        await send(ws, set_market(sid, RHO=1.5))
        await rejected("a correlation dragged outside [-1, 1] after the session opened",
                       basket_frame(sid, brow2),
                       "instrument.option.basket.correlation_id", E.Error.INVALID_ARGUMENT)
        await send(ws, set_market(sid, RHO=brow2["rho"]))

        # The third engine the C++ test runs over the same table, at the 1%
        # relative tolerance it uses: one Monte Carlo price per basket kind,
        # against the closed form rather than against a published number.
        for kind in ["min", "max", "spread"]:
            mrow = next(r for r in T.BASKET if r["basketType"] == kind)
            await send(ws, set_market(sid, **row_market(mrow)))
            closed = await send(ws, basket_frame(sid, mrow))
            sampled = await send(ws, basket_frame(sid, mrow,
                                                  method=EN.Engine.METHOD_MONTE_CARLO))
            ok = closed.HasField("price_result") and sampled.HasField("price_result")
            gap = (abs(closed.price_result.npv - sampled.price_result.npv) / mrow["s1"]
                   if ok else 1.0)
            check(f"Monte Carlo agrees with the closed form on a {kind} basket",
                  ok and gap < 1.0e-2,
                  f"analytic={closed.price_result.npv:.6f} mc={sampled.price_result.npv:.6f} "
                  f"relative gap={gap:.2e}" if ok else "did not price")

        # And the kind with no closed form at all, which is why Monte Carlo is
        # the only method open on it.
        arow = next(r for r in T.BASKET if r["basketType"] == "min")
        await send(ws, set_market(sid, **row_market(arow)))
        reply = await send(ws, basket_frame(sid, arow, kind="average",
                                            method=EN.Engine.METHOD_MONTE_CARLO,
                                            weights=[0.7, 0.3]))
        check("a weighted average basket prices on Monte Carlo",
              reply.HasField("price_result") and reply.price_result.npv > 0,
              f"npv={reply.price_result.npv:.6f}" if reply.HasField("price_result")
              else f"{reply.error.field_path!r} {reply.error.message!r}")

        # Three pairwise correlations, each legal on its own and jointly
        # impossible. StochasticProcessArray would have repaired this one and
        # priced the nearest possible market without a word.
        f = E.ClientFrame(request_id=next_id())
        f.open_session.evaluation_date.iso = TODAY.isoformat()
        m = f.open_session.market.add()
        m.id = "BAD"
        m.correlation.labels.extend(["A", "B", "C"])
        for a, b, c in [(1.0, 0.9, 0.9), (0.9, 1.0, -0.9), (0.9, -0.9, 1.0)]:
            for v in (a, b, c):
                m.correlation.values.add().fixed = v
        await rejected("a correlation matrix no set of assets could have", f,
                       "market[0].correlation.values", E.Error.INVALID_ARGUMENT)

        # Two shapes whose size check used to overflow an int. 65,536 x 65,536
        # is 2^32, which a 32-bit count reads as zero, so a surface with no
        # volatilities passed and the loop after it read past the end of the
        # list into a 34 GB matrix; 65,537 labels squared wrapped the same way.
        # Each now stops at a cap on the axis, by name, before anything is
        # allocated. A connection of their own, because a refused open still
        # holds a session slot on the socket it came from.
        async with connect(URL, max_size=None) as side:
            f = E.ClientFrame(request_id=next_id())
            f.open_session.evaluation_date.iso = TODAY.isoformat()
            m = f.open_session.market.add()
            m.id = "HUGE"
            act360(m.volatility.day_counter)
            vs = m.volatility.variance_surface
            for _ in range(65536):
                vs.expiries.add().iso = "2027-09-01"
            vs.strikes.extend([100.0] * 65536)
            reply = await send(side, f)
            check("a variance surface too large to count is refused by its size",
                  reply.HasField("error")
                  and reply.error.field_path == "market[0].volatility.variance_surface.expiries"
                  and reply.error.code == E.Error.INVALID_ARGUMENT,
                  f"{reply.error.field_path!r} {E.Error.Code.Name(reply.error.code)}"
                  if reply.HasField("error") else "opened")

            f = E.ClientFrame(request_id=next_id())
            f.open_session.evaluation_date.iso = TODAY.isoformat()
            m = f.open_session.market.add()
            m.id = "HUGE"
            m.correlation.labels.extend(f"L{i}" for i in range(65537))
            for _ in range(131073):   # 65,537^2 as it wrapped in 32 bits
                m.correlation.values.add().fixed = 0.0
            reply = await send(side, f)
            check("a correlation matrix too large to count is refused by its size",
                  reply.HasField("error")
                  and reply.error.field_path == "market[0].correlation.labels"
                  and reply.error.code == E.Error.INVALID_ARGUMENT,
                  f"{reply.error.field_path!r} {E.Error.Code.Name(reply.error.code)}"
                  if reply.HasField("error") else "opened")

        # And the style arm that is no longer a product of its own.
        f, opt = base_frame(sid)
        opt.payoff.type = OPTION_TYPE[brow2["type"]]
        opt.payoff.plain.strike = brow2["strike"]
        set_exercise(opt, "european", brow2["t"])
        underlying(opt)
        opt.spread.SetInParent()
        f.price.engine.method = EN.Engine.METHOD_ANALYTIC
        await rejected("the spread style, which QuantLib now prices as a basket", f,
                       "instrument.option.spread", E.Error.UNSUPPORTED)

        # And the two performance engines, which the schema could not ask for
        # until `performance` was added to it. No published value: the C++ test
        # checks the Monte Carlo one against the closed form, which is the same
        # cross-check the quanto barriers get.
        closedForm = await send(ws, cliquet_frame(sid, krow, performance=True))
        sampled = await send(ws, cliquet_frame(sid, krow, performance=True,
                                               method=EN.Engine.METHOD_MONTE_CARLO))
        if closedForm.HasField("price_result") and sampled.HasField("price_result"):
            gap = abs(closedForm.price_result.npv - sampled.price_result.npv)
            check("the Monte Carlo performance cliquet agrees with the closed form",
                  gap < 1.5e-2,
                  f"analytic={closedForm.price_result.npv:.6f} "
                  f"mc={sampled.price_result.npv:.6f} gap={gap:.2e} "
                  f"stderr={sampled.price_result.results['errorEstimate'].scalar:.2e}")
        else:
            check("the Monte Carlo performance cliquet agrees with the closed form", False,
                  f"{closedForm.WhichOneof('payload')} / {sampled.WhichOneof('payload')}")

        # And one the build does serve: the engine's own additional results.
        f = vanilla_frame(sid, row)
        f.price.include_additional_results = True
        reply = await send(ws, f)
        keys = set(reply.price_result.results)
        check("include_additional_results returns the engine's own map",
              reply.HasField("price_result") and keys,
              " ".join(sorted(keys)))

        # A quote id on an interpolated curve node would look live and never
        # move: InterpolatedZeroCurve copies its rates at construction.
        f = E.ClientFrame(request_id=next_id())
        f.open_session.evaluation_date.iso = TODAY.isoformat()
        m = f.open_session.market.add()
        m.id = "Z"
        act360(m.yield_curve.day_counter)
        m.yield_curve.calendar.name = C.Calendar.NULL_CALENDAR
        m.yield_curve.zero.compounding = C.CONTINUOUS
        m.yield_curve.zero.frequency = C.ANNUAL
        for tenor, v in [("0D", 0.03), ("5Y", 0.04)]:
            n = m.yield_curve.zero.nodes.add()
            n.tenor = tenor
            n.value.quote_id = "R"
        await rejected("a quote id on an interpolated curve node", f,
                       "market[0].yield_curve.zero.nodes[0].value.quote_id",
                       E.Error.UNSUPPORTED)

        # A curve whose first node is 6M out starts six months from now and
        # discounts nothing before then; the reference date is taken from it.
        f = E.ClientFrame(request_id=next_id())
        f.open_session.evaluation_date.iso = TODAY.isoformat()
        m = f.open_session.market.add()
        m.id = "Z"
        act360(m.yield_curve.day_counter)
        m.yield_curve.calendar.name = C.Calendar.NULL_CALENDAR
        m.yield_curve.zero.compounding = C.CONTINUOUS
        m.yield_curve.zero.frequency = C.ANNUAL
        for tenor, v in [("6M", 0.03), ("5Y", 0.04)]:
            n = m.yield_curve.zero.nodes.add()
            n.tenor = tenor
            n.value.fixed = v
        await rejected("an interpolated curve not anchored on the evaluation date", f,
                       "market[0].yield_curve.zero.nodes[0]", E.Error.INVALID_ARGUMENT)

        # A malformed date or tenor names its field. QuantLib's parsers throw
        # "stoi" or "invalid format" with nothing to say where, and that was
        # what came back.
        f = E.ClientFrame(request_id=next_id())
        f.open_session.evaluation_date.iso = "2026-09-xx"
        await rejected("a malformed ISO date", f, "evaluation_date", E.Error.INVALID_ARGUMENT)
        f = E.ClientFrame(request_id=next_id())
        f.open_session.evaluation_date.iso = TODAY.isoformat()
        m = f.open_session.market.add()
        m.id = "Z"
        act360(m.yield_curve.day_counter)
        m.yield_curve.calendar.name = C.Calendar.NULL_CALENDAR
        m.yield_curve.zero.compounding = C.CONTINUOUS
        m.yield_curve.zero.frequency = C.ANNUAL
        for tenor in ("0D", "5Q"):
            n = m.yield_curve.zero.nodes.add()
            n.tenor = tenor
            n.value.fixed = 0.03
        await rejected("a malformed tenor", f, "market[0].yield_curve.zero.nodes[1].tenor",
                       E.Error.INVALID_ARGUMENT)

        # Interpolation schemes the curve and surface builders never read. A
        # zero curve is linear in rates and a discount curve log-linear in
        # discount factors whatever was named; now anything else is refused.
        def interpolated(shape, interpolator):
            f = E.ClientFrame(request_id=next_id())
            f.open_session.evaluation_date.iso = TODAY.isoformat()
            m = f.open_session.market.add()
            m.id = "Z"
            act360(m.yield_curve.day_counter)
            m.yield_curve.calendar.name = C.Calendar.NULL_CALENDAR
            curve = getattr(m.yield_curve, shape)
            curve.interpolator = interpolator
            for tenor, v in [("0D", 1.0 if shape == "discount" else 0.03),
                             ("5Y", 0.8 if shape == "discount" else 0.04)]:
                n = curve.nodes.add()
                n.tenor = tenor
                n.value.fixed = v
            if shape == "zero":
                curve.compounding = C.CONTINUOUS
                curve.frequency = C.ANNUAL
            return f

        await rejected("a cubic zero curve, which interpolates linearly",
                       interpolated("zero", M.INTERPOLATOR_CUBIC),
                       "market[0].yield_curve.zero.interpolator", E.Error.UNSUPPORTED)
        await rejected("a linear discount curve, which interpolates log-linearly",
                       interpolated("discount", M.INTERPOLATOR_LINEAR),
                       "market[0].yield_curve.discount.interpolator", E.Error.UNSUPPORTED)

        def surface(**fields):
            f = E.ClientFrame(request_id=next_id())
            f.open_session.evaluation_date.iso = TODAY.isoformat()
            m = f.open_session.market.add()
            m.id = "VS"
            act360(m.volatility.day_counter)
            vs = m.volatility.variance_surface
            for t in (1.0, 2.0):
                vs.expiries.add().iso = expiry(t)
            vs.strikes.extend([90.0, 110.0])
            for _ in range(4):
                vs.volatilities.add().fixed = 0.2
            for name, value in fields.items():
                setattr(vs, name, value)
            return f, vs

        f, _ = surface(interpolator=M.INTERPOLATOR_CUBIC)
        await rejected("a bicubic variance surface, which interpolates bilinearly", f,
                       "market[0].volatility.variance_surface.interpolator", E.Error.UNSUPPORTED)
        f, _ = surface(strike_extrapolation=M.VarianceSurface.EXTRAPOLATION_CONSTANT)
        await rejected("a variance surface held flat across strikes", f,
                       "market[0].volatility.variance_surface.strike_extrapolation",
                       E.Error.UNSUPPORTED)
        f, _ = surface(time_extrapolation=M.VarianceSurface.EXTRAPOLATION_CONSTANT)
        await rejected("a variance surface extended past its last expiry", f,
                       "market[0].volatility.variance_surface.time_extrapolation",
                       E.Error.UNSUPPORTED)
        # A quote id here used to read as a volatility of zero.
        f, vs = surface()
        vs.volatilities[2].quote_id = "V"
        await rejected("a quote id on a variance surface cell", f,
                       "market[0].volatility.variance_surface.volatilities[2].quote_id",
                       E.Error.UNSUPPORTED)
        f, vs = surface()
        vs.volatilities[3].ClearField("fixed")
        await rejected("a variance surface cell with no value", f,
                       "market[0].volatility.variance_surface.volatilities[3]",
                       E.Error.INVALID_ARGUMENT)

        # -- an interpolated curve that is spelled correctly ------------------
        print("\n  -- a second session, built a different way --")
        f = E.ClientFrame(request_id=next_id())
        o = f.open_session
        o.evaluation_date.iso = (TODAY + timedelta(days=1)).isoformat()
        m = o.market.add()
        m.id = "S"
        m.quote.value = 100.0
        m = o.market.add()
        m.id = "V"
        m.quote.value = 0.20
        m = o.market.add()
        m.id = "Z"
        act360(m.yield_curve.day_counter)
        m.yield_curve.calendar.name = C.Calendar.NULL_CALENDAR
        m.yield_curve.zero.compounding = C.CONTINUOUS
        m.yield_curve.zero.frequency = C.ANNUAL
        m.yield_curve.zero.interpolator = M.INTERPOLATOR_LINEAR
        # The first node has to sit on the reference date: an
        # InterpolatedZeroCurve takes its reference from dates[0], so a curve
        # whose first pillar is 6M out starts six months from now and
        # discounts nothing before then.
        for tenor, v in [("0D", 0.05), ("1Y", 0.05), ("5Y", 0.05)]:
            n = m.yield_curve.zero.nodes.add()
            n.tenor = tenor
            n.value.fixed = v
        m = o.market.add()
        m.id = "VOL"
        act360(m.volatility.day_counter)
        m.volatility.constant.volatility.quote_id = "V"
        reply = await send(ws, f)
        ok = reply.HasField("session_opened")
        check("a session on an interpolated zero curve opens", ok,
              "" if ok else f"{reply.error.field_path!r} {reply.error.message!r}")

        if ok:
            sid2 = reply.session_id
            # The second session's evaluation date is a day later. Under
            # QL_ENABLE_SESSIONS that is thread-local, so the two disagree by
            # one day of theta; without it they would agree exactly and this
            # is the check that catches the wrong QuantLib being linked
            # (DESIGN §2).
            f, opt = base_frame(sid2)
            opt.payoff.type = I.Payoff.OPTION_TYPE_CALL
            opt.payoff.plain.strike = 100.0
            opt.exercise.type = I.Exercise.TYPE_EUROPEAN
            opt.exercise.dates.add().iso = expiry(1.0)
            u = opt.underlyings.add()
            u.spot_quote_id, u.discount_curve_id, u.volatility_id = "S", "Z", "VOL"
            opt.vanilla.SetInParent()
            f.price.engine.method = EN.Engine.METHOD_ANALYTIC
            second = await send(ws, f)
            ok2 = second.HasField("price_result")
            check("the second session prices", ok2,
                  "" if ok2 else f"{second.error.field_path!r} {second.error.message!r}")

            if ok2:
                await send(ws, set_market(sid, S=100.0, Q=0.0, R=0.05, V=0.20))
                first = await send(ws, vanilla_frame(
                    sid, dict(type="call", strike=100.0, s=100.0, q=0.0, r=0.05,
                              t=1.0, v=0.20)))
                gap = abs(first.price_result.npv - second.price_result.npv)
                check("two sessions hold their own evaluation dates",
                      1.0e-6 < gap < 1.0e-1,
                      f"one day of theta = {gap:.6f}; exactly zero would mean "
                      f"QL_ENABLE_SESSIONS is off")

            await send(ws, E.ClientFrame(request_id=next_id(), session_id=sid2,
                                         close_session=E.CloseSession()))


        # -- progress, and the cancels that depend on it ----------------------
        #
        # Three shapes take a stop where they stand: a batched Monte Carlo
        # between batches, a sweep between points, and a batch between trades.
        # QuantLib cannot be interrupted inside an engine call, so anywhere
        # else the cancel frees the client rather than the machine -- the
        # worker is disowned after a grace and the session replayed
        # (DESIGN §3). Each row of the table in HANDLERS.md is checked below.
        print("\n  -- progress and cancel --")
        await send(ws, set_market(sid, **row_market(row)))

        f = vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO, mc=(11, 400000))
        f.price.engine.mc.progress_every_paths = 50000
        await ws.send(f.SerializeToString())
        reply, progress = await collect(ws, f.request_id)
        check("batched Monte Carlo reports progress",
              reply.HasField("price_result") and progress >= 4,
              f"frames={progress} npv={reply.price_result.npv:.6f}")

        # The batched price is not the single-shot price: independent batches
        # with derived seeds partition the RNG stream differently. Both are
        # valid estimates, and the echo is what lets a client tell them apart.
        check("the batch size is echoed with the seed",
              reply.price_result.engine.mc.progress_every_paths == 50000)

        f = vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO, mc=(11, 4000000))
        f.price.engine.mc.progress_every_paths = 20000
        await ws.send(f.SerializeToString())
        # Wait for the calculation to be genuinely under way before cancelling,
        # so this tests the stop rather than a race with the queue.
        seen = 0
        while seen < 2:
            raw = await asyncio.wait_for(ws.recv(), timeout=60.0)
            g = E.ServerFrame()
            g.ParseFromString(raw)
            if g.HasField("progress"):
                seen += 1

        c = E.ClientFrame(request_id=next_id(), session_id=sid)
        c.cancel.target_request_id = f.request_id
        await ws.send(c.SerializeToString())

        terminals = {}
        deadline = time.time() + 60.0
        while f.request_id not in terminals or c.request_id not in terminals:
            raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.time()))
            g = E.ServerFrame()
            g.ParseFromString(raw)
            if g.terminal:
                terminals[g.request_id] = g

        cancelled = terminals[f.request_id]
        check("a cancelled request terminates as CANCELLED",
              cancelled.HasField("error") and cancelled.error.code == E.Error.CANCELLED,
              cancelled.error.message[:60] if cancelled.HasField("error") else "priced anyway")
        # Every request gets exactly one terminal frame, the cancel included:
        # nothing downstream answers a cancel, so the gateway does (DESIGN §9.5).
        check("the cancel itself is acknowledged",
              terminals[c.request_id].HasField("ack"))

        reply = await send(ws, vanilla_frame(sid, row))
        check("the session still prices after a cancel",
              reply.HasField("price_result") and abs(reply.price_result.npv - analytic) < 1e-12)

        async def cancel_after_progress(frame, label):
            """Sends, waits for the work to be genuinely under way, then stops it.

            Waiting for progress rather than sleeping is what makes this a test
            of the stop rather than a race with the queue.
            """
            await ws.send(frame.SerializeToString())
            seen = 0
            terminals = {}
            cancel = None
            deadline = time.time() + 60.0
            while True:
                raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.time()))
                g = E.ServerFrame()
                g.ParseFromString(raw)
                if g.HasField("progress"):
                    seen += 1
                    if seen == 2 and cancel is None:
                        cancel = E.ClientFrame(request_id=next_id(), session_id=sid)
                        cancel.cancel.target_request_id = frame.request_id
                        await ws.send(cancel.SerializeToString())
                if g.terminal:
                    terminals[g.request_id] = g
                if frame.request_id in terminals and cancel is not None \
                        and cancel.request_id in terminals:
                    break
            check(f"the cancel of {label} is acknowledged",
                  terminals[cancel.request_id].HasField("ack"))
            return terminals[frame.request_id]

        # A sweep stops between points and keeps the ones it priced: they were
        # computed correctly and the client paid for them.
        f = vanilla_frame(sid, row, EN.Engine.METHOD_LATTICE, steps=900)
        axis = f.price.scenarios.add()
        axis.quote_id = "S"
        axis.linear.begin = 60.0
        axis.linear.end = 140.0
        axis.linear.steps = 1200
        axis.plot = R.RESULT_KIND_NPV
        stopped = await cancel_after_progress(f, "a sweep")
        ok = stopped.HasField("scenario_result")
        check("a cancelled sweep keeps the points it already priced", ok,
              "" if ok else f"terminated as {stopped.WhichOneof('payload')}")
        if ok:
            partial = stopped.scenario_result
            check("and says how far it got rather than pretending to be whole",
                  0 < partial.abandoned_after < 1200
                  and len(partial.prices) == partial.abandoned_after,
                  f"abandoned_after={partial.abandoned_after} prices={len(partial.prices)}")
            check("and trims its axis to the ladder it is returning",
                  len(partial.axes[0].values) == len(partial.prices)
                  and len(partial.series.x) == len(partial.prices),
                  f"axis={len(partial.axes[0].values)} series={len(partial.series.x)}")

        reply = await send(ws, vanilla_frame(sid, row))
        check("a cancelled sweep leaves the market where it found it",
              abs(reply.price_result.npv - analytic) < 1.0e-12,
              f"before={analytic:.10f} after={reply.price_result.npv:.10f}")

        # A batch stops between trades, on the same terms.
        f = E.ClientFrame(request_id=next_id(), session_id=sid)
        for _ in range(400):
            one = f.batch.requests.add()
            one.CopyFrom(vanilla_frame(sid, row, EN.Engine.METHOD_LATTICE, steps=900).price)
        stopped = await cancel_after_progress(f, "a batch")
        ok = stopped.HasField("batch_result")
        check("a cancelled batch keeps the prices it already had", ok,
              "" if ok else f"terminated as {stopped.WhichOneof('payload')}")
        if ok:
            book = stopped.batch_result
            priced = sum(1 for e in book.entries if e.HasField("price"))
            check("and names the rest rather than dropping them",
                  0 < book.abandoned_after < 400 and len(book.entries) == 400
                  and priced == book.abandoned_after
                  and book.entries[-1].error.code == E.Error.CANCELLED,
                  f"abandoned_after={book.abandoned_after} priced={priced} "
                  f"entries={len(book.entries)}")

        # And the row the documentation used to get wrong: a single engine call
        # cannot be interrupted, so the cancel frees the client rather than the
        # machine. The request still terminates, and the session survives it --
        # the worker is disowned and the session replayed into a fresh one.
        f = vanilla_frame(sid, row, EN.Engine.METHOD_MONTE_CARLO, mc=(11, 20000000))
        await ws.send(f.SerializeToString())
        # Under way first: a cancel that reaches the request while it is still
        # queued removes it without running it, which is the cheap path tested
        # below rather than this one. A single engine call reports no progress
        # to wait for, so this waits out the relocation to a sacrificial worker
        # instead.
        await asyncio.sleep(0.5)
        c = E.ClientFrame(request_id=next_id(), session_id=sid)
        c.cancel.target_request_id = f.request_id
        await ws.send(c.SerializeToString())
        terminals = {}
        deadline = time.time() + 60.0
        while f.request_id not in terminals or c.request_id not in terminals:
            raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.time()))
            g = E.ServerFrame()
            g.ParseFromString(raw)
            if g.terminal:
                terminals[g.request_id] = g
        uninterruptible = terminals[f.request_id]
        check("a cancel inside one engine call still terminates the request",
              uninterruptible.HasField("error")
              and uninterruptible.error.code == E.Error.CANCELLED,
              E.Error.Code.Name(uninterruptible.error.code)
              if uninterruptible.HasField("error") else "priced anyway")
        reply = await send(ws, vanilla_frame(sid, row))
        check("and the session survives it, replayed into a fresh worker",
              reply.HasField("price_result")
              and abs(reply.price_result.npv - analytic) < 1.0e-12,
              f"{reply.price_result.npv:.10f}" if reply.HasField("price_result")
              else E.Error.Code.Name(reply.error.code))

        # A cancel stops the request it names and nothing else. The two ways
        # to get that wrong both used to end in a kill, which takes whatever
        # else is running on the worker with it: a target that is not running
        # at all, and one still queued behind another request. A long sweep is
        # the other request here, because a stop that reached it would show:
        # a sweep stops between points and says how far it got, and a kill
        # would fail it outright. Left alone, it prices every point.
        def long_sweep():
            f = vanilla_frame(sid, row, EN.Engine.METHOD_LATTICE, steps=MAX_LATTICE_STEPS)
            axis = f.price.scenarios.add()
            axis.quote_id = "S"
            axis.linear.begin = 60.0
            axis.linear.end = 140.0
            axis.linear.steps = LONG_SWEEP_POINTS
            return f

        def whole(frame):
            return (frame.HasField("scenario_result")
                    and frame.scenario_result.abandoned_after == 0
                    and len(frame.scenario_result.prices) == LONG_SWEEP_POINTS)

        def swept(frame):
            if frame.HasField("scenario_result"):
                r = frame.scenario_result
                return f"{len(r.prices)} of {LONG_SWEEP_POINTS} points, abandoned_after={r.abandoned_after}"
            return outcome(frame)

        async def terminals_for(*ids, timeout=60.0):
            """Every terminal frame for these ids, in the order they arrived."""
            order, frames = [], {}
            deadline = time.time() + timeout
            while len(frames) < len(ids):
                raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.time()))
                g = E.ServerFrame()
                g.ParseFromString(raw)
                if g.terminal and g.request_id in ids:
                    frames[g.request_id] = g
                    order.append(g.request_id)
            return frames, order

        def outcome(frame):
            if frame.HasField("error"):
                return E.Error.Code.Name(frame.error.code)
            return frame.WhichOneof("payload")

        slow = long_sweep()
        started = time.time()
        await ws.send(slow.SerializeToString())
        c = E.ClientFrame(request_id=next_id(), session_id=sid)
        c.cancel.target_request_id = slow.request_id + 1000000   # nothing by that id
        await ws.send(c.SerializeToString())
        got, _ = await terminals_for(slow.request_id, c.request_id)
        took = time.time() - started
        check("a cancel naming no running request is acknowledged",
              got[c.request_id].HasField("ack"), outcome(got[c.request_id]))
        check("and stops nothing: the request that was running prices every point",
              whole(got[slow.request_id]), f"{swept(got[slow.request_id])} after {took:.2f}s")

        slow = long_sweep()
        queued = vanilla_frame(sid, row)
        await ws.send(slow.SerializeToString())
        await ws.send(queued.SerializeToString())
        c = E.ClientFrame(request_id=next_id(), session_id=sid)
        c.cancel.target_request_id = queued.request_id
        await ws.send(c.SerializeToString())
        got, order = await terminals_for(slow.request_id, queued.request_id, c.request_id)
        check("a cancel of a queued request removes it without running it",
              got[queued.request_id].HasField("error")
              and got[queued.request_id].error.code == E.Error.CANCELLED
              and order.index(queued.request_id) < order.index(slow.request_id),
              f"{outcome(got[queued.request_id])}, answered "
              f"{'before' if order.index(queued.request_id) < order.index(slow.request_id) else 'after'}"
              " the request ahead of it")
        check("and leaves the request ahead of it running to the end",
              whole(got[slow.request_id]), swept(got[slow.request_id]))
        check("and the cancel of a queued request is acknowledged",
              got[c.request_id].HasField("ack"), outcome(got[c.request_id]))

        # A frame naming another connection's session is refused to the socket
        # that sent it, and nowhere else. Routed by the session it named, the
        # refusal used to go to the owner instead, under the owner's own
        # request id, and the sender waited for an answer that never came.
        slow = long_sweep()
        await ws.send(slow.SerializeToString())
        async with connect(URL, max_size=None) as other:
            intruder = vanilla_frame(sid, row)
            intruder.request_id = slow.request_id   # an id the owner is waiting on
            try:
                reply = await asyncio.wait_for(send(other, intruder), timeout=10.0)
                heard = (E.Error.Code.Name(reply.error.code) if reply.HasField("error")
                         else reply.WhichOneof("payload"))
            except asyncio.TimeoutError:
                reply, heard = None, "no answer in 10 s"
        got, _ = await terminals_for(slow.request_id)
        check("a frame naming another connection's session is refused to its sender",
              reply is not None and reply.HasField("error")
              and reply.error.code == E.Error.SESSION_NOT_FOUND, heard)
        check("and the owner hears nothing of it: its request ends in its own result",
              whole(got[slow.request_id]), swept(got[slow.request_id]))

        # -- the quanto barrier benchmark -------------------------------------
        #
        # testBarrierValues in quantooption.cpp carries a "TODO: bench against
        # an existing prop calculator" and a tolerance of 0.5 to match. There
        # is no vendor pricer here either, but the same file benchmarks the
        # quanto vanilla against a PDE, and FdBlackScholesBarrierEngine is
        # single-argument constructible, so QuantoEngine can wrap it and the
        # trick works for barriers too. See test/BENCHMARK.md.
        print("\n  -- quanto barriers: analytic against the PDE --")
        for n, brow in enumerate(T.QUANTO_BARRIER):
            await send(ws, set_market(sid, **row_market(brow)))
            got = (await send(ws, barrier_frame(sid, brow, quanto=True))).price_result.npv

            errs = {}
            for name, preset in [("coarse", EN.FdParameters.PRESET_COARSE),
                                 ("standard", EN.FdParameters.PRESET_STANDARD),
                                 ("fine", EN.FdParameters.PRESET_FINE)]:
                pde = await send(ws, barrier_frame(
                    sid, brow, method=EN.Engine.METHOD_FINITE_DIFFERENCE, preset=preset,
                    quanto=True))
                if not pde.HasField("price_result"):
                    check(f"quanto barrier[{n}] PDE {name}", False,
                          f"{E.Error.Code.Name(pde.error.code)} {pde.error.message!r}")
                    break
                errs[name] = abs(pde.price_result.npv - got)
                fine = pde.price_result.npv
            else:
                check(f"quanto barrier[{n}] analytic agrees with the PDE",
                      errs["fine"] <= 1.0e-3,
                      f"analytic={got:.6f} pde={fine:.6f} err={errs['fine']:.2e}")
                # A fixed offset would mean the two methods agree on a
                # different instrument. Refinement has to keep closing the gap.
                check(f"quanto barrier[{n}] PDE converges",
                      errs["coarse"] > errs["standard"] > errs["fine"],
                      " > ".join(f"{k}={errs[k]:.2e}" for k in ("coarse", "standard", "fine")))
                gap = abs(got - brow["result"])
                # Reported, not asserted. The recorded value is the thing under
                # test here, and it is the one with no provenance.
                print(f"         recorded={brow['result']} ours={got:.6f} gap={gap:.3e}"
                      + ("   <-- recorded value not reproduced" if gap > 1.0e-2 else ""))

        # In-out parity and an unreachable barrier need no second engine at
        # all, and both hold exactly.
        brow = dict(T.QUANTO_BARRIER[0], rebate=0.0)
        await send(ws, set_market(sid, **row_market(brow)))
        plain = (await send(ws, vanilla_frame(sid, brow, quanto=True))).price_result.npv
        down_in = (await send(ws, barrier_frame(
            sid, dict(brow, barrier_type="down_in"), quanto=True))).price_result.npv
        down_out = (await send(ws, barrier_frame(
            sid, dict(brow, barrier_type="down_out"), quanto=True))).price_result.npv
        check("in-out parity", abs(down_in + down_out - plain) < 1.0e-9,
              f"{down_in:.8f} + {down_out:.8f} vs {plain:.8f}")

        unreachable = (await send(ws, barrier_frame(
            sid, dict(brow, barrier_type="down_out", barrier=1.0e-6), quanto=True))
        ).price_result.npv
        check("an unreachable barrier degrades to the vanilla price",
              abs(unreachable - plain) < 1.0e-9)


        # -- swaps: an index, a bootstrapped curve, and the cycle between them --
        #
        # The curve's pillars name the index for their conventions and the
        # index names the curve to forecast off, so one of them is always a
        # forward reference. The session resolves it with a relinkable handle
        # the way QuantLib's own bootstrap does; this is the check that the
        # cycle is legal in the order a client would naturally write it.
        print("\n  -- swaps --")

        def swap_session(forwarding_curve="DISC"):
            f = E.ClientFrame(request_id=next_id())
            o = f.open_session
            o.evaluation_date.iso = TODAY.isoformat()
            for qid, v in [("D1M", 0.038), ("D3M", 0.039), ("D6M", 0.040),
                           ("D12M", 0.041), ("S2Y", 0.042), ("S5Y", 0.045), ("S10Y", 0.047),
                           ("FIX", 0.04)]:
                m = o.market.add()
                m.id = qid
                m.quote.value = v
            m = o.market.add()
            m.id = "EUR6M"
            ix = m.index
            ix.family = M.Index.FAMILY_IBOR
            ix.name, ix.tenor, ix.fixing_days = "Euribor", "6M", 2
            ix.fixing_calendar.name = C.Calendar.TARGET
            ix.convention = C.MODIFIED_FOLLOWING
            ix.day_counter.family = C.DayCounter.ACTUAL_360
            ix.end_of_month = M.FLAG_FALSE
            ix.forwarding_curve_id = forwarding_curve   # defined below, not above
            m = o.market.add()
            m.id = "DISC"
            yc = m.yield_curve
            yc.day_counter.family = C.DayCounter.ACTUAL_365_FIXED
            yc.calendar.name = C.Calendar.TARGET
            yc.settlement_days = 2
            yc.bootstrap.traits = M.BootstrappedCurve.TRAITS_DISCOUNT
            yc.bootstrap.interpolator = M.INTERPOLATOR_LOG_LINEAR
            for qid, tenor in [("D1M", "1M"), ("D3M", "3M"), ("D6M", "6M"), ("D12M", "12M")]:
                pl = yc.bootstrap.pillars.add()
                pl.quote_id, pl.tenor, pl.kind, pl.index_id = qid, tenor, M.Pillar.KIND_DEPOSIT, "EUR6M"
            # Swap pillars past the deposits, so a 5Y trade is inside the curve
            # rather than extrapolated off its last deposit.
            for qid, tenor in [("S2Y", "2Y"), ("S5Y", "5Y"), ("S10Y", "10Y")]:
                pl = yc.bootstrap.pillars.add()
                pl.quote_id, pl.tenor, pl.kind, pl.index_id = qid, tenor, M.Pillar.KIND_SWAP, "EUR6M"
                pl.calendar.name = C.Calendar.TARGET
                pl.fixed_frequency = C.ANNUAL
                pl.fixed_convention = C.MODIFIED_FOLLOWING
                pl.fixed_day_counter.family = C.DayCounter.THIRTY_360
                pl.fixed_day_counter.thirty_360 = C.DayCounter.BOND_BASIS
            return f

        def swap_frame(sid, results=(), method=EN.Engine.METHOD_DISCOUNTING):
            f = E.ClientFrame(request_id=next_id(), session_id=sid)
            sw = f.price.instrument.swap
            sw.discount_curve_id = "DISC"
            for kind, pays, freq in [(I.Leg.KIND_FIXED, M.FLAG_TRUE, C.ANNUAL),
                                     (I.Leg.KIND_IBOR, M.FLAG_FALSE, C.SEMIANNUAL)]:
                lg = sw.legs.add()
                lg.kind, lg.pays = kind, pays
                lg.notionals.append(1.0e6)
                lg.day_counter.family = C.DayCounter.ACTUAL_360
                sc = lg.schedule
                sc.start.iso = (TODAY + timedelta(days=2)).isoformat()
                sc.maturity.iso = (TODAY + timedelta(days=2 + 5 * 365)).isoformat()
                sc.frequency = freq
                sc.calendar.name = C.Calendar.TARGET
                sc.convention = C.MODIFIED_FOLLOWING
                sc.date_generation = I.Schedule.DATE_GENERATION_BACKWARD
                sc.end_of_month = M.FLAG_FALSE
                if kind == I.Leg.KIND_FIXED:
                    lg.rate_quote_id = "FIX"
                else:
                    lg.index_id = "EUR6M"
                    lg.in_arrears = M.FLAG_FALSE
            f.price.engine.method = method
            f.price.results.extend(results)
            return f

        reply = await send(ws, swap_session())
        ok = reply.HasField("session_opened")
        check("an index may forward-reference the curve bootstrapped off it", ok,
              "" if ok else f"{reply.error.field_path!r} {reply.error.message!r}")

        if ok:
            sid3 = reply.session_id
            reply = await send(ws, swap_frame(sid3, [R.RESULT_KIND_FAIR_RATE,
                                                     R.RESULT_KIND_LEG_NPV]))
            ok = reply.HasField("price_result")
            check("a fixed-for-Ibor swap prices through the general Swap", ok,
                  "" if ok else f"{reply.error.field_path!r} {reply.error.message!r}")

            if ok:
                res = reply.price_result.results
                npv = reply.price_result.npv
                legs = res["legNPV.0"].scalar + res["legNPV.1"].scalar
                check("leg NPVs sum to the swap NPV", abs(legs - npv) < 1e-6,
                      f"npv={npv:.4f} legs={legs:.4f}")

                # The fair rate is the fixed rate that zeroes the NPV. A fixed
                # leg is frozen at construction (session.cpp, buildLeg), so the
                # check is a quote write followed by a *new* request, which is
                # exactly what the comment there tells a client to do.
                fair = res["fairRate"].scalar
                await send(ws, set_market(sid3, FIX=fair))
                at_par = (await send(ws, swap_frame(sid3))).price_result.npv
                check("repricing at the fair rate gives zero NPV",
                      abs(at_par) < 1e-4 * 1.0e6,
                      f"fairRate={fair:.6f} npv={at_par:.6f}")

                # A pillar bump has to reach the swap through two handles: the
                # curve rebootstraps, and the index forecasts off the relinked
                # curve. If the relink had made a copy, this would not move.
                await send(ws, set_market(sid3, S5Y=0.055))
                bumped = (await send(ws, swap_frame(sid3))).price_result.npv
                check("a pillar bump moves the swap through the relinked index",
                      abs(bumped - at_par) > 1.0,
                      f"{at_par:.4f} -> {bumped:.4f}")

                # A forced re-bootstrap that fails. It runs after the writes
                # are committed, so the graph holds a pillar the log does not:
                # the update is refused and dropped from the log, and the
                # session used to go on pricing off the graph that kept it.
                # Now it is dirty, replayed from the log, and prices as before.
                f = set_market(sid3, D1M=-50.0)
                f.update_market.force_rebootstrap = True
                reply = await send(ws, f)
                check("a forced re-bootstrap that cannot bootstrap is refused",
                      reply.HasField("error"), reply.WhichOneof("payload"))
                again = await send(ws, swap_frame(sid3))
                check("...and the session prices from its log, without the refused pillar",
                      again.HasField("price_result")
                      and abs(again.price_result.npv - bumped) < 1e-6,
                      f"{bumped:.4f} -> {again.price_result.npv:.4f}"
                      if again.HasField("price_result")
                      else f"{again.error.message[:120]!r}")

                # The cash-flow table is the panel showing its working, and the
                # property that makes it worth showing is that its present
                # values add up to the NPV. Leg 0 pays and leg 1 receives, so
                # the signs come from the trade rather than from the rows.
                f = swap_frame(sid3)
                f.price.include_cashflows = True
                reply = await send(ws, f)
                rows = list(reply.price_result.cashflows)
                total = sum((-1.0 if r.leg == 0 else 1.0) * r.present_value for r in rows)
                check("the cash-flow table adds up to the NPV",
                      bool(rows) and abs(total - reply.price_result.npv) < 1e-8,
                      f"{len(rows)} rows, sum={total:.6f} npv={reply.price_result.npv:.6f}")

                fixed = [r for r in rows if r.leg == 0]
                floating = [r for r in rows if r.leg == 1]
                check("a fixed row carries its accrual and its rate",
                      bool(fixed) and all(r.notional > 0 and r.accrual_period > 0
                                          and r.rate > 0 for r in fixed),
                      f"{len(fixed)} fixed rows")
                # This swap starts in two days, so none of its fixings has
                # happened; what is checked is that the flag agrees with the
                # date beside it rather than that both kinds are present.
                consistent = all(
                    r.is_past_fixing == (r.fixing_date.iso <= TODAY.isoformat())
                    for r in floating)
                check("a floating row's fixing flag agrees with its fixing date",
                      bool(floating) and consistent,
                      f"{len(floating)} rows, first fixing {floating[0].fixing_date.iso if floating else '-'}"
                      f", evaluation date {TODAY.isoformat()}")

                # A leg that does not name its fixing days fixes on the
                # index's -- two, for EUR6M -- as IborLeg does by default. The
                # field had no presence, so unset read as 0 and every such leg
                # fixed on its accrual start; 0 is still there, when named.
                async def first_fixing(fixing_days=None):
                    f = swap_frame(sid3)
                    f.price.include_cashflows = True
                    if fixing_days is not None:
                        f.price.instrument.swap.legs[1].fixing_days = fixing_days
                    reply = await send(ws, f)
                    row = next((r for r in reply.price_result.cashflows if r.leg == 1), None)
                    return (row.fixing_date.iso, row.accrual_start.iso) if row else None

                unset, two, zero = [await first_fixing(n) for n in (None, 2, 0)]
                check("a leg with no fixing days fixes on the index's",
                      unset is not None and unset == two and unset[0] < unset[1],
                      f"unset={unset} two={two}")
                check("...and fixing days of 0, named, fix on the accrual start",
                      zero is not None and zero[0] == zero[1], f"zero={zero}")

            # The fair-rate formula assumes fixed first, floating second; any
            # other order would return a wrong number silently.
            f = swap_frame(sid3, [R.RESULT_KIND_FAIR_RATE])
            legs = f.price.instrument.swap.legs
            first, second = I.Leg(), I.Leg()
            first.CopyFrom(legs[1]); second.CopyFrom(legs[0])
            del legs[:]
            legs.add().CopyFrom(first); legs.add().CopyFrom(second)
            await rejected("fair rate with the legs in the other order", f,
                           "instrument.swap.legs", E.Error.UNSUPPORTED)

            f = swap_frame(sid3)
            f.price.instrument.swap.legs[0].ClearField("pays")
            await rejected("unset pays Flag on a leg", f, "instrument.swap.legs[0].pays",
                           E.Error.UNSPECIFIED_ENUM)

            # The v1 defect: a swap priced with an unset engine succeeded.
            f = swap_frame(sid3, method=EN.Engine.METHOD_UNSPECIFIED)
            await rejected("a swap with no engine method", f, "engine.method",
                           E.Error.UNSPECIFIED_ENUM)

            await send(ws, E.ClientFrame(request_id=next_id(), session_id=sid3,
                                         close_session=E.CloseSession()))

        # A forward reference that never resolves is a named rejection, not an
        # empty handle that fails at the first forecast.
        await rejected("an index forwarding off a curve that never appears",
                       swap_session(forwarding_curve="NOPE"),
                       "market[8].index.forwarding_curve_id", E.Error.UNKNOWN_ID)

        # Dual-curve bootstrapping: no helper is given a discounting curve, so
        # a pillar naming one was bootstrapped single-curve without a word.
        f = swap_session()
        f.open_session.market[9].yield_curve.bootstrap.pillars[4].discount_curve_id = "OIS"
        await rejected("a dual-curve pillar, which bootstraps single-curve",
                       f, "market[9].yield_curve.bootstrap.pillars[4].discount_curve_id",
                       E.Error.UNSUPPORTED)

        # -- close -----------------------------------------------------------
        print("\n  -- close --")
        reply = await send(ws, E.ClientFrame(request_id=next_id(), session_id=sid,
                                             close_session=E.CloseSession()))
        check("CloseSession", reply.HasField("ack"))

        f = vanilla_frame(sid, row)
        reply = await send(ws, f)
        check("pricing a closed session fails as SESSION_NOT_FOUND",
              reply.HasField("error") and reply.error.code == E.Error.SESSION_NOT_FOUND)

        # -- resume: a session, and the work in it, outliving the socket ------
        #
        # The socket used to take the session with it, and whatever was running
        # in it: a blink half way through a long Monte Carlo cost the
        # calculation. The bootstrap was never the expensive part -- 0.009 ms
        # for this market -- so what the window is for is the work.
        print("\n  -- resume --")

        async with connect(URL, max_size=None) as doomed:
            opened = await send(doomed, open_session())
            resume_id = opened.session_opened.session_id
            token = opened.session_opened.resume_token
            grace = opened.session_opened.resume_grace_seconds
            check("SessionOpened carries a resume token and a window",
                  len(token) >= 32 and grace > 0,
                  f"token {len(token)} chars, grace {grace}s")
            check("and says it is not itself a resume", not opened.session_opened.resumed)

            # Forty million paths in batches: long enough to still be running
            # when the socket goes, and reporting so we know it started.
            mc = vanilla_frame(resume_id, dict(type="call", strike=100.0, t=1.0),
                               EN.Engine.METHOD_MONTE_CARLO, mc=(42, 40_000_000))
            mc.price.engine.mc.progress_every_paths = 200_000
            await doomed.send(mc.SerializeToString())
            started = False
            for _ in range(50):
                frame = E.ServerFrame()
                frame.ParseFromString(await asyncio.wait_for(doomed.recv(), 30))
                if frame.HasField("progress"):
                    started = True
                    break
            check("a batched Monte Carlo reports before the socket dies", started)

        # The socket is gone. Come back for the session, and for the answer.
        async with connect(URL, max_size=None) as returning:
            back = E.ClientFrame(request_id=next_id())
            back.resume_session.session_id = resume_id
            back.resume_session.resume_token = token
            reply = await send(returning, back)
            resumed = reply.HasField("session_opened") and reply.session_opened.resumed
            check("ResumeSession gives the same session back",
                  resumed and reply.session_opened.session_id == resume_id,
                  reply.session_opened.session_id if resumed
                  else E.Error.Code.Name(reply.error.code))

            outcome = None
            for _ in range(400):
                frame = E.ServerFrame()
                frame.ParseFromString(await asyncio.wait_for(returning.recv(), 120))
                if frame.request_id == mc.request_id and frame.terminal:
                    outcome = frame
                    break
            check("and the calculation that was running survived the drop",
                  outcome is not None and outcome.HasField("price_result")
                  and outcome.price_result.npv > 0.0,
                  f"npv={outcome.price_result.npv:.6f}" if outcome is not None
                  and outcome.HasField("price_result") else "no terminal frame came back")

            # A token is a bearer secret, so a wrong one is refused -- and it
            # is refused with the same answer as a session that never existed,
            # which is what stops a guess from being a probe.
            wrong = E.ClientFrame(request_id=next_id())
            wrong.resume_session.session_id = resume_id
            wrong.resume_session.resume_token = "0" * len(token)
            reply = await send(returning, wrong)
            check("a wrong token is refused",
                  reply.HasField("error") and reply.error.code == E.Error.SESSION_NOT_FOUND,
                  E.Error.Code.Name(reply.error.code) if reply.HasField("error") else "resumed it")

            await send(returning, E.ClientFrame(request_id=next_id(), session_id=resume_id,
                                                close_session=E.CloseSession()))

        # Closed on purpose is not dropped by accident: there is nothing to
        # come back for, and the token dies with the session.
        async with connect(URL, max_size=None) as after:
            back = E.ClientFrame(request_id=next_id())
            back.resume_session.session_id = resume_id
            back.resume_session.resume_token = token
            reply = await send(after, back)
            check("a session closed on purpose cannot be resumed",
                  reply.HasField("error") and reply.error.code == E.Error.SESSION_NOT_FOUND,
                  E.Error.Code.Name(reply.error.code) if reply.HasField("error") else "resumed it")

            # Expiry is a wall-clock wait, so it is only exercised when the
            # daemon was started with a window short enough to sit through:
            #     ./build/ql-backend --port 9111 --session-grace 2
            if grace <= 5:
                # On a socket of its own, which then goes: a session still
                # held by a live connection is not detached at all, and
                # checking that would prove nothing about the window.
                async with connect(URL, max_size=None) as brief:
                    opened = await send(brief, open_session())
                    short_id = opened.session_opened.session_id
                    short_token = opened.session_opened.resume_token
                await asyncio.sleep(grace + 1.5)
                back = E.ClientFrame(request_id=next_id())
                back.resume_session.session_id = short_id
                back.resume_session.resume_token = short_token
                reply = await send(after, back)
                check("and a window that has expired is gone",
                      reply.HasField("error") and reply.error.code == E.Error.SESSION_NOT_FOUND,
                      E.Error.Code.Name(reply.error.code) if reply.HasField("error") else "resumed it")

        # -- liveness ---------------------------------------------------------
        #
        # The socket connecting used to be the only liveness signal, which a
        # proxy or an orchestrator cannot use: it has to open a WebSocket to
        # find out whether to restart the process.
        print("\n  -- liveness --")
        with urllib.request.urlopen(HEALTH, timeout=5) as response:
            status = response.status
            headers = dict(response.headers)
            health = json.loads(response.read())
        check("GET /healthz answers without a WebSocket",
              status == 200 and health.get("status") == "ok",
              f"{status} {health}")
        check("and names the build a bug report would have to quote",
              bool(health.get("build")) and bool(health.get("quantlib")),
              f"{health.get('build')} on QuantLib {health.get('quantlib')}")

        # The numbers have to be the real ones, or the endpoint is a constant
        # dressed as a measurement. This connection is open, and every session
        # it held is closed by now -- which the count used to miss, reporting
        # them as open until the socket went -- so sessions are checked below
        # by opening one and closing it again.
        #
        # Unless a token is required, in which case there are no numbers at
        # all: a probe cannot present a secret and still has to be able to ask
        # whether the process is alive, so what an unauthenticated caller may
        # read is trimmed to that instead. How much is open is the business of
        # whoever can open something.
        if TOKEN:
            check("and tells an unauthenticated caller nothing but that it is alive",
                  "connections" not in health and "sessions" not in health,
                  f"{health}")
        else:
            check("and counts the sockets it is actually serving",
                  health.get("connections", 0) >= 1 and "sessions" in health,
                  f"connections={health.get('connections')} sessions={health.get('sessions')}")

            # And counts them exactly: one more while a session is open, and
            # none once it is closed. A closed session's route used to stay
            # until its socket went, so it went on being counted as open.
            def sessions_now():
                with urllib.request.urlopen(HEALTH, timeout=5) as response:
                    return json.loads(response.read())["sessions"]

            before = sessions_now()
            async with connect(URL, max_size=None) as probe:
                opened = await send(probe, open_session())
                during = sessions_now()
                await send(probe, E.ClientFrame(request_id=next_id(),
                                                session_id=opened.session_opened.session_id,
                                                close_session=E.CloseSession()))
                after = sessions_now()
            check("and counts a session while it is open and not once it is closed",
                  during == before + 1 and after == before,
                  f"before={before} open={during} closed={after}")

        # A browser may send this request from any page it likes; without the
        # header it cannot read the reply, and these counts are not something a
        # random tab should be able to poll.
        check("and does not let a browser read it cross-origin",
              "Access-Control-Allow-Origin" not in headers,
              str(headers.get("Access-Control-Allow-Origin")))

        # -- the door ---------------------------------------------------------
        #
        # A WebSocket upgrade is not bound by the same-origin policy, so
        # loopback is not a boundary against a browser: any page in any tab can
        # open this socket. The rule is that a present Origin must be allowed
        # and an absent one is not the browser case at all -- which is why
        # every check above, sending none, still works.
        print("\n  -- the door --")
        check("a client that sends no origin is served, which is this test",
              ws.state.name == "OPEN")

        try:
            await connect(URL, origin="https://evil.example",
                                     max_size=16 << 20)
            refused = False
        except Exception as e:
            refused = "403" in str(e)
        check("a browser origin that is not allowed is refused at the upgrade",
              refused)

        allowed = await connect(URL, origin="http://localhost:5173",
                                           max_size=16 << 20)
        check("and the development server's origin is let through",
              allowed.state.name == "OPEN")
        await allowed.close()

        # Sockets are capped too, and refused at the upgrade so the client reads
        # a status rather than an unexplained disconnect.
        extra = []
        refused_at = None
        for n in range(64):
            try:
                extra.append(await connect(URL, max_size=16 << 20))
            except Exception as e:
                refused_at = (n, "503" in str(e))
                break
        check("a connection past the socket limit is refused at the upgrade",
              refused_at is not None and refused_at[1],
              f"opened {len(extra)} more before {refused_at}")
        for e in extra:
            await e.close()

        # A session is a live graph on a worker seat, so the cap on them is what
        # protects the pool. Sixteen is the default; the seventeenth is refused
        # by name rather than by running out of seats. On a connection of its
        # own, so the count is known exactly -- and after four opens the worker
        # refused, which used to keep their slots: this connection then held
        # twelve sessions, not sixteen, and the check could not tell.
        async with connect(URL, max_size=None) as limited:
            refused = 0
            for _ in range(4):
                f = E.ClientFrame(request_id=next_id())
                f.open_session.evaluation_date.iso = TODAY.isoformat()
                m = f.open_session.market.add()
                m.id = "BAD"
                m.correlation.labels.extend(["A", "B", "C"])
                for a, b, c in [(1.0, 0.9, 0.9), (0.9, 1.0, -0.9), (0.9, -0.9, 1.0)]:
                    for v in (a, b, c):
                        m.correlation.values.add().fixed = v
                refused += (await send(limited, f)).HasField("error")

            held = []
            while len(held) < 17:
                reply = await send(limited, open_session())
                if not reply.HasField("session_opened"):
                    break
                held.append(reply.session_opened.session_id)
            check("a connection past its session limit is refused as OVERLOADED",
                  reply.HasField("error") and reply.error.code == E.Error.OVERLOADED,
                  E.Error.Code.Name(reply.error.code) if reply.HasField("error")
                  else "opened a seventeenth")
            check("and a refused open gives its slot back: sixteen are held, not fewer",
                  refused == 4 and len(held) == 16,
                  f"{refused} opens refused, then {len(held)} held")

            # Refused, not broken: closing one makes room, so a client that hit
            # the limit recovers by tidying up rather than by reconnecting.
            if held:
                await send(limited, E.ClientFrame(request_id=next_id(), session_id=held[0],
                                                  close_session=E.CloseSession()))
            reply = await send(limited, open_session())
            check("and closing one makes room again",
                  reply.HasField("session_opened"),
                  "" if reply.HasField("session_opened")
                  else E.Error.Code.Name(reply.error.code))

    print(f"\n{checks} checks, {len(failures)} failed")
    if failures:
        print("FAILED: " + ", ".join(failures))
        return 1
    print("\nALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
