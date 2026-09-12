# Greeks and the other result kinds

Nineteen result kinds are served. Fifteen of them are derivatives of the price;
four are quantities of a different sort — a probability, an implied
volatility, a fair rate and a basis-point value.

Every formula below is the European analytic case, which is what QuantLib's
`BlackCalculator` computes (`ql/pricingengines/blackcalculator.cpp`). Other
engines either differentiate their own solution or **do not publish the
quantity at all**, which is why so many rows come back "not supplied"
({doc}`../HANDLERS`).

## The derivatives

With $d_1, d_2$ as in {doc}`black-scholes`, $\varphi$ the standard normal
density, and the call forms given ($\eta = +1$ for a call, $-1$ for a put,
where the two differ):

| Kind | Definition | Call value |
| --- | --- | --- |
| delta | $\partial V/\partial S$ | $D_q N(d_1)$ |
| gamma | $\partial^2 V/\partial S^2$ | $\dfrac{D_q\,\varphi(d_1)}{S\sigma\sqrt{T}}$ |
| vega | $\partial V/\partial \sigma$ | $S D_q\,\varphi(d_1)\sqrt{T}$ |
| rho | $\partial V/\partial r$ | $K T D_r N(d_2)$ |
| dividend rho | $\partial V/\partial q$ | $-T S D_q N(d_1)$ |
| theta | $\partial V/\partial t$ | $-\dfrac{S D_q \varphi(d_1)\sigma}{2\sqrt{T}} + q S D_q N(d_1) - r K D_r N(d_2)$ |
| theta per day | theta over 365 | |
| delta forward | $\partial V/\partial F$ | $D_r N(d_1)$ |
| strike sensitivity | $\partial V/\partial K$ | $-D_r N(d_2)$ |
| elasticity | $\dfrac{\partial V}{\partial S}\dfrac{S}{V}$ | $\Delta\,S/V$ |

Three of these are worth a sentence each.

**Delta forward** is the delta with respect to the forward rather than the
spot, which is the hedge ratio you want when the hedge instrument is a forward
rather than the stock. The two differ by the carry: $\partial V/\partial S =
(D_q/D_r)\,\partial V/\partial F$.

**Elasticity** is delta scaled into a percentage-for-percentage figure: how
many percent the option moves for one percent on the underlying. It is large
and negative for out-of-the-money puts, and it is the natural units for
comparing options of very different premiums.

**Theta per day** is simply the annual figure divided by 365 — a calendar
convention, not a day count. Over a weekend the realised decay is three of
these, not one.

## The probability

$$\text{ITM cash probability} = N(d_2)\ \text{(call)},\qquad N(-d_2)\ \text{(put)}.$$

This is the risk-neutral probability that the option finishes in the money —
which is also, up to the discount factor, the price of a cash-or-nothing option
paying one unit. It is *not* a real-world probability: it carries the
risk-neutral drift $r-q$, not the asset's expected return.

## Implied volatility

The only result computed from an input the request carries rather than from the
market. Given a target price $V^{\ast}$, it solves

$$\sigma^{\ast}: \quad V(\sigma^{\ast}) = V^{\ast}$$

by a bracketed root find (QuantLib uses Brent). The price is strictly
increasing in $\sigma$ for a vanilla, so the root is unique when it exists; a
target outside the no-arbitrage range has no root and the search fails rather
than returning a boundary value.

Supply the bracket and tolerance if the default search is too wide or too
loose. QuantLib can invert a vanilla, a barrier and a double barrier; on any
other style the kind comes back named absent. See {doc}`numerics` for what the
root find costs.

## The quanto trio

These are sensitivities to the *foreign* leg of a quanto trade
({doc}`quanto`):

| Kind | Definition |
| --- | --- |
| qrho | $\partial V/\partial r_f$, the foreign risk-free rate |
| qvega | $\partial V/\partial \sigma_X$, the FX volatility |
| qlambda | $\partial V/\partial \rho$, the correlation between asset and FX rate |

They exist because the quanto adjustment enters the drift through exactly those
three quantities, so their derivatives are the risk of the adjustment itself.
On a non-quanto trade they come back unavailable.

## The swap kinds

| Kind | Definition |
| --- | --- |
| fair rate | The fixed rate that makes the swap worth zero |
| leg NPV | The present value of one leg |
| leg BPS | The value of one basis point on a leg |

The formulas are in {doc}`rates`. The fair rate needs exactly two legs, fixed
first, then Ibor: any other arrangement is refused rather than answered off the
wrong leg.

## Why a greek is missing

An engine publishes what its solution gives it cheaply. The analytic European
engine differentiates its closed form and has everything above. A binomial
tree has delta, gamma and theta from the nodes around the root and **no vega**,
because volatility is not a state variable in the tree. The American
approximations publish almost nothing beyond the price.

If you need a greek an engine will not give you, the honest options are to
price the same trade on an analytic engine — reading the engine echo on both —
or to bump the market yourself with a sweep and difference the ladder, which is
a finite-difference greek and says so.
