// FeedHandler.cpp
//
// Parses Polymarket CLOB WebSocket messages and drives BookState + StrategyEngine.
//
// Two message types handled:
//
//   1. "book" snapshot (bare JSON array, sent once per token after subscription)
//      Format: [{"asset_id":"...", "bids":[{"price":"0.95","size":"100"},...],
//                "asks":[{"price":"0.96","size":"200"},...], ...}]
//      Action: extract bids[0] (best bid) and asks[0] (best ask) for each token.
//
//   2. "price_change" event (JSON object)
//      Format: {"event_type":"price_change","price_changes":[
//                 {"asset_id":"...", "best_bid":"0.95", "best_ask":"0.96", ...}
//               ]}
//      Action: use best_bid/best_ask directly — already the NBBO after the change.
//
// simdjson ondemand is used; the parser is thread_local so zero alloc per tick.
// The padded buffer is also thread_local (allocated once per WS I/O thread).

#include "FeedHandler.hpp"

// simdjson included here only, never in the public header.
#include <simdjson.h>
#include <spdlog/spdlog.h>

#include <algorithm>  // std::min
#include <array>
#include <cstring>    // std::memcpy, std::memset

namespace bot
{

    // ── Buffer constants ──────────────────────────────────────────────────────
    static constexpr std::size_t MAX_PAYLOAD  = 64 * 1024; // 64 KB
    static constexpr std::size_t SIMD_PAD     = 64;        // simdjson requirement

    // ─────────────────────────────────────────────────────────────────────────
    FeedHandler::FeedHandler(BookState& books, StrategyEngine& strategy)
        : books_(books), strategy_(strategy)
    {}

    // ─────────────────────────────────────────────────────────────────────────
    // on_message — entry point from IXWebSocket I/O thread
    // ─────────────────────────────────────────────────────────────────────────
    void FeedHandler::on_message(const std::string& payload)
    {
        if (payload.empty() || payload.size() > MAX_PAYLOAD) return;

        // Copy payload into a thread_local padded buffer for simdjson.
        static thread_local std::array<char, MAX_PAYLOAD + SIMD_PAD> tl_buf{};
        std::memcpy(tl_buf.data(), payload.data(), payload.size());
        std::memset(tl_buf.data() + payload.size(), 0, SIMD_PAD);

        const std::string_view padded(tl_buf.data(), payload.size());

        // Detect JSON type: array → book snapshot, object → event.
        thread_local simdjson::ondemand::parser tl_parser;

        const simdjson::padded_string_view psv(
            padded, padded.size() + simdjson::SIMDJSON_PADDING);

        simdjson::ondemand::document doc;
        if (tl_parser.iterate(psv).get(doc) != simdjson::SUCCESS) return;

        simdjson::ondemand::json_type dtype;
        if (doc.type().get(dtype) != simdjson::SUCCESS) return;

        if (dtype == simdjson::ondemand::json_type::array)
        {
            handle_book_snapshot(padded, payload.size());
        }
        else if (dtype == simdjson::ondemand::json_type::object)
        {
            handle_price_change(padded, payload.size());
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // handle_book_snapshot
    //
    // Processes a bare JSON array of book objects.
    // For each object: reads bids[0].price (best bid) and asks[0].price (best ask).
    // ─────────────────────────────────────────────────────────────────────────
    void FeedHandler::handle_book_snapshot(std::string_view padded_json,
                                            std::size_t len)
    {
        thread_local simdjson::ondemand::parser tl_parser;
        const simdjson::padded_string_view psv(
            padded_json, len + simdjson::SIMDJSON_PADDING);

        simdjson::ondemand::document doc;
        if (tl_parser.iterate(psv).get(doc) != simdjson::SUCCESS) return;

        simdjson::ondemand::array arr;
        if (doc.get_array().get(arr) != simdjson::SUCCESS) return;

        for (simdjson::ondemand::object snap : arr)
        {
            std::string_view asset_id;
            if (snap["asset_id"].get_string().get(asset_id) != simdjson::SUCCESS)
                continue;

            double best_bid = 0.0;
            double best_ask = 0.0;

            // bids are sorted descending — first element is best bid.
            simdjson::ondemand::array bids;
            if (snap["bids"].get_array().get(bids) == simdjson::SUCCESS)
            {
                for (simdjson::ondemand::object bid : bids)
                {
                    simdjson::error_code ec =
                        bid["price"].get_double_in_string().get(best_bid);
                    (void)ec;
                    break; // only need first element
                }
            }

            // asks are sorted ascending — first element is best ask.
            simdjson::ondemand::array asks;
            if (snap["asks"].get_array().get(asks) == simdjson::SUCCESS)
            {
                for (simdjson::ondemand::object ask : asks)
                {
                    simdjson::error_code ec =
                        ask["price"].get_double_in_string().get(best_ask);
                    (void)ec;
                    break; // only need first element
                }
            }

            if (best_bid <= 0.0 || best_ask <= 0.0) continue;

            const std::string cid = books_.on_tick(asset_id, best_bid, best_ask);
            if (!cid.empty()) strategy_.on_tick(cid);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // handle_price_change
    //
    // Processes a price_change event object.
    // Iterates price_changes array; uses best_bid/best_ask from each element.
    // ─────────────────────────────────────────────────────────────────────────
    void FeedHandler::handle_price_change(std::string_view padded_json,
                                           std::size_t len)
    {
        thread_local simdjson::ondemand::parser tl_parser;
        const simdjson::padded_string_view psv(
            padded_json, len + simdjson::SIMDJSON_PADDING);

        simdjson::ondemand::document doc;
        if (tl_parser.iterate(psv).get(doc) != simdjson::SUCCESS) return;

        simdjson::ondemand::object obj;
        if (doc.get_object().get(obj) != simdjson::SUCCESS) return;

        std::string_view event_type;
        if (obj["event_type"].get_string().get(event_type) != simdjson::SUCCESS)
            return;

        if (event_type != "price_change") return;

        simdjson::ondemand::array changes;
        if (obj["price_changes"].get_array().get(changes) != simdjson::SUCCESS)
            return;

        for (simdjson::ondemand::object change : changes)
        {
            std::string_view asset_id;
            if (change["asset_id"].get_string().get(asset_id) != simdjson::SUCCESS)
                continue;

            double best_bid = 0.0;
            double best_ask = 0.0;

            // best_bid / best_ask are string-encoded doubles.
            simdjson::error_code e1, e2;
            e1 = change["best_bid"].get_double_in_string().get(best_bid);
            e2 = change["best_ask"].get_double_in_string().get(best_ask);
            (void)e1; (void)e2;

            if (best_bid <= 0.0 || best_ask <= 0.0) continue;

            const std::string cid = books_.on_tick(asset_id, best_bid, best_ask);
            if (!cid.empty()) strategy_.on_tick(cid);
        }
    }

} // namespace bot
