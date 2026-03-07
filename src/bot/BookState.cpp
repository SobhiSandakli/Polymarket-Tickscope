// BookState.cpp
#include "BookState.hpp"

#include <algorithm> // std::transform
#include <cctype>    // ::tolower

namespace bot
{

    void BookState::add_market(const MarketMeta& meta)
    {
        std::lock_guard<std::mutex> lk(mutex_);

        // Deduplication: skip if already tracked.
        if (books_.count(meta.condition_id)) return;

        books_[meta.condition_id]  = MarketBook{};
        metas_[meta.condition_id]  = meta;
        token_to_cid_[meta.yes_token_id] = meta.condition_id;
        token_to_cid_[meta.no_token_id]  = meta.condition_id;
    }

    std::string BookState::on_tick(std::string_view token_id,
                                   double bid, double ask)
    {
        std::lock_guard<std::mutex> lk(mutex_);

        const std::string key(token_id);
        auto it = token_to_cid_.find(key);
        if (it == token_to_cid_.end()) return {};

        const std::string& cid = it->second;

        auto bk_it  = books_.find(cid);
        auto meta_it = metas_.find(cid);
        if (bk_it == books_.end() || meta_it == metas_.end()) return {};

        MarketBook& bk       = bk_it->second;
        const MarketMeta& mt = meta_it->second;

        if (mt.yes_token_id == key)
        {
            bk.yes_bid = bid;
            bk.yes_ask = ask;
        }
        else
        {
            bk.no_bid = bid;
            bk.no_ask = ask;
        }

        return cid; // return by value (copy inside mutex)
    }

    bool BookState::has(std::string_view cid) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return books_.count(std::string(cid)) > 0;
    }

    bool BookState::get_book(std::string_view cid, MarketBook& out) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = books_.find(std::string(cid));
        if (it == books_.end()) return false;
        out = it->second;
        return true;
    }

    bool BookState::get_meta(std::string_view cid, MarketMeta& out) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = metas_.find(std::string(cid));
        if (it == metas_.end()) return false;
        out = it->second;
        return true;
    }

    std::size_t BookState::size() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return books_.size();
    }

} // namespace bot
