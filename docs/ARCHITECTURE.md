# System Architecture

## Overview

Three independent processes, each pinned to its own CPU core(s):

```
┌─────────────────────────────────────────────────────────────────────┐
│                        AWS EC2 / Local                              │
│                                                                     │
│  ┌──────────────────────┐  ┌──────────────────────┐                │
│  │  polymarket_harvester│  │  coinbase_harvester   │                │
│  │  core 0 (TP)         │  │  core 2              │                │
│  │  core 1 (WS I/O)     │  │                      │                │
│  │                      │  │  Coinbase WS ─────────│                │
│  │  Polymarket WS ──────│  │  → simdjson parse    │                │
│  │  → simdjson parse    │  │  → BtcJournal        │                │
│  │  → MPSC ring buffer  │  │  → btc_*.bin files   │                │
│  │  → Tickerplant       │  │                      │                │
│  │  → polymarket_*.bin  │  └──────────────────────┘                │
│  └──────────────────────┘                                          │
│                                                                     │
│  ┌──────────────────────┐  ┌──────────────────────┐                │
│  │  polymarket_bot      │  │  log_to_parquet.py   │                │
│  │  (execution infra)   │  │  (offline, on-demand)│                │
│  │  not deployed —      │  │                      │                │
│  │  no validated edge   │  │  *.bin → Parquet     │                │
│  └──────────────────────┘  └──────────────────────┘                │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Hot Path

Every incoming Polymarket WebSocket message travels this path before hitting disk:

```
Network frame arrives
       │
       ▼ (~1–5μs, kernel → user space)
IXWebSocket callback (core 1)
       │
       ▼ (~0.1–0.4μs, simdjson SIMD parse)
Tick struct (128B) constructed
       │
       ▼ (~20ns, single atomic store)
MPSC ring buffer slot written
       │
       ▼ (~20ns, atomic load + cache line read)
Tickerplant pops slot (core 0)
       │
       ▼ (~200–400ns, NVMe write into 4KB page buffer)
Binary journal appended
```

Total hot-path latency from message receipt to durable write: **~2–6μs**. No heap allocation, no virtual dispatch, no mutex anywhere in this path.

---

## Component Design Decisions

### Lock-free MPSC Ring Buffer

**ADR:** [`docs/adr/0001-lock-free-mpsc-ring-buffer.md`](adr/0001-lock-free-mpsc-ring-buffer.md)

The I/O thread (WebSocket callbacks) and the journal writer (Tickerplant) run on separate cores. They share data via a 65,536-slot ring buffer using a single producer / single consumer lock-free protocol.

**Why not a mutex-protected queue?**
- A `std::mutex` lock/unlock roundtrip takes 500ns–2μs under contention.
- At 5,000 ticks/sec that adds 2.5–10ms of unnecessary latency per second.
- A cache-line-aligned atomic CAS takes ~20ns — two orders of magnitude faster.
- More importantly: the Tickerplant never blocks the I/O thread. A slow disk write cannot stall incoming tick processing.

**Why 65,536 slots?**
Power-of-2 allows index wrapping with a bitmask (`& 0xFFFF`) instead of modulo — avoids integer division in the hot path.

### simdjson for Tick Parsing

Polymarket sends JSON messages over WebSocket. Each message contains one or more CLOB updates (price change or last trade).

**Why simdjson and not nlohmann/RapidJSON?**
- simdjson uses SIMD (AVX2/SSE4.2) to scan multiple bytes simultaneously: ~2.5 GB/s throughput.
- RapidJSON peaks at ~600 MB/s. nlohmann is slower still.
- At 500 messages/sec the difference is ~200μs/sec aggregate — small but measurable in a latency study.
- Zero-copy DOM: simdjson parses directly from the network receive buffer. No intermediate `std::string` allocation in the hot path.

### 128-Byte Tick Record

`include/polymarket/core/Tick.hpp` defines a 128-byte struct:

```
Offset  Field        Type        Bytes
     0  timestamp    uint64_t      8    Unix epoch ms
     8  price        double        8    probability [0, 1]
    16  size         double        8    order quantity
    24  best_bid     double        8
    32  best_ask     double        8
    40  side         uint8_t       1    BID / ASK
    41  event_type   uint8_t       1    PRICE_CHANGE / LAST_TRADE
    42  asset_id     char[80]     80    token ID (null-terminated)
   122  (padding)                  6    cache-line alignment
                                 ───
                                 128   = 2 × 64-byte cache lines
