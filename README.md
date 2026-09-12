# Interactive QuantLib Service

A stateful C++ pricing backend for interactive derivatives pricing, built on
[QuantLib](https://www.quantlib.org/). A client opens a session over a
WebSocket; the server keeps that session's QuantLib object graph alive between
requests, so moving one quote reprices only the instruments that depend on it.
That is what lets a spot slider in a UI feel immediate instead of rebuilding
every curve on every keystroke.

The wire format is Protobuf: one request frame in, exactly one terminal reply
frame out — including for failures and cancellations.

```
browser / client  ──WebSocket──>  gateway  ──>  worker threads
                                                 └─ live QuantLib graph,
                                                    one session each
```

## What it does today

- **Prices options** across eight payoffs, three exercises and ten styles,
  reaching 49 compiled engines — analytic, lattice, finite-difference,
  integral and Monte Carlo — with quanto composing over the other styles
  rather than duplicating them.
- **Reprices incrementally.** A quote write invalidates only the affected
  instruments; a single frame can sweep a quote across a ladder of values.
- **Reports progress and cancels.** A long Monte Carlo streams progress and
  stops on request, leaving its session alive.
- **Survives a dropped connection** for a 60-second window, work in flight
  included, via `ResumeSession`.
- **Says what it supports.** A `Hello` frame answers with this build's
  capabilities as data, so a client never has to probe.

Anything in the schema that this build does not price is rejected by name —
never quietly substituted.

## Correctness

`test/smoke_v2.py` prices **369 rows of QuantLib's own published reference
values** over the wire, each within the tolerance QuantLib's test suite uses
for it. The rows are extracted from the library's `test-suite/*.cpp` by a
checked-in generator rather than transcribed by hand. The suite also
cross-checks the analytic quanto barriers against a PDE, and finds one of
QuantLib's three recorded values not reproducible.

## Quick start

Needs CMake ≥ 3.16, a C++17 compiler, Boost headers, and Protobuf with its
CMake package config. On macOS: `brew install cmake boost protobuf`.

```bash
git clone https://github.com/markccchiang/ql-backend.git
cd ql-backend
git submodule update --init --recursive                     # schema, uWebSockets
git submodule update --init --checkout --depth 1 third_party/QuantLib
cmake -S . -B build -DQLSERVICE_VENDOR_QUANTLIB=ON -Wno-dev
cmake --build build -j
./build/ql-backend --port 9111
```

This route builds QuantLib from the pinned submodule, which is slow the first
time but cannot be misconfigured — the service needs a QuantLib built with
`QL_ENABLE_SESSIONS`, and linking one without it produces wrong numbers rather
than an error. [`doc/INSTALL.md`](doc/INSTALL.md) explains both routes and the
failures that are silent.

## Documentation

Full documentation lives in [`doc/`](doc/index.md) and builds with Sphinx:

```bash
python3 -m venv .venv && .venv/bin/pip install -r doc/requirements.txt
.venv/bin/python -m sphinx -b html doc doc/_build/html
```

| Page | What it covers |
| --- | --- |
| [Overview](doc/index.md) | Status, the file map, and what is not built yet |
| [Install](doc/INSTALL.md) | Prerequisites, both build routes, and what fails silently |
| [Design](doc/DESIGN.md) | The architecture, and the QuantLib constraint forcing each decision |
| [Handlers](doc/HANDLERS.md) | Every frame the service accepts, with a worked client session |
| [Testing](doc/TESTING.md) | Running the end-to-end suite and regenerating the reference tables |
| [Benchmark](doc/BENCHMARK.md) | The quanto barrier cross-check against a PDE |

The wire schema is a separate repository,
[ql-protobuf](https://github.com/markccchiang/ql-protobuf), mounted here as
the `proto` submodule so a frontend can consume it without cloning this one.

## Status

It runs and it prices. What is missing is the process boundary: workers are
threads inside the gateway process today, so a cancel that has to *kill* a
running calculation can only disown it. The design for fork/exec workers, and
what that buys, is in [`doc/DESIGN.md`](doc/DESIGN.md) §3.
