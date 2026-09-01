import asyncio, sys
sys.path.insert(0, sys.argv[1])
import websockets
from quantlib.v1 import envelope_pb2 as E
from quantlib.v1 import conventions_pb2 as C

URL = "ws://127.0.0.1:9111"  # matches ./build/qlserviced --port 9111
rid = 0
def nid():
    global rid; rid += 1; return rid

def open_session(eval_iso, spot):
    f = E.ClientFrame(request_id=nid())
    o = f.open_session
    o.evaluation_date.iso = eval_iso
    for q, v in [("DEP.6M", 0.040), ("SPOT", spot), ("VOL", 0.20)]:
        o.quotes.add(quote_id=q, value=v)
    c = o.curves.add()
    c.curve_id = "D"
    c.day_counter.family = C.DayCounter.ACTUAL_365_FIXED
    c.calendar.name = C.Calendar.TARGET
    c.settlement_days = 2
    c.bootstrap.traits = E.CurveBootstrap.DISCOUNT
    c.bootstrap.interpolator = E.CurveBootstrap.LOG_LINEAR
    p = c.pillars.add()
    p.quote_id, p.tenor, p.kind = "DEP.6M", "6M", E.CurvePillar.DEPOSIT
    p.index.family, p.index.tenor = C.IborIndex.EURIBOR, "6M"
    return f

def price(sid, expiry, kind=E.Engine.ANALYTIC, samples=0, seed=0, every=0):
    f = E.ClientFrame(request_id=nid(), session_id=sid)
    v = f.price.instrument.vanilla_option
    v.type, v.strike, v.expiry.iso = E.VanillaOption.CALL, 100.0, expiry
    v.spot_quote_id, v.vol_quote_id, v.discount_curve_id = "SPOT", "VOL", "D"
    f.price.engine.kind = kind
    if samples: f.price.engine.samples, f.price.engine.seed = samples, seed
    if every: f.price.progress_every_paths = every
    return f

# Terminal frames for different requests interleave, so they are buffered by
# request_id rather than assumed to arrive in the order they were asked for.
DONE = {}

async def term(ws, want, timeout=60.0):
    while want not in DONE:
        raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
        f = E.ServerFrame(); f.ParseFromString(raw)
        if f.terminal:
            DONE[f.request_id] = f
    return DONE.pop(want)

async def main():
    fails = []
    def check(n, ok, d=""):
        print(("  PASS  " if ok else "  FAIL  ") + n + ("  " + d if d else ""))
        if not ok: fails.append(n)

    async with websockets.connect(URL, max_size=8 << 20) as ws:
        # Two sessions, different evaluation dates, same connection.
        # Under QL_ENABLE_SESSIONS each worker thread has its own Settings.
        fa = open_session("2024-01-02", 100.0); await ws.send(fa.SerializeToString())
        a = (await term(ws, fa.request_id)).session_id
        fb = open_session("2025-01-02", 100.0); await ws.send(fb.SerializeToString())
        b = (await term(ws, fb.request_id)).session_id
        check("two sessions open", bool(a and b and a != b), f"{a} {b}")

        # Same option, same spot/vol. Only the sessions' evaluation dates
        # differ, so the time to expiry differs and so must the price.
        pa = price(a, "2024-07-02"); pb = price(b, "2025-07-02")
        await ws.send(pa.SerializeToString()); await ws.send(pb.SerializeToString())
        ra, rb = await term(ws, pa.request_id), await term(ws, pb.request_id)
        na, nb = ra.price_result.npv, rb.price_result.npv
        check("both price", ra.HasField("price_result") and rb.HasField("price_result"),
              f"{na:.6f} / {nb:.6f}")
        # Each session holds its own evaluation date. 2024-01-02 -> 2024-07-02
        # is 182 days (leap year); 2025-01-02 -> 2025-07-02 is 181. So the two
        # prices must differ by about one day of theta. A process-wide Settings
        # would give both threads whichever date was written last, and the two
        # would agree exactly -- that collapse is the bug this checks for.
        check("per-thread evaluation dates", 5e-3 < abs(na - nb) < 5e-2,
              f"|{na:.9f} - {nb:.9f}| = {abs(na-nb):.2e}, one day of theta")

        # Interleave many prices on both sessions; a shared Settings would
        # make these disagree with the values just computed.
        for _ in range(15):
            qa, qb = price(a, "2024-07-02"), price(b, "2025-07-02")
            await ws.send(qa.SerializeToString()); await ws.send(qb.SerializeToString())
            xa, xb = await term(ws, qa.request_id), await term(ws, qb.request_id)
            if abs(xa.price_result.npv - na) > 1e-12 or abs(xb.price_result.npv - nb) > 1e-12:
                check("interleaved prices stable", False,
                      f"{xa.price_result.npv} {xb.price_result.npv}")
                break
        else:
            check("interleaved prices stable", True, "30 prices, 2 sessions, no drift")

        # Cancel a batched Monte Carlo: the polite stop lands between batches.
        big = price(a, "2024-07-02", E.Engine.MONTE_CARLO,
                    samples=40_000_000, seed=11, every=200_000)
        await ws.send(big.SerializeToString())
        raw = await asyncio.wait_for(ws.recv(), timeout=30)  # wait for first progress
        c = E.ClientFrame(request_id=nid(), session_id=a)
        c.cancel.target_request_id = big.request_id
        await ws.send(c.SerializeToString())
        ack = await term(ws, c.request_id)
        check("cancel acked", ack.HasField("ack"))
        done = await term(ws, big.request_id, timeout=60)
        check("cancelled terminally", done.HasField("error") and done.error.code == E.Error.CANCELLED,
              f"code={E.Error.Code.Name(done.error.code) if done.HasField('error') else 'result'}")

        # The session survives a cancel and still prices.
        after = price(a, "2024-07-02"); await ws.send(after.SerializeToString())
        r = await term(ws, after.request_id)
        check("session alive after cancel", r.HasField("price_result"),
              f"npv={r.price_result.npv:.6f}")

    print("\n" + ("ALL PASS" if not fails else f"FAILURES: {fails}"))
    return 1 if fails else 0

sys.exit(asyncio.run(main()))
