#pragma once
// CoinbaseFeed.hpp
//
// Connects to the Coinbase BTC-USD ticker WebSocket stream, parses each
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
// Coinbase ticker message format:
//   {"type":"ticker","product_id":"BTC-USD","best_bid":"98400.50","best_ask":"98401.00"}
//   Fields used: "best_bid", "best_ask".

#include "BtcJournal.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace coinbase
{

    class CoinbaseFeed
    {
    public:
        // journal  — binary file writer (BtcTick records)
        // url      — Coinbase WebSocket endpoint
        // core_id  — CPU core to pin the I/O thread to (-1 = no pinning)
        explicit CoinbaseFeed(BtcJournal &journal,
                             const char *url,
                             int core_id);
        ~CoinbaseFeed();

        // Non-copyable: owns an IXWebSocket connection.
        CoinbaseFeed(const CoinbaseFeed &) = delete;
        CoinbaseFeed &operator=(const CoinbaseFeed &) = delete;

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
        std::unique_ptr<Impl> impl_;

        BtcJournal &journal_;
        std::string url_;
        int core_id_;
        // Defaults to true so a stop() delivered before run() starts (e.g. from
        // a signal handler) is not erased. run() must not re-assert this.
        std::atomic<bool> running_{true};
        std::atomic<double> mid_{0.0}; // latest BTC mid, readable from any thread
    };

} // namespace coinbase
