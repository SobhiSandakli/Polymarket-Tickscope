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
        // Test-only: just raise a request. The consumer thread performs the
        // fclose/fopen inside run(), so journal_fp_ is never touched from two
        // threads (the original version mutated it here, racing the hot loop's
        // fwrite — a use-after-free of the FILE*). Safe to call while running.
        force_rotate_.store(true, std::memory_order_relaxed);
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

    void Tickerplant::rotate(std::time_t now)
    {
        if (journal_fp_)
        {
            std::fclose(journal_fp_);
            journal_fp_ = nullptr;
        }

        // Snap to the window that `now` actually falls in. The caller only
        // invokes rotate() once it has observed time_slot(now) != current_slot_,
        // so this never skips a live window. Using the real wall clock (rather
        // than the old current_slot_ + 1) means that after a quiet gap spanning
        // several windows, the filename jumps straight to the current window
        // instead of crawling forward one window per rotation — which used to
        // stamp fresh data with a window that had already been sealed/deleted.
        current_slot_ = time_slot(now);

        const std::string path = make_journal_path(now);
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
        uint64_t loop_iters = 0;
        std::time_t last_maintenance = std::time(nullptr);

        while (running_.load(std::memory_order_relaxed))
        {
            // ── Test-only forced rotation ────────────────────────────────────
            // Performed here on the consumer thread (see force_rotate()) so the
            // FILE* is never touched concurrently. Checked at the top of the
            // loop so a rotation requested between two tick batches takes effect
            // before the second batch is written.
            if (force_rotate_.load(std::memory_order_relaxed))
            {
                force_rotate_.store(false, std::memory_order_relaxed);
                ++current_slot_;
                if (journal_fp_)
                {
                    std::fclose(journal_fp_);
                    journal_fp_ = nullptr;
                }
                const std::time_t synthetic = static_cast<std::time_t>(
                    current_slot_ * rotation_interval_minutes_ * 60);
                const std::string path = make_journal_path(synthetic);
                journal_fp_ = std::fopen(path.c_str(), "ab");
            }

            if (ring_.consume(tick))
            {
                if (journal_fp_)
                {
                    std::fwrite(&tick, sizeof(core::Tick), 1, journal_fp_);
                }
                ticks_written_.fetch_add(1, std::memory_order_release);
            }

            // ── Time-based maintenance: rotation + flush ─────────────────────
            // Runs on the wall clock, NOT on tick volume — so it fires even
            // during a quiet market when no ticks arrive. The clock is read at
            // most ~once per 1024 iterations (single-cycle mask) and the real
            // work is gated to once per second by the last_maintenance guard.
            //
            // Why this matters (data-loss fix): rotation used to be checked only
            // on tick receipt, and there was no fflush at all. An idle segment
            // could therefore sit open past its window with buffered-but-unwritten
            // ticks, while hourly_flush.sh / the retention sweep treat any file
            // with mtime > 20min as sealed — and delete it out from under the
            // still-open writer. Flushing on a timer keeps the on-disk mtime
            // honest, and a time-based rotation closes idle segments on schedule.
            if ((++loop_iters & 0x3FF) == 0)
            {
                const std::time_t now = std::time(nullptr);
                if (now != last_maintenance)
                {
                    last_maintenance = now;
                    if (journal_fp_)
                    {
                        std::fflush(journal_fp_);
                    }
                    if (rotation_interval_minutes_ > 0 &&
                        time_slot(now) != current_slot_)
                    {
                        rotate(now);
                    }
                }
            }
        }

        // ── Step 3: Drain the ring before exiting ────────────────────────────
        // stop() only flips running_ to false; ticks still sitting in the ring
        // at that moment (up to 65536 = 8 MB during a burst + SIGINT) would be
        // lost without this final pass.
        while (ring_.consume(tick))
        {
            if (journal_fp_)
            {
                std::fwrite(&tick, sizeof(core::Tick), 1, journal_fp_);
            }
            ticks_written_.fetch_add(1, std::memory_order_release);
        }
        if (journal_fp_)
        {
            std::fflush(journal_fp_);
        }
    }

} // namespace polymarket::tickerplant
