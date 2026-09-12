# Your first price

One script, start to finish: connect, ask what the build prices, open a
session, price a European call, hand the seat back. It needs the Python
bindings from the previous page and a running service.

```bash
./build/ql-backend --port 9111 &
.venv/bin/python first_price.py pb
```

```python
"""Connect, ask what this build prices, open a session, price one option."""

import asyncio
import sys

sys.path.insert(0, sys.argv[1])          # where protoc put the bindings

import websockets
from quantlib.v1 import conventions_pb2 as C
from quantlib.v2 import engine_pb2 as EN
from quantlib.v2 import envelope_pb2 as E
from quantlib.v2 import instrument_pb2 as I
from quantlib.v2 import market_pb2 as M
from quantlib.v2 import results_pb2 as R

URL = "ws://127.0.0.1:9111"


async def roundtrip(ws, frame):
    """Send one frame; return the terminal reply, skipping Progress."""
    await ws.send(frame.SerializeToString())
    while True:
        reply = E.ServerFrame()
        reply.ParseFromString(await ws.recv())
        if reply.WhichOneof("payload") != "progress":
            return reply


async def main():
    async with websockets.connect(URL, subprotocols=["qlservice.v2"]) as ws:
        # 1. What does this build price? No session needed.
        hello = await roundtrip(ws, E.ClientFrame(request_id=1, hello=E.Hello()))
        caps = hello.capabilities
        print("build      ", caps.build, "on QuantLib", caps.quantlib_version)
        print("styles     ", ", ".join(caps.option_styles))

        # 2. Open a session: an evaluation date, then the market in
        #    dependency order -- quotes, then the curves that observe them.
        f = E.ClientFrame(request_id=2)
        o = f.open_session
        o.evaluation_date.iso = "2026-09-01"
        for qid, value, unit in (("S", 100.0, M.Quote.UNIT_ABSOLUTE),
                                 ("R", 0.05, M.Quote.UNIT_RATE),
                                 ("V", 0.20, M.Quote.UNIT_RATE)):
            m = o.market.add(); m.id = qid
            m.quote.value, m.quote.unit = value, unit

        m = o.market.add(); m.id = "RC"
        m.yield_curve.day_counter.family = C.DayCounter.ACTUAL_360
        m.yield_curve.flat.rate.quote_id = "R"
        m.yield_curve.flat.compounding = C.CONTINUOUS
        m.yield_curve.flat.frequency = C.ANNUAL

        m = o.market.add(); m.id = "VOL"
        m.volatility.day_counter.family = C.DayCounter.ACTUAL_360
        m.volatility.constant.volatility.quote_id = "V"

        opened = await roundtrip(ws, f)
        sid = opened.session_id

        # 3. Price: payoff x exercise x underlying x style.
        f = E.ClientFrame(request_id=3, session_id=sid)
        opt = f.price.instrument.option
        u = opt.underlyings.add()
        u.spot_quote_id, u.volatility_id, u.discount_curve_id = "S", "VOL", "RC"
        u.process = I.Underlying.PROCESS_BLACK_SCHOLES_MERTON
        opt.payoff.type = I.Payoff.OPTION_TYPE_CALL
        opt.payoff.plain.strike = 100.0
        opt.exercise.type = I.Exercise.TYPE_EUROPEAN
        opt.exercise.dates.add().iso = "2027-09-01"
        opt.vanilla.SetInParent()
        f.price.engine.method = EN.Engine.METHOD_ANALYTIC
        f.price.results.append(R.RESULT_KIND_DELTA)

        priced = await roundtrip(ws, f)
        if priced.WhichOneof("payload") == "error":
            print("error      ", priced.error.code, priced.error.field_path)
            return 1
        r = priced.price_result
        print("npv        ", round(r.npv, 6))
        print("delta      ", round(r.results["delta"].scalar, 6))

        # 4. Give the seat back.
        f = E.ClientFrame(request_id=4, session_id=sid)
        f.close_session.SetInParent()
        await roundtrip(ws, f)
    return 0


sys.exit(asyncio.run(main()))
```

What it prints, against this build:

```
build       ql-backend on QuantLib 1.43
styles      vanilla, barrier, double_barrier, asian, lookback, forward_start,
            compound, chooser, cliquet, basket
npv         10.539466
delta       0.637739
```

## What each step is doing

**`Hello` needs no session.** What a build can price is a property of the
service, so a client can ask before it commits a worker seat — and should,
rather than hardcoding a list that will drift.

**The market arrives in dependency order.** The session resolves ids as it
fills, so `RC` can name the quote `R` only because `R` came first. The one
exception is an index naming a curve defined later, which is how a
bootstrapped curve and the index its pillars quote can refer to each other.

**Omitting the dividend curve means a flat zero yield**, not the risk-free
curve. That is the single most common surprise in a first price.

**The result map is keyed by QuantLib's own names**, so `RESULT_KIND_DELTA`
comes back under `"delta"` and `RESULT_KIND_THETA_PER_DAY` under
`"thetaPerDay"` — the same keys an engine uses in its `additionalResults`, so
a client dumping both reads one vocabulary.

**Closing is optional but polite.** A session holds a worker seat until it is
closed, the socket drops and its grace expires, or the cap evicts the
longest-waiting one.
