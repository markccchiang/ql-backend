# Smoke tests

Two end-to-end scripts that drive `qlserviced` over a real WebSocket. They are
the only proof the service works; there is no unit-test target yet.

`smoke_pricing.py` covers one session's life — open, analytic price, quote bump,
reprice off the live graph, Monte Carlo with and without progress, a rejected
engine kind, an unknown session, close.

`smoke_sessions.py` covers what needs two of them — per-thread evaluation dates
under `QL_ENABLE_SESSIONS`, thirty interleaved prices with no drift, and a
cancel that terminates as `CANCELLED` while leaving the session able to price.

Both need Python bindings for the schema and a client library, neither of which
the repository carries:

```bash
python3 -m venv /tmp/qlvenv && /tmp/qlvenv/bin/pip install websockets protobuf
protoc -I proto --python_out=/tmp/qlpb proto/quantlib/v1/*.proto

./build/qlserviced --port 9111 &
/tmp/qlvenv/bin/python test/smoke_pricing.py  /tmp/qlpb
/tmp/qlvenv/bin/python test/smoke_sessions.py /tmp/qlpb
```

Each prints one line per check and exits non-zero if any failed.
