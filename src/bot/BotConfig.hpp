#pragma once
// BotConfig.hpp
//
// All compile-time + runtime-overridable constants for the ConvergenceNo bot.
// Strategy parameters are locked from the backtest (threshold=0.40, resolution
// base rate 74.9%, half-Kelly sizing).  Operational constants mirror the prod
// HFT harvester where applicable (Gamma/CLOB endpoints, log file path).

namespace bot
{

    // ── Strategy parameters (locked from backtest) ───────────────────────────
    inline constexpr double THRESHOLD = 0.40;      // YES_mid ≤ this → entry
    inline constexpr double EXIT_THRESHOLD = 0.96; // NO_mid  ≥ this → exit
    inline constexpr double MAX_ENTRY_ASK = 0.65;  // NO_ask guard: reject if above
    inline constexpr double SLIPPAGE = 0.0005;     // 5 bps fill-price adjustment

    // ── Kelly sizing (from backtest: 179/239 resolved NO wins) ───────────────
    inline constexpr double KELLY_WIN_RATE = 0.749; // base rate
    inline constexpr double KELLY_FRACTION = 0.5;   // half-Kelly
    inline constexpr double KELLY_MAX_PCT = 0.20;   // max 20% of capital per trade
    inline constexpr double KELLY_MIN_SH = 1.0;     // minimum 1 share

    // ── Fee model (from backtest fills.py — Polymarket dynamic fee) ────────────
    // fee = fee_rate × (p × (1−p))^exponent   where p = fill price
    // Buy fee (USDC)  = shares × base × price
    // Sell fee (USDC) = shares × base
    inline constexpr double FEE_RATE = 0.25;
    inline constexpr int FEE_EXPONENT = 2;

    // ── Operational constants ─────────────────────────────────────────────────
    inline constexpr int REDISCOVER_S = 30;     // seconds between tail polls
    inline constexpr int MIN_REMAINING_S = 120; // skip markets expiring within 2 min
    inline constexpr int EXPIRY_GRACE_S = 30;   // force-close positions 30s before expiry
    inline constexpr int CLOB_PORT = 443;
    inline constexpr int CONNECT_TIMEOUT_S = 10;
    inline constexpr int READ_TIMEOUT_S = 30;
    inline constexpr int PAGE_SIZE = 200;    // markets per Gamma API page
    inline constexpr int MAX_PAGES = 25;     // safety cap: 200×25 = 5,000 markets
    inline constexpr int N_SCAN_WORKERS = 8; // parallel page fetchers during scan_all

    inline constexpr const char *GAMMA_REST = "gamma-api.polymarket.com";
    inline constexpr const char *CLOB_WS =
        "wss://ws-subscriptions-clob.polymarket.com/ws/market";
    inline constexpr const char *LOG_FILE = "paper_trades.log";
    inline constexpr const char *STATUS_LOG_FILE = "paper_status.log";
    inline constexpr const char *DEFAULT_FILTER = "up or down";

} // namespace bot
