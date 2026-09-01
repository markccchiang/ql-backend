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
