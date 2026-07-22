#pragma once

#include <polymarket/core/Tick.hpp>
#include <polymarket/memory/RingBuffer.hpp>
#include <polymarket/rdb/OrderBook.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio> // std::FILE
#include <string>
#include <thread>

namespace polymarket::tickerplant
{

    // ---------------------------------------------------------------------------
    // Tickerplant
    //
    // The single-threaded, CPU-pinned consumer that drains the MPSC ring buffer
    // and durably journals every tick to an NVMe-backed file.
    //
    // Architecture position
    // ─────────────────────
    //  WS Workers → simdjson → RingBuffer<Tick,65536> → [Tickerplant] → { NVMe journal
    //                                                                    + live RDB book }
    //
    //  As it drains each tick, the Tickerplant does two things: durably journals
    //  the raw Tick to disk (the WAL, replayable offline) AND applies book-level
    //  updates to an in-memory rdb::OrderBook (the classic kdb+ tickerplant→RDB
    //  split: the journal is the historical record, the RDB is the live view a
    //  strategy queries with zero disk I/O). See book() and rdb/OrderBook.hpp.
    //
    // Journal rotation
    // ────────────────
    //  Files are named polymarket_YYYYMMDD_HHMM.bin in the journal directory.
    //  Every `rotation_interval_minutes` the hot loop closes the current file
    //  and opens a new one.  The clock is checked on a time basis (~once per
    //  1024 loop iterations, gated to once per second) rather than on tick
    //  volume, so an idle segment still rotates on schedule instead of staying
    //  open until the next tick arrives.
    //
    // Threading model
    // ───────────────
    //  One OS thread, pinned to a dedicated physical core via
    //  pthread_setaffinity_np(pthread_self(), ...) at the start of run().
    //
    // Write-Ahead Log (WAL) strategy
    // ───────────────────────────────
    //  Raw binary Tick structs are appended with std::fwrite().  We rely on
    //  the Linux Page Cache for async durability:
    //   fwrite → glibc stdio buffer → (auto-flush) → kernel page cache → NVMe
    //  There is no fflush()/fsync() on the per-tick path; a single fflush()
    //  runs at most once per second (off the per-tick path) so the file's
    //  on-disk mtime reflects recent writes before the retention sweep's
    //  mtime-based "sealed" threshold can act on it.
    //
    // Hot-path constraints
    // ─────────────────────
    //  • Zero dynamic allocation after construction.
    //  • No software locks anywhere.
    //  • One std::fwrite per tick (128 bytes — fits in glibc buffer).
    //  • No virtual functions, no exceptions.
    // ---------------------------------------------------------------------------
    class Tickerplant
    {
    public:
        // journal_dir          : directory for rotated journal files
        // core_id              : physical CPU core index for thread affinity
        // rotation_minutes     : time window per file (0 = no rotation)
        explicit Tickerplant(
            memory::RingBuffer<core::Tick, 65536> &ring,
            const char *journal_dir,
            int core_id = 0,
            int rotation_minutes = 15);

        ~Tickerplant();

        // Non-copyable, non-movable: the hot loop holds `this` permanently.
        Tickerplant(const Tickerplant &) = delete;
        Tickerplant &operator=(const Tickerplant &) = delete;
        Tickerplant(Tickerplant &&) = delete;
        Tickerplant &operator=(Tickerplant &&) = delete;

        void start();
        void stop() noexcept;

        [[nodiscard]] uint64_t ticks_written() const noexcept
        {
            return ticks_written_.load(std::memory_order_acquire);
        }

        // Live top-of-book view maintained alongside the journal. Read-only for
        // callers; only the consumer thread mutates it (via apply_tick in run()).
        // Safe to query from another thread for diagnostics — see OrderBook's
        // thread-safety note on aligned double reads.
        [[nodiscard]] const rdb::OrderBook &book() const noexcept
        {
            return book_;
        }

        // Request an immediate journal rotation.  Intended for tests only —
        // production rotation happens automatically via the clock check. The
        // actual fclose/fopen is performed by the consumer thread inside run(),
        // so this is safe to call while the Tickerplant is running (journal_fp_
        // is never mutated from two threads at once).
        void force_rotate();

    private:
        void run() noexcept;

        // Build filename: "<journal_dir_>/polymarket_YYYYMMDD_HHMM.bin"
        // from a time_t.  Rounds HHMM down to the nearest rotation boundary.
        std::string make_journal_path(std::time_t now) const;

        // Close the current journal file and open a new one for the window that
        // `now` falls in.  Called from the hot loop when the wall clock crosses
        // a rotation boundary.
        void rotate(std::time_t now);

        // Compute the time-slot index: minutes-since-epoch / interval
        int64_t time_slot(std::time_t now) const noexcept;

        // ── Cold fields (set at construction, never touched in hot loop) ────────
        memory::RingBuffer<core::Tick, 65536> &ring_;
        std::string journal_dir_;
        // Only read inside the Linux-only affinity block in run(); [[maybe_unused]]
        // silences the warning on platforms where that block is compiled out (macOS).
        [[maybe_unused]] int core_id_;
        int rotation_interval_minutes_;
        std::thread thread_;

        // ── Warm fields (touched infrequently — on rotation only) ───────────────
        std::FILE *journal_fp_ = nullptr;
        int64_t current_slot_ = 0;

        // ── Hot fields — each on its own cache line ──────────────────────────
        alignas(64) std::atomic<bool> running_{false};
        alignas(64) std::atomic<uint64_t> ticks_written_{0};

        // In-memory RDB: live top-of-book, updated per tick on the consumer
        // thread. 256 KB flat array (fits in L2); its own cache-line-aligned
        // slots, so updating one token never evicts another. Owned by value —
        // the Tickerplant is the single writer, matching OrderBook's contract.
        rdb::OrderBook book_;

        // Test-only forced-rotation request. Set by force_rotate() (any thread),
        // consumed by run() on the consumer thread which does the actual
        // fclose/fopen — keeping journal_fp_ single-threaded.
        std::atomic<bool> force_rotate_{false};
    };

} // namespace polymarket::tickerplant
