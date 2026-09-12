# The exotic styles

All nine exotic styles this build prices have closed forms inside the same
Black-Scholes model — that is why each one's default engine is analytic rather
than a grid. Each is a different set of boundary conditions on the same
equation, and several are assembled out of the terms of the ones above them:
the cliquet is a sum of forward starts, the knock digital is the barrier
formula with a different payoff, and the one-touch is a single term of it.

Three of the nine also have a sampled engine, and none of the three is an
approximation of a closed form for its own sake: the arithmetic-average Asian
has no closed form at all, the performance cliquet's Monte Carlo engine is the
only sampled cliquet engine QuantLib has, and a basket past two assets leaves
the closed forms behind entirely.

Throughout: $b = r - q$, $v = \sigma\sqrt{T}$, and

$$\mu = \frac{b - \tfrac{1}{2}\sigma^2}{\sigma^2},
\qquad
\lambda = \sqrt{\mu^2 + \frac{2r}{\sigma^2}}.$$

## Barrier

A barrier option is a vanilla that is switched on (*in*) or off (*out*) if the
spot touches $H$ before expiry. The closed form is Merton (1973) and
Reiner-Rubinstein (1991), and it is assembled from six terms. With
$\phi = \pm 1$ for call/put and $\eta = \pm 1$ for down/up:

$$x_1 = \frac{\ln(S/K)}{v} + (1+\mu)v, \qquad
x_2 = \frac{\ln(S/H)}{v} + (1+\mu)v,$$

$$y_1 = \frac{\ln\!\big(H^2/(SK)\big)}{v} + (1+\mu)v, \qquad
y_2 = \frac{\ln(H/S)}{v} + (1+\mu)v, \qquad
z = \frac{\ln(H/S)}{v} + \lambda v,$$

$$\begin{aligned}
A &= \phi S D_q N(\phi x_1) - \phi K D_r N(\phi x_1 - \phi v),\\
B &= \phi S D_q N(\phi x_2) - \phi K D_r N(\phi x_2 - \phi v),\\
C &= \phi S D_q (H/S)^{2(\mu+1)} N(\eta y_1) - \phi K D_r (H/S)^{2\mu} N(\eta y_1 - \eta v),\\
D &= \phi S D_q (H/S)^{2(\mu+1)} N(\eta y_2) - \phi K D_r (H/S)^{2\mu} N(\eta y_2 - \eta v),\\
E &= R\,D_r\big[N(\eta x_2 - \eta v) - (H/S)^{2\mu} N(\eta y_2 - \eta v)\big],\\
F &= R\big[(H/S)^{\mu+\lambda} N(\eta z) + (H/S)^{\mu-\lambda} N(\eta z - 2\eta\lambda v)\big],
\end{aligned}$$

where $R$ is the rebate. Each of the eight barrier types is a signed sum of
these: a down-and-in call with $K > H$ is $C + E$; a down-and-out call with
$K > H$ is $A - C + F$; and so on. $E$ is the rebate paid *at expiry*, $F$ the
rebate paid *at hit*.

Two properties are worth using as checks, and the service's own test suite
uses both:

$$V_{\text{in}} + V_{\text{out}} = V_{\text{vanilla}} \quad (R = 0),$$

and an unreachable barrier degrades to the vanilla price exactly.

**What is not built:** discrete monitoring and partial-time (window) barriers.
The formula above assumes the barrier is monitored *continuously*, which is
worth remembering when comparing against a contract that is monitored daily —
continuous monitoring is worth strictly more knock-out protection, so the
prices differ systematically rather than randomly.

## Double barrier

Two barriers, $0 < L < U$, both monitored continuously. The closed form is
Ikeda-Kunitomo (1992): repeated reflection between the two levels produces an
**infinite series of image terms**, each one a normal probability, converging
geometrically. QuantLib truncates it; the truncation error is far below the
tolerance you would price to.

There is no finite-difference route for this style in the build, because
QuantLib's only FD double-barrier engine is a Heston engine and takes a
calibrated model rather than a process.

## Asian

An Asian option pays off against the **average** of the underlying rather than
its terminal value. Whether there is a closed form depends entirely on which
average.

**Geometric, continuous.** The geometric average of a geometric Brownian motion
is itself lognormal, so the option is a Black-Scholes option with adjusted
parameters. QuantLib prices it as a Black formula with

$$\sigma_G = \frac{\sigma}{\sqrt{3}},
\qquad
q_G = \frac{1}{2}\left(r + q + \frac{\sigma^2}{6}\right),$$

the second being an effective dividend yield
(`ql/pricingengines/asian/analytic_cont_geom_av_price.cpp`). The
$1/\sqrt{3}$ is the whole story of an Asian: averaging over the life of the
option cuts the variance of the terminal quantity to a third, so an Asian is
always worth less than the corresponding vanilla.

