#include <polymarket/version.hpp>
#include <polymarket/core/Tick.hpp>
#include <polymarket/memory/RingBuffer.hpp>
#include <polymarket/tickerplant/Tickerplant.hpp>
#include <polymarket/gateway/WebSocketClient.hpp>
#include <polymarket/gateway/MarketDiscovery.hpp>

// spdlog async headers — startup code only, never in hot paths.
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>      // std::strcmp
#include <thread>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Global ring buffer — MPSC queue connecting N WebSocket I/O threads to the
// single Tickerplant consumer.  Static storage: never freed, never moved.
// ---------------------------------------------------------------------------
static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> g_ring;

// ---------------------------------------------------------------------------
// Global WebSocketClient pointer — used by the signal handler only.
// Set in main() before signal registration; never dereferenced after main().
// ---------------------------------------------------------------------------
static polymarket::gateway::WebSocketClient *g_wsc = nullptr;

// ---------------------------------------------------------------------------
// g_rediscover_running — cleared by the signal handler to stop the
// re-discovery thread within 30 s (chunked sleep interval).
// ---------------------------------------------------------------------------
static std::atomic<bool> g_rediscover_running{false};

// ---------------------------------------------------------------------------
// SIGINT / SIGTERM handler
//
// Calls wsc.stop() (relaxed atomic store) and clears g_rediscover_running so
// the re-discovery thread exits on its next 30-second chunk boundary.
// ---------------------------------------------------------------------------
static void sighandler(int /*sig*/) noexcept
{
    if (g_wsc)
        g_wsc->stop();
    g_rediscover_running.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// init_logger
//   One-time setup of the spdlog async logger.  The 8192-slot queue and the
//   background I/O thread are pre-allocated here and never touched again
//   once the trading loops start.
// ---------------------------------------------------------------------------
static void init_logger()
{
    spdlog::init_thread_pool(8192, 1);

    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    auto logger = std::make_shared<spdlog::async_logger>(
        "polymarket",
        sink,
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);

    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%^%-5l%$] [tid:%t] %v");
}

// ---------------------------------------------------------------------------
// rediscover_loop
//
// Cold-path thread: wakes every interval_hours, diffs the live market list
// against known_ids, sends incremental WS subscriptions for new tokens, and
// merges the metadata CSV.  Sleeps in 30-second chunks so Ctrl-C exits within
// 30 s even when mid-sleep.
// ---------------------------------------------------------------------------
static void rediscover_loop(
    std::atomic<bool> &running,
    polymarket::gateway::WebSocketClient &wsc,
    std::string csv_path,
    std::unordered_set<std::string> known_ids,
    int interval_mins,
    std::string filter_str)
{
    const int total_seconds = interval_mins * 60;
    constexpr int CHUNK_SECONDS = 30;

    while (running.load(std::memory_order_relaxed))
    {
        // Sleep in 30-second chunks so SIGINT exits promptly.
        int slept = 0;
        while (slept < total_seconds && running.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::seconds(CHUNK_SECONDS));
            slept += CHUNK_SECONDS;
        }

        if (!running.load(std::memory_order_relaxed))
            break;

        spdlog::info("[rediscover] Starting re-discovery cycle ...");

        auto fresh = polymarket::gateway::fetch_active_tokens();
        if (fresh.empty())
        {
            spdlog::warn("[rediscover] fetch_active_tokens() returned 0 — "
                         "skipping cycle (network issue?)");
            continue;
        }

        // Always merge full CSV before filtering — same principle as boot sequence.
        polymarket::gateway::merge_and_write_metadata_csv(fresh, csv_path.c_str());

        // Apply filter for subscription diffing only.
        const auto &subscribe_fresh = filter_str.empty()
            ? fresh
            : polymarket::gateway::filter_by_question(fresh, filter_str);

        // Diff: tokens in subscribe_fresh that aren't in known_ids.
        std::vector<std::string> new_ids;
        for (const auto &m : subscribe_fresh)
        {
            if (known_ids.find(m.token_id) == known_ids.end())
            {
                new_ids.push_back(m.token_id);
                known_ids.insert(m.token_id);
                spdlog::info("[rediscover] New token discovered: {}", m.token_id);
            }
        }

        if (!new_ids.empty())
            wsc.subscribe(new_ids);

        spdlog::info("[rediscover] Cycle complete — {} new token(s), {} total active",
                     new_ids.size(), fresh.size());
    }

    spdlog::info("[rediscover] Re-discovery thread exiting.");
}

