# Handlers

What the running service accepts today, and how to drive it.

`DESIGN.md` says *why* each of these is shaped the way it is and cites the
QuantLib constraint that forces it; this page is the flat list, derived from the
code rather than from the schema. The distinction matters: `proto/` describes
more than the service builds, and everything in the schema that is not here is
**rejected as `UNSUPPORTED` naming the field**, never priced on a substitute.
`README.md` has the orientation and `INSTALL.md` the build.

The wire schema itself is a submodule at [`proto/`](https://github.com/markccchiang/ql-protobuf).

## The eight frames

One `ClientFrame` in, one *terminal* `ServerFrame` out. Always exactly one,
including for failures and for the cancel itself — a client that never sees a
terminal frame waits forever (DESIGN §9.5).

| `ClientFrame.payload` | Handled in | Terminal reply |
| --- | --- | --- |
| `open_session` | `Supervisor::dispatch` → `Worker::serve` | `SessionOpened` |
| `update_market` | `Worker::serve`, under one `UpdateGuard` | `Ack` |
| `price` | `Worker::serve` → `Session::price` | `PriceResult`, or `ScenarioResult` when `scenarios` is non-empty |
| `batch` | `Worker::serveBatch` → `Session::price`, once per entry | `BatchResult` |
| `cancel` | `Gateway`, which answers it itself | `Ack` |
| `hello` | `Gateway`, which answers it itself | `Capabilities` |
| `close_session` | `Gateway` / `Supervisor` | `Ack` |
| `resume_session` | `Gateway`, which answers it itself | `SessionOpened` with `resumed` set |

There is one thing that is not a frame at all: `GET /healthz` answers over
plain HTTP, for a proxy or an orchestrator that cannot speak this protocol.
See [Liveness](#liveness).

`hello` needs no session: what the build can price is a property of the
service, and a client has to be able to ask before it opens one. The reply
carries **sets** — the styles, methods, trees, approximations, result kinds,
market shapes and leg kinds this build implements — and deliberately not the
combinations. Whether an analytic barrier takes an American exercise is a rule
about a pair, and there are more pairs than are worth putting on the wire; a
client keeps its own table for those. What the handshake removes is the drift
that actually happens, which is a value appearing in or disappearing from one
of these lists while every client's copy of this page says otherwise.
`src/session/capabilities.cpp` is where the lists live, next to the dispatch
they describe.

`Progress` is the one non-terminal frame. Three shapes emit it: a batched Monte
Carlo (`engine.mc.progress_every_paths`), a scenario sweep, and a batch. Those
are also the three that can be stopped where they stand — see
[Cancellation](#cancellation), which is not the one-line story this sentence
used to tell.

Every frame carries `request_id` (yours, echoed on every reply) and
`session_id` (set on every server frame, because one socket can hold several
sessions).

## A session, end to end

The wire is Protobuf over WebSocket. This is the idiom `test/smoke_v2.py` uses:

```python
from quantlib.v1 import conventions_pb2 as C
from quantlib.v2 import engine_pb2 as EN, envelope_pb2 as E
from quantlib.v2 import instrument_pb2 as I, market_pb2 as M

def act360(msg):
    msg.family = C.DayCounter.ACTUAL_360

# 1. Open a session: the evaluation date, then the market in dependency order.
f = E.ClientFrame(request_id=1)
o = f.open_session
o.evaluation_date.iso = "2026-09-01"

m = o.market.add(); m.id = "S"                       # spot
m.quote.value = 100.0; m.quote.unit = M.Quote.UNIT_ABSOLUTE
for qid, v in (("R", 0.05), ("Q", 0.02), ("V", 0.20)):
    m = o.market.add(); m.id = qid
    m.quote.value = v; m.quote.unit = M.Quote.UNIT_RATE

for cid, qid in (("RC", "R"), ("QC", "Q")):          # flat curves on live quotes
    m = o.market.add(); m.id = cid
    act360(m.yield_curve.day_counter)
    m.yield_curve.flat.rate.quote_id = qid
    m.yield_curve.flat.compounding = C.CONTINUOUS
    m.yield_curve.flat.frequency = C.ANNUAL

m = o.market.add(); m.id = "VOL"
act360(m.volatility.day_counter)
m.volatility.constant.volatility.quote_id = "V"
# -> SessionOpened.session_id

# 2. Move the graph. One UpdateGuard covers the whole batch.
f = E.ClientFrame(request_id=2, session_id=sid)
f.update_market.quotes.add(quote_id="S", value=105.0)
# -> Ack

# 3. Price off it: payoff x exercise x underlying x style.
f = E.ClientFrame(request_id=3, session_id=sid)
opt = f.price.instrument.option
u = opt.underlyings.add()
u.spot_quote_id, u.volatility_id = "S", "VOL"
u.discount_curve_id, u.dividend_curve_id = "RC", "QC"
u.process = I.Underlying.PROCESS_BLACK_SCHOLES_MERTON
opt.payoff.type = I.Payoff.OPTION_TYPE_CALL
opt.payoff.plain.strike = 100.0
opt.exercise.type = I.Exercise.TYPE_EUROPEAN
opt.exercise.dates.add().iso = "2027-09-01"
opt.vanilla.SetInParent()
f.price.engine.method = EN.Engine.METHOD_ANALYTIC
# -> PriceResult.npv = 12.459717
```

The session holds the live QuantLib graph. `update_market` writes quotes and
reprices off the same objects; anything that changes graph *structure* — a new
curve shape, a new index — is a new session.

## Market objects

`OpenSession.market` is an ordered, id-addressed namespace. Ids are unique
within the session and are what instruments name.

| `MarketObject.kind` | Built |
| --- | --- |
| `quote` | yes — the concrete `SimpleQuote`s everything else observes |
| `yield_curve` | yes, four of six shapes |
| `volatility` | yes, three of four shapes |
| `index` | yes, two of six families |
| `fixings` | yes |
| `default_curve`, `inflation_curve`, `correlation` | no — `UNSUPPORTED` |

**Objects must arrive in dependency order**, because the session resolves ids
against its maps as it fills. One exception: `Index.forwarding_curve_id` may
name a curve defined *later*, since a bootstrapped curve's pillars name the
index and the index names the curve. The index is built on a
`RelinkableHandle` and linked when its curve appears; one that never appears is
`UNKNOWN_ID` against `Index.forwarding_curve_id`.

### Yield curves

| `YieldCurve.shape` | Live pillars? |
| --- | --- |
| `flat` | **yes** — `rate` may be a `quote_id` |
| `bootstrap` | **yes** — helper quotes are live |
| `zero` | no — nodes must be `Number.fixed` |
| `discount` | no — nodes must be `Number.fixed` |
| `forward`, `spread` | not built — `UNSUPPORTED` |

A `quote_id` on an interpolated node is rejected rather than accepted and never
observed: QuantLib's interpolated curves copy their nodes at construction. The
first node must also equal the session's evaluation date, because
`InterpolatedZeroCurve` takes `dates[0]` as its reference.

`bootstrap` is an explicit instantiation table — three traits × three
interpolators, nine compiled types:

| `traits` | `interpolator` |
| --- | --- |
| `DISCOUNT`, `ZERO_YIELD`, `FORWARD_RATE` | `LINEAR`, `LOG_LINEAR`, `CUBIC` |

Adding a pair is a line of source and a recompile (DESIGN §6.1). Deposit
helpers take their tenor from the pillar and their conventions from the named
index — one index can therefore back pillars of several tenors.

### Volatility

| `VolatilitySurface.shape` | Live? |
| --- | --- |
| `constant` | **yes** — `volatility` may be a `quote_id` |
| `variance_curve` | built, frozen |
| `variance_surface` | built, frozen (wire is row-major expiries × strikes; transposed on the way in) |
| `local` | not built — `UNSUPPORTED` |

### Indices and fixings

`FAMILY_IBOR` and `FAMILY_OVERNIGHT` build. `SWAP`, `INFLATION_ZERO` and
`INFLATION_YOY` are `UNSUPPORTED`. `name` is required — the service builds the
index from the conventions you send rather than looking it up in a table of
hardcoded indices, because that table is what goes stale.

`FixingSeries` may arrive in `OpenSession` or in `UpdateMarket`: past fixings
are graph input, not graph structure, so a leg that cannot price without one
does not need a new session to get it.

## Instruments

Two of the nine `Instrument.kind` arms are dispatched: `option` and `swap`.
The rest — `swaption`, `cap_floor`, `bond`, `credit_default_swap`, `fra`,
`fx_forward`, `variance_swap` — are `UNSUPPORTED`.

### Option

An option is **payoff × exercise × underlying × style**, with quanto orthogonal
to all four. Six of twelve styles are built:

| `Option.style` | Exercise | Engine methods | Quanto |
| --- | --- | --- | --- |
| `vanilla` | European, American, Bermudan | `ANALYTIC`, `LATTICE`, `FINITE_DIFFERENCE`; `INTEGRAL` and `MONTE_CARLO` European only | `ANALYTIC` or `FINITE_DIFFERENCE`, European |
| `barrier` | European; American via lattice or FD | `ANALYTIC` (European only), `LATTICE`, `FINITE_DIFFERENCE`, `MONTE_CARLO` | `ANALYTIC` or `FINITE_DIFFERENCE`, European |
| `double_barrier` | European | `ANALYTIC` | yes, `ANALYTIC` |
| `forward_start` | European | `ANALYTIC` | yes |
| `asian` | European | `ANALYTIC` (geometric), `MONTE_CARLO` (arithmetic) | no — QuantLib has no quanto Asian engine |
| `lookback` | European | `ANALYTIC` | no |
| `cliquet`, `digital`, `compound`, `chooser`, `basket`, `spread` | — | not built — `UNSUPPORTED` | |

Style-specific rules worth knowing before you send one:

- **Barrier.** `ANALYTIC` is European only; an American barrier goes to
  `LATTICE` or `FINITE_DIFFERENCE`. The barrier lattice is
  **Cox-Ross-Rubinstein only** (with the Derman-Kani correction) — the engine
  takes a second template argument for the discretisation, so a full menu would
  be trees × discretisations. `monitoring_dates` and `window_start` are
  `UNSUPPORTED`: discrete and partial-time barriers are not built.
- **Double barrier.** Needs `0 < lower < upper`. `ANALYTIC` only — QuantLib's
  sole FD double-barrier engine is Heston, which takes a calibrated model rather
  than a process.
- **Forward start.** Send no strike: it is `moneyness` × the spot at `reset`,
  filled in by the forward engine, and a struck payoff would be silently
  overwritten. `performance` is a `Flag` — it pays the return rather than the
  amount, a different price for the same trade description.
- **Asian.** No `fixing_dates` means continuously averaged, which has a closed
  form for the **geometric** average only. With fixing dates, geometric goes to
  `ANALYTIC` and arithmetic to `MONTE_CARLO`.
- **Lookback.** Continuous only; `window_start` is `UNSUPPORTED`.
  `running_extremum` is required and must be positive — an option already
  running whose extremum is dropped prices as if it had just started. A
  `floating` payoff selects the floating-strike instrument.
- **Vanilla.** A binary payoff on an American exercise is a one-touch and goes
  to `AnalyticDigitalAmericanEngine`. An American `ANALYTIC` price **must** name
  an approximation (below).

**Payoffs.** Seven build: `plain`, `percentage_strike`, `asset_or_nothing`,
`cash_or_nothing`, `gap`, `super_fund`, `super_share`. `floating` is valid on a
lookback only.

**Exercise.** `EUROPEAN`, `AMERICAN` and `BERMUDAN` all build.
`Exercise.dates` carries one date for European and American and every exercise
date for Bermudan. `payoff_at_expiry` is a `Flag` read on American and Bermudan
only, where it must be set explicitly.

**Underlying.** `PROCESS_BLACK_SCHOLES_MERTON` (the default),
`PROCESS_BLACK_SCHOLES` and `PROCESS_BLACK` build; `GARMAN_KOHLHAGEN`,
`HESTON`, `BATES` and `LOCAL_VOL` are `UNSUPPORTED`. `PROCESS_BLACK_SCHOLES`
rejects a `dividend_curve_id` rather than ignoring it. **Omitting
`dividend_curve_id` means a flat zero dividend yield**, not the risk-free curve.
Exactly one underlying — a second is `INVALID_ARGUMENT` on
`instrument.option.underlyings`.

**Quanto** needs all three of `fx_risk_free_curve_id`, `fx_volatility_id` and
`correlation_id`, and is not a product: it wraps the *engine*, so it composes
over any style whose engine is constructible from a process alone. That is also
why a quanto FD path takes only an `engine.fd.preset` — `QuantoEngine` builds
its inner engine as `make_shared<Engine>(process)` and leaves no seam for grid
sizes, so `engine.fd.custom` is `UNSUPPORTED` there.

### Swap

A general n-leg `Swap`. Two of the leg kinds build — `KIND_FIXED` and
`KIND_IBOR` — and `engine.method` must be `METHOD_DISCOUNTING`.

- `pays` is a `Flag` on every leg and must be set.
- `RESULT_KIND_FAIR_RATE` requires exactly two legs, **fixed first, then Ibor**;
  any other arrangement is `UNSUPPORTED` on `instrument.swap.legs` rather than a
  fair rate computed off the wrong leg.
- Per-leg `caps`, `floors`, `discount_curve_id` and `currency` are
  `UNSUPPORTED`.
- **A fixed leg freezes its rate.** `FixedRateLeg` takes a value rather than a
  handle, so `rate_quote_id` is read once at construction; bumping that quote
  needs a new `PriceRequest`, not an `UpdateMarket`. It is the one place the
  Handle discipline of DESIGN §5 is knowingly not kept.

## Engines

`method` selects the parameter block. A field that does not apply cannot be
set, rather than being set and dropped.

| `Engine.Method` | Parameters | Notes |
| --- | --- | --- |
| `METHOD_ANALYTIC` | `analytic` | `approximation` required for an American vanilla |
| `METHOD_LATTICE` | `lattice` | `tree` and non-zero `steps` both required |
| `METHOD_FINITE_DIFFERENCE` | `fd` | `preset` or `custom` |
| `METHOD_MONTE_CARLO` | `mc` | non-zero `seed` required |
| `METHOD_INTEGRAL` | — | European vanilla only |
| `METHOD_DISCOUNTING` | — | swaps |
| `METHOD_FOURIER` | `fourier` | not built — the models it exists for are not built |

`Engine.model` is carried but only `MODEL_BLACK_SCHOLES` is reachable, since
Heston, Bates and local vol are not built.

**American approximations.** QuantLib has three and they disagree in the third
decimal, so the client names one rather than inheriting a default:
`BARONE_ADESI_WHALEY`, `BJERKSUND_STENSLAND`, `JU_QUADRATIC`. Omitting it is
`UNSPECIFIED_ENUM` on `engine.analytic.approximation`.

**Lattice trees.** Seven compile for a vanilla — `COX_ROSS_RUBINSTEIN`,
`JARROW_RUDD`, `ADDITIVE_EQUIPROBABILITIES`, `TRIGEORGIS`, `TIAN`,
`LEISEN_REIMER`, `JOSHI4`. A barrier takes `COX_ROSS_RUBINSTEIN` only.

**Finite difference.** `preset` is `COARSE` (100×100), `STANDARD` (400×200) or
`FINE` (2000×800), each of them Douglas with no damping. `custom` — explicit
`time_steps`, `asset_steps`, `damping_steps` and `scheme` — works on the plain
paths and is `UNSUPPORTED` under quanto. All four fields are read: a custom
grid that named a scheme and got Douglas anyway was a defect, not a
simplification, and it stood for three milestones.

`scheme` has no default, for the reason the grid has none: two schemes are two
prices for one trade. Five of the six arms build.

| `Scheme` | What it is here |
| --- | --- |
| `DOUGLAS` | QuantLib's own default, second order. What to send with no opinion |
| `CRANK_NICOLSON` | Douglas to within a bit in one dimension |
| `CRAIG_SNEYD` | Douglas *exactly* in one dimension — there are no directions to alternate |
| `HUNDSDORFER` | A different theta; differs from Douglas in the seventh digit |
| `IMPLICIT_EULER` | First order, unconditionally stable; differs in the third digit |
| `EXPLICIT_EULER` | `UNSUPPORTED` |

Explicit Euler is refused rather than offered because it is stable only while
the time step is small against the square of the asset step, and the asset step
belongs to a mesher QuantLib builds inside the engine rather than to anything
on the frame. An unstable run does not fail: at 100×200 this build answered
2.4e140 and at 400×200 a NaN. Implicit Euler is first order too, with no such
condition.

`damping_steps` are Rannacher's fix for the oscillation a Crank-Nicolson-family
scheme shows against a kinked payoff or a barrier — the first few steps taken
fully implicit. They are counted out of `time_steps`, so a request with at
least as many damping steps as time steps is `INVALID_ARGUMENT`.

**Monte Carlo.** `seed` must be non-zero, because QuantLib otherwise seeds from
the clock and the same inputs would price differently on every request. Give
`samples` or `absolute_tolerance`, not both. Setting `progress_every_paths`
switches to the batched path, which is what emits `Progress` frames and what
lets a cancel stop the work rather than only the waiting — and it **changes the
answer**, because batches draw
from the RNG stream differently from one run of the same total. Reproducibility
keys on `(seed, samples, progress_every_paths)`, which is why `PriceResult`
echoes the whole `Engine` message back.

## Results

`PriceResult.npv` always. `results` is a `map<string, Value>` keyed by the name
of each `ResultKind` you asked for in `PriceRequest.results`; nineteen are
mapped:

`NPV`, `DELTA`, `GAMMA`, `THETA`, `VEGA`, `RHO`, `DIVIDEND_RHO`,
`THETA_PER_DAY`, `DELTA_FORWARD`, `ELASTICITY`, `STRIKE_SENSITIVITY`,
`ITM_CASH_PROBABILITY`, `IMPLIED_VOLATILITY`, `QRHO`, `QVEGA`, `QLAMBDA`,
`FAIR_RATE`, `LEG_NPV`, `LEG_BPS`.

`IMPLIED_VOLATILITY` is the one that takes an input of its own. It is a root
find, not a published result, so `PriceRequest.implied_volatility` carries the
price to invert and, optionally, the bracket and tolerance to look in; without
a target the request is `INVALID_ARGUMENT`, because inverting the price the
request is about to compute would hand back the volatility the client sent.
QuantLib can invert a vanilla, a barrier and a double barrier; asked of any
other style the kind comes back in `unavailable_results` like any other result
an engine cannot supply.

Anything else in the enum — the bond, credit and remaining cash-flow kinds — is
`UNSUPPORTED`.

An engine that cannot supply a result you asked for **names the absence**: the
kind comes back in `PriceResult.unavailable_results` and the price comes back
with it. This page used to promise a rejection instead, and the code never did
it; the document was right about the problem and wrong about the remedy. A
frontend that asked for vega and got a map without it cannot tell that from a
vega of zero — but refusing the whole request would cost the price as well, and
a client that wanted the NPV would learn to ask for nothing. `AnalyticEuropean-
Engine` has vega, the binomial one does not, and a frontend should be able to
ask both the same question and be told which answered.

The same field carries a kind that does not apply to the instrument at all —
a fair rate asked of an option, a greek asked of a swap. A kind this build does
not implement anywhere is a different matter, and `Hello` answers that before
the request is sent.

`include_additional_results` returns whatever the engine published in its own
`additionalResults` map. Every value this layer reports is a **scalar** — the
vector and matrix arms of `Value` are not filled. `error_estimate` is populated
whenever the engine has one.

`include_cashflows` is served for the cash-flow instruments, which today means
swaps. Each row carries its payment date, amount, the discount the engine used
and their product, so the **sum of the present-value column is the NPV** — that
property is what makes the table worth showing rather than decorating, and
`test/smoke_v2.py` checks it. Coupons add their accrual dates, notional and
rate; floating coupons add the fixing date, the spread and the gearing, and
say whether the fixing came from `IndexManager` or is still a forecast. Rows
that have already been paid are left out. Asked of an **option** it is
`UNSUPPORTED`: an empty table would read as an instrument that happens to have
no cash flows rather than one that was never going to have any.

`curve_samples` is served. Each entry names a market object and a quantity and
comes back as a `Series` on the result, sampled from the very handle the engine
priced against — which is the point of it: the alternative is shipping the term
structure and re-implementing QuantLib's interpolation in the client, which is
how a frontend ends up drawing a curve the backend did not price with. Four
quantities are built:

| `CurveSample.Quantity` | On | Needs |
| --- | --- | --- |
| `ZERO_RATE` | a yield curve | `compounding`, `frequency`, and `day_counter` when sampling by date |
| `FORWARD_RATE` | a yield curve | the same |
| `DISCOUNT_FACTOR` | a yield curve | nothing; a `compounding` set beside one is rejected rather than ignored |
| `BLACK_VOLATILITY` | a volatility surface | exactly one `strikes` entry |

`dates` or `times`, one of the two. The other four quantities in the enum are
`UNSUPPORTED` naming the field: `SURVIVAL_PROBABILITY`, `HAZARD_RATE` and
`LOCAL_VOLATILITY` need term structures this build does not construct, and
`VARIANCE` is the same surface `BLACK_VOLATILITY` samples, reported twice.
Sampling a surface across several strikes is refused for a different reason —
it would be a matrix rather than a series.

## Batches

`PriceBatch` prices a book of trades against one graph in one frame, and
`BatchResult` answers one entry per request **in order**, so a client matches
by position and needs no ids of its own.

A failing entry does not fail the batch. Refusing the whole book because trade
seventeen names a curve that is not there would throw away sixteen prices that
were computed correctly — the same argument that made an unsupplied result a
named absence rather than a rejection. An entry carries either a `PriceResult`
or the `Error` it would have been sent on its own, with `field_path` prefixed by
`batch.requests[n]` so a blotter can put the complaint on the row it belongs to.

Three things are refused rather than absorbed:

| Refusal | Where it lands | Why |
| --- | --- | --- |
| an empty `requests` | the whole frame, on `batch.requests` | The request itself is wrong, not one of its rows |
| a `scenarios` sweep inside an entry | that entry, `UNSUPPORTED` | Both shapes mean "price this many times", and nesting them is a product one `completed`/`total` pair cannot describe |
| a failure that dirties the graph | the rest of the book | Every later price would be computed against a half-invalidated graph |

`BatchResult.abandoned_after` is how many entries were attempted before a dirty
graph or a cancel stopped the book, and zero when the whole of it priced. A
count rather than an index, so "the first entry broke it" is 1 rather than
indistinguishable from the ordinary case. The abandoned entries are still
present, carrying the reason rather than a price.

A batch reports `Progress` per entry and checks the stop flag between them, so
it is cancellable at trade boundaries in exactly the way a sweep is cancellable
at point boundaries (see [Cancellation](#cancellation)). It also takes the placement decision over the whole book: a
Monte Carlo eleven trades in moves the session to a sacrificial worker before
the batch starts, because the batch runs to completion wherever it begins.

## Scenario sweeps

`PriceRequest.scenarios` prices N times off one live graph and replies with a
`ScenarioResult` instead of a `PriceResult`. Points come three ways — exactly
one of:

| `points` | Meaning |
| --- | --- |
| `explicit` | the values themselves |
| `linear` | `begin`, `end`, `steps` (named `begin`/`end` because `from` is a Python keyword) |
| `relative` | multipliers of the quote's current value |

### More than one axis

`scenarios` is repeated, and several axes sweep **as a product**: spot at 21
points against vol at 5 is 105 prices, in row-major order with the last axis
varying fastest. `ScenarioResult.axes` names them outermost first, so
`prices[i * len(axes[1].values) + j]` is `axes[0].values[i]` against
`axes[1].values[j]`.

The rules a grid adds, each rejected by its own `scenarios[n]` path:

| Rule | Why |
| --- | --- |
| A quote may appear on one axis only | The later write would win at every point and the earlier axis would move nothing, which reads as a flat dimension rather than as a mistake |
| Only `scenarios[0]` may set `plot` | The plot is a property of the sweep. Taking it from whichever axis happened to set one would draw something the client did not ask for |
| The product must fit `Capabilities.max_scenario_points` | A grid multiplies, so a step count one digit too long is a session-length request rather than a slow one. The ceiling is advertised so a client can refuse it before spending the round trip |

What this is *not* is a lockstep shift — move these three quotes together.
That is a market edit with an undo, which `UpdateMarket` already does.

`plot` names the single `ResultKind` to shape for drawing; leave it unset to get
a full `PriceResult` per point and no plot. One axis fills
`ScenarioResult.series`, two fill `ScenarioResult.surface` — a `DoubleMatrix`
whose `row_labels` and `column_labels` are the axis values, so nothing else has
to be sent to place a cell — and beyond two axes neither is filled and `prices`
is the answer. A kind with no single value per point is `UNSUPPORTED` on
`scenarios[0].plot`.

`ScenarioResult.abandoned_after` is how many points were priced before a cancel
stopped the sweep, and zero when the whole ladder ran. The axes are trimmed to
what actually priced, so the series and the values still line up; a *grid*
stopped mid-row has no rectangle to report, so its axes are cleared and `prices`
is the whole of the answer.

Every swept quote is **restored by default**, on the way out of a failure as
well as a success. `keep_final_value` is per axis — a grid can leave spot where
it ended and put vol back — and leaves that quote at its last swept value — phrased that way round because proto3 defaults it to false
and the default has to be the safe one: a sweep is a question, not an edit. A
kept sweep is folded into the session log as a synthetic `UpdateMarket`, so it
survives a replay.

## Liveness

```
GET /healthz -> 200 application/json
{"status":"ok","build":"ql-backend","quantlib":"1.43",
 "uptimeSeconds":142,"connections":3,"sessions":7,"maxConnections":32}
```

What this answer proves is that **the loop is turning**. The gateway is
single-threaded and everything below it runs on worker threads, so a reply here
means frames are being served and says nothing about whether any particular
graph is healthy. That is the honest scope of a liveness check, and it is the
one an orchestrator wants: restarting on it is right, and it will not restart
the process because a client sent a bad trade.

Before this the socket connecting was the only liveness signal, which a proxy
or a container runtime cannot use — it would have to speak WebSocket and
Protobuf to find out whether to restart something.

The counts are the live ones rather than a fixed string, which is what makes
them worth reading: `connections` and `sessions` are what the gateway is
actually holding, and `maxConnections` is what it will hold before refusing.
There is deliberately **no** `Access-Control-Allow-Origin`. A browser may send
this request from any page; without the header it cannot read the reply, and
these numbers are not something a random tab should be able to poll.

## The door

There is no authentication, and this section is about what stands in for it.

**Origin.** A WebSocket upgrade is not subject to the same-origin policy, so
binding to loopback is not a boundary against a *browser*: any page in any tab
can open `ws://127.0.0.1:9111` and drive this service. There is nothing here to
steal and a great deal to spend — one frame can commit a hundred thousand
engine calls. So an `Origin` header that is **present** must be on the allowed
list, and one that is **absent** is let through: only browsers send it, which
means the check closes the browser path and leaves `test/smoke_v2.py`, any CLI
and any proxy that has already checked exactly as they were.

```
ql-backend --allow-origin https://desk.internal   # replaces the defaults
ql-backend --any-origin                           # behind a proxy that checks
```

The defaults are the development server's origins (`5173`, `4173` on both
`localhost` and `127.0.0.1`). A refused upgrade is `403 Forbidden` with a
reason, not a dropped connection.

**Limits.** The other half of having no authentication is that nothing stops
one client taking everything:

| Limit | Default | Refused with | Why this is the unit |
| --- | --- | --- | --- |
| sockets served at once | 32 | `503` at the upgrade | A refused connection is a better failure than a gateway that cannot accept the one that matters |
| sessions per socket | 16 | `OVERLOADED` | A session is a live QuantLib graph on a worker seat, so this is what protects the pool rather than the socket |
| frame size | 4 MB | closed by the transport | An `OpenSession` with a few hundred pillars exceeds uWebSockets' 16 KB default |
| points per sweep | 100,000 | `INVALID_ARGUMENT` | A grid multiplies; see [Scenario sweeps](#scenario-sweeps) |

`--max-connections` and `--max-sessions` move the first two. Both are refusals
rather than breakages: close a session and the next one opens.

**What is deliberately not here.** TLS, users, tokens. If this is ever served
off the machine it runs on, a reverse proxy terminates TLS, authenticates and
checks origin, and `ql-backend` goes on binding to loopback behind it. A
pricing engine that grew its own TLS stack would be a worse pricing engine and
a worse edge server.

## Cancellation

A `CancelRequest` names one `request_id`. The cancel itself gets its own `Ack`
from the gateway, because nothing downstream answers it, and the target gets
exactly one terminal frame like every other request.

**Every request can be cancelled.** What differs is what the cancel costs, and
that is worth knowing before offering the button.

| Where the request is | What a cancel does | What it costs |
| --- | --- | --- |
| between Monte Carlo batches (`progress_every_paths > 0`) | the engine loop sees the stop flag and returns | nothing: the worker is healthy, the graph is still warm |
| between scenario sweep points | the sweep stops and returns the points it priced | nothing, and the partial ladder is kept |
| between batch entries | the book stops and returns the prices it managed | nothing, and the partial book is kept |
| anywhere else — inside one engine call | the request is terminated for the client after a 250 ms grace and the session is replayed into a fresh worker | one bootstrap, and the abandoned calculation runs to completion on a thread nobody is listening to |

That last row is the one to be honest about. QuantLib cannot be interrupted
inside an engine call, and this build hosts workers as threads, so the "kill"
in `Supervisor::onStopGraceExpired` is a disown rather than a kill:
`ThreadProcessHost::kill` asks the worker to stop, marks its seat dead and
detaches it. The client is freed in 250 ms and the session survives, but the
CPU is not given back until that engine call ends on its own.

So a cancel is always worth offering, and a UI that says "cancel" on a
finite-difference price is not lying — it is promising to give the user their
session back, not to stop the machine. The three boundary rows are the ones
where it also stops the work.

A stop taken at a boundary reports `CANCELLED` rather than `CALCULATION_FAILED`:
the client asked for it, and telling a user their trade failed to price would be
a different and wrong statement. A sweep and a batch each terminate with their
own result message carrying `abandoned_after`, not with an error, because the
part they finished is worth having.

## Resuming a session

A socket that dies takes nothing with it for `resume_grace_seconds` — 60 by
default, `--session-grace 0` to turn it off. Inside that window the session
keeps its graph, its worker seat **and its running requests**, which is the
point of it: the bootstrap it saves costs 0.009 ms on this market, and the
Monte Carlo it saves can cost a minute.

`SessionOpened` carries what a resume needs:

| Field | What it is |
| --- | --- |
| `resume_token` | 128 bits, minted per session, empty when the window is off |
| `resume_grace_seconds` | How long the session outlives its socket |
| `resumed` | True when this frame answers a `ResumeSession` rather than an `OpenSession` |

`ResumeSession{session_id, resume_token}` needs no session of its own —
the socket it arrives on has none yet. The reply is the *original*
`SessionOpened`, replayed with `resumed` set: the same id, the same
`market_ids`, and the `bootstrap_seconds` that bootstrap actually cost, then.
A resume does not build anything, and reporting a second bootstrap that never
happened would make the field a lie.

Then whatever finished while nobody was attached arrives, in the order it
finished. `Progress` frames from that period are gone — shed for the reason
§9.3 sheds them under backpressure — so a client that resumes into a running
calculation sees progress resume mid-stream, and one that resumes after it
finished gets the terminal frame straight away.

Three refusals, one answer. A wrong token, an expired window and a session
closed with `CloseSession` all come back `SESSION_NOT_FOUND`, because telling
them apart would let a guess be a probe. The client's move is the same in every
case: `OpenSession` and replay the market.

The token is a bearer secret. A WebSocket upgrade is not subject to the
same-origin policy (DESIGN §9.6), so any page on the machine can reach this
socket; the token is the only thing that stops one adopting another's session.
Hold it in memory, keep it out of logs and out of URLs.

Held sessions are seats nobody is sitting in, so there is a cap on them — 16 by
default. Past it the longest-waiting session is closed rather than the newest
refused. `GET /healthz` reports the count as `detached`.

## Errors

Every rejection carries a `Code` and, where it is attributable to a wire field,
a dotted proto path — `"instrument.vanilla_swap.fixed_leg.frequency"`.

| Code | What it means for the client |
| --- | --- |
| `INVALID_ARGUMENT` | the field is set to something that cannot work — fix the value |
| `UNSPECIFIED_ENUM` | a `*_UNSPECIFIED` enum reached the registry — fill the field in |
| `UNKNOWN_ID` | a quote, curve or index id that is not in the session |
| `UNSUPPORTED` | the schema expresses it; this build does not price it |
| `SESSION_NOT_FOUND` | the session is gone — open a new one and replay |
| `BOOTSTRAP_FAILED` | the curve did not bootstrap |
| `CALCULATION_FAILED` | the maths failed; no field, because it is not a field's fault |
| `CANCELLED` | terminal frame for a request you cancelled |
| `WORKER_DIED`, `OVERLOADED`, `INTERNAL` | infrastructure; retry or back off |

The three that a frontend must tell apart are `UNSPECIFIED_ENUM` (you left a
field out), `INVALID_ARGUMENT` (you filled it in wrongly) and `UNSUPPORTED`
(the request is well-formed and this build refuses to price it on a
substitute).

## Verification

Every handler above is exercised by `test/smoke_v2.py`, which drives a running
`ql-backend` over a real WebSocket: 247 rows of QuantLib's own reference values
plus the rejection cases, 145 checks in all. `test/README.md` explains how to run
it; `test/BENCHMARK.md` is the analytic-vs-PDE cross-check of the quanto
barriers.
