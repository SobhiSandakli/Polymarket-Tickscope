# Polymarket HFT Research Platform

A production-grade, low-latency market data system built to test information-latency arbitrage on prediction markets. Written in C++20 with a Python research layer for strategy development and analysis.

**The hypothesis:** prediction markets reprice slower than the underlying information arrives — that lag is the edge. **The finding:** after live data capture and systematic strategy testing, every tested edge collapsed. The market is more efficient than it looks. The infrastructure built to test this is the actual output of the project.

---

## Quick Start

**Requirements:** GCC 12+ or Clang 15+, CMake 3.20+, OpenSSL. Or just Docker.

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target polymarket_harvester -j$(nproc)

# Run — captures live tick data for any market matching the filter.
# No API keys required. Polymarket is a public CLOB.
POLYMARKET_MARKET_FILTER="Bitcoin" \
  ./build/src/harvester/polymarket_harvester

# What you'll see:
#   [market-discovery] Fetching pages 1..50 from Gamma API (~10s)
#   [market-discovery] filter 'Bitcoin' → 18/10000 tokens
#   [ws open] connected to wss://ws-subscriptions-clob.polymarket.com/ws/market
#   Ticks write silently to data/polymarket_YYYYMMDD_HHMM.bin (15-min rotation)
#
# Ctrl-C to stop. Then decode what was captured:
python3 scripts/harvester/log_to_parquet.py data/polymarket_*.bin
```

Or with Docker (no local C++ toolchain needed):

```bash
# docker compose uses POLYMARKET_MARKET_FILTER env var
POLYMARKET_MARKET_FILTER="Bitcoin" docker compose up harvester
```

---

## Architecture

```
                      Data Collection (C++)
┌────────────────────────────────────────────────────┐
│                                                    │
│  Polymarket WS ───► simdjson ───► Tick (128B)      │
│  (core 1)          zero-copy    ───► MPSC Ring     │
│                                     [65536 slots]  │
│                                     ───► Tickerplant (core 0)
│                                          ───► .bin journal
│                                                    │
│  Binance WS ──────► simdjson ───► BtcTick (64B)    │
│  (core 2)                       ───► BtcJournal    │
│                                      ───► .bin journal
│                                                    │
└──────────────────────────┬─────────────────────────┘
                           │ log_to_parquet.py
                           ▼
                       Parquet files
                           │
                           ▼
                   Research (Python + DuckDB)
