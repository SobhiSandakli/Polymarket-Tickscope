#pragma once
// StrategyEngine.hpp
//
// ConvergenceNo strategy — header-only (logic is O(1) and short).
//
// Entry  : YES_mid <= THRESHOLD  AND  NO_ask <= MAX_ENTRY_ASK
// Exit   : NO_mid  >= EXIT_THRESHOLD
// Sizing : half-Kelly based on backtest win rate (74.9% NO wins)
//
// on_tick() is called from the FeedHandler (IXWebSocket I/O thread) every
// time a market's book is updated.  It is O(1): one hash lookup + a handful
// of comparisons + (rarely) a virtual call to OrderGateway.

#include "BotConfig.hpp"
#include "BookState.hpp"
#include "OrderGateway.hpp"

#include <spdlog/spdlog.h>

#include <algorithm> // std::clamp
#include <chrono>
#include <cmath> // std::round, std::max
#include <string>
#include <vector>

namespace bot
{

    class StrategyEngine
    {
    public:
        StrategyEngine(BookState &books,
                       OrderGateway &gateway,
                       double threshold = THRESHOLD,
                       double exit_thresh = EXIT_THRESHOLD,
                       double max_entry_ask = MAX_ENTRY_ASK)
            : books_(books), gateway_(gateway),
              threshold_(threshold), exit_thresh_(exit_thresh),
              max_entry_ask_(max_entry_ask)
        {
        }

        // Called from the WS I/O thread.  Evaluates entry/exit for one market.
        void on_tick(const std::string &cid)
        {
            MarketBook bk;
            if (!books_.get_book(cid, bk))
                return;

            const int64_t now = now_ms();

            if (!gateway_.has_position(cid))
            {
                // ── ENTRY ──────────────────────────────────────────────────
                if (!bk.yes_valid() || !bk.no_valid())
                    return;
                if (bk.yes_mid() > threshold_)
                    return;
                if (bk.no_ask > max_entry_ask_)
                    return;

                // Don't enter markets about to expire
                MarketMeta meta;
                if (books_.get_meta(cid, meta) && meta.end_time_ms > 0)
                {
                    if (now + static_cast<int64_t>(MIN_REMAINING_S) * 1000 >= meta.end_time_ms)
                        return;
                }

                const double fill_px = bk.no_ask * (1.0 + SLIPPAGE);
                if (gateway_.available_capital() < fill_px * KELLY_MIN_SH)
                    return;

                const double shares = kelly_size(fill_px, gateway_.available_capital());

                if (!meta.condition_id.empty())
                    gateway_.buy(cid, meta.question, shares, fill_px,
                                 bk.yes_mid(), bk.no_ask);
                else
                {
                    books_.get_meta(cid, meta);
                    gateway_.buy(cid, meta.question, shares, fill_px,
                                 bk.yes_mid(), bk.no_ask);
                }
            }
            else
            {
                // ── EXIT (price-based) ─────────────────────────────────────
                if (!bk.no_valid())
                    return;

                bool should_exit = (bk.no_mid() >= exit_thresh_);

                // ── EXIT (time-based) ──────────────────────────────────────
                // Force-close if market is within EXPIRY_GRACE_S of expiry
                if (!should_exit)
                {
                    MarketMeta meta;
                    if (books_.get_meta(cid, meta) && meta.end_time_ms > 0)
                    {
                        if (now + static_cast<int64_t>(EXPIRY_GRACE_S) * 1000 >= meta.end_time_ms)
                        {
                            should_exit = true;
                            spdlog::info("[strategy] force-exit (expiry) \"{}\"", meta.question);
                        }
                    }
                }

                if (!should_exit)
                    return;

                const auto pos_map = gateway_.positions(); // snapshot copy
                auto it = pos_map.find(cid);
                if (it == pos_map.end())
                    return;

                const double fill_px = bk.no_bid * (1.0 - SLIPPAGE);
                gateway_.sell(cid, it->second.shares, fill_px, bk.no_bid);
            }
        }

        // Called from the main loop every REDISCOVER_S seconds.
        // Sweeps ALL open positions and force-closes any whose market has expired.
        // Returns number of positions force-closed.
        int sweep_expired()
        {
            const int64_t now = now_ms();
            const int64_t grace_ms = static_cast<int64_t>(EXPIRY_GRACE_S) * 1000;

            // Take ONE snapshot of positions (a copy — see OrderGateway::positions()).
            // Iterating a live reference here while sell() erases from it — or while
            // the WS thread mutates it — was the original bug.
            const auto positions = gateway_.positions();

            // Collect CIDs to close (can't sell while iterating).
            std::vector<std::string> expired_cids;

            for (const auto &[cid, pos] : positions)
            {
                (void)pos;
                MarketMeta meta;
                if (!books_.get_meta(cid, meta))
                    continue;
                if (meta.end_time_ms <= 0)
                    continue;
                if (now + grace_ms >= meta.end_time_ms)
                    expired_cids.push_back(cid);
            }

            for (const auto &cid : expired_cids)
            {
                auto it = positions.find(cid);
                if (it == positions.end())
                    continue;

                MarketBook bk;
                double exit_px = it->second.entry_price * 0.5; // worst-case fallback
                if (books_.get_book(cid, bk) && bk.no_bid > 0.0)
                    exit_px = bk.no_bid * (1.0 - SLIPPAGE);

                spdlog::info("[strategy] sweep-close (expired) \"{}\""
                             " px={:.4f}",
                             it->second.question, exit_px);
                gateway_.sell(cid, it->second.shares, exit_px, exit_px);
            }

            return static_cast<int>(expired_cids.size());
        }

    private:
        BookState &books_;
        OrderGateway &gateway_;
        double threshold_;
        double exit_thresh_;
        double max_entry_ask_;

        static int64_t now_ms() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        // Kelly fraction sizing — identical to the backtest KellySizer.
        // b = payoff-to-risk ratio, f* = edge / b, clamped to half-Kelly max.
        static double kelly_size(double fill_price, double available) noexcept
        {
            const double b = (1.0 - fill_price) / fill_price;
            double f = KELLY_WIN_RATE - (1.0 - KELLY_WIN_RATE) / b;
            f = std::clamp(f * KELLY_FRACTION, 0.0, KELLY_MAX_PCT);
            const double n = f * available / fill_price;
            return std::max(KELLY_MIN_SH, std::round(n * 10.0) / 10.0);
        }
    };

} // namespace bot
