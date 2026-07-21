// BotDiscovery.cpp
//
// Scans the Polymarket Gamma REST API for markets matching a filter string.
//
// scan_all()  – parallel: N_SCAN_WORKERS threads per batch, each fetching one
//               page over its own SSL connection using offset-based pagination.
//
// tail_poll() – sequential: re-fetches from last_offset_ to catch new markets.
//
// The Gamma API response is a top-level JSON array and uses clobTokenIds (a
// JSON-string-encoded array: "[\"yes_id\",\"no_id\"]") requiring double-parse.
// This pattern is copied from the proven harvester at
//   src/gateway/MarketDiscovery.cpp (parse_clob_token_ids / fetch_active_tokens).

// httplib pulls in <sys/socket.h> etc.; include before standard headers.
#include <httplib.h>

#include "BotDiscovery.hpp"

#include <simdjson.h>
#include <spdlog/spdlog.h>

#include <algorithm> // std::transform
#include <cctype>    // ::tolower
#include <chrono>
#include <cstring> // std::memset
#include <ctime>   // strptime, timegm
#include <string>
#include <thread>
#include <vector>

namespace bot
{

    // ─────────────────────────────────────────────────────────────────────────
    // Helpers: epoch milliseconds + ISO 8601 → epoch ms parser
    // ─────────────────────────────────────────────────────────────────────────
    static int64_t now_ms() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // Parse ISO 8601 string → epoch ms.
    // The Gamma API returns endDateIso as EITHER:
    //   "2026-03-05T19:10:00Z"  (full datetime)   — rare
    //   "2026-03-05"            (date only)        — common (5-min markets)
    // We try full datetime first, then date-only (midnight UTC).
    // Returns 0 on failure.
    static int64_t parse_iso8601_ms(std::string_view iso_str) noexcept
    {
        if (iso_str.empty())
            return 0;
        const std::string s(iso_str);
        std::tm tm{};

        // Try full datetime first: "2026-03-05T19:10:00Z"
        const char *ret = strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
        if (!ret)
        {
            // Fallback: date-only "2026-03-05" → treat as end-of-day 23:59:59 UTC
            std::memset(&tm, 0, sizeof(tm));
            ret = strptime(s.c_str(), "%Y-%m-%d", &tm);
            if (!ret)
                return 0;
            tm.tm_hour = 23;
            tm.tm_min = 59;
            tm.tm_sec = 59;
        }

        const time_t epoch = timegm(&tm);
        if (epoch < 0)
            return 0;
        return static_cast<int64_t>(epoch) * 1000;
    }

    // Returns true if the market should be skipped (already expired or expiring soon).
    static bool is_expired_or_too_soon(int64_t end_time_ms) noexcept
    {
        if (end_time_ms <= 0)
            return true; // unknown end time → SKIP (don't risk stale markets)
        const int64_t cutoff_ms = now_ms() + static_cast<int64_t>(MIN_REMAINING_S) * 1000;
        return end_time_ms <= cutoff_ms;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // BotHttpClient — pImpl wrapper keeps httplib out of the public header.
    // ─────────────────────────────────────────────────────────────────────────
    struct BotHttpClient
    {
        httplib::SSLClient client;

        BotHttpClient()
            : client(GAMMA_REST, CLOB_PORT)
        {
            client.set_connection_timeout(CONNECT_TIMEOUT_S);
            client.set_read_timeout(READ_TIMEOUT_S);
            // TLS verification left enabled (httplib default): discovery output
            // drives which markets the bot subscribes to and trades, so forged
            // responses must not be accepted. Uses the system CA bundle.
        }
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Constructor / Destructor
    // ─────────────────────────────────────────────────────────────────────────
    BotDiscovery::BotDiscovery(BookState &books, std::string filter_str)
        : books_(books),
          filter_(std::move(filter_str)),
          http_client_(std::make_unique<BotHttpClient>())
    {
        std::transform(filter_.begin(), filter_.end(), filter_.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(::tolower(c)); });
    }

    BotDiscovery::~BotDiscovery() = default;

    // ─────────────────────────────────────────────────────────────────────────
    // matches_filter — case-insensitive substring check
    // ─────────────────────────────────────────────────────────────────────────
    bool BotDiscovery::matches_filter(std::string_view question,
                                      std::string_view filter) noexcept
    {
        if (filter.empty())
            return true;

        std::string q_lower(question);
        std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(::tolower(c)); });

