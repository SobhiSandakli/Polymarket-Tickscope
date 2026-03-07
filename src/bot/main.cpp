// main.cpp — polymarket_bot entry point
//
// Threading model:
//   Main thread   : startup, BotDiscovery::scan_all(), tail_poll loop, status
//   IXWebSocket   : I/O thread (internal) → FeedHandler → BookState → Strategy
//
// Usage:
//   ./polymarket_bot --capital 1000 [--threshold 0.40] [--exit 0.96]
//                    [--filter "up or down"] [--live --key 0x...]

// IXWebSocket headers — included here so they don't pollute public headers.
#include <ixwebsocket/IXWebSocket.h>

#include "BotConfig.hpp"
#include "BookState.hpp"
#include "BotDiscovery.hpp"
#include "FeedHandler.hpp"
#include "OrderGateway.hpp"
#include "StrategyEngine.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib> // std::atof
#include <cstring> // std::strcmp
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── Signal handler ────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

static std::string utc_now_str()
{
    const auto now = std::chrono::system_clock::now();
    const auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now_t);
#else
    gmtime_r(&now_t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

static void append_status_log(std::size_t n_markets,
                              std::size_t n_pos,
                              double cash,
                              double equity,
                              double rpnl,
                              double fees)
{
    std::FILE *f = std::fopen(bot::STATUS_LOG_FILE, "a");
    if (!f)
        return;

    std::fprintf(f,
                 "%s | status | markets=%zu | open_positions=%zu | cash=%.2f | equity=%.2f | realised_pnl=%+.2f | fees=%.2f\n",
                 utc_now_str().c_str(), n_markets, n_pos, cash, equity, rpnl, fees);
    std::fclose(f);
}

// ── CLI argument parser ───────────────────────────────────────────────────────
struct CliArgs
{
    double capital = 0.0;
    double threshold = bot::THRESHOLD;
    double exit_thresh = bot::EXIT_THRESHOLD;
    std::string filter = bot::DEFAULT_FILTER;
    bool live_mode = false;
    std::string priv_key;
    std::string api_key;
    std::string api_secret;
    std::string passphrase;
};

static void print_usage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s --capital <amount> [options]\n"
                 "  --capital  <float>   Initial capital in USDC (required)\n"
                 "  --threshold <float>  YES_mid entry threshold (default: %.2f)\n"
                 "  --exit     <float>   NO_mid  exit  threshold (default: %.2f)\n"
                 "  --filter   <string>  Market question filter  (default: \"up or down\")\n"
                 "  --live               Use LiveGateway (stub — requires --key etc.)\n"
                 "  --key      <0x...>   EVM private key (live mode)\n"
                 "  --api-key  <str>     CLOB API key (live mode)\n"
                 "  --api-secret <str>   CLOB API secret (live mode)\n"
                 "  --passphrase <str>   CLOB API passphrase (live mode)\n",
                 prog, bot::THRESHOLD, bot::EXIT_THRESHOLD);
}

static CliArgs parse_args(int argc, char **argv)
{
    CliArgs args;

    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];

        auto next = [&](const char *flag) -> const char *
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "Error: %s requires a value\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };

        if (std::strcmp(a, "--capital") == 0)
            args.capital = std::atof(next(a));
        else if (std::strcmp(a, "--threshold") == 0)
            args.threshold = std::atof(next(a));
        else if (std::strcmp(a, "--exit") == 0)
            args.exit_thresh = std::atof(next(a));
        else if (std::strcmp(a, "--filter") == 0)
            args.filter = next(a);
        else if (std::strcmp(a, "--live") == 0)
            args.live_mode = true;
        else if (std::strcmp(a, "--key") == 0)
            args.priv_key = next(a);
        else if (std::strcmp(a, "--api-key") == 0)
            args.api_key = next(a);
        else if (std::strcmp(a, "--api-secret") == 0)
            args.api_secret = next(a);
        else if (std::strcmp(a, "--passphrase") == 0)
            args.passphrase = next(a);
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0)
        {
            print_usage(argv[0]);
            std::exit(0);
        }
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", a);
            print_usage(argv[0]);
            std::exit(1);
        }
    }

    if (args.capital <= 0.0)
    {
        std::fprintf(stderr, "Error: --capital is required and must be > 0\n");
        print_usage(argv[0]);
        std::exit(1);
    }

    return args;
}

