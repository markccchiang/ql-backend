# Quanto

A quanto option pays in a currency other than the underlying's, converted at a
**fixed** rate. The holder gets the option's payoff in domestic currency
without the FX exposure — which is not free, because the hedge for the option
has to be run in the foreign asset and unwound at a moving exchange rate.

## The adjustment

Under the domestic risk-neutral measure the foreign asset's drift picks up a
covariance term. Writing $r_d$ for the domestic rate, $r_f$ for the foreign
rate, $\sigma_X$ for the volatility of the exchange rate and $\rho$ for the
correlation between the asset and that rate, the asset drifts at

$$b_{\text{quanto}} = r_f - q - \rho\,\sigma\,\sigma_X$$

instead of $r - q$. Everything else about the option is unchanged, which is
why quanto is not a product in this schema: it is a change of drift, and it
therefore composes over any style whose engine is built from a process alone.

QuantLib implements exactly that by wrapping the *dividend* term structure
(`ql/termstructures/yield/quantotermstructure.hpp`):

$$q_{\text{quanto}}(t) = q(t) + r_d(t) - r_f(t) + \rho\,\sigma\,\sigma_X,$$

so that $r_d - q_{\text{quanto}} = r_f - q - \rho\sigma\sigma_X$, which is the
line above. The engine then prices the ordinary option against that adjusted
process and discounts at the domestic rate.

## Reading the sign

The correlation term is the whole of the quanto effect, and its sign is the
part that trips people up.

- $\rho > 0$ — the asset rises when the foreign currency strengthens. The
  hedger is long the asset in a currency that is getting more expensive to
  convert back at the fixed rate, so the drift is reduced and a **call is worth
  less**.
- $\rho < 0$ — the reverse; a call is worth more.
- $\rho = 0$ — the adjustment vanishes and only the rate substitution
  $r \to r_f$ remains.

The size of it is $\rho\sigma\sigma_X T$ in the log-drift: with a 25% asset
vol, a 12% FX vol and correlation 0.3, that is about 0.9% per year of drift —
small, but not small enough to ignore on a long-dated trade.

## What you have to supply

All three of an FX risk-free curve, an FX volatility and a correlation. The
interface will not let you send a partial quanto: two of the three is not a
cheaper approximation, it is a different model.

## Where it composes, and where it does not

| Style | Quanto |
| --- | --- |
| vanilla | yes — analytic or FD, European |
| barrier | yes — analytic or FD, European |
| double barrier | yes — analytic |
| forward start | yes |
| Asian | **no** — QuantLib has no quanto Asian engine |
| lookback | **no** — refused by name |

The lookback row has history worth knowing. This build used to price a quanto
lookback as a *plain* lookback and report no error at all — the wrapper was
silently dropped. The service refuses it by name now, and says so in the
`Capabilities` it answers `Hello` with, so a client can rule the combination
out before it costs a round trip.

Under quanto, a finite-difference request takes one of the three grid presets
rather than an explicit size: the wrapper constructs its inner engine from a
process alone, so each grid is a separate compiled type and an arbitrary pair
has nowhere to go.

## The quanto greeks

Three result kinds exist only for these trades, and they are the derivatives of
the adjustment itself:

$$\text{qrho} = \frac{\partial V}{\partial r_f},
\qquad
\text{qvega} = \frac{\partial V}{\partial \sigma_X},
\qquad
\text{qlambda} = \frac{\partial V}{\partial \rho}.$$

Since $\rho$, $\sigma_X$ and $r_f$ enter only through $b_{\text{quanto}}$, all
three are proportional to the trade's sensitivity to the carry — qlambda in
particular is $-\sigma\sigma_X$ times it. On a non-quanto trade they come back
named absent rather than zero.