        return q_lower.find(filter) != std::string::npos;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // parse_clob_token_ids
    //
    // The Gamma API encodes clobTokenIds as a JSON string containing a JSON
    // array, e.g.:  "[\"yes_token_id\", \"no_token_id\"]"
    // Index 0 → YES token, Index 1 → NO token (Polymarket convention).
    //
    // Requires a second simdjson parser instance (inner_parser) because the
    // outer parser is already consuming the page-level document.
    // ─────────────────────────────────────────────────────────────────────────
    static bool parse_clob_token_ids(
        std::string_view raw_str,
        simdjson::ondemand::parser &inner_parser,
        std::string &yes_token_out,
        std::string &no_token_out)
    {
        simdjson::padded_string padded(raw_str);
        auto inner_doc = inner_parser.iterate(padded);

        simdjson::ondemand::array arr;
        if (inner_doc.get_array().get(arr) != simdjson::SUCCESS)
            return false;

        int idx = 0;
        for (auto elem : arr)
        {
            std::string_view sv;
            if (elem.get_string().get(sv) == simdjson::SUCCESS && !sv.empty())
            {
                if (idx == 0)
                    yes_token_out = std::string(sv);
                else
                    no_token_out = std::string(sv);
            }
            ++idx;
        }

        return !yes_token_out.empty() && !no_token_out.empty();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // fetch_gamma_page — fetch one page from the Gamma API.
    //
    // Uses the persistent http_client_ connection (with reconnect on failure).
    // Parses the top-level JSON array, filters by question substring, extracts
    // conditionId + clobTokenIds, and adds new markets to BookState.
    //
    // Returns the number of markets on the page (< PAGE_SIZE = last page).
    // ─────────────────────────────────────────────────────────────────────────
    int BotDiscovery::fetch_gamma_page(int offset,
                                       std::vector<std::string> &new_tokens)
    {
        const std::string path =
            "/markets?closed=false&active=true"
            "&order=volume24hr"
            "&limit=" +
            std::to_string(PAGE_SIZE) +
            "&offset=" + std::to_string(offset);

        spdlog::debug("[discovery] GET https://{}{}", GAMMA_REST, path);

        auto res = http_client_->client.Get(path);
        if (!res)
        {
            spdlog::warn("[discovery] connection lost (offset={}), reconnecting…", offset);
            http_client_ = std::make_unique<BotHttpClient>();
            res = http_client_->client.Get(path);
        }
        if (!res)
        {
            spdlog::error("[discovery] HTTP request failed after retry (offset={})", offset);
            return 0;
        }
        if (res->status != 200)
        {
            spdlog::error("[discovery] HTTP {} (offset={}) — {:.80}",
                          res->status, offset, res->body);
            return 0;
        }

        simdjson::ondemand::parser outer_parser;
        simdjson::ondemand::parser inner_parser;
        simdjson::padded_string padded(res->body);
        auto doc = outer_parser.iterate(padded);

        // Gamma API returns a top-level JSON array (not {"data": [...]})
        simdjson::ondemand::array markets;
        if (doc.get_array().get(markets) != simdjson::SUCCESS)
        {
            spdlog::error("[discovery] response at offset={} is not a JSON array", offset);
            return 0;
        }

        int page_count = 0;

        for (auto market_val : markets)
        {
            simdjson::ondemand::object market;
            if (market_val.get_object().get(market) != simdjson::SUCCESS)
                continue;

            // ── Extract fields ─────────────────────────────────────────────

            std::string_view question_sv;
            if (market["question"].get_string().get(question_sv) != simdjson::SUCCESS)
                continue;
            if (!matches_filter(question_sv, filter_))
            {
                ++page_count;
                continue;
            }

            std::string_view cid_sv;
            if (market["conditionId"].get_string().get(cid_sv) != simdjson::SUCCESS)
                continue;

            const std::string cid(cid_sv);
            if (books_.has(cid))
            {
                ++page_count;
                continue;
            }

            // ── endDateIso — skip expired / imminent markets ───────────────
            int64_t end_ms = 0;
            {
                std::string_view end_sv;
                if (market["endDateIso"].get_string().get(end_sv) == simdjson::SUCCESS)
                    end_ms = parse_iso8601_ms(end_sv);
            }
            if (is_expired_or_too_soon(end_ms))
            {
                spdlog::debug("[discovery] skipping expired/imminent cid={}", cid);
                ++page_count;
                continue;
            }

            // ── clobTokenIds — double-parse ────────────────────────────────
            std::string_view clob_str;
            if (market["clobTokenIds"].get_string().get(clob_str) != simdjson::SUCCESS)
            {
                ++page_count;
                continue;
            }

            MarketMeta meta;
            meta.condition_id = cid;
            meta.question = std::string(question_sv);
            meta.end_time_ms = end_ms;

            if (!parse_clob_token_ids(clob_str, inner_parser,
                                      meta.yes_token_id, meta.no_token_id))
            {
                spdlog::debug("[discovery] incomplete clobTokenIds for cid={}", cid);
                ++page_count;
                continue;
            }

            books_.add_market(meta);
            new_tokens.push_back(meta.yes_token_id);
            new_tokens.push_back(meta.no_token_id);
            spdlog::info("[discovery] +market \"{}\"", meta.question);

            ++page_count;
        }

        return page_count;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // fetch_gamma_page_worker — standalone version for parallel scan_all.
    //
    // Each worker creates its own BotHttpClient + parsers (no shared state).
    // Populates out_markets; returns page_count.
    // ─────────────────────────────────────────────────────────────────────────
    static int fetch_gamma_page_worker(
        int offset,
        std::vector<MarketMeta> &out_markets,
        const std::string &filter_lower)
    {
        const std::string path =
            "/markets?closed=false&active=true"
            "&order=volume24hr"
            "&limit=" +
            std::to_string(PAGE_SIZE) +
            "&offset=" + std::to_string(offset);

        // Each worker gets its own SSL connection.
        auto cli = std::make_unique<BotHttpClient>();

        constexpr int MAX_ATTEMPTS = 3;
        httplib::Result res;

        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
        {
            res = cli->client.Get(path);
            if (res && res->status == 200)
                break;

            spdlog::warn("[discovery] worker reconnecting (offset={}, attempt={}/{})",
                         offset, attempt + 1, MAX_ATTEMPTS);
            cli = std::make_unique<BotHttpClient>();
        }

        if (!res || res->status != 200)
        {
            spdlog::error("[discovery] worker failed after {} attempts (offset={})",
                          MAX_ATTEMPTS, offset);
            return -1; // signal error
        }

        simdjson::ondemand::parser outer_parser;
        simdjson::ondemand::parser inner_parser;
        simdjson::padded_string padded(res->body);
        auto doc = outer_parser.iterate(padded);

        simdjson::ondemand::array markets;
        if (doc.get_array().get(markets) != simdjson::SUCCESS)
            return -1;

        int page_count = 0;

        for (auto market_val : markets)
        {
            simdjson::ondemand::object market;
            if (market_val.get_object().get(market) != simdjson::SUCCESS)
                continue;

            std::string_view question_sv;
            if (market["question"].get_string().get(question_sv) != simdjson::SUCCESS)
                continue;

            // Always count toward page_count — even if filtered out.
            ++page_count;

            if (!BotDiscovery::matches_filter(question_sv, filter_lower))
                continue;

            std::string_view cid_sv;
            if (market["conditionId"].get_string().get(cid_sv) != simdjson::SUCCESS)
                continue;

            // ── endDateIso — skip expired / imminent markets ───────────────
            int64_t end_ms = 0;
            {
                std::string_view end_sv;
                if (market["endDateIso"].get_string().get(end_sv) == simdjson::SUCCESS)
                    end_ms = parse_iso8601_ms(end_sv);
            }
            if (is_expired_or_too_soon(end_ms))
                continue;

            std::string_view clob_str;
            if (market["clobTokenIds"].get_string().get(clob_str) != simdjson::SUCCESS)
                continue;

            MarketMeta meta;
            meta.condition_id = std::string(cid_sv);
            meta.question = std::string(question_sv);
            meta.end_time_ms = end_ms;

            if (!parse_clob_token_ids(clob_str, inner_parser,
                                      meta.yes_token_id, meta.no_token_id))
                continue;

            out_markets.push_back(std::move(meta));
        }

        return page_count;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // scan_all — parallel batch scan using offset-based pagination
    //
    // Pre-computes offsets (0, 200, 400, ...) and dispatches batches of
    // N_SCAN_WORKERS threads.  Stops when any worker's page returns fewer
    // than PAGE_SIZE markets (= last page).
    //
    // BookState merge happens sequentially after each batch joins — no extra
    // locking needed here (BookState is internally mutex-protected).
    // ─────────────────────────────────────────────────────────────────────────
    std::vector<std::string> BotDiscovery::scan_all()
    {
        spdlog::info("[discovery] Starting parallel scan "
                     "(filter=\"{}\", workers={}, gamma API)",
                     filter_, N_SCAN_WORKERS);

        struct WorkerResult
        {
            std::vector<MarketMeta> markets;
            int page_count{-1}; // -1 = error, >=0 = market count on page
            int offset{0};
        };

        std::vector<std::string> all_tokens;
        bool done = false;
        int next_offset = 0;

        while (!done && next_offset < MAX_PAGES * PAGE_SIZE)
        {
            // Don't exceed MAX_PAGES total
            const int remaining_pages = MAX_PAGES - (next_offset / PAGE_SIZE);
            const int batch_size = std::min(N_SCAN_WORKERS, remaining_pages);
            if (batch_size <= 0)
                break;

            std::vector<WorkerResult> results(batch_size);
            std::vector<std::thread> workers;
            workers.reserve(batch_size);

            for (int w = 0; w < batch_size; ++w)
            {
                const int off = next_offset + w * PAGE_SIZE;
                results[w].offset = off;

                workers.emplace_back([w, off, &results, this]()
                                     { results[w].page_count = fetch_gamma_page_worker(
                                           off, results[w].markets, filter_); });
            }

            for (auto &t : workers)
                t.join();

            // Merge results into BookState sequentially.
            for (int w = 0; w < batch_size; ++w)
            {
                for (auto &meta : results[w].markets)
                {
                    if (books_.has(meta.condition_id))
                        continue;
                    books_.add_market(meta);
                    all_tokens.push_back(meta.yes_token_id);
                    all_tokens.push_back(meta.no_token_id);
                    spdlog::info("[discovery] +market \"{}\"", meta.question);
                }

                // page_count < 0 = network error → skip (don't stop scan)
                if (results[w].page_count < 0)
                    continue;

                // page_count < PAGE_SIZE = last page → stop
                if (results[w].page_count < PAGE_SIZE)
                {
                    last_offset_ = results[w].offset;
                    done = true;
                    break;
                }
            }

            next_offset += batch_size * PAGE_SIZE;
        }

        spdlog::info("[discovery] scan_all complete — {} markets tracked, "
                     "{} tokens to subscribe",
                     books_.size(), all_tokens.size());
        return all_tokens;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // tail_poll — re-scan from last_offset_ to catch new markets (sequential)
    //
    // Uses the persistent http_client_ connection for efficiency.
    // Stops when a page returns fewer than PAGE_SIZE markets.
    // ─────────────────────────────────────────────────────────────────────────
    std::vector<std::string> BotDiscovery::tail_poll()
    {
        std::vector<std::string> new_tokens;
        int offset = last_offset_;

        for (int page = 0; page < MAX_PAGES; ++page)
        {
            const int count = fetch_gamma_page(offset, new_tokens);

            if (count < PAGE_SIZE)
            {
                last_offset_ = offset;
                break;
            }

            offset += PAGE_SIZE;
        }

        if (!new_tokens.empty())
            spdlog::info("[discovery] tail_poll found {} new token(s)", new_tokens.size());

        return new_tokens;
    }

} // namespace bot
