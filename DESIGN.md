# Architecture Design — Interactive QuantLib Service

A stateful C++ pricing backend driving a TypeScript frontend over WebSocket,
with Protobuf as the wire format.

This document records the design decisions and, more importantly, the QuantLib
constraints that force them. Claims about the library are cited against
`third_party/QuantLib`, the submodule this repository pins at v1.43, so the
line numbers hold against a fixed tree rather than against whichever QuantLib
happens to be installed. Most carry a line or a range; the rest name a file or
a whole directory, which is all a claim about an *absence* can be cited to —
§3 rests on one of those. Source comments cite back to this document by
section, as `DESIGN §2.1`.

Build instructions are in [`INSTALL.md`](INSTALL.md); current status and the
file map are in [`README.md`](README.md). Where the design and the code differ,
the section says so — today that is §3: workers are threads in the gateway
process, so the kill half of a cancel is a disown rather than a kill.

| Section | What it settles |
| --- | --- |
| [1. Shape](#1-shape) | The components and what each owns |
| [1.1 Process topology](#11-process-topology) | Why the supervisor is a class, not a service |
| [1.2 Where state lives](#12-where-state-lives-and-what-it-survives) | The graph is the service's, the definition is the client's, and nothing is on a disk |
| [1.3 Scaling out](#13-scaling-out-and-what-pins-a-session-to-one-box) | Sessions shard, they do not replicate, and what pins one to a box |
| [2. Global state forces session pinning](#2-global-state-forces-session-pinning) | `QL_ENABLE_SESSIONS`, one session per thread |
| [2.1 How many sessions per process](#21-how-many-sessions-per-process) | Shared vs. sacrificial workers, replay, placement |
| [2.2 QuantLib does not parallelise for you](#22-quantlib-does-not-parallelise-for-you) | No OpenMP in workers |
| [3. QuantLib has no cancel hook](#3-no-cancellation--hence-processes-not-threads) | Cancel-by-kill, the stop grace, the three shapes that stop politely, and the host that cannot honour a kill |
| [4. Determinism is not free](#4-determinism-is-not-free) | Required seeds, and why progress changes the price |
| [5. The lazy graph is why the backend is stateful](#5-the-lazy-graph-is-why-the-backend-is-stateful) | Observables and batched updates |
| [6. Protobuf: the schema is the work](#6-protobuf-the-schema-is-the-work-the-wire-format-is-not) | The hand-written registry, enum hazards |
| [6.1 Where the registry pattern stops working](#61-where-the-registry-pattern-stops-working) | Templates: the explicit instantiation table |
| [6.2 Why the schema lives under a version segment](#62-why-the-schema-lives-under-a-version-segment) | What a version buys, and when to open the next one |
| [6.3 The general schema](#63-the-general-schema) | Payoff x exercise x style, the market namespace, what is served |
| [7. Prior art](#7-prior-art) | ORE |
| [8. Open decisions](#8-open-decisions) | What is still undecided |
| [9. The gateway](#9-the-gateway) | The loop, the worker channel, backpressure, disconnects |
| [9.1 One loop, and everything on it](#91-one-loop-and-everything-on-it) | Why nothing calls the supervisor off the loop |
| [9.2 The worker channel](#92-the-worker-channel) | How frames reach a worker and come back |
| [9.3 Backpressure](#93-backpressure-shed-progress-never-a-terminal-frame) | Shed `Progress`, never a terminal frame |
| [9.4 A disconnect holds the session](#94-a-disconnect-holds-the-session-briefly) | The resume window, the token, and what a held seat costs |
| [9.5 What the gateway tracks](#95-what-the-gateway-tracks-that-nothing-else-does) | Outstanding ids, routing, the cancel's own Ack |
| [9.6 The door](#96-the-door-in-place-of-authentication) | Origin, the two caps, and `/healthz` |

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

cancel: ask the worker to stop at its next step; kill it only if the
        request outlives the grace, then replay the session log into a fresh
        worker — session_id survives either path, and every request is
        cancellable: what differs is whether it keeps its partial results
```

The gateway owns sockets, correlation ids, and backpressure. Workers own
QuantLib. They are separate processes, for the reason in §3 — in the design.
What runs today is `ThreadProcessHost`, which makes them thread groups in the
gateway process and therefore cannot honour a kill; §3 says what that costs.

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
| **Gateway** (one) | Sockets, correlation ids, backpressure, the event loop, and the `Supervisor` — which holds every `SessionLog` in this process, on the gateway's behalf | Everything. Logs are in memory and are not persisted |
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
session on the box. That asymmetry is the design working as intended —
expensive state where it can be thrown away, cheap state where it cannot — but
it does mean the gateway is a single point of failure that nothing here
mitigates. What makes that survivable is not in this process at all: the client
holds the definition and replays it, which §1.2 sets out.

### 1.2 Where state lives, and what it survives

A session is two things kept apart on purpose: a **graph**, which is expensive
and derivable, and a **definition**, which is cheap and is not. The service
owns the first and the client owns the second, and every recovery path in this
document falls out of that split.

| State | Where it lives | What it outlives |
| --- | --- | --- |
| The QuantLib object graph — quotes, curves, surfaces, indices, instruments | Worker thread memory, one session per thread (§2) | Every request in the session; a dropped socket, for the resume window (§9.4) |
| The `SessionLog` — the `OpenSession`, plus every `UpdateMarket` that was accepted, plus a kept sweep as a synthetic write | `Supervisor::sessions_`, in the gateway process (§2.1) | A killed or dead worker: it is what a replay is made of |
| The resume token, and the `SessionOpened` a resume answers with | The gateway, per session (§9.4) | The socket, for the window. Nothing else |
| Outstanding `request_id`s | The gateway, per session (§9.5) | Nothing: they are what a disrupted session has to fail |
| The market definition and the trade — the *document* | The client. In `ql-frontend` it is a Redux slice, saved to one `localStorage` key as canonical Protobuf JSON | Everything on this side, including the process |
| Prices and greeks | The client, in memory | They are results, not state: the service keeps none |

**Nothing here touches a disk.** There is no database, no file, no journal:
`grep` the service for a write and the only one is the socket. That is a
decision rather than an omission — a session is a graph built from a message
the client still has, so the copy worth keeping is the client's — and its price
is the row in §1.1: the gateway is the single point of failure, and a process
that dies takes every graph, every log and every in-flight calculation with it.

What follows from the split, in the order a client meets it:

| The event | What the service still has | What the client does |
| --- | --- | --- |
| A request finishes | The graph, moved on by whatever the request wrote | Reads the result |
| A worker dies or is killed for a cancel | The `SessionLog` | Nothing: the session is replayed under it and keeps its id (§2.1) |
| The socket drops, and comes back inside the window | The graph, the seat, the running requests | `ResumeSession` (§9.4) |
| The socket drops for longer, or the client reloads | Nothing after the window expires | Opens a session and replays the document |
| The gateway restarts | Nothing | The same replay, which is why this path must not rot |
| The client's browser storage is cleared | Nothing that helps | Starts from the seed document. This is the one loss with no recovery |

The last two rows are the reason the resume in §9.4 is described as an
optimisation over replay rather than a replacement for it. Replay is the only
path that works when the service remembers nothing, and it works because the
definition was never the service's to keep.

### 1.3 Scaling out, and what pins a session to one box

One box is the unit. Within it the design already spreads work — sessions
packed into shared workers, long requests sent to sacrificial ones (§2.1).
Across boxes it does not spread at all, and §1.2 is the reason: a session is a
graph in a worker's memory plus a log in one gateway's, and that pair is
reachable only from the process holding it.

That rules out the cheap form of horizontal scaling — identical replicas
behind a round-robin balancer — and leaves the other one. **Sessions shard;
they do not replicate.** A router in front of N gateways must send every frame
of a session to the gateway that opened it, which costs almost nothing here: a
session lives on one long-lived WebSocket, so connection affinity *is* session
affinity, and nothing has to parse a `session_id` out of a Protobuf body to
route correctly. `/healthz` (§9.6) is already the liveness signal such a
router needs, and is deliberately honest about its scope — it proves the loop
is turning, not that any graph is well.

Four things pin a session to its box. Only the first is trivial to remove:

| What pins it | Where | What unpinning takes |
| --- | --- | --- |
| Session ids come from a per-process counter, `"s-" + ++nextSessionId` | `gateway.cpp:204`, `:465` | A uuid, or a per-gateway prefix. Two gateways today both hand out `s-1` |
| The `SessionLog` lives in the gateway's memory and is written nowhere | `Supervisor::sessions_` (§1.1, §1.2) | An owner for the log other than the socket that produced it — §8's open question, and the hard half of splitting the supervisor out |
| The resume token and its window are the gateway's | §9.4 | That same owner. A reconnect landing elsewhere is a reopen, not a resume |
| The supervisor is a class on the gateway's loop: callbacks in, no mutex on `sessions_` | §1.1 | Turning those callbacks into RPCs, which §1.1 already names as the seam |

**Decision: none of this is built, and the client-held definition is why it
does not block.** Replay is the failover path (§1.2). A gateway lost to a
crash or a deploy costs its clients the work in flight and one round trip
rather than recoverable state, because the document never left the browser and
a rebuild is 0.009 ms for the seven-object market `HANDLERS.md` opens with and
3.6 ms for a curve stripped from forty-eight swap pillars (§9.4). Sharding by
connection therefore needs the id fix and nothing else; failing a session
*over* to another box needs the log to have an owner, and that earns its
complexity only once one gateway's core count is the binding constraint rather
than its liveness.

**What the statefulness actually buys, which is what should decide the shape.**
It is not bootstrap amortisation: §9.4 measured that at single-digit
milliseconds, which is exactly why the resume window there is justified by the
work in flight instead. It is the observer graph of §5 — a quote write
invalidates only the instruments depending on it, so a slider drag recomputes
a subgraph rather than a market. A load that is mostly one-shot pricing would
scale more simply as stateless replicas and would lose little by it; this
design earns its constraints when a client is dragging a spot and expecting
the panel to keep up.

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
comes down to for any request that is a single engine call. The polite stop in
§3 covers the three shapes with a loop of our own to check a flag in — a batched
Monte Carlo, a sweep and a batch — and cannot touch the rest. Sizing the grace
short is therefore right: on everything else it is pure added latency before the
kill that was always going to happen.

**Placement is a property of the request, not of the session.** A session opens
in the shared pool and moves when `placementFor()` disagrees with where it is:
a `CloseSession` on the old worker and a replay into the new one, never a
migration, because the graph is thread-bound and cannot be handed over — and
never a kill either, because the process it is leaving is probably hosting
other sessions. A panel that alternates an analytic price with a Monte Carlo on
the same session pays that bootstrap in each direction, so requests of one kind
belong on a session of their own. The engine method is a crude proxy for
"long" — a
200-path Monte Carlo is cheap and a large FD grid is not — and a sample count
or a client-declared budget would be the better signal (§8).

**The packing is the supervisor's, not the process host's.** The host starts,
feeds and stops processes and decides nothing; which shared worker takes the
next session is policy, and it lives beside the placement rule so both can be
tested against a fake host. A session takes a seat on the first shared worker
with room — `Options::sessionsPerSharedWorker`, 8 by default — and forks only
when none has any. Since one session is one thread, that knob is a thread count
per process, and what it bounds is the blast radius of a kill on a shared
worker: how many co-tenants a cancel that goes the hard way takes with it. It
does not bound concurrency. A session that finds every shared worker full gets
a fresh one, so threads on the box follow sessions open; capping the pool and
queueing sessions on it is §8. A process is retired once its last session
leaves, so process count tracks live sessions rather than their high-water
mark.

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
`ql/methods/montecarlo/` or `ql/pricingengines/mcsimulation.hpp`. A single
engine call runs to completion and cannot be stopped from inside. Everything
below follows from that, including the three exceptions — they are ours, not
QuantLib's, and they exist only where this service drives the engine in a loop
it wrote.

**Decision: workers are processes, supervised.** `CancelRequest` first asks the
worker to stop at its next step and gives it a short grace (250 ms by default).
When the grace expires with the request still running, the worker is killed and
respawned. Either half terminates the outstanding `request_id` with
`Error{code: CANCELLED}` — exactly once, which is why the gateway reports
terminal frames back to the supervisor — and the killing half then replays the
session definition into a fresh worker per §2.1, so the `session_id` stays valid
and the client sees latency rather than loss.

**Which requests can take the grace, and what it means that the rest cannot.**
This section long said a batched Monte Carlo could stop politely and nothing
else could. Three shapes can, and they are the three that have an *outer* loop
this service wrote: a batched Monte Carlo between path batches, a scenario sweep
between points, and a `PriceBatch` between entries. Each checks the stop flag,
keeps what it computed, and answers with a partial result rather than nothing.
Everything else is one engine call with no seam to check a flag in, and there
the kill is the whole mechanism.

That distinction is worth stating positively, because stating it negatively is
what misled the frontend for a milestone: **every request is cancellable**, and
what differs is the cost — the three shapes above give the session back with
their partial results, and everything else gives the session back after a
rebuild. A client that reads "cannot be interrupted" as "cannot be cancelled"
will offer a stop button on a third of its requests, which is what happened.

**What actually runs is a thread host, and it cannot honour a kill.**
`ThreadProcessHost` (`src/gateway/threadhost.hpp`) runs workers as thread groups
in the gateway process, so everything above except process control is exercised
end to end in one binary. Its `kill()` is a *disown*: it asks the worker to
stop, drops every frame the seat emits afterwards, and detaches the thread,
which runs its Monte Carlo to completion against nothing while the supervisor
replays the session elsewhere. The client sees the designed behaviour — one
`CANCELLED`, a live `session_id`, a rebuilt graph — and the machine keeps
burning a core until the abandoned call finishes. The decision above stands; a
fork-based host is what makes it real, and it is the one piece of this design
that is specified and not yet built.

**Decision: progress is opt-in and computed by us.** A `PriceRequest` may set
`engine.mc.progress_every_paths` — on the Monte Carlo parameter block, because
it changes the answer and belongs beside the seed and the sample count that
determine it; the worker then runs Monte Carlo in path batches and
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
`(seed, samples, progress_every_paths)`. `PriceResult` echoes the whole
`Engine` message rather than three named copies of it, so the promise cannot
fall behind the schema the way three hand-maintained fields would. A user who turns a progress bar on must not be told the price moved.

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
cross-product, written out — which is why `BootstrappedCurve` offers three traits
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

### 6.2 Why the schema lives under a version segment

The version is not decoration on a directory name. It appears in four places
that have to agree, and only one of them is a free choice:

| Where | Form |
| --- | --- |
| On disk | `proto/quantlib/v2/` |
| Proto package | `package quantlib.v2;` (`envelope.proto:14`) |
| Generated C++ namespace | `quantlib::v2`, aliased to `qlpb` (`session.cpp:71`) |
| Generated headers | `${CMAKE_BINARY_DIR}/generated/quantlib/v2/`, included as `"quantlib/v2/envelope.pb.h"` |

`v1` still occupies the same four slots, because §6.3 imports its
`conventions.proto` rather than forking it. The alias is the only line that had
to move when the service changed generation, which is the whole return on the
segment.

The directory has to mirror the package because `protoc` resolves imports by
path relative to `-I proto`: `import "quantlib/v1/conventions.proto"`
(`envelope.proto:17`) is a filesystem path, not a package reference. The free
choice is whether there is a version segment at all, and the layout
`<package-path>/<version>/<file>.proto` is the Protobuf community convention.

**What it is for.** Protobuf's own compatibility rules cover only additive
change: a new field, a new enum value, a removed field whose number is
reserved. A change outside that set cannot be expressed by editing this package
at all — renumbering a field, changing its type, or deleting a message a
deployed client still sends. The convention is then to add `quantlib/v2/`
*beside* `v1` and serve both through a transition, and that works precisely
because the version is in the package: the two generate into `quantlib::v1` and
`quantlib::v2`, register in the descriptor pool under distinct full names, and
land in distinct header paths, so one binary can link and serve both. Without
the segment, an incompatible change forces every client to cut over in the same
release.

**The dangerous class is not the one the wire format catches.** Renumbering a
field breaks loudly. Reusing a field number for a different meaning, or
redefining what an existing value denotes while leaving its type alone, is
wire-compatible and silently wrong — the same failure mode the `*_UNSPECIFIED`
rule in §6 exists to prevent, one level up. A change of that kind needs `v2`
even though nothing in the encoding would object.

**The rule.** Stay on a version while every change is additive; open the next
one the first time a change is not. The quanto family was the worked example on
the additive side: within `v1` it added messages, oneof arms, enum values, and
the fields `CurveDefinition.flat`, `Engine.fd_grid` and `PriceResult.fd_grid`
— all additive, so a `PriceRequest` serialized before it still parsed after it.
(Those are `v1` field names; §6.3 spells the same ideas differently, which is
exactly what makes it a version rather than an extension.)

**It earned its keep sooner than expected.** The reasoning above was written
against a hypothetical `v2`; §6.3 is one, and it arrived from a survey rather
than from a deployment. The cost of having the segment already there was a
directory level; the cost of not having it would have been renaming the package
line, every import, every generated include path and the `qlpb` alias, in a
tree where `v1` still has to keep working.

### 6.3 The general schema

`proto/quantlib/v2/` is `v1` re-derived from QuantLib's own decomposition
instead of from the two instruments that happened to be needed first. It is what
the service speaks; the paragraph below on what is implemented says how much of
it is served.

**Where it comes from.** QuantLib's `test-suite/` is 188 `.cpp` files, and its
test-data structs are an inventory of what a pricing request has to carry.
They rhyme. `EuropeanOptionData`, `AmericanOptionData`, `DigitalOptionData`,
`ForwardOptionData` and `BasketOptionOneData` are the same seven fields —
`{type, strike, s, q, r, t, v}` — and every exotic is those seven plus a small
block: `NewBarrierOptionData` adds a barrier, a rebate and an exercise type,
`DiscreteAverageData` adds fixings and a running average, `LookbackOptionData`
adds an extremum and a window. The recurring part is the underlying and its
market; the varying part is small, named, and per shape.

**What `v1` costs.** It writes one message per product, so the fields common to
all of them are re-declared per product and the message count grows as
shape × quanto × exercise. It is already visible: `VanillaOption`,
`QuantoVanillaOption`, `QuantoForwardVanillaOption`, `QuantoBarrierOption` and
`QuantoDoubleBarrierOption` each declare `type`, `strike` and `expiry`; the
barrier fields appear twice; a plain barrier option would be a sixth message
duplicating all of it; an American vanilla would be a seventh. None of those
duplications is a distinction QuantLib makes.

**The decomposition.** `Option::arguments` is a payoff and an exercise
(`ql/option.hpp:64-65`), and that is the whole of it:

| Axis | `v2` | Menu from |
| --- | --- | --- |
| Payoff | `Payoff`, a oneof | `ql/instruments/payoffs.hpp` |
| Exercise | `Exercise`, type + dates | `ql/exercise.hpp:35-97` |
| Underlying | `Underlying`, market ids + process | the `{s, q, r, v}` of every test row |
| Shape | `Option.style`, a oneof | the instrument classes that exist |
| Currency | `Option.quanto`, optional | `QuantoEngine`, which wraps an engine |
| Method | `Engine.method` × `Engine.model` | `ql/pricingengines/` |

The four quanto messages collapse into zero: quanto stops being a product and
becomes what it is in QuantLib, an adjustment to the engine, so it composes
with any style instead of needing a message per pairing.

**`style` is a oneof, not a repeated feature list.** The composable form is the
obvious generalization and it is wrong. QuantLib has a fixed menu of instrument
classes; there is no quanto-barrier-lookback, and a repeated feature list would
have the schema promise a product space perhaps a tenth of which can be built,
with the other nine tenths failing at runtime with no way for a client to know
in advance. A oneof says what exists. Quanto sits outside it because it is the
one axis that genuinely is orthogonal — and it is orthogonal precisely because
it wraps the engine rather than the instrument.

**The market becomes a namespace.** In `v1` each instrument named the market
objects it needed in its own fields, which made the market a property of the
trade and meant a new instrument added market fields. In `v2` the session owns
an ordered list of `MarketObject`, each with an id, and instruments refer to
ids. That is what makes it possible to add a volatility surface at all: the
test suite builds `BlackVarianceSurface`, `BlackVarianceCurve` and three kinds
of local volatility, and `v1` has no object to put any of them in — volatility
enters as a single quote and every instrument gets a `BlackConstantVol`. Same
for interpolated zero curves, which the test suite uses more often than
bootstrapped ones (`zerocurve.hpp` in 28 files against `piecewiseyieldcurve.hpp`
in 13), and for `IndexManager` fixings, which nothing in `v1` populates at all.

**The output half was the more wrong one.** `v1`'s `PriceResult.results` is
`map<string, double>`. QuantLib's `additionalResults` is `map<string, any>`,
and the engines put vectors and matrices in it — `forwards`, `probabilities`,
`spotVols`, `TimeGrid`, per-leg `legNPV` — which are exactly the results worth
plotting rather than printing. A double-valued map drops every one of them, and
drops them silently. `v2` carries a `Value` variant that mirrors the `any`, and
adds the two things a panel needs that no engine publishes: a `CashFlow` table
(the working behind a swap price) and a `Series` (a line, with its axes named,
for a curve or a ladder or a convergence trace).

**`Flag` closes the hole `*_UNSPECIFIED` left open.** §6 rejects zero for
every convention enum because proto3 cannot tell unset from the first value.
The same hole is open for every `bool`, and `v1` has several: `end_of_month` on
a swap leg, `performance` on a quanto forward. A forgotten one prices as
`false` with no complaint. `v2` uses a three-valued `Flag` wherever the bit
moves a price or its sign — end-of-month, in-arrears, pay-or-receive,
long-or-short, payoff-at-expiry, knock-in — and keeps plain `bool` for request
options, where `false` is a smaller answer rather than a wrong one. The rule:
a flag that changes the number is a `Flag`, a flag that changes the reply is a
`bool`.

**`Scenario` is the session model's payoff, made explicit.** A spot ladder is
one frame, one graph, and N lazy recomputes of only what the bumped quote
invalidated — against N round trips that each rebuild everything. That is the
entire argument for a stateful backend (§5), and until this schema nothing let a
client ask for it in one request. It is `repeated Scenario scenarios` on the
same field number the singular one had, because a message field and a
one-element repeated field are the same bytes: the grid arrived without a
version. Several axes multiply into one row-major surface, still one frame and
still one graph, which is the same argument with a dimension added.

**What arrived after this section was first written.** Five additions, each
because a client could not otherwise ask a question the service could answer:
`Hello`/`Capabilities`, so the support matrix is served rather than copied into
every client; `PriceResult.unavailable_results`, so an engine that cannot supply
a greek names the absence instead of returning a map the client has to diff;
`ImpliedVolatility`, the one result computed from an input the request carries
rather than from the market; `PriceBatch`/`BatchResult`, a book against one
graph in one frame with a failing entry costing one row; and `CurveSample`, so a
panel draws the term structure the engine priced against rather than one it
rebuilt. Each is additive, which is what kept them inside `v2` under the rule in
§6.2.

**The port.** `ql-backend` moved to `v2` in the same pass: `Session` is rebuilt
around the market namespace and the payoff x exercise x style decomposition, and
the transport above it — worker, supervisor, gateway — carries `v2` frames.
`v1` remains in the tree because `v2` imports its conventions and because §6.2
is about it, but nothing serves it and its three client scripts are gone.

**What is implemented, and what is only in the schema.** The one-asset option
family is live: eight payoffs, three exercises, and the vanilla, barrier,
double-barrier, forward-start, Asian, lookback, compound, chooser, cliquet and
the multi-asset basket, against analytic (including the three American approximations), lattice,
finite-difference, integral and Monte Carlo engines, with quanto composing over
the shapes QuantLib has a wrappable engine for. Swaps are the general n-leg
`Swap` rather than `VanillaSwap`. The market builds quotes,
flat/interpolated/bootstrapped yield curves, three volatility shapes, Ibor and
overnight indices, and past fixings.

Not implemented, and rejected by name as `UNSUPPORTED` rather than mispriced:
bonds, credit, inflation, FX forwards, variance swaps, swaptions, caps and
floors; the two styles the schema carries and this build does not price, both
of which are the schema describing something twice rather than an engine
missing — `spread`, which QuantLib 1.43 prices as a basket kind, and `digital`,
which is a barrier carrying a binary payoff;
discrete dividends, discretely monitored and partial-time barriers; the Heston,
Bates and local-volatility processes; and the correlation matrix, which the
schema carries for the basket case the engines do not yet cover. Each is the
same work the quanto family was — a registry mapping, an explicit instantiation
table where a template is involved (§6.1), and a test row with a reference
value — and the schema is now the part that does not have to be redesigned each
time.

**How it is checked.** `test/smoke_v2.py` prices 369 rows of QuantLib's own
published reference values over the wire and compares each against the value
its test suite records, at the tolerance that test uses. The rows are not
transcribed: `test/extract_tables.py` parses them out of `test-suite/*.cpp`,
which is the only way a benchmark stays one.

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
- Whether the shared pool should be capped. Today every session gets a seat
  and a full pool grows by a process, so `sessionsPerSharedWorker` bounds a
  kill's blast radius and nothing else (§2.1); a client that opens a session
  per strike oversubscribes the cores rather than queueing. A cap with a
  queue in front of it is the alternative, and it needs a measurement of
  what a fanned-out panel actually does to a box before it is worth its
  latency.
- Whether an OpenMP-enabled worker binary is worth maintaining for the
  sacrificial processes (§2.2). It would help binomial and FD requests and
  nothing else, and it doubles the build matrix, so it needs a measured win on
  a real request before it earns its place.
- Whether `Progress` also carries a running NPV estimate, which is cheap for
  Monte Carlo and meaningless for calibration.
- ~~Whether a reconnecting client may resume its sessions rather than reopen
  them (§9.4).~~ **Settled, and not the way this list expected.** The
  measurement it asked for says a rebuild costs single-digit milliseconds, so
  resuming to save a bootstrap would not have been worth a held seat. Resuming
  to save the *work in flight* is: `ResumeSession` and a 60-second window are
  in §9.4, and the client identity the protocol did not have is a token minted
  per session rather than per client.
- Whether the gateway persists session logs. Today it does not, which is what
  makes it the single point of failure §1.1 admits to — and, per §1.3, the one
  thing standing between sharding sessions across gateways and failing them
  over between gateways.

## 9. The gateway

Every section above imposes obligations on the gateway and none of them
designs it. This one settles the four choices that block writing it — the
loop, the worker channel, backpressure, and what a dropped connection means —
and states what the gateway has to track that nothing else does.

Authentication, TLS termination, multi-tenancy and persistence are out of
scope, and §9.6 is what stands in for the first of them. The single point of
failure in §1.1 stands: this section does not mitigate it.

Claims about uWebSockets below are cited against `third_party/uWebSockets`,
the submodule this repository pins at v20.66.0, so every line number holds for
the version that is actually built.

### 9.1 One loop, and everything on it

**Decision: a single-threaded event loop, uWebSockets over uSockets.** This is
not a preference. `Supervisor` mutates `sessions_` and `workers_` from six
entry points with no mutex, and that is sound only while all six run on one
thread (§1.1). The loop is therefore part of the supervisor's correctness
argument, not a transport detail underneath it.

Three consequences, each of which is a rule for the code that has yet to be
written:

- **Nothing calls into the supervisor from another thread.** `Loop::defer`
  (`src/Loop.h:169`) queues onto the loop thread and wakes it, and it is the
  only sanctioned way in. A worker-side callback that reaches the supervisor
  directly is a data race that will not reproduce under test.
- **`DeadlineTimer` is one repeating tick, not a timer per deadline.** The
  obvious implementation is a one-shot `us_timer_t`
  (`uSockets/src/libusockets.h:110,120`) closed from its own callback, and it
  is wrong: `us_timer_close` frees the timer immediately rather than deferring
  it the way a socket close is deferred, so the dispatch loop is left holding
  freed memory. A single `us_timer_t` firing every 25 ms against a sorted queue
  of deadlines has no such edge. Resolution costs nothing here — the only
  deadline is the 250 ms stop grace, and the supervisor already assumes a timer
  it cannot disarm, which is why cancel rounds carry a generation.
- **The loop must not block.** Every long calculation is in another process by
  construction (§3), so the only way to violate this is to do work in a frame
  handler — parsing a large `OpenSession`, say — that belongs on the worker.

### 9.2 The worker channel

Data plane and control plane leave by different routes (§1.1), and the split
decides the mechanism for each.

**Decision: frames to workers are length-prefixed.** §1 settles the wire
format only for WebSocket, which delimits messages for us; a pipe does not.
Each direction carries a 4-byte little-endian length followed by one
serialized `ClientFrame` or `ServerFrame` — the same types, so nothing is
re-encoded at the boundary and a frame can be forwarded from socket to pipe
without being understood.

**Decision: worker pipes join the same loop as raw polls.**
`us_create_poll` / `us_poll_init` / `us_poll_start`
(`uSockets/src/libusockets.h:254,260,263`) put a worker's read end directly
into the event loop. The alternative — a reader thread per worker calling
`Loop::defer` — adds a thread per process, a queue hop per frame, and an
ordering question at every hand-off, in exchange for nothing.

**Decision: control travels out of band, and process death comes back in
band.** `spawn` is fork/exec, `requestStop` is a signal or a flag in shared
memory, and `kill` is `SIGKILL`; none may ride the frame pipe, because a
worker inside a Monte Carlo is not reading it and that is exactly the case a
cancel exists to interrupt (§3). Child exits must arrive as
`onWorkerDied()` **on the loop** — through a self-pipe, `signalfd`, or the
loop's own child watcher — never from a `SIGCHLD` handler, which shares the
supervisor's lack of a mutex with none of its guarantees.

### 9.3 Backpressure: shed `Progress`, never a terminal frame

This is the one place where the transport can break the protocol, and the
default configuration does.

`WebSocket::send` skips the message and returns `DROPPED` when buffered data
exceeds `maxBackpressure` (`src/WebSocket.h:92-110`), which defaults to 64 KB
(`src/App.h:242`), and `closeOnBackpressureLimit` defaults to `false`
(`src/App.h:243`). A slow client therefore loses frames while its connection
stays open and healthy-looking. Lose a `Progress` frame and a bar stutters.
Lose the terminal frame and the client waits forever for a result that was
computed, paid for, and discarded — which silently breaks the exactly-once
terminal guarantee that §3 spends a process kill to provide.

**Decision: the gateway sheds `Progress` itself, before uWebSockets can.** At
most one `Progress` frame per request is outstanding on a connection; if the
previous one has not drained (`getBufferedAmount()`, with the `drain` handler
to resume), the next is dropped by us. Progress is a hint and every frame
supersedes the last, so shedding it costs nothing. This keeps the buffer clear
for the frames that matter.

**Decision: `closeOnBackpressureLimit = true`.** A client that still cannot
keep up after `Progress` has been shed is not one whose results can be
delivered, and a loud disconnect — handled by 9.4 — is better than a request
that never terminates.

**Decision: every `send` return value is checked.** `DROPPED` on a terminal
frame is a bug in this policy, not a condition to handle: it means the two
decisions above failed and a client has been left hanging. It must be logged
as such.

One inbound default needs raising too: `maxPayloadLength` is 16 KB
(`src/App.h:238`), and an `OpenSession` carrying a few hundred pillars and
their conventions will exceed it. The limit belongs where it can be reported
against a field — a session too large should come back as `INVALID_ARGUMENT`
naming what was too big, not as a transport-level close with no frame to
attribute it to.

### 9.4 A disconnect holds the session, briefly

This section used to say that closing a socket closed every session on it, and
argued it from the cost of a rebuild: a session is a bootstrap, not a document,
and §2.1 prices a rebuild at exactly one. §8 said the question should be
settled by that measurement rather than by preference, so it was. §1.2 is the
split it rests on — the graph is this service's, the definition is the
client's.

**The measurement, taken here against this build.** A bootstrap costs 0.009 ms
for the seven-object market `HANDLERS.md` opens with, 1.5 ms for a curve
stripped from thirty swap pillars, and 3.6 ms for one stripped from
forty-eight. The rebuild was never the expensive part of a dropped socket.
What was expensive is what was *running*: a request in flight died with the
session, so a network blink half way through a twenty-million-path Monte Carlo
cost the calculation, and the client had to start it again.

**Decision: a session outlives its socket by a grace window, and so does the
work in it.** The default is 60 seconds; `--session-grace 0` restores the old
rule exactly. Within the window the session keeps its graph, its worker seat
and its running requests, and the client takes it back with `ResumeSession`,
naming the session and presenting the token `SessionOpened` minted for it. A
terminal frame produced while nobody was attached is held and delivered on the
way back in; `Progress` is shed, for the reason §9.3 sheds it under
backpressure — by the time anyone reads it the work has moved on.

**The token is what makes this safe to offer at all.** A WebSocket upgrade is
not subject to the same-origin policy (§9.6), so any page on the machine can
reach this socket; without a secret, a resumable session id would be a session
any page could adopt. It is 128 bits from `std::random_device`, minted by the
gateway — which owns connection-scoped identity (§9.5) and is the only part of
this service that should have an opinion about who a socket belongs to. Every
refusal is `SESSION_NOT_FOUND`: a wrong token, an expired window and a session
that was closed on purpose are one answer, so a guess learns nothing from the
difference, and the client's move is the same for all three.

**What it costs, and what bounds it.** Every held session is a worker seat
nobody is sitting in. Two limits keep that from becoming the failure mode the
old rule avoided: the window itself, and a cap on how many sessions may be
held at once (16 by default). Past the cap the *longest-waiting* one is closed
rather than the newest refused — the oldest is the likeliest to have been
abandoned. `/healthz` reports the count as `detached`, so the seats being held
for absent clients are visible rather than inferred.

**Consequence for the frontend: `session_id` survives a reconnect, if the
client asks for it.** A client that resumes keeps its id, its graph and its
in-flight work; a client that does not — or that comes back too late, or to a
service that has been restarted — opens a session and replays, exactly as
before. Replay is still the fallback and still has to work: this window is an
optimisation over it, not a replacement for it.

A related default: uWebSockets closes an idle connection after 120 seconds
(`src/App.h:240`) but sends pings first (`sendPingsAutomatically`,
`src/App.h:247`), so a socket survives a long silent calculation as long as the
client's WebSocket stack answers them. Browsers do this automatically; a
hand-rolled client that does not will lose its connection — and by the rule
above, its sessions — in the middle of the Monte Carlo it was waiting for.

### 9.5 What the gateway tracks that nothing else does

- **Outstanding `request_id`s, per session.** `DisruptionSink` reports a
  killed session and nothing more, because the supervisor deliberately does not
  know request ids (`supervisor.hpp`). Failing what was in flight with
  `WORKER_DIED`, or `CANCELLED` for a cancel target, is only possible from the
  gateway's own book.
- **`session_id` assignment.** Server-minted and opaque; a client-chosen id
  would let two connections collide on one graph.
- **A route for frames that have no session.** Delivery is keyed on
  session -> connection, so a rejection naming a session that was never opened,
  or one belonging to somebody else's connection, has no route and would never
  be sent — leaving the client waiting forever for the terminal frame every
  request is promised. The gateway therefore falls back to the socket the
  request arrived on, which the single-threaded loop makes unambiguous.
- **The terminal frame for a `CancelRequest` itself.** Nothing downstream
  produces one: the supervisor acts on the target request and never forwards
  the cancel frame to a worker, so the cancel's own `request_id` is
  acknowledged here or not at all.
- **Which terminal frames to report back.** `onRequestTerminated()` must see
  every terminal frame that came *from a worker* — it is how the `SessionLog`
  advances past `OpenSession` and how a polite cancel is told from one that
  needs a kill (§2.1). It must **not** see the frames the supervisor emitted
  itself, through `FrameSink`, on the kill and replay-failure paths: those are
  outcomes it already knows, and feeding them back is a re-entrant call into a
  class with no re-entrancy.

- **How much one client is holding.** Connections open, and sessions per
  connection. The supervisor packs sessions onto workers and has no idea which
  socket asked for them; the count that has to be capped is per client, so it
  can only be kept here (§9.6).

The `SessionLog`s themselves are not gateway members: they live in
`Supervisor::SessionState`, in the gateway's process and on its behalf. The
failure domain in §1.1 is drawn around the process, so the argument there is
unaffected either way.

### 9.6 The door, in place of authentication

Binding to loopback was taken as the boundary, and against a browser it is not
one. **A WebSocket upgrade is not subject to the same-origin policy**: any page
in any tab can open `ws://127.0.0.1:9111` and drive this service. Nothing here
is worth stealing and a great deal is worth spending — one `PriceBatch` or one
grid can commit a hundred thousand engine calls on somebody else's cores.

**Decision: check `Origin` when it is present, and let it through when it is
not.** Only browsers send the header, so the check closes exactly the path that
was open and leaves `test/smoke_v2.py`, a CLI and a proxy that has already
authenticated untouched. A present-and-wrong origin is refused at the upgrade
with `403` and a reason, rather than accepted and dropped, because a client that
is refused should be able to say so — the frontend asks `/healthz` on a failed
connect for precisely this and can then tell "nothing there" from "there and
refusing you". The defaults are the frontend's dev and preview origins;
`--allow-origin` replaces them and `--any-origin` disables the check for a
deployment that terminates it upstream.

This is not authentication and does not pretend to be. It is a bet that the
attacker is a page rather than a process, which is the correct bet for a service
on a developer's machine and the wrong one anywhere else — where the answer
stays a reverse proxy that terminates TLS, authenticates, and leaves this
process on loopback behind it.

**Decision: an optional shared secret, presented at the upgrade.**
`--token-file FILE` is what makes that bet unnecessary on a machine with other
users on it, and it is the one thing a proxy cannot supply: a proxy stands
beside this process rather than in front of its loopback socket, so anything
local still dials the port directly. Three choices inside it are the whole
design:

- **A file, not a flag.** Arguments are world-readable through `ps`, so
  `--token abc` would publish the secret to exactly the local processes it
  exists to exclude. The file is read when it exists and minted at `0600` when
  it does not, and it is refused when anyone but its owner can read it —
  a secret that is not is not one.
- **At the upgrade, not in a frame.** A refusal costs no connection slot, no
  session and no worker seat, which is why `Hello` was not given a token field.
  A wrong token and a missing one get the same `401`, for the reason a refused
  resume gets one answer (§9.4); the log distinguishes them.
- **Two places to put it, because a browser has only one.** Anything that can
  set a header sends `Authorization: Bearer`. A browser cannot — `new
  WebSocket` takes a URL and a list of subprotocols and nothing else (RFC 6455
  §11.3.4) — so it sends `token.<secret>` as a subprotocol beside
  `qlservice.v2`. A server must then select exactly one protocol or the browser
  fails the handshake, so the offered list is parsed and answered rather than
  echoed. The third option would be the query string, and it is the worst:
  a URL reaches logs, history and referrers, and a secret in one is written
  down in several places nobody is guarding.

What this cannot do belongs beside what it can. The protection is bounded by
who can read the token, so it keeps out another user's process and not one
running as the user who owns the file — which is an operating-system boundary
and not something this gateway can supply.

**Decision: refuse to bind anywhere but loopback without one.** Fail closed.
Everything above is a door against a page and none of it is a boundary against
the network, so a routable `--host` with no token is refused at startup with
exit code 2 and both ways out named. It is the accident far more than the
decision: nothing else here would have said a word before answering the office.

**Decision: cap connections per process and sessions per connection.** The other
half of having no authentication is that nothing else stops one client taking
everything: 32 sockets, refused at the upgrade with `503`, and 16 sessions per
socket, refused with `OVERLOADED`. The session limit is the one that matters,
because a session is a worker seat holding a live graph (§2.1) while a socket is
almost free — and it is per connection rather than per process so that one
client cannot starve the others. Both are refusals rather than failures: close a
session and the next one opens, which is why `OVERLOADED` is its own error code
and the frontend's remedy for it is "close a tab" rather than "retry".

**Liveness is not a frame.** `GET /healthz` answers over plain HTTP, because a
proxy or a container runtime cannot be asked to speak Protobuf over WebSocket to
find out whether to restart something. What a reply proves is that the loop is
turning — which is the honest scope, and the right one: everything that can go
wrong inside a graph happens on a worker thread and must not restart the
process. It carries the live connection and session counts, and deliberately no
`Access-Control-Allow-Origin`, so the tab that was just refused an upgrade
cannot read them either. Once a token is required the counts are left out
altogether: this endpoint cannot ask for one, since a container runtime holds
no secret and still has to decide whether to restart the process, so what an
unauthenticated caller may read is trimmed to liveness instead.
