# System Architecture

Three independent processes running on the same AWS EC2 instance:

```
┌─────────────────────────────────────────────────────────────────────┐
│                        AWS EC2 Instance                            │
│                                                                     │
│  ┌──────────────────────┐  ┌──────────────────────┐                │
│  │  polymarket_harvester│  │  binance_harvester   │                │
│  │  (core 0 + core 1)  │  │  (core 2)            │                │
│  │                      │  │                      │                │
│  │  Polymarket WS ──────│  │  Binance WS ─────────│                │
│  │  → simdjson parse    │  │  → simdjson parse    │                │
│  │  → MPSC ring buffer  │  │  → BtcJournal        │                │
│  │  → Tickerplant       │  │  → btc_*.bin files   │                │
│  │  → polymarket_*.bin  │  │                      │                │
│  └──────────────────────┘  └──────────────────────┘                │
│                                                                     │
│  ┌──────────────────────┐                                          │
│  │  polymarket_bot      │                                          │
│  │  (IXWebSocket thread)│                                          │
│  │                      │                                          │
│  │  Polymarket WS ──────│                                          │
│  │  → FeedHandler       │                                          │
│  │  → BookState         │                                          │
│  │  → StrategyEngine    │  ┌──────────────────────┐                │
│  │  → OrderGateway      │  │  hourly_flush.sh     │                │
│  └──────────────────────┘  │  (cron, every hour)  │                │
│                             │                      │                │
│                             │  *.bin → Parquet → S3│                │
│                             └──────────────────────┘                │
└─────────────────────────────────────────────────────────────────────┘
```

## Data Flow

```
Polymarket harvester:
  WebSocket → simdjson → Tick (128B) → MPSC Ring[65536] → Tickerplant
  → polymarket_YYYYMMDD_HHMM.bin (15-min rotation)
  → hourly_flush.sh → Parquet → S3

Binance harvester:
  WebSocket → simdjson → BtcTick (64B) → BtcJournal (64KB buffer)
  → btc_YYYYMMDD_HHMM.bin (15-min rotation)
  → hourly_flush.sh → Parquet → S3

Bot:
  WebSocket → FeedHandler → BookState → StrategyEngine → OrderGateway
  (independent process, same market data but evaluated for trade signals)
```

## Binary Record Formats

| Field | Polymarket Tick (128B) | Binance BtcTick (64B) |
|---|---|---|
| timestamp | uint64 epoch ms | uint64 epoch ms |
| price | double (probability) | — |
| size | double (quantity) | — |
| best_bid | double | double (BTC USD) |
| best_ask | double | double (BTC USD) |
| mid | — | double (precomputed) |
| side | uint8 (BID/ASK) | — |
| event_type | uint8 (PRICE_CHANGE/TRADE) | — |
| asset_id | char[80] | — |

Both use the same local system clock for timestamps, making them directly
joinable by `ts_ms` in analysis.

## Executables

| Binary | Source | Purpose |
|---|---|---|
| `polymarket_harvester` | `src/harvester/` | Collects ALL Polymarket tick data |
| `binance_harvester` | `src/binance/` | Collects Binance BTCUSDT quotes |
| `polymarket_bot` | `src/bot/` | Execution infrastructure (paper + live modes); not deployed — no validated edge |

## Shared Libraries

| Library | Source | Used By |
|---|---|---|
| `polymarket_feedhandler` | `src/feedhandler/` | harvester |
| `polymarket_tickerplant` | `src/tickerplant/` | harvester |
| `polymarket_gateway` | `src/gateway/` | harvester |
| `polymarket_rdb` | `src/rdb/` | harvester |