// ---------------------------------------------------------------------------
// main
//
// Startup sequence
// ─────────────────
//  1. Init logger (spdlog async, pre-allocated queue).
//  2. Market discovery — HTTPS GET to Gamma API to fetch active token IDs.
//  3. Start Tickerplant on core 0 — drains g_ring, writes WAL journal.
//  4. Register SIGINT/SIGTERM handlers.
//  5. Create WebSocketClient on core 1 — I/O thread receives and parses ticks.
//     The IXWebSocket I/O thread receives Polymarket ticks, parses JSON,
//     and pushes Tick structs into g_ring.
//
// Shutdown sequence (SIGINT / Ctrl-C)
// ─────────────────────────────────────
//  1. Signal handler calls wsc.stop() — sets running_ = false.
//  2. run() exits its sleep-loop; calls ws.stop() to join the I/O thread.
//  3. run() returns; tp.stop() drains the ring and joins the Tickerplant.
//  4. Logger is flushed and the process exits cleanly.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// resolve_data_dir
//
// Priority: command-line arg > POLYMARKET_DATA_DIR env var > "./data"
// This lets the same binary run unmodified in the dev container, on AWS,
// or in any other environment without recompilation.
// ---------------------------------------------------------------------------
static std::string resolve_data_dir(int argc, char *argv[])
{
    if (argc >= 2)
        return argv[1];
    const char *env = std::getenv("POLYMARKET_DATA_DIR");
    if (env && env[0] != '\0')
        return env;
    return "./data";
}