```

**Why exactly 128 bytes?**
A CPU cache line is 64 bytes on x86-64. A struct that straddles a cache line boundary causes a split-load: the CPU must fetch two cache lines and merge them on every read. At 5,000 reads/sec that's 5,000 unnecessary cache misses. 128B = exactly 2 × 64B eliminates this.

**Why not pack smaller?** The asset_id (Polymarket token ID) is a 78-digit integer stored as a string — it needs ~78 bytes. There's no meaningful way to shrink the record further without losing information or adding pointer indirection.

### Core Affinity

The harvester pins:
- Core 1: IXWebSocket I/O thread (network receive + simdjson parse)
- Core 0: Tickerplant (ring buffer consumer + journal write)

**Why?**
The OS scheduler migrates threads between cores to balance load. Each migration invalidates L1/L2 cache for that thread — 100–400 cycle penalty, plus TLB flush overhead. Pinning eliminates scheduler interference and makes latency measurements consistent across runs.

On macOS these calls are compiled out (`#if defined(__linux__)`) — the system still works, just without the affinity guarantee.

### Binary Journal Format

Ticks are written as raw `Tick` structs (128 bytes, no delimiters) to rotating `.bin` files.

**Why binary and not CSV/SQLite/Parquet?**
- Sequential binary write: OS flushes dirty pages to NVMe at ~500 MB/s.
- CSV: string formatting adds ~500ns per record. Parsing on read is slow.
- SQLite: B-tree insert with lock acquisition. WAL commit overhead.
- Parquet: columnar format requires buffering a full row group (~128k rows) before writing. Crash loses the buffer.

Binary files are decoded offline by `scripts/harvester/log_to_parquet.py` into columnar Parquet for analysis.

**Why 15-minute rotation?**
Balances two costs: fewer files = better I/O throughput; smaller files = less data lost on crash (at most 15 minutes). At 640 KB/sec, a 15-minute file is ~576 MB — manageable for S3 upload or DuckDB scan.

### Focused Market Filter

**ADR:** [`docs/adr/0003-focused-market-filter-to-prevent-oom.md`](adr/0003-focused-market-filter-to-prevent-oom.md)

The env var `POLYMARKET_MARKET_FILTER` (comma-separated substrings) restricts discovery to matching markets before subscribing.

**Why is this necessary?**
Subscribing to all ~10,000 active tokens generates a WebSocket subscription message ~800 KB in size. The Polymarket server drops connections with messages above a threshold (WebSocket close code 1006). With a focused filter, the subscription message is ~1 KB.

A secondary reason: on AWS T-series burstable instances, subscribing to 10,000 markets exhausted CPU credits within minutes and caused OOM — the original failure mode that triggered this redesign.

Setting `POLYMARKET_MARKET_FILTER=""` reverts to full-firehose mode.

### Gamma API Pagination

**ADR:** [`docs/adr/0004-gamma-api-pagination-descending-order.md`](adr/0004-gamma-api-pagination-descending-order.md)

Two non-obvious constraints discovered through testing:
- The API hard-caps at 100 results per page regardless of the `limit` parameter.
- Default sort order is ascending (lowest volume first). Without `ascending=false`, the first 100 markets returned are the least liquid ones — high-volume game markets never appeared.

---

## Data Formats

### Polymarket Tick (128 bytes)

Struct format string: `<QddddBB80s6x`

| Field | C type | Bytes | Notes |
|---|---|---|---|
| timestamp | uint64_t | 8 | Unix epoch ms, same clock as Coinbase feed |
| price | double | 8 | probability [0, 1] |
| size | double | 8 | order quantity |
| best_bid | double | 8 | 0.0 for LAST_TRADE events |
| best_ask | double | 8 | 0.0 for LAST_TRADE events |
| side | uint8_t | 1 | 0=BID, 1=ASK |
| event_type | uint8_t | 1 | 0=PRICE_CHANGE, 1=LAST_TRADE |
| asset_id | char[80] | 80 | token ID (Polymarket CLOB asset) |
| (padding) | — | 6 | cache-line alignment |

### Coinbase BtcTick (64 bytes, 1 cache line)

| Field | C type | Bytes |
|---|---|---|
| timestamp | uint64_t | 8 |
| best_bid | double | 8 |
| best_ask | double | 8 |
| mid | double | 8 (precomputed) |
| (padding) | — | 32 |

Both use the same local system clock → directly joinable by `ts_ms` in DuckDB.

---

## Executables

| Binary | Source | Purpose |
|---|---|---|
| `polymarket_harvester` | `src/harvester/` | Focused tick data capture via `POLYMARKET_MARKET_FILTER` |
| `coinbase_harvester` | `src/coinbase/` | Coinbase BTC-USD top-of-book feed |
| `polymarket_bot` | `src/bot/` | Full execution path: FeedHandler → BookState → StrategyEngine → OrderGateway (paper + live). Not deployed — no strategy survived validation. |
