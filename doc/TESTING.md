# Smoke tests

`smoke_v2.py` drives `ql-backend` over a real WebSocket and is the only proof
the service works; there is no unit-test target yet.

It checks two different kinds of thing, and the distinction matters.

**Numbers.** 369 rows of QuantLib's own published reference values — European
and American vanillas, barriers and knock digitals, forward-start, compound,
chooser, cliquet and two-asset basket options and the four quanto shapes — each
priced through the wire and compared against the value the C++ test suite
records, at the tolerance that test uses. Neither the values nor the
tolerances are typed in: `reference_tables.py` is generated from
`test-suite/*.cpp` by `extract_tables.py`, and the generator is checked in
beside its output. Transcribing a benchmark by hand is how it quietly stops
being one — a digit changes and the test still passes, against the wrong
number.

Three of those rows have no table behind them. `chooseroption.cpp` publishes
its two values, and `cliquetoption.cpp` its one, as local variables in the
bodies of their test cases, so `extract_tables.py` reads them out variable by
variable — naming the C++ variable each field comes from — rather than letting
three constants be typed here. A pattern that stops matching is a hard failure, which is the point: the
alternative is a stale `6.1071` that outlives the number it was copied from.

Re-run the generator when the QuantLib checkout moves:

```bash
python3 test/extract_tables.py ~/CLionProjects/QuantLib > test/reference_tables.py
```

The diff is then the library's change rather than ours.

**Behaviour.** One session's life — open, bump a quote, reprice off the live
graph, sweep a quote, cancel a batched Monte Carlo, close — plus the rejections
that keep a forgotten field from pricing on a default nobody chose, and two
sessions disagreeing by one day of theta. That last one is the check that
catches the wrong QuantLib being linked: without `QL_ENABLE_SESSIONS` the two
share one `Settings::evaluationDate()` and agree exactly (DESIGN §2).

Two of the behaviour checks are replays in disguise. A sweep that keeps its
final value is followed by a finite-difference request, which moves the
session to a sacrificial worker and rebuilds it from the session log; the
kept spot has to survive that, which is only true if the supervisor folded it
into the log. And a swap section builds an index that forward-references the
curve bootstrapped off it, then bumps a swap pillar and checks the trade moves
— through the rebootstrapped curve *and* the relinked index, which a copy
would not.

It also benchmarks, which is the part worth reading: it cross-checks the
analytic quanto barrier prices against a PDE, because `testBarrierValues` in
`quantooption.cpp` asks for exactly that benchmark and never got one. The
numbers, and what they do and do not establish, are in [the quanto barrier
benchmark](BENCHMARK.md). One of the three recorded values does not reproduce.

The three earlier scripts — `smoke_pricing.py`, `smoke_sessions.py` and
`smoke_quanto.py` — were `v1` clients. The service speaks `v2` now (DESIGN
§6.3), so they could not be left in place; everything they covered is here,
and the quanto tables and the PDE benchmark came across intact.

It needs Python bindings for the schema and a client library, neither of which
the repository carries:

```bash
python3 -m venv /tmp/qlvenv && /tmp/qlvenv/bin/pip install websockets protobuf
protoc -I proto --python_out=/tmp/qlpb2 proto/quantlib/v1/*.proto proto/quantlib/v2/*.proto
touch /tmp/qlpb2/quantlib/__init__.py /tmp/qlpb2/quantlib/v{1,2}/__init__.py

./build/ql-backend --port 9111 &
/tmp/qlvenv/bin/python test/smoke_v2.py /tmp/qlpb2
```

It prints one line per check and exits non-zero if any failed.

Regenerate the bindings whenever the `proto` submodule moves. They are written
to a directory outside the repository and nothing rebuilds them for you, so a
suite run against a stale copy fails on a field the service is sending and the
bindings have never heard of.

Against a daemon started with `--token-file`, put the secret in `QL_TOKEN` and
every connection the suite opens carries it as an `Authorization` header:

```bash
./build/ql-backend --port 9111 --token-file /tmp/qltoken &
QL_TOKEN=$(cat /tmp/qltoken) /tmp/qlvenv/bin/python test/smoke_v2.py /tmp/qlpb2
```

The whole suite passes either way. One check reads differently: `/healthz`
stops reporting its connection and session counts once a token is required, and
the check asserts whichever shape applies.

One check only runs when the daemon is started with a short resume window:

```bash
./build/ql-backend --port 9111 --session-grace 2
```

The resume section otherwise proves that a session, and the calculation running
in it, survive a dropped socket — but the window *expiring* is a wall-clock
wait, and sixty seconds of it does not belong in a test run. With a two-second
window it is one more check.

`test/lifecycle.py` checks what the suite cannot, because the suite talks to a
daemon someone else started: the process itself. It starts the binary on port
9181, twice, and needs nothing else running there:

```bash
/tmp/qlvenv/bin/python test/lifecycle.py /tmp/qlpb2 ./build/ql-backend
```

A second daemon on a port the first holds has to fail, without having said it
was listening. `SIGTERM` has to drain: the Monte Carlo running when it arrives
still gets its answer, the socket is then closed with 1001, and the process
exits 0.
