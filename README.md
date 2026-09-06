# Interactive QuantLib Service

A stateful C++ pricing backend driving a TypeScript frontend over WebSocket,
with Protobuf as the wire format. A session keeps a live QuantLib object graph
alive between requests, so a what-if from the UI moves a quote and recomputes
only the instruments that depend on it.

The design, and the QuantLib constraints that force it, are in
[`DESIGN.md`](DESIGN.md).

**Status.** It runs, and it prices correctly. `ql-backend` serves the protocol
over a WebSocket: a session opens, a quote bump reprices off the live graph,
one frame sweeps a quote across a spot ladder, Monte Carlo reports progress,
and a cancel comes back as `CANCELLED` with the session still alive.

The wire schema is `v2` (DESIGN §6.3), derived from what QuantLib's own test
suite shows a pricing request has to carry: an option is a payoff, an exercise,
an underlying and a style, with quanto composing over the styles rather than
multiplying them into their own messages. Eight payoffs, three exercises and
nine of the schema's twelve styles are built, and they reach 46 distinct
compiled engines — analytic, lattice, finite-difference, integral and Monte
Carlo, of which 11 are quanto wrappers around another engine on the list. A
`Hello` frame answers with the whole of that as data, from
`src/session/capabilities.cpp`, so a client never has to send a request to find
out what this build prices.

`test/smoke_v2.py` prices **312 rows of QuantLib's published reference values**
over the wire, each within the tolerance its own test uses. The rows are not
transcribed: `test/extract_tables.py` parses them out of `test-suite/*.cpp`.
It also cross-checks the analytic quanto barriers against a PDE, which answers
the `TODO: bench against an existing prop calculator` that
`test-suite/quantooption.cpp` leaves open, and finds one of its three recorded
values not reproducible ([`test/BENCHMARK.md`](test/BENCHMARK.md)). Builds
warning-free under `QLSERVICE_WERROR=ON` on both routes — the vendored
QuantLib v1.43 and a 1.44-dev install — with AppleClang 21 at C++17, schema
clean under `protoc 34.0`.

A session now outlives its socket by a minute, and so does the work in it: a
client that loses its connection takes the session back with `ResumeSession`
and the token it was given, and the Monte Carlo that was running is still
running (DESIGN §9.4). The bootstrap was never what a dropped socket cost.

What is missing is the process boundary. Workers are threads in the gateway
process for now, so a cancel that has to kill cannot (DESIGN §3), and
replay-after-death is never exercised.

```bash
git submodule update --init --recursive                        # proto, uWebSockets + uSockets
git submodule update --init --checkout third_party/QuantLib    # QuantLib v1.43
cmake -S . -B build -DQLSERVICE_VENDOR_QUANTLIB=ON
cmake --build build -j
./build/ql-backend --port 9111
```

The port is 9111 by convention rather than by default — every script and test
in both repositories passes `--port 9111`, and the built-in default is 9001.
A browser is let in from the Vite dev server's ports only; anything else needs
`--allow-origin URL` (DESIGN §9.6), and `--help` lists that flag, the two
caps, and `--any-origin` for a deployment behind a proxy that already checks.

That builds QuantLib from the submodule, which is the route that works
unmodified from a clean clone: `CMakeLists.txt` forces `QL_ENABLE_SESSIONS=ON`,
without which two sessions in one process silently share one
`Settings::evaluationDate()` (DESIGN §2). Linking an installed QuantLib instead
is the default and is much faster to rebuild, but it can only be got right by
hand.

[`INSTALL.md`](INSTALL.md) has both routes, the prerequisites, how to build a
sessions-enabled QuantLib into a prefix of its own, and the failures that are
silent rather than loud.

Compiling it was worth doing. Four things in this code were wrong in ways no
amount of re-reading would have shown: `namespace pb` collides with protobuf's
own alias in `extension_set.h`, `RateHelper` is a typedef and cannot be forward
declared, `ql/indexes/ibor/overnightindex.hpp` does not exist (`OvernightIndex`
lives in `iborindex.hpp`), and `std::min` cannot deduce between `Size` and the
`uint64` a proto field returns.

