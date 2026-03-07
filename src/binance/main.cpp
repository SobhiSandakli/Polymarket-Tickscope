// main.cpp — binance_harvester entry point
//
// Connects to the Coinbase Advanced Trade ticker WebSocket and writes every
// BTC-USD quote update to a rotating binary journal.  Runs alongside the
// Polymarket harvester on the same AWS instance — the two datasets share
// the same local clock and can be joined by timestamp in analysis.
//
// Why Coinbase instead of Binance?
//   Binance.com returns HTTP 451 (geo-blocked) from all AWS US-region IPs.
//   Coinbase is US-domiciled and accessible from any AWS region.
//   BTC-USD liquidity is equivalent for our latency-arb research purposes.
//
// Threading model:
//   Main thread   : startup + signal wait
//   IXWebSocket   : I/O thread → BinanceFeed → BtcJournal (direct write)
//
// Usage:
//   ./binance_harvester [data_dir]
//
//   data_dir defaults to BINANCE_DATA_DIR env var, then ./data.
//   Journal files: btc_YYYYMMDD_HHMM.bin (rotated every 15 minutes).

#include "BinanceFeed.hpp"
#include "BtcJournal.hpp"

#include <polymarket/version.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
// Coinbase Advanced Trade public ticker — no auth required, no geo-block.
// Message format: {"type":"ticker","product_id":"BTC-USD","best_bid":"...","best_ask":"..."}
static constexpr const char *BINANCE_WS_URL =
    "wss://ws-feed.exchange.coinbase.com";
static constexpr int CORE_ID        = 1;   // CPU core for I/O thread (0-indexed; use -1 to disable)
static constexpr int ROTATION_MIN   = 15;  // journal rotation interval

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------
static binance::BinanceFeed *g_feed = nullptr;
static std::atomic<bool>     g_running{true};

static void sighandler(int /*sig*/) noexcept
{
    g_running.store(false, std::memory_order_relaxed);
    if (g_feed)
        g_feed->stop();
}

// ---------------------------------------------------------------------------
// Resolve data directory (same priority chain as Polymarket harvester)
// ---------------------------------------------------------------------------
static std::string resolve_data_dir(int argc, char *argv[])
{
    if (argc >= 2)
        return argv[1];
    const char *env = std::getenv("BINANCE_DATA_DIR");
    if (env && env[0] != '\0')
        return env;
    return "./data";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    const std::string data_dir = resolve_data_dir(argc, argv);

    // Logger.
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger  = std::make_shared<spdlog::logger>("binance", console);
    logger->set_level(spdlog::level::info);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%^%-5l%$] %v");
    spdlog::set_default_logger(logger);

    spdlog::info("binance_harvester v{}.{}.{} — starting up",
                 polymarket::VERSION_MAJOR,
                 polymarket::VERSION_MINOR,
                 polymarket::VERSION_PATCH);
    spdlog::info("Data directory : {}", data_dir);
    spdlog::info("Endpoint       : {}", BINANCE_WS_URL);
    spdlog::info("Rotation       : {} min", ROTATION_MIN);
    spdlog::info("I/O core       : {}", CORE_ID);

    // Journal.
    binance::BtcJournal journal(data_dir.c_str(), ROTATION_MIN);

    // Feed.
    binance::BinanceFeed feed(journal, BINANCE_WS_URL, CORE_ID);

    // Signal handlers — register AFTER feed is constructed.
    g_feed = &feed;
    std::signal(SIGINT,  sighandler);
    std::signal(SIGTERM, sighandler);

    spdlog::info("Press Ctrl-C to stop.");

    feed.run(); // blocks until stop()

    // Clean shutdown.
    g_feed = nullptr;
    spdlog::info("Ticks written: {}", journal.ticks_written());
    spdlog::info("binance_harvester shut down cleanly.");

    return EXIT_SUCCESS;
}
