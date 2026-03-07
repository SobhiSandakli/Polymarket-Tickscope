#pragma once
// BinanceFeed.hpp
//
// Connects to the Binance BTCUSDT bookTicker WebSocket stream, parses each
// quote update with simdjson, and writes the result to a BtcJournal.
//
// Thread model
// ─────────────
//  • run() blocks the calling thread (gateway thread).
//  • IXWebSocket internally spawns one I/O thread.  The on-message callback
//    runs on that thread: JSON parsing → BtcJournal::write() all in-line.
//  • The I/O thread is pinned to core_id on the first Open event via
//    pthread_setaffinity_np(pthread_self(), ...).
//
// Hot-path properties
// ────────────────────
//  • Zero dynamic allocation per message: simdjson parser and padded buffer
//    are thread_local statics, allocated once.
//  • No std::mutex in the message path.
//  • File I/O is buffered in BtcJournal (64 KB buffer, flushed on full/rotate).
//
// Binance bookTicker message format:
//   {"u":400900217,"s":"BTCUSDT","b":"98400.50","B":"1.23","a":"98401.00","A":"0.56"}
//   Fields used: "b" (best bid), "a" (best ask).

#include "BtcJournal.hpp"

#include <atomic>
#include <string>

namespace binance
{

    class BinanceFeed
    {
    public:
        // journal  — binary file writer (BtcTick records)
        // url      — Binance WebSocket endpoint
        // core_id  — CPU core to pin the I/O thread to (-1 = no pinning)
        explicit BinanceFeed(BtcJournal &journal,
                             const char *url,
                             int core_id);
        ~BinanceFeed();

        // Non-copyable: owns an IXWebSocket connection.
        BinanceFeed(const BinanceFeed &) = delete;
        BinanceFeed &operator=(const BinanceFeed &) = delete;

        // Starts the WebSocket connection and blocks until stop() is called.
        void run();

        // Signal run() to exit.  Thread-safe; signal-handler-safe (atomic store).
        void stop() noexcept;

        // Current BTC mid price — readable from any thread (atomic).
        // Returns 0.0 if no tick has been received yet.
        [[nodiscard]] double current_mid() const noexcept
        {
            return mid_.load(std::memory_order_relaxed);
        }

    private:
        // Called from the IXWebSocket I/O thread on every message.
        void on_message(const std::string &payload);

        struct Impl;
        Impl *impl_;

        BtcJournal &journal_;
        std::string url_;
        int core_id_;
        std::atomic<bool> running_{false};
        std::atomic<double> mid_{0.0}; // latest BTC mid, readable from any thread
    };

} // namespace binance
