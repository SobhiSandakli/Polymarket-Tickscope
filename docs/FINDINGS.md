# Strategy Research Findings

Every strategy was evaluated using a fill-simulated backtest engine with Polymarket's
dynamic fee model (`research/backtest/`). Dataset: ~179M ticks collected over several
weeks via the C++ harvester on AWS EC2.

---

## ConvergenceNo — Shelved (overfit)

**Thesis:** BTC "up or down" markets resolve NO (down) ~70% of the time once the YES
token drops below 0.40. Buy NO at the ask and hold to resolution.

**Initial result (36h sample, 179M ticks):**

| Param | Value |
|---|---|
| Threshold | YES mid < 0.40 |
| Win rate | 74.9% (179/239 conditions) |
| Simulated P&L | +$138 on ~$1,600 capital |
| Kelly fraction | half-Kelly, f = 0.108 |

**What killed it:** extending the dataset to several weeks collapsed the edge to near
zero. The 36h sample was a regime — a stretch where BTC was trending down — not a
persistent structural edge. Classic small-sample overfit.

**Lesson:** resolution base rate is not a tradeable signal once the market has already
priced in the direction. By the time YES < 0.40, the market has already moved; you're
not finding an inefficiency, you're just buying momentum at elevated cost.

---

## MeanReversion — Dead

**Thesis:** buy YES/NO tokens that have diverged from fair value and wait for reversion.

**Result:** Net -$1,207 in simulation. Polymarket's spreads and taker fees (up to 2%)
make reversion plays deeply negative expected value — the edge required to break even
on mean reversion exceeds what the market offers.

---

## MarketMaking — Dead

**Thesis:** post passive limit orders on both YES and NO sides, earn the spread.

**Result:** ask-only fills 11.75× more frequent than bid fills. In a binary market that
resolves 0 or 1, informed traders lift offers when they have directional information —
a market maker is systematically adversely selected. Net negative after fees.

---

## ArbYesNo / GraphArb — Dead

**Thesis:** YES + NO should sum to ~1.00 (minus fees). Exploit deviations.

**Result:** zero exploitable opportunities found across the dataset. Polymarket's
matching engine enforces the complement constraint tightly enough that any deviation
is smaller than the round-trip fee cost.

---

## Binance Lag Arb — Active

**Thesis:** when BTC moves sharply on Binance, Polymarket's BTC up/down markets take
time to reprice. The stale quote is the edge — no prediction required, only speed.

**Status:** data collection complete. Lag measurement in progress
(`research/notebooks/binance_lag_analysis.ipynb`). Decision gate: lag > order
latency + fees.

**Why this thesis is different from the others:** it makes no prediction about market
direction. It only requires that information travels from one venue to another with
measurable latency — a structural, physics-constrained edge rather than a statistical one.

---

## Summary

Prediction-based edges on Polymarket are hard to find: the market is efficient enough
that any signal that can be computed from public data is already in the price. The only
thesis not yet killed is latency — and that requires infrastructure, not just statistics.
That infrastructure is what this repo is.