**Geometric, discrete.** With fixing dates the average is over a finite set,
and $\ln G$ is again normal — with mean and variance computed from the fixing
times rather than from the closed-form limit. Same Black formula, different
moments.

**Arithmetic.** The sum of lognormals is not lognormal and there is no exact
closed form, so this is the one style in the build that *must* go to Monte
Carlo. QuantLib's engine can use the geometric average as a **control
variate** — it is highly correlated with the arithmetic average and its value
is known exactly, so subtracting its simulation error removes most of the
noise. Turn it on when you have one; it is worth far more than the same time
spent on extra paths.

## Lookback

A lookback pays off against the running extremum of the path, which must be
supplied: an option already part-way through its life whose extremum is dropped
would price as though it had just started.

- **Floating strike:** a call pays $S_T - m$, where $m$ is the running minimum.
- **Fixed strike:** a call pays $\max(M - K, 0)$, where $M$ is the running
  maximum.

Both have closed forms under continuous monitoring — Goldman-Sosin-Gatto (1979)
for the floating case, Conze-Viswanathan (1991) for the fixed. The floating
call is

$$c = S D_q N(a_1) - m D_r N(a_2)
 + S D_r \frac{\sigma^2}{2b}\left[\left(\frac{S}{m}\right)^{-2b/\sigma^2} N\!\left(-a_1 + \frac{2b\sqrt{T}}{\sigma}\right) - e^{bT}N(-a_1)\right],$$

$$a_1 = \frac{\ln(S/m) + (b + \tfrac{1}{2}\sigma^2)T}{\sigma\sqrt{T}},
\qquad a_2 = a_1 - \sigma\sqrt{T}.$$

The third term is the value of the *option to have done better* — the part that
distinguishes a lookback from a vanilla struck at the extremum. Note it is
singular as $b \to 0$; the engine handles that limit, but it is why a lookback
is unusually sensitive to the carry.

Continuous monitoring again: a lookback priced against a daily-observed
contract is an upper bound.

## Forward start

The strike is not known yet. It is set at the reset date $t_1$ to a multiple of
the spot then, $K = m\,S_{t_1}$, so the option's *moneyness* is fixed but its
strike is not — which is why the trade carries no strike and a struck payoff
would be silently overwritten.

Because the Black-Scholes price is homogeneous of degree one in $(S, K)$, the
value at $t_1$ is $S_{t_1}$ times the value of a unit-spot option struck at
$m$ over the remaining life $\tau = T - t_1$. Discounting that back:

$$V_0 = D_q(t_1)\; S_0 \; V_{\text{BS}}\big(1,\ m,\ \tau\big).$$

QuantLib composes this literally: the forward engine prices the inner vanilla
and multiplies by the dividend discount to the reset date
(`ql/pricingengines/forward/forwardengine.hpp`).

The **performance** variant pays the *return* rather than the amount — it
settles $\max(S_T/S_{t_1} - m, 0)$ — so the payoff is already unit-scaled and
the discount to the reset date is the risk-free one instead:

$$V_0^{\text{perf}} = D_r(t_1)\; V_{\text{BS}}\big(1,\ m,\ \tau\big).$$

Same trade description, different price. The switch is on the style card.

## Cliquet

A cliquet — a *ratchet* — is a series of forward starts laid end to end. Reset
dates $t_1 < \dots < t_{n-1}$ split the life into $n$ periods, the last ending
at expiry, and each period is struck at $m$ times the spot when that period
opens. So it is the section above, once per period:

$$V_0 = \sum_{i=1}^{n} D_q(t_{i-1})\; S_0\; V_{\text{BS}}\big(1,\ m,\
t_i - t_{i-1}\big), \qquad t_0 = 0,\ t_n = T.$$

QuantLib assembles it exactly that way — one Black calculation per period, at
that period's forward and its *forward* variance, weighted by the dividend
discount to the period's start (`analyticcliquetengine.cpp:70-84`) — and it
appends the expiry to the reset list to close the final period. The
**performance** form makes the same substitution the forward start does,
discounting at the risk-free rate and paying the return:

$$V_0^{\text{perf}} = \sum_{i=1}^{n} D_r(t_{i-1})\; V_{\text{BS}}\big(1,\
m,\ t_i - t_{i-1}\big).$$

**There are no caps or floors here, and that is a property of the library
rather than of this page.** A capped cliquet truncates each period's return
before summing, which is a different sum and not a scaling of this one.
QuantLib's instrument carries the four fields and never passes them to an
engine, so the price you would get is the uncapped sum above. The service
refuses them by name rather than returning it.

## Compound

An option on an option. The mother, struck at $K_1$ and expiring at $\tau_1$,
buys the daughter, struck at $K_2$ and expiring at $\tau_2 > \tau_1$.

