# Polymarket HFT

Ultra-low latency C++20 trading infrastructure for Polymarket, with a Python research layer for strategy development and backtesting.

---

## Project Structure

```
polymarket/
├── src/
│   ├── harvester/                      # Polymarket tick data harvester
│   │   └── main.cpp                    #   WebSocket → MPSC ring → Tickerplant → journal
│   ├── binance/                        # Binance BTC price harvester
│   │   ├── main.cpp                    #   WebSocket → BtcJournal → binary files
│   │   ├── BinanceFeed.hpp/.cpp        #   IXWebSocket + simdjson parser
│   │   ├── BtcJournal.hpp              #   Rotating binary journal writer
│   │   └── CMakeLists.txt
│   ├── bot/                            # Live trading bot
│   │   ├── main.cpp                    #   Entry point, CLI args, WS setup
│   │   ├── BotConfig.hpp               #   Strategy params, fee model constants
│   │   ├── BookState.*                 #   Per-market YES/NO bid/ask state
│   │   ├── BotDiscovery.*              #   REST market discovery (Gamma API)
│   │   ├── FeedHandler.*               #   WS message parser → BookState updates
│   │   ├── OrderGateway.*              #   Paper / live order execution
│   │   ├── StrategyEngine.hpp          #   ConvergenceNo entry/exit logic
│   │   └── CMakeLists.txt
│   ├── feedhandler/                    # Shared: simdjson tick parser
│   ├── gateway/                        # Shared: WebSocket client + market discovery
│   ├── rdb/                            # Shared: in-memory order book (RDB)
│   ├── tickerplant/                    # Shared: single-thread journal writer
│   └── CMakeLists.txt
│
├── include/polymarket/                 # Public C++ headers
│   ├── core/
│   │   ├── Tick.hpp                    #   128-byte Polymarket tick (2 cache lines)
│   │   └── BtcTick.hpp                #   64-byte Binance BTC tick (1 cache line)
│   ├── memory/RingBuffer.hpp           #   Lock-free MPSC ring buffer
│   ├── feedhandler/
│   ├── gateway/
│   ├── rdb/
│   ├── tickerplant/
│   └── version.hpp
│
├── tests/                              # C++ unit tests (GoogleTest)
│
├── research/                           # Python research layer
│   ├── backtest/                       #   Backtest framework
│   │   ├── strategies/
│   │   │   ├── base.py                 #     BaseStrategy interface
│   │   │   └── convergence_no.py       #     ConvergenceNo (tested, shelved)
│   │   ├── data_loader.py              #     DuckDB-backed tick data loader
│   │   ├── engine.py                   #     Fill simulation + MTM P&L
│   │   ├── fills.py                    #     Dynamic fee model (Polymarket spec)
│   │   ├── metrics.py                  #     Round-trip trade metrics
│   │   ├── optimizer.py                #     Parameter grid search
│   │   ├── run_optimizer.py            #     CLI entry point
│   │   └── types.py                    #     Signal, Fill, Side, OrderType
│   ├── notebooks/                      #   Jupyter analysis
│   │   ├── run_backtest.ipynb          #     Standard backtest runner
│   │   ├── run_optimizer.ipynb         #     Parameter sweep runner
│   │   ├── oos_validation.ipynb        #     Out-of-sample validation
│   │   ├── bot_pnl_audit.ipynb         #     Paper trading P&L template
│   │   ├── resolution_detector.ipynb   #     Market resolution analysis
│   │   └── binance_lag_analysis.ipynb  #     Binance vs Polymarket lag study
│   └── scripts/
│       ├── resolution_detector.py
│       └── fetch_markets.py            #     Arb scanner (historical research)
│
├── scripts/                            # Ops / DevOps
│   ├── harvester/
│   │   ├── hourly_flush.sh             #     .bin → Parquet → S3 (cron)
│   │   ├── log_to_parquet.py           #     Polymarket Tick journal decoder
│   │   ├── btc_to_parquet.py           #     Binance BtcTick journal decoder
│   │   └── local_tester.sh             #     Local dev: watch + convert journals
│   └── bot/
│       └── bot_daily_s3.sh             #     Bot log upload to S3
│
├── deploy/                             # Deployment configs
│   ├── harvester/
│   │   └── polymarket-harvester.service
│   ├── binance/
│   │   └── binance-harvester.service
│   └── bot/
│       ├── polymarket-bot.service
│       └── polymarket-bot.env.example
│
├── docs/
│   ├── ARCHITECTURE.md                 # System overview + data flow
│   ├── BOT_SETUP.md                    # Bot AWS deployment guide
│   └── BINANCE_SETUP.md               # Binance harvester deployment guide
│
├── data/                               # Local data (gitignored)
├── logs/                               # Runtime logs (gitignored)
├── CMakeLists.txt
└── README.md
```

---

## Architecture

```
                      Data Collection (C++)
┌────────────────────────────────────────────────────┐
│                                                    │
│  Polymarket WS ───► simdjson ───► Tick (128B)      │
│  (core 1)          parse        ───► MPSC Ring     │
│                                     ───► TP (core 0) ───► .bin journal
│                                                    │
│  Binance WS ──────► simdjson ───► BtcTick (64B)    │
│  (core 2)          parse        ───► BtcJournal ───► .bin journal
│                                                    │
└──────────────────────────┬─────────────────────────┘
                           │ hourly_flush.sh (cron)
                           ▼
                    Parquet on S3
                           │
                           ▼
                   Research (Python)
┌──────────────────────────────────────────────────────┐
│  DuckDB ← Parquet files                             │
│  Notebooks: backtest, parameter sweep, lag analysis  │
│  Strategies: ConvergenceNo (tested, shelved)         │
└──────────────────────────────────────────────────────┘
                           │
                           ▼ if edge confirmed
                      Bot (C++)
┌──────────────────────────────────────────────────────┐
│  Polymarket WS ───► FeedHandler ───► BookState       │
│                     ───► StrategyEngine              │
│                     ───► OrderGateway (paper / live)  │
└──────────────────────────────────────────────────────┘
```

**Hard constraints:** zero heap allocation in hot path, no mutexes in tick processing,
no virtual dispatch, explicit memory ordering on all atomics.

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Individual targets:
cmake --build build --target polymarket_harvester
cmake --build build --target binance_harvester
cmake --build build --target polymarket_bot
```

---

## Strategy Research

All strategies were evaluated against a full-fee, fill-simulated backtest engine
(see `research/`). Every prediction-based approach was killed — see [`docs/FINDINGS.md`](docs/FINDINGS.md)
for the full breakdown.

**ConvergenceNo** (buy NO on BTC up/down markets when YES mid < threshold) showed an
apparent +$138 edge on an initial 36h dataset, then collapsed on extended out-of-sample
data — a textbook small-sample overfit. Not deployed.

The execution path in `src/bot/` is fully implemented (paper + live order modes, Kelly
sizing, time-based expiry exits) and is ready to wire to any validated strategy.

---

## Research Status

| Strategy | Status | Reason |
|---|---|---|
| **ConvergenceNo** | **Shelved** | Overfit — edge vanished out-of-sample |
| Binance lag arb | **Active** | Latency measurement in progress |
| MeanReversion | Dead | Net -$1,207 in simulation |
| ArbYesNo / GraphArb | Dead | Zero opportunities found |
| MarketMaking | Dead | Ask-only fills lose 11.75× spread |
| All others | Dead | See `research/` notebooks |
