# Interactive QuantLib Service

A stateful C++ pricing backend driving a TypeScript frontend over WebSocket,
with Protobuf as the wire format. A session keeps a live QuantLib object graph
alive between requests, so a what-if from the UI moves a quote and recomputes
only the instruments that depend on it.

The design, and the QuantLib constraints that force it, are in
[`DESIGN.md`](DESIGN.md).

**Status.** It runs. `qlserviced` serves the protocol over a WebSocket and
prices: a session opens, a quote bump reprices off the live graph, Monte Carlo
reports progress, and a cancel comes back as `CANCELLED` with the session still
alive. `test/` drives all of that end to end. Builds warning-free against
QuantLib 1.44 with AppleClang 21 at C++17, schema clean under `protoc 34.0`.

What is missing is the process boundary. Workers are threads in the gateway
process for now, so a cancel that has to kill cannot (DESIGN §3), and
replay-after-death is never exercised.

```bash
git submodule update --init --recursive                        # uWebSockets + uSockets
git submodule update --init --checkout third_party/QuantLib    # QuantLib v1.43
cmake -S . -B build -DQLSERVICE_VENDOR_QUANTLIB=ON
cmake --build build -j
./build/qlserviced --port 9111
```

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
`uint64` a proto field returns. What compiles is still unproven — nothing here
has priced anything — but it is no longer unproven *and* unbuildable.

| File | What it holds |
| --- | --- |
| [`INSTALL.md`](INSTALL.md) | Prerequisites, both build routes, and what fails silently |
| [`DESIGN.md`](DESIGN.md) | The architecture: what each component owns and which QuantLib constraint forces it |
| [`proto/quantlib/v1/envelope.proto`](proto/quantlib/v1/envelope.proto) | Transport envelope, session lifecycle, cancellation, progress |
| [`proto/quantlib/v1/conventions.proto`](proto/quantlib/v1/conventions.proto) | Convention enums and messages — the schema half of the registry |
| [`src/conventions/registry.hpp`](src/conventions/registry.hpp) | Registry interface: proto → QuantLib objects |
| [`src/conventions/registry.cpp`](src/conventions/registry.cpp) | Reference implementation of the translation |
| [`src/session/session.hpp`](src/session/session.hpp) / [`.cpp`](src/session/session.cpp) | One client's live object graph: quotes, curves, pricing |
| [`src/session/updateguard.hpp`](src/session/updateguard.hpp) | RAII batching of observer notifications |
| [`src/session/worker.hpp`](src/session/worker.hpp) / [`.cpp`](src/session/worker.cpp) | The thread owning a session; serializes its requests |
| [`src/session/supervisor.hpp`](src/session/supervisor.hpp) / [`.cpp`](src/session/supervisor.cpp) | Session log, worker pool and placement, cancel-by-kill with replay |
| [`src/errors/fielderror.hpp`](src/errors/fielderror.hpp) | The exception that carries a wire code and the proto field to blame |
| [`src/gateway/gateway.hpp`](src/gateway/gateway.hpp) / [`.cpp`](src/gateway/gateway.cpp) | The WebSocket front end: loop, session ids, backpressure, deadlines |
| [`src/gateway/threadhost.hpp`](src/gateway/threadhost.hpp) / [`.cpp`](src/gateway/threadhost.cpp) | The staging `ProcessHost`: workers as threads, so a kill only disowns |
| [`src/app/main.cpp`](src/app/main.cpp) | `qlserviced` entry point |
| [`test/`](test/README.md) | End-to-end smoke scripts, and how to run them |
| [`CMakeLists.txt`](CMakeLists.txt) | protoc invocation, the library, uSockets, and the executable |
| `third_party/uWebSockets` | Submodule pinned at v20.66.0, with uSockets nested inside |
| `third_party/QuantLib` | Opt-in submodule pinned at v1.43, for `-DQLSERVICE_VENDOR_QUANTLIB=ON` |