| File | What it holds |
| --- | --- |
| [`INSTALL.md`](INSTALL.md) | Prerequisites, both build routes, and what fails silently |
| [`DESIGN.md`](DESIGN.md) | The architecture: what each component owns and which QuantLib constraint forces it |
| [`HANDLERS.md`](HANDLERS.md) | Every handler the service implements today, and how to drive it |
| [`proto/quantlib/v2/envelope.proto`](https://github.com/markccchiang/ql-protobuf/blob/main/quantlib/v2/envelope.proto) | Transport envelope, session lifecycle, pricing, sweeps, cancellation |
| [`proto/quantlib/v2/market.proto`](https://github.com/markccchiang/ql-protobuf/blob/main/quantlib/v2/market.proto) | The market namespace: quotes, curves, volatility, indices, fixings |
| [`proto/quantlib/v2/instrument.proto`](https://github.com/markccchiang/ql-protobuf/blob/main/quantlib/v2/instrument.proto) | Payoff × exercise × underlying × style, and the legs |
| [`proto/quantlib/v2/engine.proto`](https://github.com/markccchiang/ql-protobuf/blob/main/quantlib/v2/engine.proto) | Method × model, and the parameter block each one takes |
| [`proto/quantlib/v2/results.proto`](https://github.com/markccchiang/ql-protobuf/blob/main/quantlib/v2/results.proto) | What comes back: the `Value` variant, cash flows, plot series |
| [`proto/quantlib/v1/conventions.proto`](https://github.com/markccchiang/ql-protobuf/blob/main/quantlib/v1/conventions.proto) | Convention enums and messages — shared by both schema versions |
| [`src/conventions/registry.hpp`](src/conventions/registry.hpp) | Registry interface: proto → QuantLib objects |
| [`src/conventions/registry.cpp`](src/conventions/registry.cpp) | Reference implementation of the translation |
| [`src/session/session.hpp`](src/session/session.hpp) / [`.cpp`](src/session/session.cpp) | One client's live object graph: quotes, curves, pricing |
| [`src/session/updateguard.hpp`](src/session/updateguard.hpp) | RAII batching of observer notifications |
| [`src/session/capabilities.hpp`](src/session/capabilities.hpp) / [`.cpp`](src/session/capabilities.cpp) | What this build prices, as data: the answer to `Hello` |
| [`src/session/worker.hpp`](src/session/worker.hpp) / [`.cpp`](src/session/worker.cpp) | The thread owning a session; serializes its requests |
| [`src/session/supervisor.hpp`](src/session/supervisor.hpp) / [`.cpp`](src/session/supervisor.cpp) | Session log, worker pool and placement, cancel-by-kill with replay |
| [`src/errors/fielderror.hpp`](src/errors/fielderror.hpp) | The exception that carries a wire code and the proto field to blame |
| [`src/gateway/gateway.hpp`](src/gateway/gateway.hpp) / [`.cpp`](src/gateway/gateway.cpp) | The WebSocket front end: loop, session ids, backpressure, deadlines |
| [`src/gateway/threadhost.hpp`](src/gateway/threadhost.hpp) / [`.cpp`](src/gateway/threadhost.cpp) | The staging `ProcessHost`: workers as threads, so a kill only disowns |
| [`src/app/main.cpp`](src/app/main.cpp) | `ql-backend` entry point |
| [`test/`](test/README.md) | The end-to-end smoke script, the reference-table generator, and how to run them |
| [`CMakeLists.txt`](CMakeLists.txt) | protoc invocation, the library, uSockets, and the executable |
| `proto` | Submodule: the wire schema, [ql-protobuf](https://github.com/markccchiang/ql-protobuf) |
| `third_party/uWebSockets` | Submodule pinned at v20.66.0, with uSockets nested inside |
| `third_party/QuantLib` | Opt-in submodule pinned at v1.43, for `-DQLSERVICE_VENDOR_QUANTLIB=ON` |
