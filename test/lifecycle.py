"""Start-up and shutdown, which smoke_v2.py cannot check: it talks to a service
someone else started, and these are about the process itself.

    python test/lifecycle.py PB_DIR [path/to/ql-backend]

Starts the binary on a port of its own, twice:

- A second process on a port the first holds must fail without saying it is
  listening. It printed "listening" before it had tried to bind, and with
  uSockets' default SO_REUSEPORT it then bound anyway and took a share of the
  connections.
- SIGTERM must drain: a request in flight still gets its answer, the socket
  is then closed with 1001, and the process exits 0. It used to die mid-request.
"""

import asyncio
import signal
import subprocess
import sys
import time

sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else "pb")
BINARY = sys.argv[2] if len(sys.argv) > 2 else "./build/ql-backend"
PORT = 9181
URL = f"ws://127.0.0.1:{PORT}"

import websockets
from quantlib.v1 import conventions_pb2 as C
from quantlib.v2 import engine_pb2 as EN
from quantlib.v2 import envelope_pb2 as E
from quantlib.v2 import instrument_pb2 as I

failures = []


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}  {detail}")
    if not ok:
        failures.append(name)


def start():
    # stderr is the service's log, and nothing reads it here: a pipe would
    # fill and stop the process mid-test.
    return subprocess.Popen([BINARY, "--port", str(PORT)], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)


def wait_for_listening(proc, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline()
        if "listening" in line:
            return True
        if proc.poll() is not None:
            return False
    return False


def open_frame():
    f = E.ClientFrame(request_id=1)
    o = f.open_session
    o.evaluation_date.iso = "2026-09-01"
    for qid, v in (("S", 100.0), ("R", 0.05), ("V", 0.2)):
        m = o.market.add()
        m.id = qid
        m.quote.value = v
    m = o.market.add()
    m.id = "RC"
    m.yield_curve.day_counter.family = C.DayCounter.ACTUAL_360
    m.yield_curve.flat.rate.quote_id = "R"
    m.yield_curve.flat.compounding = C.CONTINUOUS
    m.yield_curve.flat.frequency = C.ANNUAL
    m = o.market.add()
    m.id = "VOL"
    m.volatility.day_counter.family = C.DayCounter.ACTUAL_360
    m.volatility.constant.volatility.quote_id = "V"
    return f


def slow_price(sid):
    """A batched Monte Carlo of a few seconds, so there is something to drain."""
    f = E.ClientFrame(request_id=2, session_id=sid)
    opt = f.price.instrument.option
    u = opt.underlyings.add()
    u.spot_quote_id, u.discount_curve_id, u.volatility_id = "S", "RC", "VOL"
    opt.payoff.type = I.Payoff.OPTION_TYPE_CALL
    opt.payoff.plain.strike = 100.0
    opt.exercise.type = I.Exercise.TYPE_EUROPEAN
    opt.exercise.dates.add().iso = "2027-09-01"
    opt.vanilla.SetInParent()
    f.price.engine.method = EN.Engine.METHOD_MONTE_CARLO
    f.price.engine.mc.seed = 1
    f.price.engine.mc.samples = 6_000_000
    f.price.engine.mc.progress_every_paths = 200_000
    f.price.engine.mc.rng = EN.McParameters.RNG_PSEUDO_RANDOM
    return f


async def drain(proc):
    async with websockets.connect(URL, max_size=None) as ws:
        await ws.send(open_frame().SerializeToString())
        reply = E.ServerFrame()
        reply.ParseFromString(await ws.recv())
        sid = reply.session_id
        await ws.send(slow_price(sid).SerializeToString())

        # Stopped once the run is under way.
        progress = E.ServerFrame()
        progress.ParseFromString(await ws.recv())
        proc.send_signal(signal.SIGTERM)

        answer = None
        while True:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=30)
            except websockets.ConnectionClosed as closed:
                code = closed.rcvd.code if closed.rcvd else None
                break
            frame = E.ServerFrame()
            frame.ParseFromString(raw)
            if frame.terminal and frame.request_id == 2:
                answer = frame
        check("a request in flight at SIGTERM still gets its answer",
              answer is not None and answer.HasField("price_result"),
              answer.WhichOneof("payload") if answer is not None else "none")
        check("...and the socket is then closed as going away", code == 1001, f"code={code}")


def main():
    first = start()
    try:
        check("the service says it is listening once it is", wait_for_listening(first))

        # With SO_REUSEPORT the second bound the port too and ran on,
        # splitting the connections between two processes.
        second = start()
        try:
            out, _ = second.communicate(timeout=10)
            check("a second on a taken port fails, and does not say it is listening",
                  second.returncode != 0 and "listening" not in out,
                  f"exit={second.returncode} stdout={out.strip()!r}")
        except subprocess.TimeoutExpired:
            second.kill()
            out, _ = second.communicate()
            check("a second on a taken port fails, and does not say it is listening", False,
                  f"still running after 10 s; stdout={out.strip()!r}")

        asyncio.run(drain(first))
        try:
            code = first.wait(timeout=20)
        except subprocess.TimeoutExpired:
            code = None
        check("SIGTERM ends the process cleanly", code == 0, f"exit={code}")
    finally:
        if first.poll() is None:
            first.kill()

    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
