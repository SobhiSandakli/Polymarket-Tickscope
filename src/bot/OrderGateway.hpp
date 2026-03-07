#pragma once
// OrderGateway.hpp
//
// Abstract order gateway + concrete implementations:
//   PaperGateway  — logs to paper_trades.log, tracks positions in memory
//   LiveGateway   — stub (HTTP POST to CLOB API, EIP-712 signing TBD)
//
// The swap between paper and live is done at runtime via a unique_ptr<OrderGateway>.
// Everything above the gateway (BookState, StrategyEngine) is identical in both modes.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace bot
{

    // ── Open position ─────────────────────────────────────────────────────────
    struct Position
    {
        std::string condition_id;
        std::string question;
        double shares = 0.0;
        double entry_price = 0.0; // fill price paid (NO_ask * (1 + SLIPPAGE))
        double buy_fee = 0.0;     // total buy-side fee in USDC
        int64_t entry_time_ms = 0;
    };

    // ── Abstract gateway ──────────────────────────────────────────────────────
    class OrderGateway
    {
    public:
        virtual ~OrderGateway() = default;

        virtual void buy(const std::string &cid,
                         const std::string &question,
                         double shares,
                         double fill_price,
                         double yes_mid,
                         double no_ask) = 0;

        virtual void sell(const std::string &cid,
                          double shares,
                          double fill_price,
                          double no_bid) = 0;

        virtual bool has_position(const std::string &cid) const = 0;
        virtual double available_capital() const = 0;
        virtual double realised_pnl() const = 0;
        virtual double total_fees() const = 0;
        virtual const std::unordered_map<std::string, Position> &
        positions() const = 0;
    };

    // ── Paper gateway ─────────────────────────────────────────────────────────
    // Logs all orders to LOG_FILE.  Tracks positions in memory.
    class PaperGateway final : public OrderGateway
    {
    public:
        explicit PaperGateway(double initial_capital, const char *log_path);
        ~PaperGateway() override;

        void buy(const std::string &cid, const std::string &question,
                 double shares, double fill_price,
                 double yes_mid, double no_ask) override;

        void sell(const std::string &cid, double shares,
                  double fill_price, double no_bid) override;

        bool has_position(const std::string &cid) const override;
        double available_capital() const override;
        double realised_pnl() const override;
        double total_fees() const override;
        const std::unordered_map<std::string, Position> &
        positions() const override;

    private:
        std::string log_path_;
        double capital_; // decremented on buy, incremented on sell
        double realised_pnl_ = 0.0;
        double total_fees_ = 0.0;
        std::unordered_map<std::string, Position> positions_;

        // Append one line to the log file (opens, writes, closes).
        void log(const std::string &line) const;
    };

    // ── Live gateway (stub) ───────────────────────────────────────────────────
    // EIP-712 signing and HTTP POST to CLOB API are not yet implemented.
    // The class compiles and can be constructed; buy/sell log a warning.
    class LiveGateway final : public OrderGateway
    {
    public:
        LiveGateway(double initial_capital,
                    const std::string &api_key,
                    const std::string &api_secret,
                    const std::string &passphrase,
                    const std::string &priv_key);

        void buy(const std::string &cid, const std::string &question,
                 double shares, double fill_price,
                 double yes_mid, double no_ask) override;

        void sell(const std::string &cid, double shares,
                  double fill_price, double no_bid) override;

        bool has_position(const std::string &cid) const override;
        double available_capital() const override;
        double realised_pnl() const override;
        double total_fees() const override;
        const std::unordered_map<std::string, Position> &
        positions() const override;

    private:
        double capital_;
        double realised_pnl_ = 0.0;
        double total_fees_ = 0.0;
        std::unordered_map<std::string, Position> positions_;
        std::string api_key_;
        std::string api_secret_;
        std::string passphrase_;
        std::string priv_key_;
    };

} // namespace bot
