// MarketDiscovery.cpp
//
// Boot-sequence HTTP client for the Polymarket Gamma REST API.
// httplib.h is included here and ONLY here — never in any public header.
#include <httplib.h>

#include "polymarket/gateway/MarketDiscovery.hpp"

#include <simdjson.h>
#include <spdlog/spdlog.h>

#include <algorithm> // std::min
#include <cerrno>    // errno
#include <cstdio>    // std::fopen, std::fprintf, std::fclose, std::fgets, std::rename
#include <cstring>   // std::strlen, std::strchr, std::atoi, std::strerror
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace polymarket::gateway
{

    // ---------------------------------------------------------------------------
    // Constants
    // ---------------------------------------------------------------------------
    static constexpr const char *GAMMA_HOST = "gamma-api.polymarket.com";
    static constexpr int GAMMA_PORT = 443;
    static constexpr int PAGE_SIZE = 200;
    static constexpr int MAX_PAGES = 25;
    static constexpr int CONNECT_TIMEOUT = 10;
    static constexpr int READ_TIMEOUT = 30;

    // ---------------------------------------------------------------------------
    // csv_escape_question
    //
    // Wraps the string in double-quotes and escapes any internal double-quotes
    // by doubling them (RFC 4180 CSV standard).
    // Called once per token at startup — allocating a string here is fine.
    // ---------------------------------------------------------------------------
    static std::string csv_escape_question(const std::string &s)
    {
        std::string out;
        out.reserve(s.size() + 2);
        out += '"';
        for (char c : s)
        {
            if (c == '"')
                out += '"'; // escape by doubling
            out += c;
        }
        out += '"';
        return out;
    }

    // ---------------------------------------------------------------------------
    // parse_clob_token_ids
    //
    // The Gamma API encodes clobTokenIds as a JSON string containing a JSON
    // array, e.g.:  "[\"yes_token_id\", \"no_token_id\"]"
    // Index 0 → "YES", index 1 → "NO" (Polymarket convention).
    //
    // For each token found, constructs a full TokenMeta and appends to `out`.
    // ---------------------------------------------------------------------------
    static void parse_clob_token_ids(
        std::string_view raw_str,
        simdjson::ondemand::parser &inner_parser,
        const std::string &condition_id,
        const std::string &question,
        const std::string &end_date,
        bool active,
        int taker_fee_bps,
        std::vector<TokenMeta> &out)
    {
        simdjson::padded_string padded(raw_str);
        auto inner_doc = inner_parser.iterate(padded);

        simdjson::ondemand::array arr;
        if (inner_doc.get_array().get(arr) != simdjson::SUCCESS)
            return;

        int idx = 0;
        for (auto elem : arr)
        {
            std::string_view sv;
            if (elem.get_string().get(sv) == simdjson::SUCCESS && !sv.empty())
            {
                TokenMeta meta;
                meta.token_id = std::string(sv);
                meta.condition_id = condition_id;
                meta.outcome = (idx == 0) ? "YES" : "NO";
                meta.question = question;
                meta.end_date = end_date;
                meta.active = active;
                meta.taker_fee_bps = taker_fee_bps;
                out.push_back(std::move(meta));
            }
            ++idx;
        }
    }

    // ---------------------------------------------------------------------------
    // fetch_active_tokens
    //
    // Paginates GET /markets?closed=false&active=true&order=volume24hr.
    // Extracts full TokenMeta for every CLOB token found.
    // ---------------------------------------------------------------------------
    [[nodiscard]] std::vector<TokenMeta> fetch_active_tokens()
    {
        std::vector<TokenMeta> metadata;

        httplib::SSLClient cli(GAMMA_HOST, GAMMA_PORT);
        cli.set_connection_timeout(CONNECT_TIMEOUT);
        cli.set_read_timeout(READ_TIMEOUT);
        cli.enable_server_certificate_verification(false);

        simdjson::ondemand::parser outer_parser;
        simdjson::ondemand::parser inner_parser;

        int offset = 0;

        for (int page = 0; page < MAX_PAGES; ++page)
        {
            std::string path =
                "/markets?closed=false&active=true"
                "&order=volume24hr"
                "&limit=" +
                std::to_string(PAGE_SIZE) +
                "&offset=" + std::to_string(offset);

            spdlog::info("[market-discovery] GET https://{}{}", GAMMA_HOST, path);

            auto res = cli.Get(path);

            if (!res)
            {
                spdlog::error("[market-discovery] HTTP request failed at offset={}", offset);
                break;
            }
            if (res->status != 200)
            {
                spdlog::error("[market-discovery] HTTP {} at offset={} — body: {:.120}",
                              res->status, offset, res->body);
                break;
            }

            simdjson::padded_string body_padded(res->body);
            auto doc = outer_parser.iterate(body_padded);

            simdjson::ondemand::array markets;
            if (doc.get_array().get(markets) != simdjson::SUCCESS)
            {
                spdlog::error("[market-discovery] Page {} is not a JSON array", page + 1);
                break;
            }

            int page_count = 0;

            for (auto market_val : markets)
            {
                simdjson::ondemand::object market_obj;
                if (market_val.get_object().get(market_obj) != simdjson::SUCCESS)
                    continue;

                // ── Extract all scalar fields first, then the nested clobTokenIds ──

                std::string condition_id;
                {
                    std::string_view sv;
                    if (market_obj["conditionId"].get_string().get(sv) == simdjson::SUCCESS)
                        condition_id = std::string(sv);
                }

                std::string question;
                {
                    std::string_view sv;
                    if (market_obj["question"].get_string().get(sv) == simdjson::SUCCESS)
                        question = std::string(sv);
                }

                std::string end_date;
                {
                    std::string_view sv;
                    if (market_obj["endDateIso"].get_string().get(sv) == simdjson::SUCCESS)
                        end_date = std::string(sv);
                }

                bool active = false;
                simdjson::error_code active_err =
                    market_obj["active"].get_bool().get(active);
                (void)active_err; // default false on error — intentionally ignored

                // takerBaseFee is a JSON number (e.g. 0 or 0.02).
                // Convert to basis points: 0.02 → 200 bps.
                int taker_fee_bps = 0;
                {
                    double fee = 0.0;
                    if (market_obj["takerBaseFee"].get_double().get(fee) == simdjson::SUCCESS)
                        taker_fee_bps = static_cast<int>(fee * 10000.0 + 0.5);
                }

                // ── clobTokenIds — JSON string containing a JSON array ─────────
                std::string_view clob_str;
                if (market_obj["clobTokenIds"].get_string().get(clob_str) == simdjson::SUCCESS)
                {
                    parse_clob_token_ids(
                        clob_str, inner_parser,
                        condition_id, question, end_date,
                        active, taker_fee_bps,
                        metadata);
                }

                ++page_count;
            }

            spdlog::info("[market-discovery] Page {} — {} markets, {} tokens so far",
                         page + 1, page_count, metadata.size());

            if (page_count < PAGE_SIZE)
                break; // last page
            offset += PAGE_SIZE;
        }

        spdlog::info("[market-discovery] Discovery complete — {} active CLOB tokens",
                     metadata.size());
        return metadata;
    }

    // ---------------------------------------------------------------------------
    // write_metadata_csv
    //
    // Writes one CSV row per token.  The question field is double-quoted and
    // internally double-quoted quotes are escaped (RFC 4180).
    // Pure std::fprintf — zero external dependencies.
    // ---------------------------------------------------------------------------
    void write_metadata_csv(
        const std::vector<TokenMeta> &metadata,
        const char *output_path)
    {
        std::FILE *f = std::fopen(output_path, "w");
        if (!f)
        {
            spdlog::error("[market-discovery] Cannot open metadata CSV for writing: {}",
                          output_path);
            return;
        }

        // Header
        std::fprintf(f,
                     "asset_id,condition_id,outcome,question,end_date,active,taker_fee_bps\n");

        for (const auto &m : metadata)
        {
            const std::string q = csv_escape_question(m.question);
            std::fprintf(f, "%s,%s,%s,%s,%s,%d,%d\n",
                         m.token_id.c_str(),
                         m.condition_id.c_str(),
                         m.outcome.c_str(),
                         q.c_str(),
                         m.end_date.c_str(),
                         m.active ? 1 : 0,
                         m.taker_fee_bps);
        }

        std::fclose(f);
        spdlog::info("[market-discovery] Metadata CSV written → {} ({} tokens)",
                     output_path, metadata.size());
    }

    // ---------------------------------------------------------------------------
    // read_csv_tokens
    //
    // Parses the CSV written by write_metadata_csv(), including RFC 4180 quoted
    // question fields (commas and doubled double-quotes inside quotes).
    // Returns an empty vector if the file does not exist (first run is fine).
    // ---------------------------------------------------------------------------
    [[nodiscard]] std::vector<TokenMeta> read_csv_tokens(const char *path)
    {
        std::vector<TokenMeta> result;

        std::FILE *f = std::fopen(path, "r");
        if (!f) return result; // first run — file does not exist yet

        char line[2048];
        bool header = true;

        while (std::fgets(line, sizeof(line), f))
        {
            if (header) { header = false; continue; } // skip header row

            // Strip trailing newline / carriage-return
            int len = static_cast<int>(std::strlen(line));
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = '\0';
            if (len == 0) continue;

            TokenMeta m;
            const char *p = line;

            // asset_id — up to first comma
            const char *comma = std::strchr(p, ',');
            if (!comma) continue;
            m.token_id.assign(p, comma - p);
            p = comma + 1;

            // condition_id — up to next comma
            comma = std::strchr(p, ',');
            if (!comma) continue;
            m.condition_id.assign(p, comma - p);
            p = comma + 1;

            // outcome — up to next comma
            comma = std::strchr(p, ',');
            if (!comma) continue;
            m.outcome.assign(p, comma - p);
            p = comma + 1;

            // question — RFC 4180 quoted field: starts with '"', ends at
            // unescaped '"'; internal '"' are doubled ("").
            if (*p == '"')
            {
                ++p; // skip opening quote
                m.question.clear();
                while (*p)
                {
                    if (*p == '"')
                    {
                        if (*(p + 1) == '"') // escaped double-quote
                        {
                            m.question += '"';
                            p += 2;
                        }
                        else
                        {
                            ++p; // closing quote
                            break;
                        }
                    }
                    else
                    {
                        m.question += *p++;
                    }
                }
                if (*p == ',') ++p; // skip comma after closing quote
            }
            else
            {
                // Unquoted question — shouldn't happen with our writer, but handle it.
                comma = std::strchr(p, ',');
                if (!comma) continue;
                m.question.assign(p, comma - p);
                p = comma + 1;
            }

            // end_date — up to next comma
            comma = std::strchr(p, ',');
            if (!comma) continue;
            m.end_date.assign(p, comma - p);
            p = comma + 1;

            // active — 0 or 1
            m.active = (*p == '1');
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;

            // taker_fee_bps — integer (rest of line)
            m.taker_fee_bps = std::atoi(p);

            result.push_back(std::move(m));
        }

        std::fclose(f);
        return result;
    }

    // ---------------------------------------------------------------------------
    // merge_and_write_metadata_csv
    //
    // Merge strategy:
    //   fresh upserts existing (fresh fields win for matching token_id).
    //   New-in-fresh tokens are appended.
    //   Tokens only in the existing CSV are preserved with active=0 (delisted).
    //
    // Atomic write: <path>.tmp → rename() → <path>  (POSIX atomic on same fs).
    // ---------------------------------------------------------------------------
    void merge_and_write_metadata_csv(
        const std::vector<TokenMeta> &fresh_tokens,
        const char *path)
    {
        // Load existing rows — empty vector on first run (file not yet created).
        std::vector<TokenMeta> existing = read_csv_tokens(path);

        // Build index: token_id → position in existing[]
        std::unordered_map<std::string, std::size_t> existing_index;
        existing_index.reserve(existing.size() * 2);
        for (std::size_t i = 0; i < existing.size(); ++i)
            existing_index[existing[i].token_id] = i;

        // Build set of fresh token IDs (for delisting detection below)
        std::unordered_set<std::string> fresh_ids;
        fresh_ids.reserve(fresh_tokens.size() * 2);
        for (const auto &m : fresh_tokens)
            fresh_ids.insert(m.token_id);

        // Upsert / insert fresh tokens
        for (const auto &m : fresh_tokens)
        {
            auto it = existing_index.find(m.token_id);
            if (it != existing_index.end())
            {
                existing[it->second] = m; // upsert: fresh fields win
            }
            else
            {
                existing_index[m.token_id] = existing.size();
                existing.push_back(m); // insert: new market
            }
        }

        // Preserve delisted tokens with active=0
        int inactive_count = 0;
        for (auto &m : existing)
        {
            if (fresh_ids.find(m.token_id) == fresh_ids.end())
            {
                m.active = false;
                ++inactive_count;
            }
        }

        // Atomic write: write to .tmp then rename
        const std::string tmp_path = std::string(path) + ".tmp";
        write_metadata_csv(existing, tmp_path.c_str());

        if (std::rename(tmp_path.c_str(), path) != 0)
        {
            spdlog::error("[market-discovery] rename({}, {}) failed: {}",
                          tmp_path, path, std::strerror(errno));
            return;
        }

        spdlog::info("[market-discovery] Metadata merged → {} total tokens, "
                     "{} active, {} preserved-inactive",
                     existing.size(), fresh_tokens.size(), inactive_count);
    }

} // namespace polymarket::gateway
