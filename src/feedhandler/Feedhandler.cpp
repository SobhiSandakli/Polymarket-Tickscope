#include <polymarket/feedhandler/Feedhandler.hpp>

// simdjson is included ONLY in this .cpp — never in the public header.
#include <simdjson.h>

#include <spdlog/spdlog.h>

#include <cstring>   // std::memcpy
#include <algorithm> // std::min

namespace polymarket::feedhandler
{

    // ---------------------------------------------------------------------------
    // parse_and_push — implementation
    //
    // Handles two Polymarket CLOB WebSocket event types:
    //
    // ── 1. "book" snapshot (bare JSON array) ────────────────────────────────────
    //  Emitted once per subscribed token right after subscription.
    //  Format: [{"market":"0x...","asset_id":"...","bids":[...],"asks":[...],...}]
    //  Action: silently skip — we care about live price events, not snapshots.
    //
    // ── 2. "price_change" event ──────────────────────────────────────────────────
    //  Order book level changed (resting order placed / cancelled / modified).
    //  One Tick per leg.  event_type = 0.
    //  Format:
    //    {
    //      "event_type":   "price_change",
    //      "market":       "0x...",
    //      "timestamp":    "1771776283002",     ← top-level, shared by all legs
    //      "price_changes": [
    //        {
    //          "asset_id": "46553...",
    //          "price":    "0.99",              ← string-encoded float
    //          "size":     "101709.12",         ← "0" means level removed
    //          "side":     "BUY",              ← "BUY" | "SELL"
    //          "hash":     "3a85f8...",
    //          "best_bid": "0.998",             ← current best bid after change
    //          "best_ask": "0.999"              ← current best ask after change
    //        }, ...
    //      ]
    //    }
    //
    // ── 3. "last_trade_price" event ──────────────────────────────────────────────
    //  An actual trade was executed — money changed hands on the CLOB.
    //  Flat object (NOT nested array), one event = one Tick.  event_type = 1.
    //  best_bid / best_ask are not present; they remain 0.0 in the Tick.
    //  Format:
    //    {
    //      "event_type": "last_trade_price",
    //      "asset_id":   "46553...",
    //      "market":     "0x...",
    //      "timestamp":  "1771776283002",
    //      "price":      "0.72",
    //      "size":       "500",
    //      "side":       "BUY",
    //      "fee_rate_bps": 25                  ← ignored (not stored in Tick)
    //    }
    //
    // Any other event_type or malformed JSON → silent early return (discard).
    // ---------------------------------------------------------------------------

    // ---------------------------------------------------------------------------
    // copy_asset_id — helper to memcpy a string_view into tick.asset_id safely
    // ---------------------------------------------------------------------------
    static inline void copy_asset_id(
        polymarket::core::Tick &tick,
        std::string_view sv)
    {
        const std::size_t n = std::min(sv.size(), sizeof(tick.asset_id) - 1u);
        std::memcpy(tick.asset_id, sv.data(), n);
        tick.asset_id[n] = '\0';
    }