// ── Build WS subscription JSON ────────────────────────────────────────────────
static std::string build_sub_json(const std::vector<std::string> &tokens)
{
    std::string json;
    json.reserve(tokens.size() * 80 + 32);
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

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char **argv)
{
    // ── Argument parsing ──────────────────────────────────────────────────────
    const CliArgs args = parse_args(argc, argv);

    // ── Logger initialisation ─────────────────────────────────────────────────
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("bot", console_sink);
    logger->set_level(spdlog::level::info);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_default_logger(logger);

    spdlog::info("=== polymarket_bot starting ===");
    spdlog::info("capital=${:.2f}  threshold={:.2f}  exit={:.2f}  mode={}",
                 args.capital, args.threshold, args.exit_thresh,
                 args.live_mode ? "LIVE" : "paper");

    // ── Signal handlers ───────────────────────────────────────────────────────
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ── Core objects ──────────────────────────────────────────────────────────
    bot::BookState books;

    std::unique_ptr<bot::OrderGateway> gateway;
    if (args.live_mode)
    {
        spdlog::warn("LiveGateway selected — real orders WILL be sent (stub)");
        gateway = std::make_unique<bot::LiveGateway>(
            args.capital, args.api_key, args.api_secret,
            args.passphrase, args.priv_key);
    }
    else
    {
        gateway = std::make_unique<bot::PaperGateway>(
            args.capital, bot::LOG_FILE);
    }

    bot::StrategyEngine strategy(books, *gateway,
                                 args.threshold, args.exit_thresh);
    bot::FeedHandler feed(books, strategy);
    bot::BotDiscovery discovery(books, args.filter);

    // ── IXWebSocket setup ─────────────────────────────────────────────────────
    // subscribed_tokens: full list for re-subscription on reconnect.
    // Protected by a mutex because main thread modifies it while the
    // WS I/O thread may be reading it during an Open event.
    std::vector<std::string> subscribed_tokens;
    std::mutex tokens_mutex;

    ix::WebSocket ws;
    ws.setUrl(bot::CLOB_WS);
    ws.setHandshakeTimeout(15);

    // Message callback — runs on the IXWebSocket I/O thread.
    ws.setOnMessageCallback(
        [&](const ix::WebSocketMessagePtr &msg)
        {
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                spdlog::info("[ws] connected to {}", bot::CLOB_WS);
                // Re-subscribe to all known tokens on every connect/reconnect.
                std::string sub_json;
                {
                    std::lock_guard<std::mutex> lk(tokens_mutex);
                    if (!subscribed_tokens.empty())
                        sub_json = build_sub_json(subscribed_tokens);
                }
                if (!sub_json.empty())
                    ws.send(sub_json);
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                spdlog::warn("[ws] disconnected (code={} reason={})",
                             msg->closeInfo.code, msg->closeInfo.reason);
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                spdlog::error("[ws] error: {}", msg->errorInfo.reason);
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                feed.on_message(msg->str);
            }
        });

    // ── Startup discovery (walks ALL REST pages) ──────────────────────────────
    spdlog::info("[startup] Scanning CLOB REST API for \"{}\" markets...",
                 args.filter);
    {
        const std::vector<std::string> initial_tokens = discovery.scan_all();
        {
            std::lock_guard<std::mutex> lk(tokens_mutex);
            subscribed_tokens = initial_tokens;
        }
        spdlog::info("[startup] {} markets found, {} tokens to subscribe",
                     books.size(), initial_tokens.size());
    }

    // ── Start WebSocket ───────────────────────────────────────────────────────
    ws.start(); // spawns IXWebSocket I/O thread (non-blocking)

    // ── Main loop: tail_poll + status ─────────────────────────────────────────
    spdlog::info("[bot] running — Ctrl+C to stop");

    while (g_running.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(
            std::chrono::seconds(bot::REDISCOVER_S));

        if (!g_running.load(std::memory_order_relaxed))
            break;

        // ── Tail poll: check for new markets ─────────────────────────────────
        const std::vector<std::string> new_tokens = discovery.tail_poll();
        if (!new_tokens.empty())
        {
            {
                std::lock_guard<std::mutex> lk(tokens_mutex);
                for (const auto &t : new_tokens)
                    subscribed_tokens.push_back(t);
            }
            const std::string sub_json = build_sub_json(new_tokens);
            ws.send(sub_json);
            spdlog::info("[poll] subscribed to {} new token(s)", new_tokens.size());
        }

        // ── Sweep expired positions ──────────────────────────────────────────
        const int swept = strategy.sweep_expired();
        if (swept > 0)
            spdlog::info("[expiry] force-closed {} expired position(s)", swept);

        // ── Status line ───────────────────────────────────────────────────────
        const std::size_t n_markets = books.size();
        const std::size_t n_pos = gateway->positions().size();
        const double cash = gateway->available_capital();
        const double rpnl = gateway->realised_pnl();
        const double fees = gateway->total_fees();

        // Mark-to-market: sum(position.shares × current no_bid) for open positions
        double mtm = 0.0;
        for (const auto &[cid, pos] : gateway->positions())
        {
            bot::MarketBook bk;
            if (books.get_book(cid, bk) && bk.no_bid > 0.0)
                mtm += pos.shares * bk.no_bid;
            else
                mtm += pos.shares * pos.entry_price; // fallback: cost basis
        }
        const double equity = cash + mtm;

        spdlog::info("[status] {} markets | {} open positions | "
                     "cash=${:.2f} | equity=${:.2f} | realised PnL={:+.2f} | fees=${:.2f}",
                     n_markets, n_pos, cash, equity, rpnl, fees);

        append_status_log(n_markets, n_pos, cash, equity, rpnl, fees);
    }

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    spdlog::info("[bot] shutting down...");
    ws.stop();
    spdlog::info("[bot] done");
    return 0;
}
