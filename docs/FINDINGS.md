# Strategy Research Findings

Every strategy was evaluated against a fill-simulated backtest engine with Polymarket's
dynamic fee model (`research/backtest/`). Dataset: ~179M ticks over a ~60h capture window
via the C++ harvester on AWS EC2.

The thesis across all of them: find an edge — either informational or structural — that
survives fees and out-of-sample data. None did.

---

## ConvergenceNo — Shelved (overfit)

**Thesis:** BTC "up or down" binary markets resolve NO (down) ~70% of the time once the
YES token drops below 0.40. Buy NO at the ask and hold to resolution.

**In-sample result (~60h window, ~179M ticks):**

| Metric | Value |
|---|---|
| Win rate | 74.9% (179/239 conditions) |
| Simulated P&L | +$226 on ~$1,600 notional |
| Kelly fraction | f = 0.108 (half-Kelly) |

**What killed it:** the strategy was built and tuned entirely on this ~60h window. Re-run
with the parameters locked on a few additional days of later-captured data, it turned
negative — a textbook overfit. The in-sample window was a BTC downtrend regime, not a
structural inefficiency: by the time YES < 0.40 the market has already priced the
direction, so the apparent edge was momentum bought at elevated cost.

That follow-up dataset has since been deleted, so the negative out-of-sample run is not
reproducible from this repo. The locked-parameter harness used for it remains in
`research/notebooks/oos_validation.ipynb` — point it at a fresh capture to re-test.

**Lesson:** a ~60h backtest on a directional asset in a clear recent regime is nearly
guaranteed to overfit. The sample needed to separate a real edge from noise is far larger
than it first appears — and an out-of-sample test with locked parameters is the only thing
that reliably catches it.

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
first moved **26 seconds before ESPN detected Goal 1** (mid 0.040 → 0.085, fully
repriced to 0.250 by detection time) and **33 seconds before Goal 2** (0.235 → 0.295).
By the time ESPN updated, the market had already finished repricing both times.

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
— runs end-to-end from a fresh clone against the committed sample in
[`data/samples/sports/`](../data/samples/).

---

## Coinbase → Polymarket Lag Arb — Inconclusive

**Thesis:** when BTC moves sharply on Coinbase, Polymarket's BTC binary markets take time
to reprice. The stale quote is the edge — no prediction required, only speed.

**Why this thesis is structurally different:** it makes no prediction about direction.
It only requires that information travels between venues with measurable, exploitable
latency — a physics-constrained structural edge rather than a statistical one.

**Status:** backtesting showed no consistent exploitable lag. The market appears to
price Coinbase moves within the same second in the tested data. Whether a co-location
or execution-speed advantage would change this is untested — the current setup runs
both feeds on the same AWS instance, so the measurement captures the system's own
latency floor rather than a true cross-venue gap.

Infrastructure is in place for a cleaner test: `src/coinbase/`, `research/notebooks/coinbase_lag_analysis.ipynb`.

---

## Summary

Prediction-based edges on Polymarket are hard to find. The market is efficient enough
that any signal computable from public data is already in the price. The sports
efficiency measurement provides concrete evidence: bettors react faster than any
accessible event detection pipeline.

The structural latency thesis (Coinbase → Polymarket) remains the most defensible
hypothesis because it doesn't require prediction — only speed. But it requires either
a faster reference feed or colocation to test properly.

The infrastructure built to run these experiments — live data capture, binary journals,
lock-free ring buffer, dual-feed same-clock architecture — is the durable output of
this project.
