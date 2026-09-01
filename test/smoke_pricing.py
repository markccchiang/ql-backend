import asyncio, sys, time
sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else "pb")
import websockets
from quantlib.v1 import envelope_pb2 as E
from quantlib.v1 import conventions_pb2 as C

URL = "ws://127.0.0.1:9111"  # matches ./build/qlserviced --port 9111
rid = 0
def next_id():
    global rid; rid += 1; return rid

def open_session():
    f = E.ClientFrame(request_id=next_id())
    o = f.open_session
    o.evaluation_date.iso = "2024-01-02"
    for qid, v in [("DEP.1M", 0.038), ("DEP.3M", 0.039), ("DEP.6M", 0.040),
                   ("DEP.12M", 0.041), ("SPOT", 100.0), ("VOL", 0.20)]:
        o.quotes.add(quote_id=qid, value=v)
    c = o.curves.add()
    c.curve_id = "EUR.DISC"
    c.day_counter.family = C.DayCounter.ACTUAL_365_FIXED
    c.calendar.name = C.Calendar.TARGET
    c.settlement_days = 2
    c.bootstrap.traits = E.CurveBootstrap.DISCOUNT
    c.bootstrap.interpolator = E.CurveBootstrap.LOG_LINEAR
    for qid, tenor in [("DEP.1M", "1M"), ("DEP.3M", "3M"), ("DEP.6M", "6M"), ("DEP.12M", "12M")]:
        p = c.pillars.add()
        p.quote_id, p.tenor, p.kind = qid, tenor, E.CurvePillar.DEPOSIT
        p.index.family, p.index.tenor = C.IborIndex.EURIBOR, tenor
    return f

def price(session, kind, samples=0, seed=0, every=0):
    f = E.ClientFrame(request_id=next_id(), session_id=session)
    p = f.price
    v = p.instrument.vanilla_option
    v.type, v.strike = E.VanillaOption.CALL, 100.0
    v.expiry.iso = "2024-07-02"
    v.spot_quote_id, v.vol_quote_id, v.discount_curve_id = "SPOT", "VOL", "EUR.DISC"
    p.engine.kind = kind
    if samples: p.engine.samples, p.engine.seed = samples, seed
    if every: p.progress_every_paths = every
    p.results.extend([E.RESULT_KIND_DELTA, E.RESULT_KIND_VEGA])
    return f

def bump(session, qid, value):
    f = E.ClientFrame(request_id=next_id(), session_id=session)
    f.update_market.updates.add(quote_id=qid, value=value)
    return f

async def collect(ws, want, timeout=30.0):
    """Read frames until the terminal one for `want` arrives."""
    progress = 0
    deadline = time.time() + timeout
    while True:
        raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.time()))
        f = E.ServerFrame(); f.ParseFromString(raw)
        if f.HasField("progress"):
            progress += 1
            continue
        if f.request_id == want and f.terminal:
            return f, progress

async def main():
    failures = []
    def check(name, ok, detail=""):
        print(("  PASS  " if ok else "  FAIL  ") + name + ("  " + detail if detail else ""))
        if not ok: failures.append(name)

    async with websockets.connect(URL, max_size=8 << 20) as ws:
        f = open_session(); await ws.send(f.SerializeToString())
        r, _ = await collect(ws, f.request_id)
        if not r.HasField("session_opened"):
            print("open failed:", r); return 1
        sid = r.session_id
        check("OpenSession", True, f"session={sid} bootstrap={r.session_opened.bootstrap_seconds:.4f}s")

        f = price(sid, E.Engine.ANALYTIC); await ws.send(f.SerializeToString())
        r, _ = await collect(ws, f.request_id)
        ok = r.HasField("price_result")
        base = r.price_result.npv if ok else 0.0
        check("analytic price", ok and 5.0 < base < 9.0,
              f"npv={base:.6f} delta={r.price_result.results.get('delta', float('nan')):.4f}")

        f = bump(sid, "SPOT", 110.0); await ws.send(f.SerializeToString())
        r, _ = await collect(ws, f.request_id)
        check("UpdateMarket ack", r.HasField("ack"))

        f = price(sid, E.Engine.ANALYTIC); await ws.send(f.SerializeToString())
        r, _ = await collect(ws, f.request_id)
        bumped = r.price_result.npv
        check("live graph reprices", bumped > base + 5.0, f"npv={bumped:.6f} (was {base:.6f})")

        f = price(sid, E.Engine.MONTE_CARLO, samples=20000, seed=42)
        await ws.send(f.SerializeToString())
        r, _ = await collect(ws, f.request_id)
        mc = r.price_result.npv
        check("monte carlo", abs(mc - bumped) < 0.5, f"npv={mc:.6f} vs analytic {bumped:.6f}")
        check("seed echoed", r.price_result.seed == 42, f"seed={r.price_result.seed}")

        f = price(sid, E.Engine.MONTE_CARLO, samples=200000, seed=7, every=20000)
        await ws.send(f.SerializeToString())
        r, n = await collect(ws, f.request_id)
        check("progress frames", n > 0, f"{n} progress frames")

        f = price(sid, E.Engine.BINOMIAL); await ws.send(f.SerializeToString())
        r, _ = await collect(ws, f.request_id)
        check("unwired engine rejected",
              r.HasField("error") and r.error.code == E.Error.INVALID_ARGUMENT,
              f"field={r.error.field_path!r}")

        f = E.ClientFrame(request_id=next_id(), session_id="s-999")
        f.price.instrument.vanilla_option.strike = 1.0
        await ws.send(f.SerializeToString())
        r, _ = await collect(ws, f.request_id)
        check("unknown session rejected",
              r.HasField("error") and r.error.code == E.Error.SESSION_NOT_FOUND)

        f = E.ClientFrame(request_id=next_id(), session_id=sid); f.close_session.SetInParent()
        await ws.send(f.SerializeToString())
        r, _ = await collect(ws, f.request_id)
        check("CloseSession ack", r.HasField("ack"))

    print("\n" + ("ALL PASS" if not failures else f"FAILURES: {failures}"))
    return 1 if failures else 0

sys.exit(asyncio.run(main()))