┌──────────────────────────────────────────────────────┐
│  Notebooks: backtest · parameter sweep · lag analysis │
│  Backtest engine: full-fee fill simulation            │
│  Strategies tested: 6 — all killed (see Findings)    │
└──────────────────────────────────────────────────────┘
```

### Why each design decision was made

| Component | Decision | Reason |
|---|---|---|
| **Message queue** | Lock-free MPSC ring buffer (65k slots) | A mutex between the I/O thread and journal writer adds 500ns–2μs per tick under contention. The ring buffer adds ~20ns. At 5,000 ticks/sec that's ~10ms saved per second — enough to matter in a latency study. |
| **JSON parsing** | simdjson (SIMD-accelerated) | Processes JSON at ~2.5 GB/s vs ~600 MB/s for RapidJSON. More importantly: zero-copy — parses directly from the network I/O buffer without an intermediate string allocation. |
| **Tick record** | 128 bytes (exactly 2 cache lines) | A struct that straddles a 64-byte cache line causes a split-load on every read: two cache misses instead of one. 128B = 2 × 64B — eliminates false sharing with no wasted space. |
| **Thread placement** | Core affinity: I/O → core 1, Tickerplant → core 0 | Thread migration by the OS scheduler invalidates L1/L2 cache — 100–400 cycle penalty per migration. Pinning eliminates scheduler interference and reduces jitter in latency measurements. |
| **Storage** | Binary journal, 15-min rotation | Sequential NVMe write at ~500 MB/s. No serialization overhead, no locking, no B-tree updates. 128B × 5k ticks/sec = 640 KB/sec — well within disk budget. 15-min rotation limits data loss on crash. |
| **Market filter** | `POLYMARKET_MARKET_FILTER` env var | Subscribing to all 10,000 active tokens generates an ~800 KB WebSocket subscription message — the Polymarket server drops it (code 1006). A focused filter produces ~1 KB. Also prevents OOM on burstable EC2 instances (the original failure mode on AWS). |

Architecture decision records (ADRs) with full context: [`docs/adr/`](docs/adr/)

---

## Research Findings

Six strategies tested against a fill-simulated backtest engine with Polymarket's full dynamic fee model. All killed.

| Strategy | Result | What killed it |
|---|---|---|
| **ConvergenceNo** | +$138 on 36h sample → flat OOS | Regime overfit. The 36h window was a BTC downtrend, not a structural edge. |
| **MeanReversion** | -$1,207 simulated | Taker fees up to 2% mean reversion requires a larger edge than the market provides. |
| **MarketMaking** | Ask fills 11.75× more frequent than bid | Informed traders lift offers when they have directional info. Market maker is adversely selected on every fill. |
| **ArbYesNo** | Zero opportunities in dataset | YES + NO complement constraint enforced tightly enough that deviations are sub-fee. |
| **Binance lag arb** | No exploitable lag found in backtest | Measurement attempted; inconclusive. See below. |
| **In-game sports lag arb** | Market leads all accessible feeds | Live test during 2026 Stanley Cup Finals. See below. |

### Sports market efficiency — measured live

During Game 4 of the 2026 Stanley Cup Finals (VGK vs CAR, June 7 2026), both the Polymarket harvester and an ESPN score collector ran simultaneously. After two VGK goals:

- Polymarket first price move: **23–26 seconds before ESPN detected the goal**
- First Polymarket tick after ESPN detection: **321ms** (Goal 1) and **199ms** (Goal 2)

The market fully repriced before the ESPN API updated. Every other tested source (NHL Stats API, Sportradar) has similar 15–30s delays. The market leads all publicly accessible event feeds — bettors watching live broadcast reprice faster than any HTTP polling pipeline. Analysis: [`research/notebooks/event_lag_analysis.ipynb`](research/notebooks/event_lag_analysis.ipynb)

### Binance → Polymarket lag

The structurally cleanest thesis: when BTC moves on Binance, Polymarket's BTC binary markets should take time to reprice. Both feeds run on the same machine with the same clock, so lag is directly measurable. The backtest showed no consistent exploitable lag — the market appears to price Binance moves within the same second. Whether a colocation or execution speed advantage would change this is untested. Infrastructure: `src/binance/`, `research/notebooks/binance_lag_analysis.ipynb`

Full breakdown: [`docs/FINDINGS.md`](docs/FINDINGS.md)

---

## Build Targets

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Individual targets:
cmake --build build --target polymarket_harvester   # tick data capture
cmake --build build --target binance_harvester      # Binance BTC feed
cmake --build build --target polymarket_bot         # execution infra (paper + live)

# Tests:
ctest --test-dir build
```

---

## Project Structure

```
polymarket/
├── src/
│   ├── harvester/          # Polymarket tick data harvester (main process)
│   ├── binance/            # Binance BTCUSDT feed (dual-feed latency measurement)
│   ├── bot/                # Full execution path: FeedHandler → StrategyEngine → OrderGateway
│   ├── gateway/            # WebSocket client + Gamma API market discovery
│   ├── tickerplant/        # Single-threaded journal writer (consumer side of MPSC ring)
│   ├── feedhandler/        # simdjson tick parser (shared)
│   └── rdb/                # In-memory order book (shared)
│
├── include/polymarket/
│   ├── core/Tick.hpp       # 128-byte Polymarket tick record
│   ├── core/BtcTick.hpp    # 64-byte Binance tick record
│   └── memory/RingBuffer.hpp  # Lock-free MPSC ring buffer
│
├── research/
│   ├── backtest/           # Fill simulation engine with dynamic fee model
│   └── notebooks/          # Strategy analysis, lag studies, OOS validation
│
├── scripts/
│   ├── harvester/          # log_to_parquet.py — binary journal decoder
│   └── collectors/         # ESPN + Betfair event collectors (latency study)
│
├── docs/
│   ├── ARCHITECTURE.md     # Deep-dive on system design
│   ├── FINDINGS.md         # All strategies: thesis, data, verdict
│   └── adr/                # Architecture decision records (4 documented decisions)
│
├── Dockerfile
├── docker-compose.yml      # harvester + event collector services
└── CMakeLists.txt
```
