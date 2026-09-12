# Quanto barrier benchmark

The benchmark `smoke_v2.py` runs, and the numbers it produced. Extracted from
`TESTING.md`, which now points here.

## What is being benchmarked, and why

`testBarrierValues` in QuantLib's `test-suite/quantooption.cpp` carries a
`TODO: bench results against an existing prop calculator` and a tolerance of
`0.5` to match — its three values have no provenance. We have no vendor pricer
either, but the same file shows the substitute: `testPDEOptionValues`
benchmarks the analytic quanto vanilla against a PDE at `2e-4`.
`FdBlackScholesBarrierEngine` is single-argument constructible, so
`QuantoEngine` can wrap it and the same trick works for barriers. That is what
`FdParameters` exists for.

## The market

All three rows share one market, from `QUANTO_BARRIER` in `reference_tables.py`, itself extracted from
`quantooption.cpp`:

| | |
| --- | --- |
| spot | 100.0 |
| strike | 90.0 |
| barrier | 95.0 |
| dividend yield `q` | 0.04 |
| risk-free `r` | 0.0212 |
| maturity `t` | 0.50 |
| volatility `v` | 0.25 |
| FX risk-free `fxr` | 0.05 |
| FX volatility `fxv` | 0.2 |
| correlation | 0.3 |

The two `DOWN_OUT` rows carry a rebate of 3.0; the `DOWN_IN` row carries none.

## Results

| Row | Recorded | Ours (analytic) | PDE, fine grid | Analytic vs PDE | Recorded vs ours |
| --- | --- | --- | --- | --- | --- |
| DownOut call | 8.247 | 8.244498 | 8.244557 | 5.9e-05 | 2.5e-03 |
| DownOut put | **2.274** | **2.377326** | **2.377241** | 8.6e-05 | **0.103** |
| DownIn put | 2.85 | 2.872743 | 2.872753 | 1.0e-05 | 2.3e-02 |

Rows 1 and 3 differ from the recorded values by amounts consistent with values
recorded to four and three significant figures. Row 2 is a different matter:
0.103, and the PDE sides with us to 8.6e-05. The error also shrinks by about 4x
per grid refinement, so the two methods are converging to the same number
rather than agreeing by accident.

The three grids are `PRESET_COARSE` (100 x 100, QuantLib's own defaults),
`PRESET_STANDARD` (400 x 200) and `PRESET_FINE` (2000 x 800). The script
asserts `coarse > standard > fine` on the gap to the analytic price, and that
`PriceResult.engine` echoes the grid the number came from.

## What this establishes, precisely

Both routes take the quanto adjustment from the same `QuantoTermStructure`, so
the cross-check does *not* test the quanto wrapper — it tests the barrier
closed form on top of it. The wrapper is covered separately and independently,
by the quanto vanilla rows reproducing Haug's published values to 2e-05 through
that same code, and those rows are cross-checked against the PDE as well: the
analytic side there is already pinned to Haug, so a disagreement would indict
the PDE rather than the closed form. It agrees, which is what makes the barrier
result above worth anything. The two legs together cover the price.

Two guardrails that need no second engine agree as well: in-out parity
(`down_in + down_out == vanilla` at zero rebate) holds to 1e-9, and an
unreachable barrier degrades to the vanilla price exactly.

The script reports the recorded-value gaps rather than asserting on them. The
table check stays at the C++ test's own `0.5`: those numbers are what is under
test, and tightening the tolerance against a value we believe is wrong would be
backwards.

## Reproducing it

Run `smoke_v2.py` as `TESTING.md` describes; the benchmark is the
"quanto barriers: analytic against the PDE" section near the end. The three
grid presets it sweeps are `FdParameters` `PRESET_COARSE`, `PRESET_STANDARD`
and `PRESET_FINE`.
