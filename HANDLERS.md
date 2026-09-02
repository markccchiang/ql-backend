# Handlers

What the running service accepts today, and how to drive it.

`DESIGN.md` says *why* each of these is shaped the way it is and cites the
QuantLib constraint that forces it; this page is the flat list, derived from the
code rather than from the schema. The distinction matters: `proto/` describes
more than the service builds, and everything in the schema that is not here is
**rejected as `UNSUPPORTED` naming the field**, never priced on a substitute.
`README.md` has the orientation and `INSTALL.md` the build.

The wire schema itself is a submodule at [`proto/`](https://github.com/markccchiang/ql-protobuf).

## The five frames

One `ClientFrame` in, one *terminal* `ServerFrame` out. Always exactly one,
including for failures and for the cancel itself — a client that never sees a
terminal frame waits forever (DESIGN §9.5).

| `ClientFrame.payload` | Handled in | Terminal reply |
| --- | --- | --- |
| `open_session` | `Supervisor::dispatch` → `Worker::serve` | `SessionOpened` |
| `update_market` | `Worker::serve`, under one `UpdateGuard` | `Ack` |
| `price` | `Worker::serve` → `Session::price` | `PriceResult`, or `ScenarioResult` when `scenario` is set |
| `cancel` | `Gateway`, which answers it itself | `Ack` |
| `close_session` | `Gateway` / `Supervisor` | `Ack` |

`Progress` is the one non-terminal frame. It arrives only from a batched Monte
Carlo — `engine.mc.progress_every_paths` — and is also the only point at which a
running calculation can be stopped.

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
`FINE` (2000×800). `custom` — explicit `time_steps`, `asset_steps`,
`damping_steps` and `scheme` — works on the plain paths and is `UNSUPPORTED`
under quanto.

**Monte Carlo.** `seed` must be non-zero, because QuantLib otherwise seeds from
the clock and the same inputs would price differently on every request. Give
`samples` or `absolute_tolerance`, not both. Setting `progress_every_paths`
switches to the batched path, which is what emits `Progress` frames and what
makes a cancel possible — and it **changes the answer**, because batches draw
from the RNG stream differently from one run of the same total. Reproducibility
keys on `(seed, samples, progress_every_paths)`, which is why `PriceResult`
echoes the whole `Engine` message back.

## Results

`PriceResult.npv` always. `results` is a `map<string, Value>` keyed by the name
of each `ResultKind` you asked for in `PriceRequest.results`; sixteen are
mapped:

`NPV`, `DELTA`, `GAMMA`, `THETA`, `VEGA`, `RHO`, `DIVIDEND_RHO`,
`THETA_PER_DAY`, `DELTA_FORWARD`, `ELASTICITY`, `STRIKE_SENSITIVITY`,
`ITM_CASH_PROBABILITY`, `QRHO`, `QVEGA`, `QLAMBDA`, `FAIR_RATE`.

Anything else in the enum — the bond, credit and remaining cash-flow kinds — is
`UNSUPPORTED`. An engine that cannot supply a result you asked for is a **named
rejection, not a missing key**: a frontend that asked for vega and got a map
without it cannot tell that from a vega of zero.

`include_additional_results` returns whatever the engine published in its own
`additionalResults` map. Every value this layer reports is a **scalar** — the
vector and matrix arms of `Value` are not filled. `error_estimate` is populated
whenever the engine has one. `include_cashflows` and `curve_samples` are
`UNSUPPORTED`.

## Scenario sweeps

`PriceRequest.scenario` prices N times off one live graph and replies with a
`ScenarioResult` instead of a `PriceResult`. Points come three ways — exactly
one of:

| `points` | Meaning |
| --- | --- |
| `explicit` | the values themselves |
| `linear` | `begin`, `end`, `steps` (named `begin`/`end` because `from` is a Python keyword) |
| `relative` | multipliers of the quote's current value |

`plot` names the single `ResultKind` to shape into `ScenarioResult.series`;
leave it unset to get a full `PriceResult` per point and no series. A kind with
no single value there is `UNSUPPORTED` on `scenario.plot`.

The swept quote is **restored by default**. `keep_final_value` leaves it at the
last swept value — phrased that way round because proto3 defaults it to false
and the default has to be the safe one: a sweep is a question, not an edit. A
kept sweep is folded into the session log as a synthetic `UpdateMarket`, so it
survives a replay.

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
`ql-backend` over a real WebSocket: 209 rows of QuantLib's own reference values
plus the rejection cases, 73 checks in all. `test/README.md` explains how to run
it; `test/BENCHMARK.md` is the analytic-vs-PDE cross-check of the quanto
barriers.
