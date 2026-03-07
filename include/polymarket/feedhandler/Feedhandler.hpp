#pragma once

#include <string_view>

#include <polymarket/core/Tick.hpp>
#include <polymarket/memory/RingBuffer.hpp>

namespace polymarket::feedhandler
{

    // ---------------------------------------------------------------------------
    // parse_and_push
    //
    // Parses one Polymarket CLOB WebSocket JSON message and pushes the
    // resulting Tick into the lock-free MPSC ring buffer.
    //
    // JSON contract (Polymarket price_change event)
    // ──────────────────────────────────────────────
    //  {
    //    "asset_id"  : "<78-digit decimal token ID>",
    //    "event_type": "price_change",          // field may appear in any order
    //    "price"     : "0.52",                  // string-encoded double
    //    "side"      : "BUY" | "SELL",
    //    "size"      : "100.5",                 // string-encoded double
    //    "timestamp" : "1700000000000"          // string-encoded uint64 ms
    //  }
    //
    // PADDING REQUIREMENT — caller contract
    // ──────────────────────────────────────
    //  simdjson's ondemand parser reads up to SIMDJSON_PADDING (64) bytes past
    //  the end of the message for SIMD vectorisation.  The caller MUST ensure
    //  that the buffer backing `padded_json_msg` has at least
    //      padded_json_msg.size() + simdjson::SIMDJSON_PADDING
    //  bytes allocated.  The "padded_" prefix in the parameter name encodes
    //  this contract; violating it is undefined behaviour.
    //
    // Hot-path properties
    // ────────────────────
    //  • Zero dynamic allocation: one thread_local simdjson::ondemand::parser
    //    per calling thread.  The parser pre-allocates its internal buffer on
    //    the first call and reuses it for every subsequent message.
    //  • No locks, no exceptions (errors cause a silent early return).
    //  • One atomic fetch_add (in RingBuffer::produce) is the only
    //    synchronisation primitive used.
    //
    // Parameters
    // ──────────
    //  padded_json_msg   – string_view over a buffer with ≥64 bytes of padding.
    //  ring_buffer       – the MPSC queue shared between this worker thread and
    //                      the single Tickerplant consumer thread.
    // ---------------------------------------------------------------------------
    void parse_and_push(
        std::string_view padded_json_msg,
        memory::RingBuffer<core::Tick, 65536> &ring_buffer) noexcept;

} // namespace polymarket::feedhandler
