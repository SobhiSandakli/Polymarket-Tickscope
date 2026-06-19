# Research Notebooks

Each notebook corresponds to a strategy thesis or a measurement. Together they back the
verdicts in [`../../docs/FINDINGS.md`](../../docs/FINDINGS.md). Read order below goes from
the headline results to the supporting machinery.

| Notebook | What it answers | Finding it supports |
|---|---|---|
| [`event_lag_analysis.ipynb`](event_lag_analysis.ipynb) | Does Polymarket reprice in-game sports events slower than a live score feed? Measured against an ESPN collector during the 2026 Stanley Cup Finals. | **In-game sports lag arb — dead.** The market leads every publicly accessible event feed. |
| [`coinbase_lag_analysis.ipynb`](coinbase_lag_analysis.ipynb) | When BTC moves on Coinbase, how long until Polymarket's 5-min up/down markets reprice — and is the stale quote tradable after fees? Both feeds share one clock on the same host. | **Coinbase → Polymarket lag — inconclusive.** No consistent exploitable lag in the captured data. |
| [`run_backtest.ipynb`](run_backtest.ipynb) | In-sample backtest of the ConvergenceNo thesis (buy NO when YES drops below threshold) through the full fill + fee model. | **ConvergenceNo — in-sample positive.** See OOS notebook for the validation verdict. |
| [`oos_validation.ipynb`](oos_validation.ipynb) | Out-of-sample harness with **locked** parameters. Drop new parquet in, set `OOS_START_MS`, run — params never change, so a positive result is a real survival. | **ConvergenceNo — overfit risk.** This is the notebook that decides whether the edge is real. |
| [`run_optimizer.ipynb`](run_optimizer.ipynb) | Parameter sweep / "alpha plateau" search across multiple strategies (latency sweep, coarse grid). Looks for robust plateaus rather than single lucky points. | Robustness check feeding the kill decisions in FINDINGS. |
| [`resolution_detector.ipynb`](resolution_detector.ipynb) | Single DuckDB scan to compute the **true** NO-win resolution base rate from captured ticks — the empirical foundation of the ConvergenceNo thesis. | Base-rate evidence for ConvergenceNo. |
| [`bot_pnl_audit.ipynb`](bot_pnl_audit.ipynb) | Parses `paper_trades.log` from the execution bot, verifies every trade was actually takeable, and reconciles realized/unrealized PnL. | Validates the execution path in `src/bot/` (paper mode). |

## Reproducing the headline numbers

Curated samples are committed under [`data/samples/`](../../data/samples/) — one per
market class — so notebooks run from a fresh clone:

```bash
python3 -m venv .venv && .venv/bin/pip install -r research/requirements.txt
.venv/bin/jupyter lab research/notebooks/
```

- [`event_lag_analysis.ipynb`](event_lag_analysis.ipynb) runs end-to-end against
  `data/samples/sports/` (Stanley Cup G4, ~75 min, two goals).
- `data/samples/crypto/` (BTC + Coinbase, same clock) is being captured — it will power
  [`coinbase_lag_analysis.ipynb`](coinbase_lag_analysis.ipynb) from a clone.
- The full ~179M-tick research dataset is not committed; to regenerate analysis inputs
  from your own capture, see [`docs/CAPTURE_RUNBOOK.md`](../../docs/CAPTURE_RUNBOOK.md):

```bash
# 1. Capture ticks (see top-level README)
# 2. Decode the binary journal to parquet
python3 scripts/harvester/log_to_parquet.py data/polymarket_*.bin
# 3. Point a notebook's DataLoader at data/ and run all cells
```
