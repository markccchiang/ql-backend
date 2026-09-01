# Smoke tests

Three end-to-end scripts that drive `qlserviced` over a real WebSocket. They are
the only proof the service works; there is no unit-test target yet.

`smoke_pricing.py` covers one session's life — open, analytic price, quote bump,
reprice off the live graph, Monte Carlo with and without progress, a rejected
engine kind, an unknown session, close.

`smoke_sessions.py` covers what needs two of them — per-thread evaluation dates
under `QL_ENABLE_SESSIONS`, thirty interleaved prices with no drift, and a
cancel that terminates as `CANCELLED` while leaving the session able to price.

`smoke_quanto.py` is the only script that checks a *number* rather than a
behaviour. It carries the five reference tables from QuantLib's own
`test-suite/quantooption.cpp` — eighteen rows across the four quanto shapes —
and for each one checks twice: that the `ClientFrame` built from the row still
says exactly what the row says after a serialize/parse round trip, and that the
`PriceResult` matches the row's expected value at the row's tolerance. Anything
that quietly changed how a quanto request is interpreted shows up here as a
number, not as a passing test.

It then benchmarks, which is the part worth reading.

### The barrier values do not all reproduce

`testBarrierValues` in `quantooption.cpp` carries `TODO: bench results against
an existing prop calculator` and a tolerance of `0.5` to match — those three
values have no provenance. We have no vendor pricer either, but the same file
shows the substitute: `testPDEOptionValues` benchmarks the analytic quanto
vanilla against a PDE at `2e-4`. `FdBlackScholesBarrierEngine` is
single-argument constructible, so `QuantoEngine` can wrap it and the same trick
works for barriers.

| Row | Recorded | Ours (analytic) | PDE, fine grid | Analytic vs PDE |
| --- | --- | --- | --- | --- |
| DownOut call | 8.247 | 8.244498 | 8.244557 | 5.9e-05 |
| DownOut put | **2.274** | **2.377326** | **2.377241** | 8.6e-05 |
| DownIn put | 2.85 | 2.872743 | 2.872753 | 1.0e-05 |

Rows 1 and 3 differ from the recorded values by 2.5e-03 and 2.3e-02, which is
consistent with values recorded to four and three significant figures. Row 2 is
a different matter: 0.103, and the PDE sides with us to 8.6e-05. The error also
shrinks by about 4x per grid refinement, so the two methods are converging to
the same number rather than agreeing by accident.

What this establishes, precisely. Both routes take the quanto adjustment from
the same `QuantoTermStructure`, so the cross-check does *not* test the quanto
wrapper — it tests the barrier closed form on top of it. The wrapper is covered
separately and independently, by the quanto rows reproducing Haug's published
values to 2e-05 through that same code. The two legs together cover the price.
Two guardrails that need no second engine agree as well: in-out parity
(`down_in + down_out == vanilla` at zero rebate) holds to 1e-9, and an
unreachable barrier degrades to the vanilla price exactly.

The script reports the recorded-value gaps rather than asserting on them. The
table check stays at the C++ test's own `0.5`: those numbers are what is under
test, and tightening the tolerance against a value we believe is wrong would be
backwards.

All three need Python bindings for the schema and a client library, neither of
which the repository carries:

```bash
python3 -m venv /tmp/qlvenv && /tmp/qlvenv/bin/pip install websockets protobuf
protoc -I proto --python_out=/tmp/qlpb proto/quantlib/v1/*.proto

./build/qlserviced --port 9111 &
/tmp/qlvenv/bin/python test/smoke_pricing.py  /tmp/qlpb
/tmp/qlvenv/bin/python test/smoke_sessions.py /tmp/qlpb
/tmp/qlvenv/bin/python test/smoke_quanto.py   /tmp/qlpb
```

Each prints one line per check and exits non-zero if any failed.
