# Strategy Research Findings

Every strategy was evaluated against a fill-simulated backtest engine with Polymarket's
dynamic fee model (`research/backtest/`). Dataset: ~179M ticks collected over several
weeks via the C++ harvester on AWS EC2.

The thesis across all of them: find an edge — either informational or structural — that
survives fees and out-of-sample data. None did.

---

## ConvergenceNo — Shelved (overfit)

**Thesis:** BTC "up or down" binary markets resolve NO (down) ~70% of the time once the
YES token drops below 0.40. Buy NO at the ask and hold to resolution.

**Initial result (36h sample):**

| Metric | Value |
|---|---|
| Win rate | 74.9% (179/239 conditions) |
| Simulated P&L | +$138 on ~$1,600 notional |
| Kelly fraction | f = 0.108 (half-Kelly) |

**What killed it:** extending the dataset to several weeks collapsed the edge to near
zero. The 36h window was a BTC downtrend regime — not a structural inefficiency.
By the time YES < 0.40, the market has already priced in the direction; the apparent
edge was momentum at elevated cost.

**Lesson:** a 36h backtest on a directional asset with a clear recent regime is nearly
guaranteed to overfit. The sample size needed to distinguish a real edge from noise is
much larger than it initially appears.

---

## MeanReversion — Dead

**Thesis:** buy YES/NO tokens that have diverged from fair value and wait for reversion.

**Result:** net -$1,207 in simulation. Polymarket's taker fees (up to 2% per side)
mean the edge required to break even on mean reversion exceeds what the market
realistically provides. The fee structure makes this category of strategy structurally
negative.

---

## MarketMaking — Dead

**Thesis:** post passive limit orders on both YES and NO sides, earn the spread.

**Result:** ask-only fills 11.75× more frequent than bid fills. In a binary market that
resolves 0 or 1, anyone lifting an offer has directional information. A passive market
maker is adversely selected on essentially every fill — the only trades that execute
against you are the ones you lose on.

---

## ArbYesNo / GraphArb — Dead

**Thesis:** YES + NO should sum to ≈ 1.00. Exploit deviations.

**Result:** zero exploitable opportunities found across the full dataset. The matching
engine enforces the complement constraint tightly enough that any deviation is smaller
than the round-trip fee cost. The apparent spreads in the order book do not survive the
fee model.

---

## In-Game Sports Lag Arb — Dead (market leads all accessible feeds)

**Thesis:** Polymarket's in-game sports markets reprice after real-world events with
enough lag to trade — get the event data first, buy the updated probability before the
market catches up.

**Live measurement (2026 Stanley Cup Finals, Game 4, June 7 2026):**

Both the Polymarket harvester (12 tokens, "Hurricanes vs. Golden Knights" filter) and an
ESPN NHL score collector ran simultaneously. Two goals were scored during the capture window.

| Event | ESPN detection ts | First Poly tick after ESPN | Lag |
|---|---|---|---|
| VGK 1–0 (Goal 1) | 1780796579983 ms | 1780796580304 ms | **+321ms** |
| VGK 2–0 (Goal 2) | 1780796650971 ms | 1780796650971+199 ms | **+199ms** |

The raw lag numbers look small — but they're misleading. The Polymarket WIN_YES token
first moved at `ts_ms = 1780796554007` — **26 seconds before ESPN detected Goal 1**.
The full repricing (0.040 → 0.260) happened 23 seconds before the ESPN `score_change`
event. By the time ESPN updated, the market had already fully repriced.

```
Timeline (Goal 1):

  T - 26s   Polymarket WIN_YES first price move: 0.040 → 0.085
  T - 23s   Large reprice: 0.085 → 0.180 → 0.260
  T  0s     ESPN API detects score_change (HTTP poll, 1s interval)
  T +0.3s   First Polymarket tick observed after ESPN timestamp (already at 0.250)
```

**What this means:** bettors watching a live broadcast reprice Polymarket faster than any
HTTP polling pipeline can detect the event. Every tested source (ESPN, NHL Stats API,
Sportradar) has 15–30s delays. The market is not the slow participant — the data
pipelines are.

**Conclusion:** in-game sports latency arb is not viable with publicly accessible event
feeds. A sub-second proprietary data source (official league feed or on-venue data) would
be required, and those are enterprise-licensed.

Analysis and charts: [`research/notebooks/event_lag_analysis.ipynb`](../research/notebooks/event_lag_analysis.ipynb)

---

## Binance → Polymarket Lag Arb — Inconclusive

**Thesis:** when BTC moves sharply on Binance, Polymarket's BTC binary markets take time
to reprice. The stale quote is the edge — no prediction required, only speed.

**Why this thesis is structurally different:** it makes no prediction about direction.
It only requires that information travels between venues with measurable, exploitable
latency — a physics-constrained structural edge rather than a statistical one.

**Status:** backtesting showed no consistent exploitable lag. The market appears to
price Binance moves within the same second in the tested data. Whether a co-location
or execution-speed advantage would change this is untested — the current setup runs
both feeds on the same AWS instance, so the measurement captures the system's own
latency floor rather than a true cross-venue gap.

Infrastructure is in place for a cleaner test: `src/binance/`, `research/notebooks/binance_lag_analysis.ipynb`.

---

## Summary

Prediction-based edges on Polymarket are hard to find. The market is efficient enough
that any signal computable from public data is already in the price. The sports
efficiency measurement provides concrete evidence: bettors react faster than any
accessible event detection pipeline.

The structural latency thesis (Binance → Polymarket) remains the most defensible
hypothesis because it doesn't require prediction — only speed. But it requires either
a faster reference feed or colocation to test properly.

The infrastructure built to run these experiments — live data capture, binary journals,
lock-free ring buffer, dual-feed same-clock architecture — is the durable output of
this project.