    void parse_and_push(
        std::string_view padded_json_msg,
        memory::RingBuffer<core::Tick, 65536> &ring_buffer) noexcept
    {

        // One parser per worker thread — reused for every message, zero alloc/tick.
        thread_local simdjson::ondemand::parser tl_parser;

        const simdjson::padded_string_view psv(
            padded_json_msg,
            padded_json_msg.size() + simdjson::SIMDJSON_PADDING);

        simdjson::ondemand::document doc;
        if (tl_parser.iterate(psv).get(doc) != simdjson::SUCCESS)
            return;

        // ── Detect message type ───────────────────────────────────────────────
        simdjson::ondemand::json_type doc_type;
        if (doc.type().get(doc_type) != simdjson::SUCCESS)
            return;

        // "book" snapshots are bare JSON arrays — skip entirely.
        if (doc_type == simdjson::ondemand::json_type::array)
            return;

        simdjson::ondemand::object obj;
        if (doc.get_object().get(obj) != simdjson::SUCCESS)
            return;

        std::string_view event_type;
        if (obj["event_type"].get_string().get(event_type) != simdjson::SUCCESS)
            return;

        // ── "price_change" ───────────────────────────────────────────────────
        if (event_type == "price_change")
        {

            uint64_t timestamp_ms = 0;
            if (obj["timestamp"].get_uint64_in_string().get(timestamp_ms) != simdjson::SUCCESS)
                return;

            simdjson::ondemand::array changes;
            if (obj["price_changes"].get_array().get(changes) != simdjson::SUCCESS)
                return;

            for (simdjson::ondemand::object change : changes)
            {
                core::Tick tick{};

                tick.timestamp = timestamp_ms;
                tick.event_type = 0; // price_change

                std::string_view asset_id_sv;
                if (change["asset_id"].get_string().get(asset_id_sv) != simdjson::SUCCESS)
                    continue;
                copy_asset_id(tick, asset_id_sv);

                if (change["price"].get_double_in_string().get(tick.price) != simdjson::SUCCESS)
                    continue;

                if (change["size"].get_double_in_string().get(tick.size) != simdjson::SUCCESS)
                    continue;

                std::string_view side_sv;
                if (change["side"].get_string().get(side_sv) != simdjson::SUCCESS)
                    continue;
                tick.side = (side_sv == "BUY") ? 0u : 1u;

                // best_bid / best_ask — optional; leave 0.0 if absent.
                // Assign error_code to a named variable to satisfy [[nodiscard]].
                simdjson::error_code e1, e2;
                e1 = change["best_bid"].get_double_in_string().get(tick.best_bid);
                e2 = change["best_ask"].get_double_in_string().get(tick.best_ask);
                (void)e1;
                (void)e2;

                if (!ring_buffer.try_produce(tick)) [[unlikely]]
                {
                    // Ring is full — consumer (Tickerplant) is falling behind.
                    // Increment thread-local drop counter; log every 256 drops to
                    // avoid flooding the async spdlog queue on sustained overload.
                    static thread_local uint64_t tl_drops = 0;
                    if ((++tl_drops & 0xFFu) == 0u)
                    {
                        spdlog::warn("[feedhandler] ring overflow: {} price_change "
                                     "ticks dropped (this I/O thread)",
                                     tl_drops);
                    }
                }
            }
            return;
        }

        // ── "last_trade_price" ───────────────────────────────────────────────
        if (event_type == "last_trade_price")
        {

            core::Tick tick{};
            tick.event_type = 1; // last_trade_price
            // best_bid / best_ask remain 0.0 — not present in this event type.

            if (obj["timestamp"].get_uint64_in_string().get(tick.timestamp) != simdjson::SUCCESS)
                return;

            std::string_view asset_id_sv;
            if (obj["asset_id"].get_string().get(asset_id_sv) != simdjson::SUCCESS)
                return;
            copy_asset_id(tick, asset_id_sv);

            if (obj["price"].get_double_in_string().get(tick.price) != simdjson::SUCCESS)
                return;

            if (obj["size"].get_double_in_string().get(tick.size) != simdjson::SUCCESS)
                return;

            std::string_view side_sv;
            if (obj["side"].get_string().get(side_sv) != simdjson::SUCCESS)
                return;
            tick.side = (side_sv == "BUY") ? 0u : 1u;

            if (!ring_buffer.try_produce(tick)) [[unlikely]]
            {
                static thread_local uint64_t tl_drops = 0;
                if ((++tl_drops & 0xFFu) == 0u)
                {
                    spdlog::warn("[feedhandler] ring overflow: {} last_trade "
                                 "ticks dropped (this I/O thread)",
                                 tl_drops);
                }
            }
            return;
        }

        // Any other event_type (tick_size_change, best_bid_ask, etc.) — discard.
    }

} // namespace polymarket::feedhandler
