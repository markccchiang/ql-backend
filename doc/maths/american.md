# Early exercise: American and Bermudan

An American option can be exercised at any time up to expiry, so its value is
not the solution of the Black-Scholes equation but of a *free boundary*
problem:

$$V(S,t) = \max\Big(\text{payoff}(S),\ \mathbb{E}\big[e^{-r\,dt} V(S_{t+dt}, t+dt)\big]\Big),$$

with the continuation value satisfying the Black-Scholes equation wherever
early exercise is not optimal. The boundary between the two regions — the
critical price $S^{\ast}(t)$ — is part of the answer, which is what makes the
problem hard and why this build offers four routes to it.

A **Bermudan** option is the same recursion with the maximum taken only on the
exercise dates. It is a lattice or grid problem by nature; there is no closed
form.

:::{note}
For a call on a non-dividend-paying asset ($q = 0$) early exercise is never
optimal and the American price equals the European one. If your American call
prices identically to the European, check the dividend curve before assuming a
bug.
:::

## The three approximations

QuantLib has three closed-form approximations and they **disagree in the third
decimal**, so this service refuses to pick one for you. Each assumes a
different simplification of the exercise boundary.

**Barone-Adesi-Whaley (1987).** Writes the American value as the European value
plus an early-exercise premium, and drops a term that is small when the
discount factor is close to one:

$$V_{\text{Am}} = V_{\text{Eu}} + A_2\left(\frac{S}{S^{\ast}}\right)^{q_2},
\qquad
q_2 = \frac{-(N-1) + \sqrt{(N-1)^2 + 4M/k}}{2},$$

with $M = 2r/\sigma^2$, $N = 2b/\sigma^2$ and $k = 1 - e^{-rT}$. The critical
price $S^{\ast}$ is found by a one-dimensional iteration. Fast, and least
accurate for long maturities where $k$ is far from small.

**Bjerksund-Stensland (1993/2002).** Approximates the exercise boundary by a
*flat* trigger $X$ and prices the resulting barrier-like claim in closed form,
with

$$\beta = \left(\frac{1}{2} - \frac{b}{\sigma^2}\right)
        + \sqrt{\left(\frac{b}{\sigma^2} - \frac{1}{2}\right)^2 + \frac{2r}{\sigma^2}}.$$

Generally the better of the two originals, and the usual default elsewhere in
the industry.

**Ju-Zhong (1999).** A correction to the quadratic method that restores the
term Barone-Adesi-Whaley drops, at the cost of one more evaluation. Usually
closest to a fine lattice, which is the practical test.

If a number has to be defensible rather than fast, price the same trade on a
lattice with several thousand steps and use that as the reference.

## Binomial trees

A lattice discretises the log-price $x = \ln S$ into steps of $\Delta t = T/n$
and solves the recursion backwards from the payoff, taking the maximum against
intrinsic value at every node (or at the Bermudan dates only):

$$V_{i,j} = \max\Big(\text{payoff}(S_{i,j}),\ e^{-r\Delta t}\big(p_u V_{i+1,j+1} + p_d V_{i+1,j-1}\big)\Big).$$

All seven trees are built on the **log process** with per-step drift

$$\nu\,\Delta t = \left(r - q - \tfrac{1}{2}\sigma^2\right)\Delta t,$$

which is why the probabilities below carry $\nu$ rather than $e^{(r-q)\Delta t}$.
These are QuantLib's own parameterisations, from
`ql/methods/lattices/binomialtree.cpp`:

| Tree | Jump | Probability |
| --- | --- | --- |
| Cox-Ross-Rubinstein | $\Delta x = \sigma\sqrt{\Delta t}$ | $p_u = \tfrac{1}{2} + \tfrac{1}{2}\dfrac{\nu \Delta t}{\Delta x}$ |
| Jarrow-Rudd | $\Delta x = \sigma\sqrt{\Delta t}$, nodes drift by $\nu\Delta t$ | $p_u = \tfrac{1}{2}$ |
| Additive equiprobabilities | $\Delta x = -\tfrac{1}{2}\nu\Delta t + \tfrac{1}{2}\sqrt{4\sigma^2\Delta t - 3\nu^2\Delta t^2}$ | $p_u = \tfrac{1}{2}$ |
| Trigeorgis | $\Delta x = \sqrt{\sigma^2\Delta t + \nu^2\Delta t^2}$ | $p_u = \tfrac{1}{2} + \tfrac{1}{2}\dfrac{\nu\Delta t}{\Delta x}$ |
| Tian | moment-matched $u, d$ from $q = e^{\sigma^2\Delta t}$ | $p_u = \dfrac{\tilde r - d}{u - d}$ |
| Leisen-Reimer | derived from the probability | Peizer-Pratt inversion of $d_2$ |
| Joshi4 | derived from the probability | a fourth-order variant of the same inversion |

The classic $u = e^{\sigma\sqrt{\Delta t}},\ d = 1/u,\
p = (e^{(r-q)\Delta t} - d)/(u - d)$ agrees with the Cox-Ross-Rubinstein row to
first order in $\Delta t$; QuantLib's form is the drift-adjusted one.

### Which tree to use

The first five converge to the true price as $O(1/n)$ **with oscillation**:
the error changes sign as the strike moves between nodes, so a price plotted
against step count zig-zags around the answer. Averaging two adjacent step
counts is a cheap way to kill most of that.

**Leisen-Reimer** and **Joshi4** are built to remove it. Both invert the
binomial probability so that the strike sits exactly between two terminal
nodes, both force an **odd** number of steps, and both need the strike — which
is why they are strike-aware and the others are not. Their convergence is
smooth and roughly $O(1/n^2)$, so a few hundred steps beats a few thousand of
the others.

For a **barrier** the lattice is Cox-Ross-Rubinstein only, with the
Derman-Kani correction that shifts the effective barrier onto the nearest node
layer. Barrier lattices converge badly when the barrier falls between nodes,
which is exactly what that correction is for; if a barrier price looks unstable
in the step count, the finite-difference grid is the better tool.

## The alternative: a grid

Finite difference solves the same free boundary problem by discretising the
Black-Scholes equation directly and applying the exercise constraint at every
time step. It is the more robust route for a barrier, and the only route this
build offers for an American *double* barrier — which it does not have, since
the double-barrier engine is analytic and European. See {doc}`numerics`.
