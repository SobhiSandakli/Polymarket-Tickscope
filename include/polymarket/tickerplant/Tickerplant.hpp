#pragma once

#include <polymarket/core/Tick.hpp>
#include <polymarket/memory/RingBuffer.hpp>

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
    //  WS Workers → simdjson → RingBuffer<Tick,65536> → [Tickerplant] → journal/
    //
    // Journal rotation
    // ────────────────
    //  Files are named polymarket_YYYYMMDD_HHMM.bin in the journal directory.
    //  Every `rotation_interval_minutes` the hot loop closes the current file
    //  and opens a new one.  The clock is checked every 1024 ticks via
    //  clock_gettime(CLOCK_REALTIME) (~20 ns vDSO, no syscall) — at 450 ticks/s
    //  that's once every ~2.3 seconds, effectively zero overhead.
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
    //  There is NO fflush() or fsync() in the hot loop.
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

        // Force an immediate journal rotation.  Intended for tests only —
        // production rotation happens automatically via the clock check.
        void force_rotate();

    private:
        void run() noexcept;

        // Build filename: "<journal_dir_>/polymarket_YYYYMMDD_HHMM.bin"
        // from a time_t.  Rounds HHMM down to the nearest rotation boundary.
        std::string make_journal_path(std::time_t now) const;

        // Close the current journal file and open a new one for the current
        // time window.  Called from the hot loop every rotation_interval_minutes_.
        void rotate();

        // Compute the time-slot index: minutes-since-epoch / interval
        int64_t time_slot(std::time_t now) const noexcept;

        // ── Cold fields (set at construction, never touched in hot loop) ────────
        memory::RingBuffer<core::Tick, 65536> &ring_;
        std::string journal_dir_;
        int core_id_;
        int rotation_interval_minutes_;
        std::thread thread_;

        // ── Warm fields (touched infrequently — on rotation only) ───────────────
        std::FILE *journal_fp_ = nullptr;
        int64_t current_slot_ = 0;

        // ── Hot fields — each on its own cache line ──────────────────────────
        alignas(64) std::atomic<bool> running_{false};
        alignas(64) std::atomic<uint64_t> ticks_written_{0};
    };

} // namespace polymarket::tickerplant
