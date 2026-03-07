#pragma once
// FeedHandler.hpp
//
// Parses incoming Polymarket CLOB WebSocket messages and drives the bot.
//
// Handled message types:
//   "book" snapshot  (bare JSON array)  → seeds BookState with initial bid/ask
//   "price_change"   (JSON object)      → updates BookState + evaluates strategy
//
// on_message() is called exclusively from the IXWebSocket I/O thread.
// It must be non-blocking: the only I/O is file writes in PaperGateway.

#include "BookState.hpp"
#include "StrategyEngine.hpp"

#include <string>
#include <string_view>

namespace bot
{

    class FeedHandler
    {
    public:
        FeedHandler(BookState& books, StrategyEngine& strategy);

        // Entry point — called from the IXWebSocket message callback.
        void on_message(const std::string& payload);

    private:
        BookState&     books_;
        StrategyEngine& strategy_;

        // Handlers for individual message types.
        void handle_book_snapshot(std::string_view padded_json, std::size_t len);
        void handle_price_change(std::string_view padded_json,  std::size_t len);
    };

} // namespace bot
