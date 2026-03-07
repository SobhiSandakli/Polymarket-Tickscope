#pragma once

#include "polymarket/core/Tick.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace polymarket::rdb
{

    // ---------------------------------------------------------------------------
    // MarketState — top-of-book snapshot for one Polymarket token.
    //
    // Memory layout
    // ─────────────
    //  offset  0 │ double best_bid_price  8 B │
    //  offset  8 │ double best_bid_size   8 B │─── one cache line (64 bytes)
    //  offset 16 │ double best_ask_price  8 B │
    //  offset 24 │ double best_ask_size   8 B │
    //  offset 32 │ (tail padding)        32 B │
    //  ──────────┴─────────────────────────────
    //  sizeof(MarketState) == 64
    //
    // alignas(64) ensures every token occupies exactly one cache line, so a
    // strategy thread reading token A never evicts token B from L1/L2.
    // ---------------------------------------------------------------------------
    struct alignas(64) MarketState
    {
        double best_bid_price{0.0};
        double best_bid_size{0.0};
        double best_ask_price{0.0};
        double best_ask_size{0.0};
        // 4 × 8 = 32 bytes of payload; alignas(64) pads sizeof to 64.
    };
    static_assert(sizeof(MarketState) == 64, "MarketState must be exactly one cache line.");
    static_assert(alignof(MarketState) == 64, "MarketState must be cache-line aligned.");

    // ---------------------------------------------------------------------------
    // detail — heterogeneous hash / equality for unordered_map<string, ...>
    //
    // The key trick: declare `is_transparent` on both the hasher and the
    // comparator.  The C++14/17 standard then enables overload resolution for
    // find/count/contains that accepts *any* type for which the hasher and
    // comparator are callable — specifically std::string_view.
    //
    // Result: token_to_idx_.find(string_view{tick.asset_id}) hashes the raw
    // char* directly, with ZERO heap allocation, even though the map keys are
    // std::string.
    // ---------------------------------------------------------------------------
    namespace detail
    {

        struct SvHash
        {
            using is_transparent = void; // enable heterogeneous lookup

            size_t operator()(std::string_view sv) const noexcept
            {
                return std::hash<std::string_view>{}(sv);
            }
            size_t operator()(const std::string &s) const noexcept
            {
                return std::hash<std::string_view>{}(s);
            }
        };

        struct SvEqual
        {
            using is_transparent = void;

            bool operator()(const std::string &a, std::string_view b) const noexcept { return a == b; }
            bool operator()(std::string_view a, const std::string &b) const noexcept { return a == b; }
            bool operator()(const std::string &a, const std::string &b) const noexcept { return a == b; }
        };

    } // namespace detail

    // ---------------------------------------------------------------------------
    // OrderBook — in-memory Real-Time Database for Polymarket top-of-book state.
    //
    // Hot-path contract
    // ─────────────────
    //  • apply_tick() is called by the Tickerplant thread only (single consumer).
    //  • Zero dynamic allocation once construction is complete.
    //  • First time a token is seen: ONE std::string is constructed (cold path).
    //    Subsequent calls for the same token: pure array index + write.
    //
    // Thread safety
    // ─────────────
    //  • apply_tick()   — Tickerplant thread only.
    //  • state(idx)     — any thread (strategy, analytics).  Reads are safe
    //                     because double writes are atomic on aligned addresses on
    //                     x86-64 and ARM64 (naturally aligned 8-byte load/store).
    //    NOTE: if you need a consistent snapshot of both bid AND ask, you must
    //    add an explicit seqlock here (Phase 7+).
    //
    // Capacity
    // ────────
    //  MAX_TOKENS = 4096 → books_ array = 4096 × 64 B = 256 KB (fits in L2).
    //  token_to_idx_ is pre-reserved to MAX_TOKENS buckets at construction.
    // ---------------------------------------------------------------------------
    class OrderBook
    {
    public:
        static constexpr uint16_t MAX_TOKENS = 4096;

        // Pre-allocates hash map buckets. No further heap activity in the hot path.
        explicit OrderBook()
        {
            token_to_idx_.reserve(MAX_TOKENS);
        }

        // Non-copyable, non-movable — sits in a fixed location (Tickerplant owns it).
        OrderBook(const OrderBook &) = delete;
        OrderBook &operator=(const OrderBook &) = delete;
        OrderBook(OrderBook &&) = delete;
        OrderBook &operator=(OrderBook &&) = delete;

        // -----------------------------------------------------------------------
        // apply_tick — hot path.  Called for every tick consumed from the ring.
        //
        // Steps:
        //  1. Wrap asset_id char[] in a string_view — stack only, no heap.
        //  2. Heterogeneous find() — hashes and compares without std::string.
        //  3. On first encounter: fetch_add to claim a slot, emplace ONE string.
        //  4. Write bid or ask fields into books_[idx].
        // -----------------------------------------------------------------------
        void apply_tick(const core::Tick &tick) noexcept
        {
            std::string_view key{tick.asset_id};

            auto it = token_to_idx_.find(key);

            if (it == token_to_idx_.end())
            {
                // Cold path: new token seen for the first time.
                uint16_t idx = next_idx_.fetch_add(1, std::memory_order_relaxed);
                if (idx >= MAX_TOKENS)
                {
                    // Table full — silent drop. Should never happen in practice.
                    return;
                }
                // ONE heap alloc: construct std::string key for map storage.
                auto [inserted_it, ok] = token_to_idx_.emplace(std::string{key}, idx);
                (void)ok;
                it = inserted_it;
            }

            const uint16_t idx = it->second;
            MarketState &ms = books_[idx];

            if (tick.side == 0)
            { // BID ("BUY")
                ms.best_bid_price = tick.price;
                ms.best_bid_size = tick.size;
            }
            else
            { // ASK ("SELL")
                ms.best_ask_price = tick.price;
                ms.best_ask_size = tick.size;
            }
        }

        // -----------------------------------------------------------------------
        // Read accessors — safe to call from any thread (see class doc).
        // -----------------------------------------------------------------------
        [[nodiscard]] const MarketState &state(uint16_t idx) const noexcept
        {
            return books_[idx];
        }

        [[nodiscard]] uint16_t token_count() const noexcept
        {
            return next_idx_.load(std::memory_order_relaxed);
        }

        // Convenience: look up by token string (for tests / diagnostics only —
        // not used in the hot path).
        [[nodiscard]] bool find_index(std::string_view token, uint16_t &out_idx) const noexcept
        {
            auto it = token_to_idx_.find(token);
            if (it == token_to_idx_.end())
                return false;
            out_idx = it->second;
            return true;
        }

    private:
        // Heterogeneous map: key = std::string, lookup key = std::string_view.
        // Buckets pre-reserved in constructor; no rehash after that.
        std::unordered_map<std::string, uint16_t,
                           detail::SvHash,
                           detail::SvEqual>
            token_to_idx_;

        // Flat array of top-of-book snapshots, one cache line each.
        std::array<MarketState, MAX_TOKENS> books_{};

        // Monotonically increasing slot counter.  Only written by apply_tick()
        // (single Tickerplant thread), but declared atomic so strategy threads
        // can read token_count() without data races.
        alignas(64) std::atomic<uint16_t> next_idx_{0};
    };

} // namespace polymarket::rdb
