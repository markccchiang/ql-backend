# The Black-Scholes world

Every equity engine in this build prices in one model: a single asset following
geometric Brownian motion under the risk-neutral measure,

$$dS_t = (r - q)\,S_t\,dt + \sigma S_t\,dW_t .$$

The three **processes** the underlying card offers are three ways of filling in
that drift.

| Process | Drift | Use |
| --- | --- | --- |
| Black-Scholes-Merton | $r - q$ | An equity with a dividend yield. The default |
| Black-Scholes | $r$ | No dividends. A dividend curve is *rejected*, not ignored |
| Black | $0$ | The forward is the state variable: $q = r$, so $F = S$. Options on futures |

## The equation

Under that process a claim's value $V(S,t)$ satisfies

$$\frac{\partial V}{\partial t}
  + \tfrac{1}{2}\sigma^2 S^2 \frac{\partial^2 V}{\partial S^2}
  + (r-q)\,S\,\frac{\partial V}{\partial S}
  - rV = 0,$$

with the payoff as terminal condition at $t = T$. Every method in
{doc}`../HANDLERS` is a way of solving that equation:

- **analytic** — the closed form below, when the boundary conditions admit one;
- **lattice** — a discrete approximation of the same process, solved backwards;
- **finite difference** — the equation itself, discretised on a grid;
- **Monte Carlo** — the equivalent expectation, sampled;
- **integral** — the expectation as a numerical quadrature over the terminal
  density.

They are approximations of one another, which is why comparing two engines on
the same trade is a meaningful check and why the result always echoes which one
ran.

## The closed form

For a European payoff the solution is the Black-Scholes formula. QuantLib
computes it in *forward and discount* form rather than from $r$ and $q$
directly — this is `BlackCalculator`, driven by the forward
$F = S\,D_q/D_r$, the standard deviation $v = \sigma\sqrt{T}$ and the discount
$D_r$:

$$d_1 = \frac{\ln(F/K)}{v} + \frac{v}{2},
\qquad d_2 = d_1 - v,$$

$$c = D_r\big[F\,N(d_1) - K\,N(d_2)\big],
\qquad
p = D_r\big[K\,N(-d_2) - F\,N(-d_1)\big].$$

Substituting $F = Se^{(r-q)T}$ and $D_r = e^{-rT}$ recovers the textbook form,

$$c = S e^{-qT} N(d_1) - K e^{-rT} N(d_2),
\qquad
d_1 = \frac{\ln(S/K) + (r - q + \tfrac{1}{2}\sigma^2)T}{\sigma\sqrt{T}},$$

but the forward form is the one to reason with here, because the forward and
the discount are what the curves actually supply.

## Put-call parity

$$c - p = D_r(F - K) = S e^{-qT} - K e^{-rT}.$$

Worth keeping to hand: it is the cheapest sanity check on a market that has
just been re-authored, and it holds for the European vanilla prices this
service returns to the last digit.

## The other payoffs

The same machinery prices the other payoffs, because each is a combination of
the two terms above evaluated at one strike or two. Write

$$\Phi(K) = N\big(d_2(K)\big), \qquad A(K) = D_r F\,N\big(d_1(K)\big) = S D_q N\big(d_1(K)\big),$$

so $\Phi(K)$ is the risk-neutral probability of finishing above $K$ and $A(K)$
is the present value of receiving the asset itself in that event:

| Payoff | Terminal payoff (call form) | Value |
| --- | --- | --- |
| plain vanilla | $\max(S_T - K, 0)$ | $A(K) - K D_r \Phi(K)$ |
| asset-or-nothing | $S_T$ if $S_T \ge K$ | $A(K)$ |
| cash-or-nothing | $X$ if $S_T \ge K$ | $X D_r \Phi(K)$ |
| gap | $S_T - K_2$ if $S_T \ge K_1$ | $A(K_1) - K_2 D_r \Phi(K_1)$ |
| super-fund | $S_T/K_1$ if $K_1 \le S_T < K_2$ | $\big(A(K_1) - A(K_2)\big)/K_1$ |
| super-share | $X$ if $K_1 \le S_T < K_2$ | $X D_r\big(\Phi(K_1) - \Phi(K_2)\big)$ |
| percentage strike | $S_T \max(1 - m, 0)$ | $\max(1-m,0)\,S D_q$ |
| floating | the extremum is the strike | lookbacks only ({doc}`exotics`) |

The **gap** payoff is the one to read twice: $K_1$ decides whether the option
pays and $K_2$ decides how much, so it can pay a negative amount and is not
worth $\max(S-K,0)$ for any single strike.

The **percentage strike** payoff has no optionality on its own — its payoff is
linear in $S_T$, so a call is worth $\max(1-m,0)$ of a forward. It exists to be
paired with a forward-start exercise, where the engine sets the strike to
$m\,S_{t_1}$ at the reset date ({doc}`exotics`).

## Where this stops

The model has one volatility per expiry and strike, no jumps, no stochastic
volatility and no local volatility surface. Heston, Bates and local-vol
processes are in the schema and are **not built**, so a price from this service
is always a Black-Scholes price — including the exotics, whose closed forms are
derived inside the same model.
