#pragma once

#include <polymarket/core/Tick.hpp>
#include <polymarket/memory/RingBuffer.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace polymarket::gateway
{

    // ---------------------------------------------------------------------------
    // WebSocketClient
    //
    // Connects to a Polymarket CLOB WebSocket feed, subscribes to the supplied
    // list of asset IDs (CLOB token IDs), and pushes every inbound price_change
    // event into the MPSC ring buffer for the Tickerplant to consume.
    //
    // Thread model
    // ─────────────
    //  • run()  blocks the calling thread until stop() is called.  The caller
    //    is the "gateway thread"; pin it to a dedicated core via core_id.
    //  • IXWebSocket internally spawns one I/O thread.  The on-message callback
    //    is invoked from that thread.  All hot-path work (JSON parsing, ring push)
    //    happens in that callback — no cross-thread lock required.
    //  • The I/O thread is pinned to core_id on the first WebSocket Open event
    //    via pthread_setaffinity_np(pthread_self(), ...) from inside the callback.
    //
    // Hot-path properties
    // ────────────────────
    //  • Zero dynamic allocation per message: the simdjson padded input buffer is
    //    a thread_local static array, allocated once per I/O thread.
    //  • No std::mutex or std::condition_variable in the message path.
    //  • The only synchronisation primitive is the RingBuffer's atomic sequence
    //    counter (fetch_add + acquire/release loads).
    //
    // Usage
    // ──────
    //  polymarket::gateway::WebSocketClient wsc(ring, url, core_id);
    //  wsc.run();   // blocks; receives and parses ticks into the ring
    //  wsc.stop();  // call from signal handler or another thread to exit
    // ---------------------------------------------------------------------------
    class WebSocketClient
    {
    public:
        // ring     — MPSC queue shared with the Tickerplant consumer.
        // url      — WebSocket endpoint (wss://...)
        // core_id  — CPU core to pin the IXWebSocket I/O thread to.
        // tokens   — CLOB token IDs to subscribe to (from fetch_active_tokens()).
        //            The subscription JSON is built once at construction time;
        //            the vector is not retained after the constructor returns.
        explicit WebSocketClient(
            memory::RingBuffer<core::Tick, 65536> &ring,
            const char *url,
            int core_id,
            const std::vector<std::string> &tokens);
        ~WebSocketClient();

        // Non-copyable: owns an IXWebSocket connection.
        WebSocketClient(const WebSocketClient &) = delete;
        WebSocketClient &operator=(const WebSocketClient &) = delete;

        // Starts the WebSocket connection and blocks until stop() is called.
        // Call this from the thread that should serve as the gateway event loop.
        void run();

        // Signal run() to exit.  Thread-safe; signal-handler-safe (atomic store).
        void stop() noexcept;

        // Sends an incremental subscription for new_tokens on the live connection.
        // Thread-safe. Also updates full_subscription_json_ so reconnects replay them.
        void subscribe(const std::vector<std::string> &new_tokens);

    private:
        struct Impl; // Defined in WebSocketClient.cpp — hides IXWebSocket headers.
        Impl *impl_;

        memory::RingBuffer<core::Tick, 65536> &ring_;
        std::string url_;
        std::string subscription_json_; // built at construction
        int core_id_;
        std::atomic<bool> running_{false};
        std::atomic<bool> connected_{false};      // set on Open, cleared on Close
        std::mutex subscription_mutex_;            // guards full_subscription_json_ (cold path only)
        std::string full_subscription_json_;       // startup + all rediscovered tokens
    };

} // namespace polymarket::gateway
