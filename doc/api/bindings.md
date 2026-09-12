# Generating bindings

The schema is a repository of its own,
[ql-protobuf](https://github.com/markccchiang/ql-protobuf), mounted here as
the `proto/` submodule. **No generated code is checked in anywhere** — each
consumer runs its own generator, so there is one source of truth and no stale
binding sitting beside a schema that moved.

```bash
git submodule update --init proto     # if you are working from this repository
```

The layout is what you import against:

```
quantlib/v1/conventions.proto   day counters, calendars, business-day rules
quantlib/v2/envelope.proto      frames, session lifecycle, pricing, cancellation
quantlib/v2/market.proto        quotes, curves, surfaces, indices, fixings
quantlib/v2/instrument.proto    payoff x exercise x underlying x style, legs
quantlib/v2/engine.proto        method x model, and each parameter block
quantlib/v2/results.proto       the Value variant, cash flows, plot series
```

`v2` imports `v1`'s `conventions.proto`, so generate both even though only
`v2` is served.

## Python

```bash
python3 -m venv .venv && .venv/bin/pip install websockets protobuf
mkdir -p pb
protoc -I proto --python_out=pb proto/quantlib/v1/*.proto proto/quantlib/v2/*.proto
touch pb/quantlib/__init__.py pb/quantlib/v1/__init__.py pb/quantlib/v2/__init__.py
```

The `touch` is not optional: `protoc` writes the package directories without
`__init__.py`, and the import fails without them. Put `pb` on `sys.path` and
the modules are `quantlib.v2.envelope_pb2` and friends.

Two names catch people out. `ResultKind` lives in `results_pb2`, not in
`envelope_pb2` where the request that carries it lives; and the `Value`
variant's oneof is called `v`, so the accessor is `value.WhichOneof("v")`.

## TypeScript

The frontend generates with [buf](https://buf.build) and `protoc-gen-es`,
which produces plain ES modules with no runtime service dependency:

```yaml
# buf.gen.yaml
version: v2
plugins:
    - local: protoc-gen-es
      out: src/gen
      opt:
          - target=ts
          - import_extension=js
```

```bash
npm i -D @bufbuild/buf @bufbuild/protoc-gen-es
npm i @bufbuild/protobuf
npx buf generate proto
```

Encode and decode with the schema objects rather than methods on the message:

```typescript
import {create, fromBinary, toBinary} from "@bufbuild/protobuf";
import {ClientFrameSchema, ServerFrameSchema} from "@/gen/quantlib/v2/envelope_pb";

ws.send(toBinary(ClientFrameSchema, frame));
const reply = fromBinary(ServerFrameSchema, new Uint8Array(event.data));
```

Keep the output directory out of version control and regenerate on install —
the frontend gitignores `src/gen` and runs its `gen` script from `prepare`,
so a moved schema cannot be shadowed by a checked-in binding.

## C++

This repository generates its own at build time; `CMakeLists.txt` is the
worked example. `protobuf_generate` is given the `.proto` files directly and
writes into `<builddir>/generated/`, whose root goes on the include path so a
header is included by its package path:

```cpp
#include "quantlib/v2/envelope.pb.h"
```

Find Protobuf in CONFIG mode (`find_package(Protobuf CONFIG REQUIRED)`) — the
deprecated `FindProtobuf` module misses the abseil dependencies libprotobuf
now carries.

## Versions

The schema is clean under `protoc 34.0`. Anything recent enough to know about
`optional` in proto3 will do; the schema uses no extensions, no services and
no `Any`.
