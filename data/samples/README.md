# Committed Sample Datasets

Small curated captures — one per market class — so every analysis notebook runs
end-to-end from a fresh clone, no capture required. Live harvester output lands in
`data/` (gitignored); only this directory is committed.

## `sports/` — 2026 Stanley Cup Finals, Game 4 (VGK @ CAR, June 7 2026)

| File | Contents |
|---|---|
| `polymarket_20260607_0130.parquet` | Polymarket CLOB ticks, 12 tokens, "Hurricanes vs. Golden Knights" filter |
| `polymarket_20260607_0145.parquet` | Continuation of the same capture (15-min journal rotation) |
| `espn_nhl_401874173.csv` | ESPN score events, 1-second poll, same system clock as the ticks |

~75 minutes of game time covering two VGK goals. Powers
[`event_lag_analysis.ipynb`](../../research/notebooks/event_lag_analysis.ipynb) — the
measurement showing Polymarket repriced 23–26s *before* ESPN detected each goal.

A 2026 World Cup capture (Polymarket match markets + ESPN soccer feed) is planned to
extend this beyond a single game — see [`docs/CAPTURE_RUNBOOK.md`](../../docs/CAPTURE_RUNBOOK.md).

## `crypto/` — BTC 5-minute up/down markets + Binance reference feed

*Pending: fresh AWS capture in progress.* Will contain a window of:

| File | Contents |
|---|---|
| `polymarket_btc_*.parquet` | Polymarket ticks, `POLYMARKET_MARKET_FILTER="up or down"` |
| `binance_btc_*.parquet` | Binance BTCUSDT top-of-book (`BtcTick`), same system clock |
| `market_metadata_btc.csv` | Token → question/outcome metadata for the captured window |

Both feeds run on the same host with the same epoch-ms clock, so they join directly on
`ts_ms` in DuckDB. Powers `binance_lag_analysis.ipynb` and gives `run_backtest.ipynb` /
`oos_validation.ipynb` a from-clone dataset. Capture procedure:
[`docs/CAPTURE_RUNBOOK.md`](../../docs/CAPTURE_RUNBOOK.md).

## Provenance

Every file here was produced by the pipeline in this repo (`polymarket_harvester`,
`binance_harvester`, `scripts/collectors/espn_collector.py`,
`scripts/harvester/log_to_parquet.py`) — no hand-edited data. The large research
datasets (~179M ticks) behind the headline findings are not committed; these samples
are for reproducing the *pipeline and analyses*, not the full statistics.
