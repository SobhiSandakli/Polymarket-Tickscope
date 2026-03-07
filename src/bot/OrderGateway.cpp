// OrderGateway.cpp
//
// PaperGateway: in-memory position tracking + append-only log file.
// LiveGateway:  stub — buy/sell log a warning and do nothing.
#include "OrderGateway.hpp"
#include "BotConfig.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>  // std::pow
#include <cstdio> // std::fopen, std::fprintf, std::fclose
#include <ctime>  // std::gmtime, std::strftime
#include <string>

namespace bot
{

    // ── Dynamic fee rate (matching backtest fills.py) ─────────────────────────
    // base = fee_rate × (p × (1−p))^exponent
    // At p=0.60:  0.25 × (0.24)^2 = 0.0144  → 1.44%
    // At p=0.50:  0.25 × (0.25)^2 = 0.0156  → 1.56%  (maximum)
    static double dynamic_fee_rate(double price) noexcept
    {
        const double p1p = price * (1.0 - price);
        double base = 1.0;
        for (int i = 0; i < FEE_EXPONENT; ++i)
            base *= p1p;
        return FEE_RATE * base;
    }

    // ── UTC timestamp helper ──────────────────────────────────────────────────
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

    static int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // PaperGateway
    // ═══════════════════════════════════════════════════════════════════════════

    PaperGateway::PaperGateway(double initial_capital, const char *log_path)
        : log_path_(log_path), capital_(initial_capital)
    {
        spdlog::info("[paper] PaperGateway initialised — capital=${:.2f} logfile={}",
                     initial_capital, log_path);
    }

    PaperGateway::~PaperGateway() = default;

    void PaperGateway::log(const std::string &line) const
    {
        std::FILE *f = std::fopen(log_path_.c_str(), "a");
        if (!f)
        {
            spdlog::error("[paper] cannot open log file: {}", log_path_);
            return;
        }
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }

    void PaperGateway::buy(const std::string &cid,
                           const std::string &question,
                           double shares,
                           double fill_price,
                           double yes_mid,
                           double no_ask)
    {
        const double cost = shares * fill_price;
        const double fee_base = dynamic_fee_rate(fill_price);
        const double buy_fee = shares * fee_base * fill_price; // USDC (from fills.py)
        capital_ -= (cost + buy_fee);
        total_fees_ += buy_fee;

        Position pos;
        pos.condition_id = cid;
        pos.question = question;
        pos.shares = shares;
        pos.entry_price = fill_price;
        pos.buy_fee = buy_fee;
        pos.entry_time_ms = now_ms();
        positions_[cid] = pos;

        // Log line format:
        // 2026-03-03 14:32:01 UTC | BUY  NO | 14.7sh | fill=0.6903 | fee=0.43 | YES_mid=0.3812 NO_ask=0.6900 | "..."
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "%s | BUY  NO | %.1fsh | fill=%.4f | fee=%.3f | YES_mid=%.4f NO_ask=%.4f | \"%s\"",
                      utc_now_str().c_str(),
                      shares, fill_price, buy_fee, yes_mid, no_ask,
                      question.c_str());
        log(buf);

        spdlog::info("[paper] BUY  NO {:.1f}sh @ {:.4f} fee={:.3f} (YES_mid={:.4f}) \"{}\"",
                     shares, fill_price, buy_fee, yes_mid, question);
    }

    void PaperGateway::sell(const std::string &cid,
                            double shares,
                            double fill_price,
                            double no_bid)
    {
        auto it = positions_.find(cid);
        if (it == positions_.end())
        {
            spdlog::error("[paper] SELL called with no open position for {}", cid);
            return;
        }

        const Position &pos = it->second;
        const double proceeds = shares * fill_price;
        const double cost = shares * pos.entry_price;
        const double fee_base = dynamic_fee_rate(fill_price);
        const double sell_fee = shares * fee_base;                   // USDC (from fills.py)
        const double pnl = proceeds - cost - sell_fee - pos.buy_fee; // net of ALL fees

        capital_ += (proceeds - sell_fee);
        realised_pnl_ += pnl;
        total_fees_ += sell_fee;

        const std::string question = pos.question;
        const double no_mid_approx = no_bid; // approx for log (mid not passed in)

        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "%s | SELL NO | %.1fsh | fill=%.4f | NO_mid~%.4f | fee=%.3f | pnl=%+.3f | \"%s\"",
                      utc_now_str().c_str(),
                      shares, fill_price, no_mid_approx, sell_fee, pnl,
                      question.c_str());
        log(buf);

        spdlog::info("[paper] SELL NO {:.1f}sh @ {:.4f} fee={:.3f} pnl={:+.3f} \"{}\"",
                     shares, fill_price, sell_fee, pnl, question);

        positions_.erase(it);
    }

    bool PaperGateway::has_position(const std::string &cid) const
    {
        return positions_.count(cid) > 0;
    }

    double PaperGateway::available_capital() const
    {
        return capital_;
    }

    double PaperGateway::realised_pnl() const
    {
        return realised_pnl_;
    }

    double PaperGateway::total_fees() const
    {
        return total_fees_;
    }

    const std::unordered_map<std::string, Position> &PaperGateway::positions() const
    {
        return positions_;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // LiveGateway (stub)
    // ═══════════════════════════════════════════════════════════════════════════

    LiveGateway::LiveGateway(double initial_capital,
                             const std::string &api_key,
                             const std::string &api_secret,
                             const std::string &passphrase,
                             const std::string &priv_key)
        : capital_(initial_capital),
          api_key_(api_key), api_secret_(api_secret),
          passphrase_(passphrase), priv_key_(priv_key)
    {
        spdlog::warn("[live] LiveGateway is a stub — EIP-712 signing not implemented");
    }

    void LiveGateway::buy(const std::string & /*cid*/,
                          const std::string &question,
                          double shares, double fill_price,
                          double /*yes_mid*/, double /*no_ask*/)
    {
        spdlog::warn("[live] STUB buy {:.1f}sh @ {:.4f} \"{}\" — NOT SENT",
                     shares, fill_price, question);
    }

    void LiveGateway::sell(const std::string & /*cid*/,
                           double shares, double fill_price, double /*no_bid*/)
    {
        spdlog::warn("[live] STUB sell {:.1f}sh @ {:.4f} — NOT SENT",
                     shares, fill_price);
    }

    bool LiveGateway::has_position(const std::string &cid) const
    {
        return positions_.count(cid) > 0;
    }

    double LiveGateway::available_capital() const { return capital_; }
    double LiveGateway::realised_pnl() const { return realised_pnl_; }
    double LiveGateway::total_fees() const { return total_fees_; }

    const std::unordered_map<std::string, Position> &LiveGateway::positions() const
    {
        return positions_;
    }

} // namespace bot
