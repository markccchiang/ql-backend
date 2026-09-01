# Interactive QuantLib Service

A stateful C++ pricing backend driving a TypeScript frontend over WebSocket,
with Protobuf as the wire format. A session keeps a live QuantLib object graph
alive between requests, so a what-if from the UI moves a quote and recomputes
only the instruments that depend on it.

The design, and the QuantLib constraints that force it, are in
[`DESIGN.md`](DESIGN.md).

**Status.** Design sketch, and it compiles. The two `.proto` files are clean
under `protoc 34.0`, and `src/` builds warning-free against QuantLib 1.44 with
AppleClang 21 at C++17.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/.local
cmake --build build -j
```

`CMAKE_PREFIX_PATH` points at wherever QuantLib is installed; drop it if that
is a default prefix. The target is a library rather than a program: the
gateway, the process host and the worker `main()` are described in `DESIGN.md`
but not yet written (DESIGN §1.1).

Compiling it was worth doing. Four things in this code were wrong in ways no
amount of re-reading would have shown: `namespace pb` collides with protobuf's
own alias in `extension_set.h`, `RateHelper` is a typedef and cannot be forward
declared, `ql/indexes/ibor/overnightindex.hpp` does not exist (`OvernightIndex`
lives in `iborindex.hpp`), and `std::min` cannot deduce between `Size` and the
`uint64` a proto field returns. What compiles is still unproven — nothing here
has priced anything — but it is no longer unproven *and* unbuildable.

| File | What it holds |
| --- | --- |
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
| [`CMakeLists.txt`](CMakeLists.txt) | protoc invocation and the library target |
