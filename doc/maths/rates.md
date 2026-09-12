# Curves, swaps and cash flows

## Discount factors are the primitive

Everything on the rates side reduces to one function: the discount factor
$P(0,t)$, the present value of one unit paid at $t$. Rates are ways of quoting
it, and they only mean anything alongside a **compounding convention** and a
**day count**.

| Convention | Relation |
| --- | --- |
| continuous | $P = e^{-z t}$ |
| simple | $P = \dfrac{1}{1 + z\,\tau}$ |
| compounded, frequency $f$ | $P = \left(1 + \dfrac{z}{f}\right)^{-f t}$ |

$\tau$ is the accrual factor from the day counter, which is *not* $t$ in
general — Actual/360 and Actual/365 disagree by 1.4%, and 30/360 disagrees with
both. This is why a curve-sampling request has to name the compounding, the
frequency and the day counter before it will yield a zero-rate curve, and why
a compounding set beside a discount-factor request is refused: a discount factor
has no convention, and accepting one would imply it did.

The forward rate between $t_1$ and $t_2$ follows:

$$f(t_1,t_2) = \frac{1}{\tau}\left(\frac{P(0,t_1)}{P(0,t_2)} - 1\right)
\quad\text{(simple)},
\qquad
f = \frac{\ln P(0,t_1) - \ln P(0,t_2)}{t_2 - t_1}
\quad\text{(continuous)}.$$

## The four curve shapes

| Shape | What it is | Live quotes? |
| --- | --- | --- |
| flat | One rate for all maturities | yes |
| zero | Interpolated zero rates through fixed nodes (linear) | no |
| discount | Interpolated discount factors through fixed nodes (log-linear) | no |
| bootstrap | Stripped from market instruments | yes |

Log-linear interpolation on discount factors is linear interpolation on
$\ln P$, which means **piecewise-constant continuous forwards** — the usual
default, and the reason a discount curve drawn as forwards looks like a
staircase.

## Bootstrapping

A bootstrapped curve is built so that it reprices, exactly, the instruments it
was built from. Each pillar adds one unknown and one equation, solved in
maturity order — hence *bootstrap*.

**Deposit.** A simple-rate quote over one accrual period:

$$P(0,t) = \frac{1}{1 + r\,\tau}.$$

**Par swap.** The fixed rate that makes the swap worth zero. With payment times
$t_1 \dots t_n$ and accruals $\tau_i$,

$$K \sum_{i=1}^{n} \tau_i P(0,t_i) \;=\; P(0,t_0) - P(0,t_n),$$

the right-hand side being the value of the floating leg when its forwards come
off the same curve. The sum on the left is the **annuity**.

**OIS.** The same equation with the floating leg compounded daily off the
overnight index rather than accrued over each period.

The type of curve being solved for is a choice of *traits* — what is
interpolated — and *interpolator*. This build compiles three of each (discount,
zero yield, forward rate × linear, log-linear, cubic). The choice is not
cosmetic: interpolating zero rates linearly and interpolating discount factors
log-linearly give different forwards between pillars, and both reprice the
pillars exactly. If two curves disagree away from the pillars, that is the
interpolation, not an error.

:::{tip}
The clean test that a bootstrap worked is that a par instrument reprices to
zero. The worked swap example does exactly that: a five-year swap on the 5Y
pillar prices to an NPV of $-0.000002$ with a fair rate of $0.027000$.
:::

## Swap pricing

A swap is a set of legs, each a set of cash flows, each discounted:

$$\text{NPV} = \sum_{\text{legs}} \pm \sum_{c \in \text{leg}} c\;P(0,t_c),$$

with the sign given by pay-or-receive on each leg. A fixed coupon is
$N\,\tau_i\,K$; a floating coupon is $N\,\tau_i\,(g\,f_i + s)$ for gearing $g$
and spread $s$, where $f_i$ is the index fixing — read from the fixing history
if the fixing date has passed, and **forecast off the index's forwarding
curve** otherwise.

### Leg BPS and the fair rate

The BPS of a leg is what it is worth if its rate moves by one basis point:

$$\text{BPS} = \pm\, 10^{-4} \sum_i N\,\tau_i\,P(0,t_i)
= 10^{-4} \times \text{annuity}.$$

The fair rate follows directly, and this is exactly what the service computes
for a two-leg swap (fixed first, floating second):

$$K^{\ast} = -\frac{\text{NPV}_{\text{float}}}{\text{annuity}},
\qquad
\text{annuity} = \frac{\text{BPS}_{\text{fixed}}}{10^{-4}}.$$

The leg order matters because the formula does not check which leg is which —
which is why the service refuses the request rather than computing it off the
wrong leg.

## The cash flow table

Each row carries its payment date, the amount, the discount factor the engine
used, and their product. The property that makes the table worth showing is

$$\sum_{\text{rows}} \text{present value} = \text{NPV},$$

and the service's own test asserts it. Coupons add their accrual dates,
notional and rate; floating coupons add the fixing date, spread and gearing,
and say whether the fixing was **historical or forecast** — which is the field
to check first when a swap mid-period prices unexpectedly.

Rows already paid are left out.