int main(int argc, char *argv[])
{
    const std::string data_dir = resolve_data_dir(argc, argv);
    init_logger();

    spdlog::info("polymarket HFT  v{}.{}.{}  — starting up",
                 polymarket::VERSION_MAJOR,
                 polymarket::VERSION_MINOR,
                 polymarket::VERSION_PATCH);
    spdlog::info("Pipeline: WebSocket (core 1) → simdjson → MPSC ring[65536] "
                 "→ Tickerplant (core 0) → NVMe journal");
    spdlog::info("Data directory: {}", data_dir);

    // ── Focused market filter (optional) ─────────────────────────────────
    // Set POLYMARKET_MARKET_FILTER to a comma-separated list of substrings to
    // subscribe only to markets whose question matches at least one term.
    // Example: POLYMARKET_MARKET_FILTER="Spurs vs. Knicks"
    // Unset or empty = full firehose (default behaviour, no change).
    const std::string filter_str = []() -> std::string {
        const char *e = std::getenv("POLYMARKET_MARKET_FILTER");
        return (e && *e) ? e : "";
    }();
    if (!filter_str.empty())
        spdlog::info("Market filter active: '{}'", filter_str);

    // ── Boot sequence: discover active CLOB token IDs from Gamma API ─────
    spdlog::info("Fetching active markets from Polymarket Gamma API ...");
    auto market_meta = polymarket::gateway::fetch_active_tokens();

    // ── Write / merge metadata sidecar CSV (always full set) ─────────────
    // Written before filtering so the CSV always contains every market,
    // regardless of what the subscription filter selects.
    // Needed for DuckDB JOINs in research notebooks.
    const std::string csv_path = data_dir + "/market_metadata.csv";
    polymarket::gateway::merge_and_write_metadata_csv(
        market_meta,
        csv_path.c_str());

    // ── Apply subscription filter ─────────────────────────────────────────
    // subscribe_meta is the filtered view used for WS subscriptions only.
    // market_meta (full set) is preserved for the CSV above.
    const auto &subscribe_meta = filter_str.empty()
        ? market_meta
        : polymarket::gateway::filter_by_question(market_meta, filter_str);

    if (subscribe_meta.empty())
    {
        spdlog::warn("Subscription set is empty — WebSocket will receive no ticks. "
                     "Check POLYMARKET_MARKET_FILTER or network connectivity.");
    }
    else
    {
        spdlog::info("Subscribing to {} CLOB token(s) ({} total discovered).",
                     subscribe_meta.size(), market_meta.size());
    }

    // ── Extract plain token IDs for WebSocket subscription ───────────────
    std::vector<std::string> token_ids;
    token_ids.reserve(subscribe_meta.size());
    for (const auto &m : subscribe_meta)
        token_ids.push_back(m.token_id);

    // ── Build initial known-token set for re-discovery diffing ───────────
    std::unordered_set<std::string> known_ids;
    known_ids.reserve(subscribe_meta.size() * 2);
    for (const auto &m : subscribe_meta)
        known_ids.insert(m.token_id);

    // ── Tickerplant: drain ring → write WAL journal (core 0) ─────────────
    //
    // Journals rotate every 15 minutes into <data_dir>/polymarket_YYYYMMDD_HHMM.bin.
    // hourly_flush.sh converts closed segments to Parquet and uploads to S3.
    polymarket::tickerplant::Tickerplant tp(
        g_ring,
        data_dir.c_str(),
        /*core_id=*/0,
        /*rotation_minutes=*/15);
    tp.start();
    spdlog::info("Tickerplant started on core 0  → {} (15-min rotation)", data_dir);

    // ── WebSocket gateway (core 2) ────────────────────────────────────────
    polymarket::gateway::WebSocketClient wsc(
        g_ring,
        "wss://ws-subscriptions-clob.polymarket.com/ws/market",
        /*core_id=*/1,
        token_ids // plain IDs extracted from market_meta
    );

    // Register signal handlers AFTER wsc is fully constructed.
    g_wsc = &wsc;
    std::signal(SIGINT, sighandler);
    std::signal(SIGTERM, sighandler);

    // ── Re-discovery thread ───────────────────────────────────────────────
    // Runs every POLYMARKET_REDISCOVER_MINS minutes (default 15).
    // Not pinned to a core — this thread is cold (sleeps almost all the time).
    int rediscover_mins = 15;
    if (const char *e = std::getenv("POLYMARKET_REDISCOVER_MINS"))
        if (int v = std::atoi(e); v > 0) rediscover_mins = v;

    spdlog::info("Re-discovery thread: interval = {} minute(s)", rediscover_mins);
    g_rediscover_running.store(true, std::memory_order_relaxed);
    std::thread rediscover_thread(rediscover_loop,
        std::ref(g_rediscover_running),
        std::ref(wsc),
        csv_path,              // copied by value into the thread
        std::move(known_ids),  // moved — main no longer needs it
        rediscover_mins,
        filter_str);           // copied by value into the thread

    spdlog::info("Connecting to wss://ws-subscriptions-clob.polymarket.com/ws/market ...");
    spdlog::info("Press Ctrl-C to stop.");

    wsc.run(); // ← blocks until SIGINT or stop()

    // ── Clean shutdown ─────────────────────────────────────────────────────
    g_wsc = nullptr;
    rediscover_thread.join(); // exits within 30 s (chunked sleep)
    tp.stop();

    spdlog::info("Tickerplant stopped — {} ticks journalled",
                 tp.ticks_written());
    spdlog::info("polymarket HFT shut down cleanly.");

    spdlog::shutdown(); // drain async queue
    return EXIT_SUCCESS;
}
