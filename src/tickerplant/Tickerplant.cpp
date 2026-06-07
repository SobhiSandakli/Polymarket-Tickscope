#include <polymarket/tickerplant/Tickerplant.hpp>

#include <pthread.h> // pthread_self
#if defined(__linux__)
#include <sched.h> // cpu_set_t, CPU_ZERO, CPU_SET
#endif

#include <cstdio>     // std::fopen, std::fwrite, std::fclose, std::snprintf
#include <cstring>    // std::memset
#include <ctime>      // std::time_t, std::gmtime_r, std::time
#include <stdexcept>  // std::runtime_error (startup only)
#include <sys/stat.h> // mkdir

namespace polymarket::tickerplant
{

    // ---------------------------------------------------------------------------
    // Construction / Destruction
    // ---------------------------------------------------------------------------

    Tickerplant::Tickerplant(
        memory::RingBuffer<core::Tick, 65536> &ring,
        const char *journal_dir,
        int core_id,
        int rotation_minutes)
        : ring_(ring), journal_dir_(journal_dir), core_id_(core_id), rotation_interval_minutes_(rotation_minutes)
    {
        // Create the journal directory if it doesn't exist (startup-only).
        // mode 0755 — owner rwx, group/other rx.
        ::mkdir(journal_dir_.c_str(), 0755);

        // Open the first journal file based on current UTC time.
        const std::time_t now = std::time(nullptr);
        current_slot_ = time_slot(now);

        const std::string path = make_journal_path(now);
        journal_fp_ = std::fopen(path.c_str(), "ab");
        if (!journal_fp_)
        {
            throw std::runtime_error(
                "Tickerplant: failed to open journal file: " + path);
        }
    }

    Tickerplant::~Tickerplant()
    {
        stop();
        if (thread_.joinable())
        {
            thread_.join();
        }
        if (journal_fp_)
        {
            std::fclose(journal_fp_);
            journal_fp_ = nullptr;
        }
    }

    // ---------------------------------------------------------------------------
    // start() / stop()
    // ---------------------------------------------------------------------------

    void Tickerplant::start()
    {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread(&Tickerplant::run, this);
    }

    void Tickerplant::stop() noexcept
    {
        running_.store(false, std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------------------
    // force_rotate() — test-only entry point
    // ---------------------------------------------------------------------------

    void Tickerplant::force_rotate()
    {
        // Advance the slot artificially so make_journal_path produces a new name.
        // In production, time naturally advances; this is for tests only.
        ++current_slot_;
        if (journal_fp_)
        {
            std::fclose(journal_fp_);
            journal_fp_ = nullptr;
        }

        const std::time_t synthetic =
            static_cast<std::time_t>(current_slot_ * rotation_interval_minutes_ * 60);
        const std::string path = make_journal_path(synthetic);
        journal_fp_ = std::fopen(path.c_str(), "ab");
    }

    // ---------------------------------------------------------------------------
    // time_slot() — compute the current rotation window index
    // ---------------------------------------------------------------------------

    int64_t Tickerplant::time_slot(std::time_t now) const noexcept
    {
        if (rotation_interval_minutes_ <= 0)
            return 0;
        const int64_t minutes_since_epoch =
            static_cast<int64_t>(now) / 60;
        return minutes_since_epoch / rotation_interval_minutes_;
    }

    // ---------------------------------------------------------------------------
    // make_journal_path() — build "dir/polymarket_YYYYMMDD_HHMM.bin"
    //
    // Rounds HHMM down to the nearest rotation boundary so filenames are
    // deterministic and sortable.
    // ---------------------------------------------------------------------------

    std::string Tickerplant::make_journal_path(std::time_t now) const
    {
        // Round down to the rotation boundary.
        if (rotation_interval_minutes_ > 0)
        {
            const int64_t slot = time_slot(now);
            now = static_cast<std::time_t>(
                slot * rotation_interval_minutes_ * 60);
        }

        struct std::tm utc{};
        ::gmtime_r(&now, &utc);

        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "/polymarket_%04d%02d%02d_%02d%02d.bin",
                      utc.tm_year + 1900,
                      utc.tm_mon + 1,
                      utc.tm_mday,
                      utc.tm_hour,
                      utc.tm_min);

        return journal_dir_ + buf;
    }

    // ---------------------------------------------------------------------------
    // rotate() — close the current journal, open the next time window's file
    //
    // Called once every rotation_interval_minutes_ — cold path.  Allocates a
    // std::string for the filename (acceptable at ~1 call per 15 minutes).
    // ---------------------------------------------------------------------------

    void Tickerplant::rotate()
    {
        if (journal_fp_)
        {
            std::fclose(journal_fp_);
            journal_fp_ = nullptr;
        }

        // Advance slot by exactly 1 from what triggered the rotation, rather
        // than re-reading the clock after fclose.  This prevents a race where
        // the clock still reads the old slot (e.g. called nanoseconds before
        // a boundary) and we silently skip the window.
        const int64_t new_slot = current_slot_ + 1;
        current_slot_ = new_slot;

        // Reconstruct the wall-clock time for this slot so make_journal_path
        // can format YYYYMMDD_HHMM correctly.
        const std::time_t slot_time = static_cast<std::time_t>(
            new_slot * rotation_interval_minutes_ * 60);
        const std::string path = make_journal_path(slot_time);
        journal_fp_ = std::fopen(path.c_str(), "ab");
        // If fopen fails mid-run, journal_fp_ stays null and ticks are silently
        // dropped until the next rotation.  Production would log a critical alert.
    }

    // ---------------------------------------------------------------------------
    // run() — the hot loop
    //
    // Per-tick cost on the critical path
    // ────────────────────────────────────
    //  1. RingBuffer::consume   — one acquire-load, copy, release-store
    //  2. std::fwrite           — copy 128 bytes into glibc's 8 KB stdio buffer
    //  3. fetch_add(release)    — atomic increment for the diagnostic counter
    //  4. Every 1024 ticks: clock_gettime + integer division (~22 ns total)
    //  5. load(relaxed)         — check running_ flag
    // ---------------------------------------------------------------------------

    void Tickerplant::run() noexcept
    {
        // ── Step 1: Pin this thread to its dedicated core (Linux only) ─────
#if defined(__linux__)
        {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(core_id_, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        }
#endif

        // ── Step 2: The tight hot loop ───────────────────────────────────────
        core::Tick tick;
        uint64_t tick_counter = 0;

        while (running_.load(std::memory_order_relaxed))
        {
            if (ring_.consume(tick))
            {

                if (journal_fp_)
                {
                    std::fwrite(&tick, sizeof(core::Tick), 1, journal_fp_);
                }

                ticks_written_.fetch_add(1, std::memory_order_release);
                ++tick_counter;

                // Every 1024 ticks, check if we've crossed into a new time window.
                // Bitwise AND is a single cycle — effectively free.
                if (rotation_interval_minutes_ > 0 &&
                    (tick_counter & 0x3FF) == 0)
                {
                    const std::time_t now = std::time(nullptr);
                    const int64_t slot = time_slot(now);
                    if (slot != current_slot_)
                    {
                        rotate();
                    }
                }
            }
        }
    }

} // namespace polymarket::tickerplant
