#pragma once
// BtcJournal.hpp
//
// Writes BtcTick records to rotating binary journal files on disk.
//
// Journal naming: btc_YYYYMMDD_HHMM.bin (mirrors Polymarket harvester convention).
// Rotation: every 15 minutes by default (configurable).
//
// Called directly from the BinanceFeed WebSocket I/O thread.
// No ring buffer: single producer, single consumer (file I/O).
// Acceptable because Binance bookTicker fires ~100–1000/sec and each record
// is 64 bytes — peak throughput is ~64 KB/s, trivial for NVMe.
//
// Buffering: ticks accumulate in a fixed 64 KB buffer (1024 ticks).
// Flush triggers: buffer full, journal rotation, or shutdown.

#include <polymarket/core/BtcTick.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>

namespace binance
{

    class BtcJournal
    {
    public:
        explicit BtcJournal(const char *data_dir,
                            int rotation_minutes = 15) noexcept
            : data_dir_(data_dir),
              rotation_minutes_(rotation_minutes)
        {
        }

        ~BtcJournal()
        {
            flush();
            close_segment();
        }

        // Non-copyable, non-movable.
        BtcJournal(const BtcJournal &) = delete;
        BtcJournal &operator=(const BtcJournal &) = delete;

        // Write one tick to the journal.
        // Called from the BinanceFeed I/O thread — must be fast.
        void write(const polymarket::core::BtcTick &tick) noexcept
        {
            rotate_if_needed(tick.timestamp);

            buf_[buf_pos_++] = tick;
            ++ticks_written_;

            if (buf_pos_ >= BUF_CAP)
                flush();
        }

        [[nodiscard]] uint64_t ticks_written() const noexcept
        {
            return ticks_written_;
        }

    private:
        static constexpr std::size_t BUF_CAP = 1024; // 1024 × 64 = 64 KB

        std::string data_dir_;
        int rotation_minutes_;
        std::FILE *file_ = nullptr;
        uint64_t segment_start_ms_ = 0;
        uint64_t ticks_written_ = 0;

        polymarket::core::BtcTick buf_[BUF_CAP]{};
        std::size_t buf_pos_ = 0;

        void rotate_if_needed(uint64_t ts_ms) noexcept
        {
            if (!file_)
            {
                open_segment(ts_ms);
                return;
            }

            const uint64_t elapsed_ms =
                ts_ms > segment_start_ms_ ? ts_ms - segment_start_ms_ : 0;
            const uint64_t rotation_ms =
                static_cast<uint64_t>(rotation_minutes_) * 60 * 1000;

            if (elapsed_ms >= rotation_ms)
            {
                flush();
                close_segment();
                open_segment(ts_ms);
            }
        }

        void open_segment(uint64_t ts_ms) noexcept
        {
            // Align segment_start_ms to the rotation boundary.
            const uint64_t rotation_ms =
                static_cast<uint64_t>(rotation_minutes_) * 60 * 1000;
            segment_start_ms_ = (ts_ms / rotation_ms) * rotation_ms;

            // Build filename: btc_YYYYMMDD_HHMM.bin
            const std::time_t epoch_s =
                static_cast<std::time_t>(segment_start_ms_ / 1000);
            std::tm tm{};
            gmtime_r(&epoch_s, &tm);

            char fname[128];
            std::snprintf(fname, sizeof(fname),
                          "%s/btc_%04d%02d%02d_%02d%02d.bin",
                          data_dir_.c_str(),
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min);

            file_ = std::fopen(fname, "ab");
            if (!file_)
            {
                spdlog::error("[journal] Failed to open {}", fname);
                return;
            }

            spdlog::info("[journal] Opened segment: {}", fname);
        }

        void close_segment() noexcept
        {
            if (file_)
            {
                std::fclose(file_);
                file_ = nullptr;
            }
        }

        void flush() noexcept
        {
            if (buf_pos_ == 0 || !file_)
                return;

            std::fwrite(buf_, sizeof(polymarket::core::BtcTick),
                        buf_pos_, file_);
            std::fflush(file_);
            buf_pos_ = 0;
        }
    };

} // namespace binance
