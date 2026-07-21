#include <polymarket/gateway/WebSocketClient.hpp>
#include <polymarket/feedhandler/Feedhandler.hpp>

// IXWebSocket — production WebSocket client; isolated to this translation unit.
// Headers never leak into polymarket/gateway/WebSocketClient.hpp.
#include <ixwebsocket/IXWebSocket.h>

#include <pthread.h> // pthread_self
#if defined(__linux__)
#include <sched.h> // cpu_set_t, CPU_ZERO, CPU_SET
#endif

#include <array>
#include <chrono>
#include <cstring> // std::memcpy, std::memset
#include <memory>  // std::make_unique
#include <string>
#include <thread> // std::this_thread::sleep_for
#include <vector>

#include <spdlog/spdlog.h>

namespace polymarket::gateway
{

    // ---------------------------------------------------------------------------
    // build_subscription_json
    //
    // Builds the Polymarket CLOB subscription JSON at startup from a list of
    // CLOB token IDs.  Called once in the WebSocketClient constructor.
    //
    // Output format: {"assets_ids":["<id1>","<id2>",...],"type":"market"}
    // ---------------------------------------------------------------------------
    static std::string build_subscription_json(
        const std::vector<std::string> &tokens)
    {
        std::string json;
        json.reserve(tokens.size() * 80 + 32); // rough pre-size; startup alloc fine

        json += R"({"assets_ids":[)";
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            if (i > 0)
                json += ',';
            json += '"';
            json += tokens[i];
            json += '"';
        }
        json += R"(],"type":"market"})";
        return json;
    }

    // Maximum WebSocket message size we will process. Multi-leg price_change
    // events (~250 B/leg) can exceed a few KB during volatile bursts — exactly
    // the moments worth capturing — so this must be generous. Matches the bot's
    // FeedHandler cap (64 KB). Oversized messages are counted and logged
    // (throttled), never dropped silently.
    static constexpr std::size_t MAX_PAYLOAD_BYTES = 64 * 1024; // 64 KB

    // Staleness watchdog: if we are connected but no message arrives for this
    // long, the TCP connection may be half-open (no Close/Error fires) — force a
    // reconnect. Also send WS pings so a dead peer is detected proactively.
    static constexpr int64_t STALE_TIMEOUT_MS = 90'000; // 90 s
    static constexpr int PING_INTERVAL_S = 30;          // matches the Coinbase feed

    static int64_t now_ms() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // simdjson requires SIMDJSON_PADDING (= 64) bytes of readable memory after
    // the end of every input buffer for SIMD vectorisation.  We allocate a
    // thread_local static buffer large enough to hold the payload plus padding.
    static constexpr std::size_t SIMDJSON_PADDING_BYTES = 64;

    // ---------------------------------------------------------------------------
    // Impl — wraps ix::WebSocket to keep IXWebSocket headers out of the public .hpp
    // ---------------------------------------------------------------------------
    struct WebSocketClient::Impl
    {
        ix::WebSocket ws;
    };

    // ---------------------------------------------------------------------------
    // Construction / Destruction
    // ---------------------------------------------------------------------------

    WebSocketClient::WebSocketClient(
        memory::RingBuffer<core::Tick, 65536> &ring,
        const char *url,
        int core_id,
        const std::vector<std::string> &tokens)
        : impl_(std::make_unique<Impl>()), ring_(ring), url_(url), subscription_json_(build_subscription_json(tokens)), core_id_(core_id)
    {
        full_subscription_json_ = subscription_json_;
        spdlog::info("[ws client] Subscription built for {} token IDs ({} bytes)",
                     tokens.size(), subscription_json_.size());
    }

    WebSocketClient::~WebSocketClient()
    {
        stop();
        // impl_ (std::unique_ptr<Impl>) is destroyed here, where Impl is complete.
    }

    // ---------------------------------------------------------------------------
    // run() — starts the WebSocket connection and blocks the calling thread.
    //
    // On entry: running_ is set to true.
    // The IXWebSocket I/O thread handles network events and calls our lambda.
    // The calling thread spins at low frequency (50 ms sleep) checking running_.
    // On stop(): running_ is set to false; ws.stop() joins the I/O thread.
    // ---------------------------------------------------------------------------
    void WebSocketClient::run()
    {
        // Do NOT re-assert running_ here. running_ defaults to true; if stop()
        // was already called (e.g. a SIGINT delivered before run() started),
        // re-asserting true would erase it and the process would ignore the
        // first Ctrl-C. Just bail out if we were already asked to stop.
        if (!running_.load(std::memory_order_relaxed))
            return;

        ix::WebSocket &ws = impl_->ws;
        ws.setUrl(url_);

        // Send WS pings so a dead/half-open peer is detected proactively rather
        // than sitting silent until kernel TCP keepalive eventually gives up.
        ws.setPingInterval(PING_INTERVAL_S);

        // ── Message callback (called from the IXWebSocket I/O thread) ────────
        ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg)
                                {
                                    if (msg->type == ix::WebSocketMessageType::Open)
                                    {
                                        spdlog::info("[ws open] connected to {}", url_);
                                        // ── Thread affinity (Linux only) ─────────────────────────────
                                        // Pin the IXWebSocket I/O thread to its dedicated CPU core.
                                        // pthread_self() here is the I/O thread — exactly what we want.
                                        // If affinity fails (container CPU restriction), the thread still
                                        // functions correctly but without cache isolation.
#if defined(__linux__)
                                        {
                                            cpu_set_t cpuset;
                                            CPU_ZERO(&cpuset);
                                            CPU_SET(core_id_, &cpuset);
                                            pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
                                        }
#endif

                                        // ── Subscription ─────────────────────────────────────────────
                                        {
                                            std::lock_guard<std::mutex> lk(subscription_mutex_);
                                            impl_->ws.send(full_subscription_json_); // replays ALL tokens on reconnect
                                        }
                                        last_msg_ms_.store(now_ms(), std::memory_order_relaxed);
                                        connected_.store(true, std::memory_order_release);
                                    }
                                    else if (msg->type == ix::WebSocketMessageType::Error)
                                    {
                                        spdlog::error("[ws error] {} (retries={})", msg->errorInfo.reason, msg->errorInfo.retries);
                                    }
                                    else if (msg->type == ix::WebSocketMessageType::Close)
                                    {
                                        connected_.store(false, std::memory_order_release);
                                        spdlog::info("[ws close] code={} reason={}", msg->closeInfo.code, msg->closeInfo.reason);
                                    }
                                    else if (msg->type == ix::WebSocketMessageType::Message)
                                    {
                                        // ── Hot path ─────────────────────────────────────────────────
                                        // Copy the raw WebSocket payload into a thread_local padded
                                        // buffer so simdjson can safely read 64 bytes past the end.
                                        //
                                        // thread_local: allocated once per I/O thread, zero dynamic
                                        // allocation per tick.
                                        static thread_local std::array<char,
                                                                       MAX_PAYLOAD_BYTES + SIMDJSON_PADDING_BYTES>
                                            tl_buf{};

                                        const std::string &payload = msg->str;

                                        // A message arrived — refresh the liveness timestamp for the
                                        // staleness watchdog (even if oversized: the peer is alive).
                                        last_msg_ms_.store(now_ms(), std::memory_order_relaxed);

                                        if (payload.size() > MAX_PAYLOAD_BYTES)
                                        {
                                            // Count the drop and warn (throttled: first, then every
                                            // 256th) so a burst of oversized frames is visible without
                                            // spamming the log. Silent drops here used to hide data loss
                                            // during exactly the volatile windows worth capturing.
                                            const uint64_t n =
                                                oversized_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
                                            if ((n & 0xFF) == 1)
                                            {
                                                spdlog::warn("[ws client] dropped oversized message "
                                                             "({} bytes > {} cap); total dropped={}",
                                                             payload.size(), MAX_PAYLOAD_BYTES, n);
                                            }
                                            return;
                                        }

                                        std::memcpy(tl_buf.data(), payload.data(), payload.size());

                                        // Zero the simdjson padding region after the payload bytes.
                                        std::memset(tl_buf.data() + payload.size(), 0,
                                                    SIMDJSON_PADDING_BYTES);

                                        const std::string_view padded{tl_buf.data(), payload.size()};
                                        feedhandler::parse_and_push(padded, ring_);
                                    }
                                    // Ping/pong frames are handled automatically by IXWebSocket;
                                    // no explicit case is required here.
                                });

        ws.start(); // non-blocking: spawns the I/O thread

        // ── Block calling thread until stop(); run the staleness watchdog ────
        while (running_.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // If we're connected but haven't heard anything for STALE_TIMEOUT_MS,
            // the connection may be half-open (a NAT/idle timeout that never
            // surfaces a Close/Error). IXWebSocket's auto-reconnect only fires on
            // an observed close, so force a fresh connect here. The pings above
            // make this rare; this is the backstop.
            if (connected_.load(std::memory_order_acquire))
            {
                const int64_t last = last_msg_ms_.load(std::memory_order_relaxed);
                const int64_t elapsed = now_ms() - last;
                if (last > 0 && elapsed > STALE_TIMEOUT_MS)
                {
                    spdlog::warn("[ws client] no message for {} ms — forcing "
                                 "reconnect (stale/half-open connection)",
                                 elapsed);
                    connected_.store(false, std::memory_order_release);
                    last_msg_ms_.store(now_ms(), std::memory_order_relaxed); // avoid repeat-fire
                    ws.stop();  // joins the current I/O thread
                    ws.start(); // spawns a fresh one → Open → re-subscribe
                }
            }
        }

        ws.stop(); // signals and joins the I/O thread
    }

    // ---------------------------------------------------------------------------
    // stop() — signal-handler-safe: only performs a relaxed atomic store.
    // ---------------------------------------------------------------------------
    void WebSocketClient::stop() noexcept
    {
        running_.store(false, std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------------------
    // subscribe() — cold path: send incremental subscription for new tokens.
    //
    // Thread-safe: subscription_mutex_ guards full_subscription_json_.
    // The hot-path Message callback never touches full_subscription_json_.
    // ---------------------------------------------------------------------------
    void WebSocketClient::subscribe(const std::vector<std::string> &new_tokens)
    {
        if (new_tokens.empty()) return;

        // Build incremental JSON (only new tokens) for immediate send.
        const std::string incremental = build_subscription_json(new_tokens);

        // Update full_subscription_json_ so the next reconnect replays them too.
        {
            std::lock_guard<std::mutex> lk(subscription_mutex_);
            // Inject new IDs before the closing ] of the assets_ids array.
            auto pos = full_subscription_json_.rfind(']');
            if (pos != std::string::npos)
            {
                // If the array is currently empty ("[]"), the first token must
                // NOT be prefixed with a comma, or we'd produce ["[,\"tok\"]"
                // — invalid JSON that makes the reconnect replay subscribe to
                // nothing. Reachable when the initial token list was empty
                // (e.g. POLYMARKET_MARKET_FILTER matched nothing at boot).
                const bool empty_array =
                    (pos > 0 && full_subscription_json_[pos - 1] == '[');
                std::string insert;
                for (std::size_t i = 0; i < new_tokens.size(); ++i)
                {
                    if (!(empty_array && i == 0))
                        insert += ',';
                    insert += '"';
                    insert += new_tokens[i];
                    insert += '"';
                }
                full_subscription_json_.insert(pos, insert);
            }
        }

        if (connected_.load(std::memory_order_acquire))
        {
            spdlog::info("[ws client] Subscribing to {} new token(s) ({} bytes)",
                         new_tokens.size(), incremental.size());
            impl_->ws.send(incremental);
        }
        else
        {
            spdlog::warn("[ws client] subscribe() called while disconnected — "
                         "tokens queued in full_subscription_json_ for reconnect");
        }
    }

} // namespace polymarket::gateway
