#pragma once
// BookState.hpp
//
// In-memory per-market order book for the bot.
// Tracks both legs (YES token + NO token) together under a single condition_id.
//
// Threading model:
//   add_market()            – called by BotDiscovery (main thread, cold path)
//   on_tick() / book() etc. – called by FeedHandler (WS I/O thread, hot path)
//
// A std::mutex guards ALL operations because the bot is not microsecond-
// latency-sensitive and this guarantees no data races.

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bot
{

    // ── Per-market order book (both legs) ────────────────────────────────────
    struct MarketBook
    {
        double yes_bid = 0.0;
        double yes_ask = 0.0;
        double no_bid = 0.0;
        double no_ask = 0.0;

        double yes_mid() const noexcept { return (yes_bid + yes_ask) / 2.0; }
        double no_mid() const noexcept { return (no_bid + no_ask) / 2.0; }
        bool yes_valid() const noexcept { return yes_bid > 0.0 && yes_ask > yes_bid; }
        bool no_valid() const noexcept { return no_bid > 0.0 && no_ask > no_bid; }
    };

    // ── Static metadata for each tracked market ───────────────────────────────
    struct MarketMeta
    {
        std::string condition_id;
        std::string question;
        std::string yes_token_id;
        std::string no_token_id;
        int64_t end_time_ms = 0; // epoch ms from endDateIso — 0 = unknown
    };

    // ── BookState ─────────────────────────────────────────────────────────────
    class BookState
    {
    public:
        // Cold path: called by BotDiscovery (main thread).
        // Inserts a new market.  No-op if already present (deduplication).
        void add_market(const MarketMeta &meta);

        // Hot path: called by FeedHandler (WS I/O thread).
        // Updates bid/ask for the token.  Returns the condition_id string by
        // value (copy is cheap; avoids reference invalidation under mutex).
        // Returns empty string if token is unknown.
        std::string on_tick(std::string_view token_id, double bid, double ask);

        // Read accessors (safe from any thread while mutex held).
        bool has(std::string_view cid) const;

        // Returns a COPY to avoid races after mutex release.
        // Returns false + default-constructed value if not found.
        bool get_book(std::string_view cid, MarketBook &out) const;
        bool get_meta(std::string_view cid, MarketMeta &out) const;

        // Returns number of tracked markets.
        std::size_t size() const;

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, MarketBook> books_;         // cid → book
        std::unordered_map<std::string, MarketMeta> metas_;         // cid → meta
        std::unordered_map<std::string, std::string> token_to_cid_; // token_id → cid
    };

} // namespace bot
