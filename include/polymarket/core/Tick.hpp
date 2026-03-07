#pragma once

#include <cstdint>
#include <cstddef>

namespace polymarket::core
{

    // ---------------------------------------------------------------------------
    // Tick — one normalised event from the Polymarket CLOB WebSocket feed.
    //
    // Covers two event types (distinguished by event_type field):
    //   0 = price_change   — order book level moved (best bid/ask updated)
    //   1 = last_trade_price — an actual trade was executed (money changed hands)
    //
    // Memory layout (x86-64 / ARM64, LP64 ABI)
    // ─────────────────────────────────────────
    //  offset  0 │ uint64_t timestamp    8 B │
    //  offset  8 │ double   price        8 B │
    //  offset 16 │ double   size         8 B │─── cache line 0 (bytes 0–63)
    //  offset 24 │ double   best_bid     8 B │    all numeric fields on one line
    //  offset 32 │ double   best_ask     8 B │
    //  offset 40 │ uint8_t  side         1 B │
    //  offset 41 │ uint8_t  event_type   1 B │
    //  offset 42 │ char     asset_id    80 B │─── cache line 1 (bytes 64–127)
    //  offset 122│ (tail padding)        6 B │
    //  ──────────┴──────────────────────────┤
    //  sizeof(Tick) == 128  (2 cache lines) │
    //
    // Why alignas(64)?
    // ─────────────────
    //  When a Tick is embedded in a RingBuffer Slot, the compiler adds padding
    //  between the Slot's sequence counter (8 B) and the Tick (requiring 64-byte
    //  alignment), placing the sequence counter on cache-line 0 and the entire
    //  Tick on cache-lines 1–2.  This means:
    //   • Producers never dirty the consumer's data cache-line while writing
    //     the sequence signal.
    //   • All five numeric fields (timestamp, price, size, best_bid, best_ask)
    //     plus both flag bytes land on cache-line 0 — one fetch reads the entire
    //     analytics payload without touching the asset_id cache line.
    //
    // No-dynamic-memory contract
    // ───────────────────────────
    //  Every field is a fixed-width POD.  std::string is explicitly forbidden.
    //  asset_id[80] safely holds Polymarket's ≤78-digit decimal token IDs
    //  plus a null terminator.
    //
    // Field semantics
    // ───────────────
    //  side        : 0 = BID ("BUY"),  1 = ASK ("SELL")
    //  event_type  : 0 = price_change, 1 = last_trade_price
    //  best_bid    : 0.0 for last_trade_price events (not in that message)
    //  best_ask    : 0.0 for last_trade_price events (not in that message)
    // ---------------------------------------------------------------------------
    struct alignas(64) Tick
    {
        uint64_t timestamp; // Unix epoch milliseconds
        double price;       // Probability ∈ [0.0, 1.0]
        double size;        // Order / trade quantity
        double best_bid;    // Best bid after this event  (0.0 for trades)
        double best_ask;    // Best ask after this event  (0.0 for trades)
        uint8_t side;       // 0 = BID ("BUY"), 1 = ASK ("SELL")
        uint8_t event_type; // 0 = price_change, 1 = last_trade_price
        char asset_id[80];  // Polymarket token ID: ≤78 decimal digits + '\0'
        // 6 bytes implicit tail padding to reach 128
    };

    // Compile-time layout guards.
    static_assert(sizeof(Tick) == 128, "Tick size changed — update layout diagram and Python struct fmt.");
    static_assert(alignof(Tick) == 64, "Tick alignment changed.");

} // namespace polymarket::core
