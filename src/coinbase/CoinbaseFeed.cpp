// CoinbaseFeed.cpp
//
// IXWebSocket-based client for the Coinbase Advanced Trade ticker stream.
// Parses JSON with simdjson.  Writes BtcTick records to BtcJournal.
//
// Why Coinbase (not Binance): Binance.com returns HTTP 451 from AWS US-region
// IPs. Coinbase's ticker is public (no auth) and sends bid/ask on every quote
// change, so it serves as the BTC reference feed.
//
// Subscription (sent on Open):
//   {"type":"subscribe","product_ids":["BTC-USD"],"channels":["ticker"]}
//
// Ticker message (fields we use):
//   {"type":"ticker","product_id":"BTC-USD","best_bid":"...","best_ask":"..."}
//
// The I/O thread is pinned to a specific CPU core on the first Open event
// to minimise context-switch latency and keep the L1 cache hot.

#include "CoinbaseFeed.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <simdjson.h>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace coinbase
{

    // ── Buffer constants ─────────────────────────────────────────────────────
    static constexpr std::size_t MAX_PAYLOAD = 4 * 1024; // ticker is ~120 bytes
    static constexpr std::size_t SIMD_PAD    = 64;

    // ── Pimpl: hides IXWebSocket from the public header ─────────────────────
    struct CoinbaseFeed::Impl
    {
        ix::WebSocket ws;
    };

    // ── Pin current thread to a CPU core ─────────────────────────────────────
    static void pin_to_core([[maybe_unused]] int core_id)
    {
#if defined(__linux__)
        if (core_id < 0)
            return;
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0)
            spdlog::info("[coinbase] I/O thread pinned to core {}", core_id);
        else
            spdlog::warn("[coinbase] Failed to pin I/O thread to core {}", core_id);
#endif
    }

    // ── Epoch milliseconds (local clock) ─────────────────────────────────────
    static uint64_t now_ms() noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    // ── Constructor / Destructor ─────────────────────────────────────────────
    CoinbaseFeed::CoinbaseFeed(BtcJournal &journal,
                             const char *url,
                             int core_id)
        : impl_(std::make_unique<Impl>()),
          journal_(journal),
          url_(url),
          core_id_(core_id)
    {
    }

    CoinbaseFeed::~CoinbaseFeed()
    {
        stop();
        // impl_ (std::unique_ptr<Impl>) is destroyed here, where Impl is complete.
    }

    // ── stop() — signal-handler-safe ─────────────────────────────────────────
    void CoinbaseFeed::stop() noexcept
    {
        running_.store(false, std::memory_order_relaxed);
    }

    // ── run() — blocks until stop() is called ────────────────────────────────
    void CoinbaseFeed::run()
    {
        // Do NOT re-assert running_ here: a stop() delivered before run() starts
        // (e.g. from a signal handler) must not be erased. running_ defaults to
        // true; stop() sets it false.
        if (!running_.load(std::memory_order_relaxed))
            return;

        auto &ws = impl_->ws;
        ws.setUrl(url_);
        ws.setHandshakeTimeout(10);

        // Ping/pong keepalive (Coinbase drops idle connections after 5 min).
        ws.setPingInterval(30);

        // Message callback — runs on the IXWebSocket I/O thread.
        ws.setOnMessageCallback(
            [this](const ix::WebSocketMessagePtr &msg)
            {
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    spdlog::info("[coinbase] Connected to {}", url_);
                    pin_to_core(core_id_);
                    // Coinbase requires an explicit subscription message.
                    impl_->ws.sendText(
                        R"({"type":"subscribe","product_ids":["BTC-USD"],"channels":["ticker"]})");
                }
                else if (msg->type == ix::WebSocketMessageType::Close)
                {
                    spdlog::warn("[coinbase] Disconnected (code={} reason={})",
                                 msg->closeInfo.code, msg->closeInfo.reason);
                }
                else if (msg->type == ix::WebSocketMessageType::Error)
                {
                    spdlog::error("[coinbase] Error: {}", msg->errorInfo.reason);
                }
                else if (msg->type == ix::WebSocketMessageType::Message)
                {
                    on_message(msg->str);
                }
            });

        // Enable auto-reconnect with exponential backoff.
        ws.enableAutomaticReconnection();
        ws.setMinWaitBetweenReconnectionRetries(1000);  // 1 second
        ws.setMaxWaitBetweenReconnectionRetries(10000); // 10 seconds

        ws.start(); // non-blocking: spawns I/O thread

        // Block here until stop() is called.
        while (running_.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        ws.stop();
    }

    // ── on_message — hot path: parse JSON, write BtcTick ────────────────────
    //
    // Coinbase sends several message types on the same channel:
    //   {"type":"subscriptions", ...}  — ack after subscribe (ignore)
    //   {"type":"ticker", "product_id":"BTC-USD", "best_bid":"...", "best_ask":"..."}
    //   {"type":"heartbeat", ...}       — keepalive (ignore)
    //
    // We only record ticker messages with valid bid/ask.
    void CoinbaseFeed::on_message(const std::string &payload)
    {
        if (payload.empty() || payload.size() > MAX_PAYLOAD)
            return;

        // Thread-local padded buffer for simdjson.
        static thread_local std::array<char, MAX_PAYLOAD + SIMD_PAD> tl_buf{};
        std::memcpy(tl_buf.data(), payload.data(), payload.size());
        std::memset(tl_buf.data() + payload.size(), 0, SIMD_PAD);

        // Thread-local parser — allocated once, zero per-message allocation.
        thread_local simdjson::ondemand::parser tl_parser;

        const simdjson::padded_string_view psv(
            tl_buf.data(), payload.size(),
            payload.size() + simdjson::SIMDJSON_PADDING);

        simdjson::ondemand::document doc;
        if (tl_parser.iterate(psv).get(doc) != simdjson::SUCCESS)
            return;

        simdjson::ondemand::object obj;
        if (doc.get_object().get(obj) != simdjson::SUCCESS)
            return;

        // Filter: only process ticker messages.
        std::string_view msg_type;
        if (obj["type"].get_string().get(msg_type) != simdjson::SUCCESS)
            return;
        if (msg_type != "ticker")
            return;

        // Parse best_bid and best_ask — string-encoded doubles.
        double bid = 0.0, ask = 0.0;
        if (obj["best_bid"].get_double_in_string().get(bid) != simdjson::SUCCESS)
            return;
        if (obj["best_ask"].get_double_in_string().get(ask) != simdjson::SUCCESS)
            return;

        if (bid <= 0.0 || ask <= 0.0)
            return;

        const double mid = (bid + ask) / 2.0;

        // Publish to atomic for cross-thread reads (e.g. strategy engine).
        mid_.store(mid, std::memory_order_relaxed);

        // Build tick and write to journal.
        polymarket::core::BtcTick tick{};
        tick.timestamp = now_ms();
        tick.bid       = bid;
        tick.ask       = ask;
        tick.mid       = mid;

        journal_.write(tick);
    }

} // namespace coinbase
