# Committed Sample Datasets

Small curated captures — one per market class — so every analysis notebook runs
end-to-end from a fresh clone, no capture required. Live harvester output lands in
`data/` (gitignored); only this directory is committed.

## `sports/` — 2026 World Cup, four matches (19 June 2026)

USA–Australia, Scotland–Morocco, Brazil–Haiti, Türkiye–Paraguay — captured in one
`run_capture.sh` session. Trimmed to ±5 min windows around the seven goals so the
sample stays small.

| File | Contents |
|---|---|
| `polymarket_worldcup_20260619.parquet` | Polymarket CLOB ticks for all four matches' markets, goal windows only |
| `market_metadata_worldcup.csv` | Token → question/outcome metadata (trimmed to the sample's tokens) |
| `espn_soccer_fifa.world_7604{42,43,44,45}.csv` | ESPN score events per match, 1-second poll, same system clock as the ticks |

Powers [`research/scripts/worldcup_lag_analysis.py`](../../research/scripts/worldcup_lag_analysis.py)
— the measurement showing Polymarket repriced each goal a median **~58s before**
ESPN's API reported it (range 50–73s, on the four goals with a clean market signal).

## `crypto/` — BTC 5-minute up/down markets + Coinbase reference feed

*Pending: fresh AWS capture in progress.* Will contain a window of:

| File | Contents |
|---|---|
| `polymarket_btc_*.parquet` | Polymarket ticks, `POLYMARKET_MARKET_FILTER="up or down"` |
| `coinbase_btc_*.parquet` | Coinbase BTC-USD top-of-book (`BtcTick`), same system clock |
| `market_metadata_btc.csv` | Token → question/outcome metadata for the captured window |

Both feeds run on the same host with the same epoch-ms clock, so they join directly on
`ts_ms` in DuckDB. Powers `coinbase_lag_analysis.ipynb` and gives `run_backtest.ipynb` /
`oos_validation.ipynb` a from-clone dataset. Capture procedure:
[`docs/CAPTURE_RUNBOOK.md`](../../docs/CAPTURE_RUNBOOK.md).

## Provenance

Every file here was produced by the pipeline in this repo (`polymarket_harvester`,
`coinbase_harvester`, `scripts/collectors/espn_collector.py`,
`scripts/harvester/log_to_parquet.py`) — no hand-edited data. The large research
datasets (~179M ticks) behind the headline findings are not committed; these samples
are for reproducing the *pipeline and analyses*, not the full statistics.
