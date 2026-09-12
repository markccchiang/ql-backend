# Client recipes

Short answers to the things a client has to do after its first price. Each
one is a fragment of the script on the previous page; the numbers are what
this build actually returned.

## Ask what the build supports, do not hardcode it

`Hello` answers with sets — styles, payoffs, exercises, processes, engine
methods, trees, approximations, presets, result kinds, market shapes, leg
kinds — plus the ceilings `max_batch_entries`, `max_scenario_points` and
`max_curve_sample_points`. Drive the enable/disable state of a UI from that
answer and a build that gains or loses a style needs no client release.

What it deliberately does not carry is the *combinations*: whether an
analytic barrier takes an American exercise is a rule about a pair, and there
are more pairs than are worth putting on the wire. Keep those in the client,
and let the service's named refusal be the backstop.

## Bump a quote and reprice

This is what the service is for: the graph stays alive, so a what-if is a
quote write rather than a rebuild.

```python
f = E.ClientFrame(request_id=nid(), session_id=sid)
f.update_market.quotes.add(quote_id="S", value=105.0)
await roundtrip(ws, f)                 # -> Ack
```

```
npv @100    10.539466
npv @105    13.949624
```

One `UpdateMarket` may carry many quotes and they commit together, under one
notification batch. Anything that changes the *structure* of the graph — a
new curve shape, a new index — is a new session instead.

## Sweep a quote

A sweep prices N times off the one graph and answers with a `ScenarioResult`
rather than a `PriceResult`:

```python
f = E.ClientFrame(request_id=nid(), session_id=sid)
build_option(f.price)
axis = f.price.scenarios.add()
axis.quote_id = "S"
axis.linear.begin, axis.linear.end, axis.linear.steps = 90.0, 110.0, 5
axis.plot = R.RESULT_KIND_NPV
result = (await roundtrip(ws, f)).scenario_result
# result.series.y -> [5.1636, 7.5933, 10.5395, 13.9496, 17.7546]
```

`series.x` and `series.y` are the plot when one axis is swept and `plot` is
set; two axes fill `surface` instead; beyond two, `prices` is the answer.
**Every swept quote is restored afterwards** unless the axis sets
`keep_final_value`, on the way out of a failure as well as a success — a
sweep is a question, not an edit.

## Read a result an engine will not supply

Asking a binomial engine for vega gets you the price and a named absence,
never a silent zero:

```python
f.price.engine.method = EN.Engine.METHOD_LATTICE
f.price.engine.lattice.tree = EN.LatticeParameters.TREE_COX_ROSS_RUBINSTEIN
f.price.engine.lattice.steps = 200
f.price.results.append(R.RESULT_KIND_VEGA)
```

```
npv          13.954606
results      {}
unavailable  ['RESULT_KIND_VEGA']
```

So a client reads `unavailable_results` before it reads the map, and shows
"not supplied" rather than a zero nobody computed. `AnalyticEuropeanEngine`
answers the same request with a vega; that difference is the engine's, and
the point of naming it is that a frontend can ask both the same question.

## Handle an error

Every rejection carries a `Code` and, when it is attributable to a field, the
dotted path to it — including the index of a repeated entry:

```
UNKNOWN_ID | instrument.option.underlyings[0].volatility_id
           | unknown volatility 'NOPE' at 'instrument.option.underlyings[0].volatility_id'
```

Three codes deserve different handling in a client, and conflating them is
the usual mistake:

| Code | The client's move |
| --- | --- |
| `UNSPECIFIED_ENUM` | a field was left out — fill it in |
| `INVALID_ARGUMENT` | a field was filled in wrongly — fix the value |
| `UNSUPPORTED` | well-formed, and this build will not price it on a substitute |

`SESSION_NOT_FOUND` means open a session and replay the market; the rest are
infrastructure, to retry or back off.

## Cancel something long

A `CancelRequest` names the `request_id` of the work you want stopped. The
cancel gets its own `Ack`, and the target gets its own terminal frame:

```python
f = E.ClientFrame(request_id=nid(), session_id=sid)
f.cancel.target_request_id = long_running_id
```

Every request is cancellable, but what it costs differs. Between Monte Carlo
batches (`engine.mc.progress_every_paths` set), between sweep points, or
between batch entries, the work stops where it stands and the partial result
is kept. Anywhere else the client is freed after a 250 ms grace and the
session is replayed into a fresh worker — the abandoned calculation runs to
completion on a thread nobody is listening to. A cancel is therefore always
worth offering: it promises the user their session back, not that the machine
stops.

A stop taken at a boundary is reported as `CANCELLED`, and a sweep or batch
returns its own result carrying `abandoned_after` rather than an error.

## Survive a dropped socket

A session outlives its socket by `resume_grace_seconds` — 60 by default — and
keeps its graph, its worker seat and **the requests still running on it**.
`SessionOpened` hands you what a resume needs:

```python
token = opened.session_opened.resume_token     # 128 bits, per session
```

On the new socket, before anything else:

```python
f = E.ClientFrame(request_id=1)
f.resume_session.session_id = sid
f.resume_session.resume_token = token
```

The reply is the original `SessionOpened` with `resumed` set, then whatever
finished while nobody was attached, in the order it finished. `Progress` from
that period is gone.

A wrong token, an expired window and a deliberately closed session are all
`SESSION_NOT_FOUND`, so a guess learns nothing from the difference. The
client's move is the same for all three: open a session and replay the
market — which it can do because the market definition is the client's own
document, and was never the service's to keep.
