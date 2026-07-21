#pragma once

#include <cstdint>

namespace polymarket::core
{

    // ---------------------------------------------------------------------------
    // BtcTick — one BTC-USD quote update from the Coinbase ticker stream.
    //
    // Captures the best bid/ask at the instant of the update.  Written to a
    // rotating binary journal by the Coinbase harvester, then converted to
    // Parquet by scripts/harvester/btc_to_parquet.py for offline analysis.
    //
    // Memory layout (x86-64 / ARM64, LP64 ABI)
    // ─────────────────────────────────────────
    //  offset  0 │ uint64_t timestamp    8 B │
    //  offset  8 │ double   bid          8 B │  cache line 0 (bytes 0–63)
    //  offset 16 │ double   ask          8 B │  all fields on one line
    //  offset 24 │ double   mid          8 B │
    //  offset 32 │ (padding)            32 B │
    //  ──────────┴──────────────────────────┤
    //  sizeof(BtcTick) == 64  (1 cache line)│
    //
    // Field semantics
    // ───────────────
    //  timestamp : Unix epoch milliseconds — local clock, same reference as
    //              Tick.timestamp so the two datasets are directly joinable.
    //  bid       : Coinbase BTC-USD spot best bid price (USD).
    //  ask       : Coinbase BTC-USD spot best ask price (USD).
    //  mid       : (bid + ask) / 2.0 — precomputed to save division in analysis.
    //
    // Python struct format for decoding: '<Qddd32x'
    //   Q   = uint64_t  (8)   timestamp
    //   d   = double    (8)   bid
    //   d   = double    (8)   ask
    //   d   = double    (8)   mid
    //   32x = padding   (32)
    //   Total = 64 bytes
    // ---------------------------------------------------------------------------
    struct alignas(64) BtcTick
    {
        uint64_t timestamp; // Unix epoch milliseconds (local clock)
        double   bid;       // Coinbase BTC-USD best bid price
        double   ask;       // Coinbase BTC-USD best ask price
        double   mid;       // (bid + ask) / 2.0
        char     _pad[32];  // pad to one cache line
    };

    // Compile-time layout guards.
    static_assert(sizeof(BtcTick) == 64,
                  "BtcTick size changed — update layout diagram and Python struct fmt.");
    static_assert(alignof(BtcTick) == 64,
                  "BtcTick alignment changed.");

} // namespace polymarket::core