The whole difficulty is one number: the spot $S^*$ at which the mother is worth
exercising, which is the spot at which the daughter is worth exactly what the
mother costs,

$$V_{\text{daughter}}\big(S^*,\ K_2,\ \tau_2 - \tau_1\big) = K_1.$$

That has no closed form, so QuantLib solves it with Brent
(`analyticcompoundoptionengine.cpp:92-96`) and then the price is a Geske (1979)
expression in two correlated normals — the daughter finishing in the money and
the mother being exercised, correlated by $\rho = \sqrt{\tau_1/\tau_2}$
because the first period is contained in the second. With $\phi = \pm 1$ for
a call or put daughter and $w = \pm 1$ for a call or put mother, one expression
covers all four combinations:

$$V = \phi w\, S D_q(\tau_2)\, N_2\!\big(\!-\phi w X', \phi d_+; w\rho\big)
\; - \; \phi w\, K_2 D_r(\tau_2)\, N_2\!\big(\!-\phi w X, \phi d_-; w\rho\big)
\; - \; w\, K_1 D_r(\tau_1)\, N\!\big(\!-\phi w X\big),$$

where $X$ is $S^*$ in the log-return coordinates Wystup uses, $X' = X -
\sigma\sqrt{\tau_1}$, and $d_\pm$ are the usual daughter arguments at
$\tau_2$. Read it right to left: the last term is the premium paid, the middle
one the strike paid on the daughter, the first the asset received.

**The compound must expire on or before the option it buys.** Not a modelling
choice — there is nothing to exercise into otherwise, and QuantLib's instrument
rejects it.

## Chooser

The holder decides at $t_c$ whether the option is a call or a put. Until then
it is neither, which is why the trade carries no option type at all.

**Simple chooser** — one strike $K$, one expiry $T$ for both sides. At $t_c$ the
holder takes $\max(c, p)$, and put-call parity turns that into a call plus a
put on the *forward*, giving a closed form in two univariate normals
(`analyticsimplechooserengine.cpp:74-87`):

$$W = S D_q(T)\,N(d) - K D_r(T)\,N\big(d - \sigma\sqrt{T}\big)
- S D_q(T)\,N(-y) + K D_r(T)\,N\big(-y + \sigma\sqrt{t_c}\big),$$

$$d = \frac{\ln(S/K) + \big(b + \tfrac{1}{2}\sigma^2\big)T}{\sigma\sqrt{T}},
\qquad
y = \frac{\ln(S/K) + bT + \tfrac{1}{2}\sigma^2 t_c}{\sigma\sqrt{t_c}}.$$

The first two terms are a call struck $K$ at $T$; the second two are a put on
what is left, and they carry $t_c$ rather than $T$ because the choice is made
then.

**Complex chooser** — the two sides have strikes and expiries of their own,
$(X_c, T_c)$ and $(X_p, T_p)$. Parity no longer applies, so the holder's rule
becomes a critical spot again: the $I$ at which the call and the put are worth
the same at $t_c$,

$$c\big(I, X_c, T_c\big) = p\big(I, X_p, T_p\big),$$

found by Newton-Raphson, after which the price is four bivariate normal terms
with $\rho_c = \sqrt{t_c/T_c}$ and $\rho_p = \sqrt{t_c/T_p}$
(`analyticcomplexchooserengine.cpp:38-72`).

That solver is where the one bound worth knowing comes from. QuantLib runs its
inner Black-Scholes calculation to $T - 2t_c$ rather than $T - t_c$
(`analyticcomplexchooserengine.cpp:91,99`), so a leg expiring inside twice the
choice date leaves it a negative time to work with. The service refuses that
rather than letting the volatility surface throw from inside the iteration.

## Basket

Two or more assets, accumulated to one number and handed to a plain payoff.
Which accumulation you choose is not a flavour of one formula — it decides
which formula exists at all.

**Minimum and maximum**, two assets, are Stulz (1982). The whole engine is
*one* closed form and two identities. The one form is the call on the minimum:
with $\sigma_i$ the standard deviations $\sigma_i\sqrt{T}$, $F_i$ the
forwards, and

$$\sigma^2 = \sigma_1^2 + \sigma_2^2 - 2\rho\sigma_1\sigma_2, \qquad
d = \frac{\ln(F_1/F_2) + \tfrac{1}{2}\sigma^2}{\sigma},$$

$$c_{\min} = D_r\Big[F_1 M\big(d_1^{(1)}, -d;\ \rho_1'\big)
+ F_2 M\big(d_1^{(2)},\ d - \sigma;\ \rho_2'\big)
- K\,M\big(d_1^{(1)} - \sigma_1,\ d_1^{(2)} - \sigma_2;\ \rho\big)\Big],$$

where $\rho_1' = (\rho\sigma_2 - \sigma_1)/\sigma$ and
$\rho_2' = (\rho\sigma_1 - \sigma_2)/\sigma$. Note that $\sigma$ is the
volatility of the *ratio* of the two assets — the minimum is a question about
which one is smaller, and that is a question about the ratio.

The two identities do the rest. A call on the maximum is
$c_{\max} = c_1 + c_2 - c_{\min}$, because $\max + \min = a + b$
(`stulzengine.cpp:63-79`). And each put comes from parity against a zero-strike
call, which is the accumulation itself:

$$p = K D_r - c(K = 0) + c(K).$$

QuantLib's own source carries a warning here worth passing on: the line that
discounts the dividend yields is commented *"cannot handle non zero dividends,
so don't believe this"* (`stulzengine.cpp:129`). It is more cautious than the
evidence — four of the reference rows this build prices carry dividend yields
of 6% and 9% and reproduce Haug's published values to 1e-4 — but it is the
library's own doubt about its own formula, and worth knowing before you trust
a dividend-paying basket further than the table goes.

**Spread**, two assets, is Kirk, and it is the most economical trick on this
page. A spread call pays $\max(S_1 - S_2 - K, 0)$, which is not lognormal in
anything. Kirk treats $F_2 + K$ as if it were, which turns the whole trade into
a *single* Black call on the ratio:

$$F = \frac{F_1}{F_2 + K}, \qquad
\sigma_{\text{eff}}^2 = \sigma_1^2
+ \sigma_2^2\left(\frac{F_2}{F_2+K}\right)^2
- 2\rho\,\sigma_1\sigma_2\left(\frac{F_2}{F_2+K}\right),$$

$$V = (F_2 + K)\; \text{Black}\big(F,\ 1,\ \sigma_{\text{eff}},\ D_r\big).$$

At $K = 0$ the weight $F_2/(F_2+K)$ is 1 and $\sigma_{\text{eff}}$ is exactly
the ratio's volatility — the approximation vanishes and Kirk is Margrabe's
exchange option. It degrades as $K$ grows relative to $F_2$
(`kirkengine.cpp:35-49`).

**Everything else is Monte Carlo.** A weighted average has no closed form here,
and neither does any basket of three or more assets. The assets are evolved
together through a `StochasticProcessArray`, which factorises the correlation
matrix once and drives the whole system off one set of correlated normals. That
factorisation is why the matrix has to be positive semi-definite, and why this
build checks that it is: given one that is not, QuantLib repairs it rather than
refusing it ({doc}`../HANDLERS`).

## One-touch digitals

A binary payoff on an *American* exercise is a one-touch: it pays as soon as
the level is reached rather than at expiry, and it goes to QuantLib's digital
American engine. Its value is exactly the $F$ term of the barrier formula
above — a rebate paid at hit — which is the cleanest way to remember what a
one-touch is: the rebate of a knock-out, sold on its own.

## Knock digitals

The same binary payoff on a **barrier** is a different trade and a different
engine: it pays a fixed amount, or the asset, at expiry, but only if the
barrier was touched (*in*) or was not (*out*). This is the trade the schema's
`digital` style names, and it has no instrument of its own in QuantLib — the
shape *is* the product.

The formula is the barrier assembly at the top of this page with the vanilla
payoff swapped out, which is why the two look so alike:

$$x_1 = \frac{\ln(S/K)}{v} + \mu v, \qquad
x_2 = \frac{\ln(S/H)}{v} + \mu v, \qquad
y_1 = \frac{\ln\!\big(H^2/SK\big)}{v} + \mu v, \qquad
y_2 = \frac{\ln(H/S)}{v} + \mu v,$$

with $\mu = \ln\!\big(D_q/D_r\big)/\sigma^2 T - \tfrac{1}{2}$. Sixteen
cases fall out of four barrier types × two payoff kinds × call or put — Haug's
pp. 176–180, and the sixteen rows this build prices as two reference tables.

The one thing worth carrying away is how little separates the two payoffs. A
cash-or-nothing pays $K$; an asset-or-nothing pays the asset, and in the engine
that is exactly $\mu \to \mu + 1$ with the cash amount replaced by the
forward (`analyticbinarybarrierengine.cpp:65-72`). The same six terms, one
shift of the drift.

Two branches never reach the formula. An **out** option whose spot is already
past the barrier is worth nothing, with every greek zero. An **in** option
whose spot is already past it has knocked in, so it is simply a European
digital, and QuantLib prices it with the ordinary analytic engine. Both are the
honest answer rather than a special case.

**No rebate.** The engine never reads one. A rebate sent with a knock digital
would have been taken and dropped and a price returned for a different trade,
so the service refuses it — the only rule on this page that QuantLib itself
would not have raised.
