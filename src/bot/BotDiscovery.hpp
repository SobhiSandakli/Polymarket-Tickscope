#pragma once
// BotDiscovery.hpp
//
// Gamma REST API market scanner.
//
// scan_all()  – called once at startup: walks pages in parallel using offset-
//               based pagination, returns every matching market's YES+NO token
//               IDs for WebSocket subscription.
//
// tail_poll() – called every REDISCOVER_S seconds: re-scans from last_offset_
//               to detect new markets created since the last scan.
//
// Both methods call BookState::add_market() for newly found markets and return
// the flat list of new token IDs so the caller can subscribe the WS.
//
// The Gamma API returns a top-level JSON array (not {"data":[...]}) and uses
// clobTokenIds (a JSON-string-encoded array) instead of a tokens[] sub-array.
// The double-parse pattern mirrors src/gateway/MarketDiscovery.cpp.

#include "BookState.hpp"
#include "BotConfig.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bot
{

    // Forward declaration — httplib::SSLClient is only visible in BotDiscovery.cpp
    struct BotHttpClient;

    class BotDiscovery
    {
    public:
        // filter_str: case-insensitive substring to match against question text.
        explicit BotDiscovery(BookState& books,
                              std::string filter_str = DEFAULT_FILTER);

        ~BotDiscovery();  // defined in .cpp where BotHttpClient is complete

        // Walk all REST pages from the beginning.  Returns new token IDs found.
        std::vector<std::string> scan_all();

        // Re-fetch from last_offset_ onward.  Returns new token IDs found.
        std::vector<std::string> tail_poll();

        // Case-insensitive substring check.
        static bool matches_filter(std::string_view question,
                                   std::string_view filter) noexcept;

    private:
        BookState&  books_;
        std::string filter_;        // e.g. "up or down" (already lower-cased)
        int         last_offset_{0}; // offset of the last page fetched

        // Persistent HTTPS connection — avoids TLS handshake on every page.
        std::unique_ptr<BotHttpClient> http_client_;

        // Fetch one Gamma API page at the given offset.
        // Parses markets, populates new_tokens for any newly discovered markets.
        // Returns the number of markets on the page (< PAGE_SIZE = last page).
        int fetch_gamma_page(int offset,
                             std::vector<std::string>& new_tokens);
    };

} // namespace bot
