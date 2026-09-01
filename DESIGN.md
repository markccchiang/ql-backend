# Architecture Design — Interactive QuantLib Service

A stateful C++ pricing backend driving a TypeScript frontend over WebSocket,
with Protobuf as the wire format.

This document records the design decisions and, more importantly, the QuantLib
constraints that force them. Every claim about the library is cited to a file
and line in this repository so it can be re-checked when QuantLib changes.
Source comments cite back to it by section, as `DESIGN §2.1`.

Build instructions, current status and the file map are in
[`README.md`](README.md).

| Section | What it settles |
| --- | --- |
| [1. Shape](#1-shape) | The components and what each owns |
| [1.1 Process topology](#11-process-topology) | Why the supervisor is a class, not a service |
| [2. Global state forces session pinning](#2-global-state-forces-session-pinning) | `QL_ENABLE_SESSIONS`, one session per thread |
| [2.1 How many sessions per process](#21-how-many-sessions-per-process) | Shared vs. sacrificial workers, replay, placement |
| [2.2 QuantLib does not parallelise for you](#22-quantlib-does-not-parallelise-for-you) | No OpenMP in workers |
| [3. No cancellation](#3-no-cancellation--hence-processes-not-threads) | Cancel-by-kill, the stop grace, opt-in progress |
| [4. Determinism is not free](#4-determinism-is-not-free) | Required seeds, and why progress changes the price |
| [5. The lazy graph is why the backend is stateful](#5-the-lazy-graph-is-why-the-backend-is-stateful) | Observables and batched updates |
| [6. Protobuf: the schema is the work](#6-protobuf-the-schema-is-the-work-the-wire-format-is-not) | The hand-written registry, enum hazards |
| [6.1 Where the registry pattern stops working](#61-where-the-registry-pattern-stops-working) | Templates: the explicit instantiation table |
| [7. Prior art](#7-prior-art) | ORE |
| [8. Open decisions](#8-open-decisions) | What is still undecided |

## 1. Shape

```
TS frontend
    │  WebSocket: one binary ClientFrame/ServerFrame per message
    ▼
gateway ─────── sockets, correlation ids, backpressure, session log,
    │           stop-grace timers, terminal frames reported back
    ▼
supervisor ──── worker placement (§2.1), cancel-by-kill with replay (§3)
    │
    ├──> shared worker process        cheap interactive work: quote bumps,
    │    N sessions, one thread each  analytic prices, sub-second
    │
    └──> sacrificial worker process   long cancellable work: Monte Carlo,
         one session, killable        finite difference

every worker: QL_ENABLE_SESSIONS, one client session per thread,
              live QuantLib object graph cached between requests

cancel: ask the worker to stop at a batch boundary; kill it only if the
        request outlives the grace, then replay the session log into a fresh
        worker — session_id survives either path
```

The gateway owns sockets, correlation ids, and backpressure. Workers own
QuantLib. They are separate processes, for the reason in §3.

The gateway also owns the clock. The supervisor has no thread and no timer of
its own — every entry point is called from the gateway's loop — so the deferred
half of a cancel comes back the same way: the gateway arms the grace timer and
reports each terminal frame it forwards, which is how the supervisor tells a
request that stopped politely from one that has to be killed.

WebSocket already delimits messages, so frames carry no length prefix — one
serialized `ClientFrame` or `ServerFrame` per binary WebSocket message.

### 1.1 Process topology

Two kinds of process, and one service. The supervisor is a class inside the
gateway, not a service of its own.

| Process | Owns | What its death costs |
| --- | --- | --- |
| **Gateway** (one) | Sockets, correlation ids, backpressure, the event loop, the `Supervisor` object and every `SessionLog` | Everything. Logs are in memory and are not persisted |
| **Worker** (N) | QuantLib: sessions, graphs, engines. Shared or sacrificial per §2.1 | Its sessions replay elsewhere; their in-flight requests are lost |

Three things in the code make the supervisor a component rather than a service,
and any attempt to split it out has to deal with all three:

- It is constructed with `FrameSink`, `DisruptionSink` and `DeadlineTimer` —
  plain callbacks into the gateway, which would become RPCs back to it.
- `sessions_` and `workers_` are mutated from six entry points with no mutex,
  which is sound only because all six are called from the one event loop.
- A `SessionLog` is the gateway's own record of what a client sent. Splitting
  the supervisor out means either replicating that log across the boundary or
  serializing every `UpdateMarket` twice.

It is still the natural seam if several gateways should ever share one worker
fleet: the interface is narrow — open, dispatch, cancel, worker died, request
terminated, and three callbacks out. The hard part is not the interface, it is
that the session log would need an owner other than the socket that produced
it.

**Data plane and control plane leave the gateway by different routes.**
`ClientFrame`/`ServerFrame` — the same Protobuf types used on the WebSocket —
are forwarded to a worker over its pipe or socket and routed inside it by
`session_id`, one `Worker` thread per session. But `spawn`, `kill` and
`requestStop` cannot travel that way: a worker deep in a Monte Carlo is not
reading its queue, which is exactly the situation a cancel has to interrupt
(`worker.cpp`, the `kCancel` case, spells this out). They are process control —
fork, signal, or a flag in shared memory — and that is why `ProcessHost`
exists as an interface separate from frame delivery.

**Failure domains follow the process boundary, and are deliberately lopsided.**
A worker is disposable: whatever kills it, the supervisor replays its sessions
into another one and the client keeps its `session_id`. The gateway is not:
it holds the only copy of every session definition, so losing it loses every
session on the box, and no client can reconstruct one because clients hold
results, not definitions. That asymmetry is the design working as intended —
expensive state where it can be thrown away, cheap state where it cannot — but
it does mean the gateway is a single point of failure that nothing here
mitigates.

## 2. Global state forces session pinning

Every singleton in QuantLib is session-local; none is declared `Global = true`:
`Settings`, `IndexManager`, `ObservableSettings`, `LazyObject::Defaults`,
`SeedGenerator`, `ExchangeRateManager`, `Money::Settings`,
`IborCoupon::Settings`, `Tracing`.

In a default build `Singleton<T>::instance()` returns one process-wide static
(`ql/patterns/singleton.hpp:104`). Two concurrent requests that each set
`Settings::evaluationDate()` corrupt each other silently — plausible numbers,
wrong date.

**Decision: build workers with `QL_ENABLE_SESSIONS`** (`--enable-sessions`).
`instance()` then returns a `thread_local` instance
(`ql/patterns/singleton.hpp:92`), giving each worker thread an independent
QuantLib world. Note this is automatic in current QuantLib — there is no
user-supplied `sessionId()` function, contrary to older documentation.

**Decision: pin one client session to one worker thread for its whole life.**
An object graph built on thread A must never be priced on thread B; handles and
the observer graph are shared mutable state that sessions do not protect.

Sharing one bootstrapped curve across sessions would require
`QL_ENABLE_THREAD_SAFE_OBSERVER_PATTERN` — Boost.Signals2 over a
`recursive_mutex` (`ql/patterns/observable.cpp:104`), described as "less
performant" in `Docs/pages/config.docs:83`. We pay duplicate bootstrap cost per
session instead and keep the fast observer.

### 2.1 How many sessions per process

One session to one thread is forced by QuantLib. One session to one *process*
is not — that comes only from the cancellation design in §3, and the two
choices have to be made together.

| | Isolation | Cancel blast radius | Cost |
| --- | --- | --- | --- |
| **A** — 1 session : 1 thread : 1 process | Full | 1 session | One OS process per user session |
| **B** — N sessions : N threads : 1 process | QuantLib-level only | Every session in that process | Cheap |
| **C** — Hybrid (chosen) | Full where it matters | 1 session | Two worker classes to operate |

Under B, killing a worker to serve one `CancelRequest` destroys every other
session sharing that process. What makes that survivable is that **session
state is derivable**: `OpenSession` plus the `UpdateMarket` frames that
followed it fully determine the object graph, and that is a few kilobytes the
gateway already has. A killed session is replayed into a fresh worker and keeps
its `session_id`, so the client is not told it moved. What cannot be recovered
is the work that was in flight: requests on a killed process never produce a
terminal frame, so the supervisor reports each affected session and the gateway
fails their outstanding ids with `WORKER_DIED`. A kill therefore costs one
bootstrap and the requests running at the time, not the session.

Three requirements fall out of replay and cancel, and they are protocol
obligations rather than implementation details:

- The gateway retains each session's definition as an ordered log. It compacts
  trivially: a later `QuoteUpdate` supersedes an earlier one for the same
  `quote_id`, so the log stays at `OpenSession` plus one entry per distinct
  quote no matter how long the user tweaks.
- Replay must reach the same graph, so nothing that affects pricing may enter
  the worker outside these frames — no worker-local clock reads, no
  `Settings::evaluationDate()` set from anything but `OpenSession`. A session
  whose result depends on when it ran cannot be replayed.
- The gateway reports every terminal frame it forwards back to the supervisor,
  through `onRequestTerminated()`, and hands it the whole frame rather than
  just its id. Two things ride on the outcome. A cancel target that stopped
  politely inside its grace is otherwise indistinguishable from one still
  running, so every cancel would cost a kill and a replay that were not
  needed. And an `UpdateMarket` is folded into the session log only when it
  comes back as an `Ack`: that is the sole path by which the log advances past
  `OpenSession`, so without it a replay would silently restore the session to
  the market data it opened with — the quiet failure this whole section exists
  to prevent. A write that comes back as an `Error`, or that dies with its
  worker before answering, is dropped, which keeps the log a description of
  what the worker actually holds.

**Decision: configuration C.** Cheap interactive work — a quote bump on a live
graph, sub-second — runs in shared multi-threaded workers. Anything cancellable
and long, meaning Monte Carlo and calibration, is dispatched to a
single-session process that can be killed freely. Process-per-session is then
paid only for the calculations that need it.

Isolation is what makes the kill affordable, and the kill is what a cancel
mostly comes down to: the polite batch-boundary stop in §3 exists only for a
Monte Carlo the client asked progress for, and cannot touch a single engine
call. Sizing the grace short is therefore right — on everything else it is pure
added latency before the kill that was always going to happen.

**Placement is a property of the request, not of the session.** A session opens
in the shared pool and moves when `placementFor()` disagrees with where it is:
a `CloseSession` on the old worker and a replay into the new one, never a
migration, because the graph is thread-bound and cannot be handed over — and
never a kill either, because the process it is leaving is probably hosting
other sessions. A panel that alternates an analytic price with a Monte Carlo on
the same session pays that bootstrap in each direction, so requests of one kind
belong on a session of their own. Engine kind is a crude proxy for "long" — a
200-path Monte Carlo is cheap and a large FD grid is not — and a sample count
or a client-declared budget would be the better signal (§8).

**The packing is the supervisor's, not the process host's.** The host starts,
feeds and stops processes and decides nothing; which shared worker takes the
next session is policy, and it lives beside the placement rule so both can be
tested against a fake host. A session takes a seat on the first shared worker
with room — `Options::sessionsPerSharedWorker`, 8 by default — and forks only
when none has any. Since one session is one thread, that knob is a thread count
per process: size it against cores, not against how many sessions a fanned-out
panel opens, and let the excess queue. A process is retired once its last
session leaves, so process count tracks live sessions rather than their
high-water mark.

The shape that falls out, and the ratios worth holding in mind:

```
one connection
  │  session_id per frame, request_id per request
  ├── A ──┐
  ├── B ──┤   each session: one object graph, one thread, whole life,
  ├── C ──┤   one SessionLog held by the supervisor
  └── D ──┘
          │   the supervisor places sessions; it packs, it does not
          ▼   hand each one a process
  ┌────────────────────────────┐      ┌────────────────────┐
  │ shared worker process      │      │ sacrificial worker │
  │  ┌───┐  ┌───┐  ┌───┐       │      │  ┌───┐             │
  │  │ A │  │ B │  │ C │  ≤ 8  │      │  │ D │  1 thread   │
  │  └───┘  └───┘  └───┘ thr.  │      │  └───┘             │
  └────────────────────────────┘      └────────────────────┘
   quote bumps, analytic prices        Monte Carlo, FD; killed
   a second one is forked only         to serve a cancel, and
   when this one has no room           replayed afterwards

  connection : session   1 : N     one socket multiplexes them
  session    : thread    1 : 1     for the session's whole life
  session    : process   N : 1     shared, up to sessionsPerSharedWorker
                         1 : 1     sacrificial, which is what makes the
                                   kill affordable
  threads in a process   = sessions it holds, so a shared worker is
                           multi-threaded because it hosts many sessions,
                           never because one session uses many threads
```

D is the same session as before its Monte Carlo: it left the shared worker on
a `CloseSession`, replayed into a process of its own, and goes back to the pool
on its next analytic price. Placement follows the request.

**Consequence for the frontend: within a session, everything serializes.** Two
price requests on one graph cannot overlap — not even nominally read-only ones,
because `NPV()` writes to the lazy cache as it computes
(`ql/patterns/lazyobject.hpp`). A 100-strike smile is therefore N sessions each
holding its own copy of the graph, or a sequential loop; it is never one
session with N threads. Panels that fan out plots drive pool sizing directly.

### 2.2 QuantLib does not parallelise for you

Worth stating plainly before anyone sizes a pool on the assumption that a
single price uses more than one core: it does not. There is no TBB, no thread
pool, and no `std::thread`, `std::async` or `std::future` anywhere in `ql/`.

Internal parallelism is four OpenMP pragmas, all off in a stock build
(`QL_ENABLE_OPENMP` defaults to `OFF` at `CMakeLists.txt:45`; `--enable-openmp`
at `configure.ac:57`):

- `ql/methods/lattices/lattice.hpp:173` — tree rollback, guarded
  `if(size(i) >= 1024)` so early sparse steps do not pay for a thread team
- `ql/pricingengines/swaption/gaussian1dswaptionengine.cpp:136`
- `ql/termstructures/volatility/zabrsmilesection.hpp:217` — ZABR full-FD prices
  across strikes
- `ql/methods/finitedifferences/operators/triplebandlinearop.cpp:199`

A dozen further pragmas in `triplebandlinearop.cpp` and `ninepointlinearop.cpp`
are commented out, which says the FD operators were tried and abandoned. Monte
Carlo — where parallelism would be most natural, and where this service spends
its longest calculations — has none at all: `McSimulation` walks its samples in
a plain loop.

The comment above the Gaussian1d pragma is the library authors stating this
design's central constraint in their own words
(`gaussian1dswaptionengine.cpp:107-111`):

> a lazy object is not thread safe, neither is the caching in gsrprocess.
> therefore we trigger computations here such that neither lazy object
> recalculation nor write access during caching occurs in the parallized loop
> below.

They pre-warm `yGrid`, `forwardRate`, `zerobond` and `numeraire` inside an
`#ifdef _OPENMP` block before entering the loop, precisely so that no
`LazyObject` recalculates inside it. That is the price of parallelising
anything that touches the graph, and it is why so little of the library does.

**Decision: workers are built without OpenMP.** Under configuration C every
session-thread in a shared worker would spawn its own thread team, oversubscribe
the cores, and compete with the pool that is already providing the concurrency.
If a lattice- or FD-heavy workload ever justifies it, it belongs only on the
sacrificial single-session processes, with `OMP_NUM_THREADS` pinned — never in
the shared pool.

Supporting evidence that this is the right shape: QuantLib's own tooling
parallelises with processes rather than threads. `quantlib-benchmark` takes
`--nProc=16` and fans out over Boost.Process with an interprocess message queue
(`test-suite/quantlibbenchmark.cpp:26-53`), and the parallel unit-test runner
does the same. The supervisor in §2.1 arrives at the same answer for the same
reason.

## 3. No cancellation — hence processes, not threads

QuantLib exposes no progress, interrupt, or cancel hook: there is none in
`ql/methods/montecarlo/` or `ql/pricingengines/mcsimulation.hpp`. A long Monte
Carlo or calibration runs to completion and cannot be stopped from the socket.

**Decision: workers are processes, supervised.** `CancelRequest` first asks the
worker to stop at a batch boundary and gives it a short grace (250 ms by
default); a Monte Carlo running with progress enabled can take that and keep
its worker, and nothing else can. When the grace expires with the request still
running, the worker is killed and respawned. Either half terminates the
outstanding `request_id` with `Error{code: CANCELLED}` — exactly once, which is
why the gateway reports terminal frames back to the supervisor — and the
killing half then replays the session definition into a fresh worker per §2.1,
so the `session_id` stays valid and the client sees latency rather than loss.
The kill is the honest part; the grace only buys the cheaper outcome on the
rare engine that can offer it.

**Decision: progress is opt-in and computed by us.** A `PriceRequest` may set
`progress_every_paths`; the worker then runs Monte Carlo in path batches and
accumulates on our side, emitting `Progress` frames between batches. Without
that field the engine is called once and only a terminal frame is sent. The
protocol must not promise progress QuantLib cannot supply.

## 4. Determinism is not free

`SeedGenerator` seeds from `std::time(nullptr)`
(`ql/math/randomnumbers/seedgenerator.cpp:36`). Identical inputs submitted twice
return different Monte Carlo prices, which users read as a bug.

**Decision: `seed` is a required field on any Monte Carlo request** and is
echoed in `PriceResult`. The frontend keeps it stable across what-if tweaks so
the user sees the effect of the input, not of the RNG, and varies it explicitly
when it wants an error estimate.

**Progress changes the number, so it is part of the key.** QuantLib cannot
resume an engine, so `progress_every_paths` makes the worker run independent
batches with seeds derived from the request seed and average their means. That
estimates the same quantity, but it partitions the RNG stream differently from
one run of the same total: the batched price and the single-shot price at the
same seed do not agree. Reproducibility therefore keys on
`(seed, samples, progress_every_paths)`, all three of which `PriceResult`
echoes. A user who turns a progress bar on must not be told the price moved.

## 5. The lazy graph is why the backend is stateful

This is the actual justification for WebSocket over REST. A session keeps its
object graph alive; a what-if from the UI is a `SimpleQuote::setValue()` or a
`RelinkableHandle` relink, and the observer graph invalidates exactly the
affected instruments — the next `NPV()` recomputes only those. A stateless
design rebuilds curves on every request and discards all of it.

Two rules this imposes on the code behind the protocol:

- **Anything the frontend can vary must reach the pricer as an observable** —
  a `Handle<Quote>` the instrument registered with, never a value copied at
  construction. Otherwise the slider moves and the price does not (`AGENTS.md`
  §5.2). Every field in `UpdateMarket` maps to a registered `SimpleQuote`.
- **Batched updates take one recalculation, not N** — but only in the
  deferred form. `UpdateMarket` wraps its writes in
  `ObservableSettings::instance().disableUpdates(true)` / `enableUpdates()`,
  and the argument is not optional: with the default `false`,
  `notifyObservers()` hands the observers to `registerDeferredObservers()`,
  which keeps them only when `updatesDeferred()` is set
  (`ql/patterns/observable.hpp:165`). The drop form therefore leaves
  `calculated_` set and the next `NPV()` returns the pre-update price, silently.
  The one in-tree use of the drop form
  (`ql/pricingengines/swaption/blackswaptionengine.hpp:247-249`) wants exactly
  that: it is suppressing a cascade from a temporary engine swap, not batching.
  `UpdateGuard` in `src/session/updateguard.hpp` exists so this cannot be got
  wrong by accident.

## 6. Protobuf: the schema is the work, the wire format is not

`Real` is `QL_REAL` (`ql/types.hpp`), `double` by default, so proto `double` is
correct today. If a worker is ever built against an AD type — QuantLib has
active AD-compatibility work — `Real` stops being `double`; all numbers are
therefore converted at the serialization boundary through the `wire()` helper
in `worker.cpp`. QuantLib's own `value()` (`test-suite/utilities.hpp:53-58`)
lives in the test suite and is *not* installed with the library, so it cannot
be called from here. `wire()` is an overload pair instead: an exact match on
`double` in a stock build, and a template that resolves an AD type's own
`value()` by argument-dependent lookup otherwise.

QuantLib conventions are polymorphic objects with no reflection and no
string-to-object factory. `ql/utilities/dataparsers.hpp` offers
`PeriodParser::parse` and `DateParser::parseISO` and nothing more. Everything
else is the hand-written registry in `src/conventions/`. Expect it to be the
largest single component of the backend and to need an update whenever a
convention is added.

Two schema hazards drove the design in `conventions.proto`:

- **Proto3 enums default to 0 and cannot distinguish unset from first value.**
  Every convention enum reserves `0` as `*_UNSPECIFIED` and the registry
  rejects it with `QL_REQUIRE`. Without this, a field the frontend forgot to
  set silently prices on whatever sits at 0.
- **QuantLib's numeric enum values cannot be reused as proto values.**
  `Frequency` has `NoFrequency = -1` and `Once = 0`
  (`ql/time/frequency.hpp:37-38`) — the first is a negative varint, the second
  collides with the reserved unspecified slot. The mapping is explicit and
  one-directional, never a cast.

### 6.1 Where the registry pattern stops working

Conventions are objects, so a `switch` can return them. Curve bootstrapping is
not: `PiecewiseYieldCurve<Traits, Interpolator>` is a template, and every
traits/interpolator pair is a distinct type that must be named in source.

There is no lookup table that can produce one from a wire value. What
`Session::makeCurve` holds instead is an explicit instantiation table — the
cross-product, written out — which is why `CurveBootstrap` offers three traits
and three interpolators rather than QuantLib's full set. Each addition is a
line of source and a recompile, and each instantiation costs compile time and
object size, so the schema stays small on purpose. The same applies to any
other template-parameterised machinery exposed over the wire (RNG traits on the
Monte Carlo engines, for instance): the enum is a menu of what was compiled in,
never a description of what QuantLib can do.

Related: `DayCounter` and `Calendar` are messages rather than enums, because
`Thirty360` and `ActualActual` take a sub-convention
(`ql/time/daycounters/thirty360.hpp:78`,
`ql/time/daycounters/actualactual.hpp:53`), `UnitedStates` has no default
constructor and requires a `Market` (`ql/time/calendars/unitedstates.hpp:206`),
and joint calendars nest (`ql/time/calendars/jointcalendar.hpp:84`).

## 7. Prior art

ORE (Open Source Risk Engine) is QuantLib plus exactly the configuration and
serialization layer this design reinvents. Its schemas are XML rather than
Protobuf, but the decomposition — trade definitions, market definitions,
convention registry — is worth reading before extending `conventions.proto`.

## 8. Open decisions

- Curve bootstrap ownership: per-session rebuild (assumed here) versus a
  shared, thread-safe-observer curve cache. Revisit only if bootstrap time
  dominates a session's first response.
- Warm spare workers: replay after a `CANCELLED` kill costs a full bootstrap
  (`SessionOpened.bootstrap_seconds` measures it per session). Whether that
  needs a pre-warmed pool depends on how long the bootstrap actually takes,
  which is worth measuring before building one. The same measurement settles
  the retire-on-empty rule in §2.1: holding an idle process instead trades
  memory for a fork on the next session, and a session that alternates
  placements pays that fork each way.
- The §2.1 dispatch rule — which requests count as "long" and go to a
  sacrificial process — is currently by engine kind. Sample count may be the
  better signal.
- Whether an OpenMP-enabled worker binary is worth maintaining for the
  sacrificial processes (§2.2). It would help binomial and FD requests and
  nothing else, and it doubles the build matrix, so it needs a measured win on
  a real request before it earns its place.
- Whether `Progress` also carries a running NPV estimate, which is cheap for
  Monte Carlo and meaningless for calibration.
