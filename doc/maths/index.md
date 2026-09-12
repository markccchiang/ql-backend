# Notation and conventions

These pages give the mathematics behind every number this service can
produce. Each one says which wire field feeds which symbol, and where the
formula lives in QuantLib's own source, so a claim here can be checked against
the code that produced the price.

Citations of the form `ql/pricingengines/...` are paths inside the QuantLib
checkout vendored in this repository at `third_party/QuantLib`, pinned at
**v1.43**.

## Symbols

| Symbol | Meaning | Where it comes from |
| --- | --- | --- |
| $S$ | Spot price of the underlying | a `Quote` in the session's market |
| $K$ | Strike | the payoff |
| $T$ | Time to expiry, in years by the volatility's day count | the exercise date against the evaluation date |
| $r$ | Continuously compounded risk-free rate | the risk-free curve |
| $q$ | Continuous dividend yield | the dividend curve; **absent means zero** |
| $\sigma$ | Black volatility | the volatility object |
| $b = r - q$ | Cost of carry | implied by the process |
| $D_r = P(0,T)$ | Risk-free discount factor | read off the curve, not $e^{-rT}$ |
| $D_q$ | Dividend discount factor | read off the dividend curve |
| $F = S\,D_q/D_r$ | Forward price to $T$ | computed by the engine |
| $H$ | Barrier level | the style block |
| $\rho$ | Correlation | a quote, for quanto |
| $N(\cdot),\ \varphi(\cdot)$ | Standard normal distribution and density | |

Two conventions are worth stating explicitly because they are where hand
calculations usually diverge from the service's answer:

**Discount factors come from curves, not from a rate.** QuantLib's engines ask
the term structure for $P(0,T)$ and for the forward, so a non-flat curve is
handled exactly rather than by an effective flat rate. Where these pages write
$e^{-rT}$ they mean $D_r$; the two coincide only for a flat continuously
compounded curve.

**Time is a day count, not a subtraction.** $T$ is the year fraction the
relevant term structure's day counter produces between the evaluation date and
the expiry. Change the day counter and $T$ changes, which changes the price.

## The pages

The root table of contents carries them; in the order they build on each other:

- {doc}`black-scholes` — the model everything here prices in, and the payoffs
- {doc}`greeks` — the nineteen result kinds, and why an engine withholds one
- {doc}`american` — early exercise, the three approximations, the seven trees
- {doc}`exotics` — barrier, double barrier, Asian, lookback, forward start
- {doc}`quanto` — the drift adjustment, and reading its sign
- {doc}`rates` — discounting, bootstrapping, and what a swap is worth
- {doc}`numerics` — what each method's error looks like, and what it costs
