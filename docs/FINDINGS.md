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

**Live measurement (2026 World Cup, 19 June 2026 — four matches):**

The Polymarket harvester (subscribed to all markets for four matches via the OR filter)
ran alongside one ESPN soccer collector per match. Seven goals were scored. For each
goal, the most goal-sensitive market with a real signal (the Total-Goals O/U line the
goal crosses, or the match-draw market) was measured: when did its mid reprice versus
when did ESPN's API report the goal? Both share the same epoch-ms host clock.

| Match | Goal | Market | Baseline → post | Lag (Poly − ESPN) |
|---|---|---|---|---|
| USA–Australia | 1–0 (11') | O/U 0.5 | 0.93 → 1.00 | **−62s** |
| Türkiye–Paraguay | 0–1 (2') | O/U 0.5 | 0.92 → 1.00 | **−54s** |
| Brazil–Haiti | 2–0 (37') | O/U 1.5 | 0.96 → 1.00 | **−73s** |
| Scotland–Morocco | 0–1 (2') | draw | 0.26 → 0.16 | **−50s** |

Median lead **~58s** (range 50–73s) on the four goals with a clean, large-enough market
move. The other three goals were excluded as low-signal: all came in the Brazil blowout,
where the favorite's markets were already pinned near certainty *before* the goal (jump
< 0.04), so there was nothing to measure.

The negative lag does not mean the market is clairvoyant — it means **ESPN's public
scoreboard API lags the live event by ~a minute**, while Polymarket (driven by bettors
watching the live broadcast) moves at the true goal moment. Two independent markets
(Total-Goals and draw) repriced at the same instant and then sat flat through ESPN's
later timestamp — a clean step, not drift.

**Conclusion:** in-game sports latency arb is not viable with publicly accessible event
feeds. This extends the earlier hockey measurement (ESPN's NHL API lagged ~26–33s) to
soccer, where the World Cup feed lags even more. To beat the market you would need an
event source faster than the fastest humans watching live TV — i.e. a sub-second
proprietary/official feed, which is enterprise-licensed.

Analysis and chart: [`research/scripts/worldcup_lag_analysis.py`](../research/scripts/worldcup_lag_analysis.py)
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
