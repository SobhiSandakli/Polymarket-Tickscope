#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#ifndef NDEBUG
#include <cassert>
#include <thread>
#endif

namespace polymarket::memory
{

    // Portable CPU spinloop hint.
    // x86-64 : PAUSE — reduces power, prevents memory-order violations in loops.
    // ARM64   : YIELD — signals the core to de-prioritise this hyper-thread.
    // Other   : no-op (correctness is preserved regardless).
    [[gnu::always_inline]] inline void cpu_pause() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        asm volatile("yield" ::: "memory");
#endif
    }

    // ---------------------------------------------------------------------------
    // RingBuffer<T, Capacity>
    //
    // Multi-Producer Single-Consumer (MPSC) lock-free circular queue.
    //
    // ┌─────────────────────────────────────────────────────────────────────┐
    // │  PRODUCERS (N threads)          CONSUMER (1 thread — Tickerplant)  │
    // │                                                                     │
    // │  pos = write_index_.fetch_add() ──→  Slot[pos & MASK]              │
    // │  spin until sequence == pos         spin until sequence == pos+1   │
    // │  slot.data = item                   out = slot.data                │
    // │  sequence.store(pos+1, release) ──→ sequence.load(acquire)         │
    // │                                     sequence.store(pos+Cap,release)│
    // └─────────────────────────────────────────────────────────────────────┘
    //
    // Memory ordering contract
    // ───────────────────────
    //  fetch_add  (write_index)   → relaxed   : only needs a unique slot index.
    //  sequence.store (producer)  → release   : publishes slot.data to consumer.
    //  sequence.load  (consumer)  → acquire   : syncs with producer's release,
    //                                           making slot.data visible.
    //  sequence.store (consumer)  → release   : marks slot free for next cycle;
    //                                           producers spin-acquire on this.
    //
    // Sequence counter protocol  (Dmitry Vyukov MPMC, adapted for MPSC)
    // ─────────────────────────
    //  Initial   : slot[i].sequence == i          → slot is free.
    //  After produce at pos: sequence == pos + 1  → data ready for consumer.
    //  After consume at pos: sequence == pos + Capacity → free for next lap.
    //
    // This three-state counter replaces a plain bool ready-flag and
    // correctly handles the ABA problem (a slot being reused before the
    // consumer checks it) on any number of laps around the ring.
    //
    // False-sharing prevention
    // ────────────────────────
    //  • write_index_ lives on its own 64-byte cache line.
    //  • read_index_  lives on its own 64-byte cache line.
    //  • Each Slot is alignas(64), so adjacent slots never share a line.
    //    Producers writing slot N cannot invalidate a neighbouring producer's
    //    slot N+1 in L1.
    //
    // Constraints
    // ───────────
    //  • Capacity MUST be a power of two (enforced by static_assert).
    //  • Zero dynamic allocation: slots_ is a fixed std::array member.
    //  • No virtual functions, no exceptions, no heap use post-construction.
    //  • Non-copyable, non-movable (threads hold pointers into this object).
    // ---------------------------------------------------------------------------
    template <typename T, std::size_t Capacity>
    class RingBuffer
    {
        static_assert(Capacity > 0,
                      "RingBuffer: Capacity must be greater than zero.");
        static_assert((Capacity & (Capacity - 1)) == 0,
                      "RingBuffer: Capacity must be a power of two (e.g. 1024, 65536).");
        static_assert(std::atomic<std::size_t>::is_always_lock_free,
                      "RingBuffer: std::atomic<size_t> must be lock-free on this platform.");

        static constexpr std::size_t MASK = Capacity - 1;
        static constexpr std::size_t CACHE_LINE = 64;

        // ------------------------------------------------------------------
        // Slot — one cell of the circular buffer.
        //
        // Placing `sequence` and `data` in the same alignas(64) struct means:
        //   1. Each slot starts on a fresh cache line  → no inter-slot
        //      false sharing between two concurrent producers.
        //   2. The consumer reads sequence then data from the SAME cache line
        //      (for small T) → avoids a second cache-miss after the readiness
        //      check passes.
        // ------------------------------------------------------------------
        struct alignas(CACHE_LINE) Slot
        {
            std::atomic<std::size_t> sequence;
            T data;
        };

    public:
        // Construction runs once at startup: initialise every slot's sequence
        // to its own index, priming the Vyukov protocol.
        RingBuffer() noexcept
        {
            for (std::size_t i = 0; i < Capacity; ++i)
            {
                slots_[i].sequence.store(i, std::memory_order_relaxed);
            }
        }

        // Non-copyable, non-movable — threads hold raw pointers into slots_.
        RingBuffer(const RingBuffer &) = delete;
        RingBuffer &operator=(const RingBuffer &) = delete;
        RingBuffer(RingBuffer &&) = delete;
        RingBuffer &operator=(RingBuffer &&) = delete;

        // ------------------------------------------------------------------
        // produce(item) — called by ANY producer thread.
        //
        // Step 1 — Claim a slot with a single atomic fetch_add.
        //           relaxed ordering: we only need the unique position value.
        //           No synchronisation with other producers is required here.
        //
        // Step 2 — Spin until sequence == pos.
        //           This guards against the ring being FULL: a producer that
        //           is Capacity writes ahead of the consumer cannot overwrite
        //           an unconsumed slot.  The consumer's release-store of
        //           (pos + Capacity) will unblock this spin when the slot is
        //           eventually read.
        //           _mm_pause() emits the x86 PAUSE instruction: signals the
        //           CPU that we are in a spinloop, reducing power and preventing
        //           memory-order violations from speculative loads.
        //
        // Step 3 — Copy payload (plain store, not atomic).
        //
        // Step 4 — Publish via release-store of (pos + 1).
        //           This is the store the consumer acquires in consume().
        // ------------------------------------------------------------------
        void produce(const T &item) noexcept
        {
            const std::size_t pos = write_index_.fetch_add(1, std::memory_order_relaxed);
            Slot &slot = slots_[pos & MASK];

            // Spin until this slot is free (not currently held by the consumer
            // from a previous lap, and not full).
            while (slot.sequence.load(std::memory_order_acquire) != pos)
            {
                cpu_pause();
            }

            slot.data = item;
            slot.sequence.store(pos + 1, std::memory_order_release);
        }

        // ------------------------------------------------------------------
        // try_produce(item) — non-blocking variant for single-producer contexts.
        //
        // Returns true  : item was enqueued successfully.
        // Returns false : ring is full — item is dropped, caller must handle.
        //
        // Designed for single-producer use (e.g. one WebSocket I/O thread).
        // Protocol: load write_index_ (relaxed), check whether the target slot
        // has been recycled by the consumer (sequence == pos).  If not, the
        // ring is full and we return false immediately — no spin, no block.
        // If yes, we claim the slot via fetch_add and publish exactly as
        // produce() does, preserving the full acquire/release ordering contract.
        //
        // NOT safe for concurrent multi-producer use without external coordination.
        // Use produce() for MPMC scenarios.
        // ------------------------------------------------------------------
        [[nodiscard]] bool try_produce(const T &item) noexcept
        {
#ifndef NDEBUG
            // Enforce the single-producer contract in debug builds: the first
            // caller claims ownership, and any call from a second thread trips
            // the assert immediately instead of silently corrupting the ring
            // (two producers can claim the same slot, stalling the consumer
            // forever). For true MPSC, use produce() or make this CAS-based.
            {
                const std::thread::id self = std::this_thread::get_id();
                std::thread::id expected{};
                if (!dbg_tryproduce_tid_.compare_exchange_strong(
                        expected, self, std::memory_order_relaxed) &&
                    expected != self)
                {
                    assert(false && "RingBuffer::try_produce is single-producer "
                                    "only — called from a second thread");
                }
            }
#endif
            const std::size_t pos = write_index_.load(std::memory_order_relaxed);
            Slot &slot = slots_[pos & MASK];

            // sequence != pos means the consumer has not yet recycled this slot
            // from the previous lap — the ring is full.  Drop immediately.
            if (slot.sequence.load(std::memory_order_acquire) != pos)
            {
                return false;
            }

            // Slot is free.  Claim it (advance write_index_) and publish.
            write_index_.fetch_add(1, std::memory_order_relaxed);
            slot.data = item;
            slot.sequence.store(pos + 1, std::memory_order_release);
            return true;
        }

        // ------------------------------------------------------------------
        // consume(out) — called ONLY by the single consumer thread.
        //
        // Non-blocking: returns false immediately if no data is available.
        // The Tickerplant calls this in a tight while(true) loop; the caller
        // is responsible for the outer spin.
        //
        // The acquire-load pairs with the producer's release-store of (pos+1),
        // establishing the happens-before edge that makes slot.data visible.
        //
        // After reading, the slot is freed by storing (read_index_ + Capacity)
        // with release ordering — this unblocks any producer spinning on a
        // full ring.
        // ------------------------------------------------------------------
        [[nodiscard]] bool consume(T &out) noexcept
        {
            Slot &slot = slots_[read_index_ & MASK];

            // Check readiness: producer publishes exactly (read_index_ + 1).
            if (slot.sequence.load(std::memory_order_acquire) != read_index_ + 1)
            {
                return false; // buffer empty
            }

            // Payload is now visible (happens-after producer's release-store).
            out = slot.data;

            // Free the slot for the NEXT lap (read_index_ + Capacity).
            // release: unblocks producers spinning on a full ring.
            slot.sequence.store(read_index_ + Capacity, std::memory_order_release);
            ++read_index_;
            return true;
        }

        // Approximate occupancy — safe to call from the consumer thread only.
        // Not accurate under concurrent producers; use for diagnostics only.
        [[nodiscard]] std::size_t size_approx() const noexcept
        {
            return write_index_.load(std::memory_order_relaxed) - read_index_;
        }

    private:
        // Producer-hot: written by ALL producers simultaneously.
        // Own cache line prevents false sharing with read_index_ and slots_.
        alignas(CACHE_LINE) std::atomic<std::size_t> write_index_{0};

        // Consumer-hot: written exclusively by the Tickerplant thread.
        // Non-atomic because only one thread ever touches it; aligned to its
        // own cache line to avoid false sharing with write_index_ (which is
        // hammered by producers on other cores).
        alignas(CACHE_LINE) std::size_t read_index_{0};

        // The circular slot array.  sizeof(Slot) >= CACHE_LINE guarantees
        // no two adjacent slots share a cache line, eliminating
        // producer–producer false sharing on simultaneous neighbouring writes.
        std::array<Slot, Capacity> slots_;

#ifndef NDEBUG
        // Debug-only single-producer guard for try_produce() (see there).
        // Absent from release builds, so the hot struct layout is unchanged.
        mutable std::atomic<std::thread::id> dbg_tryproduce_tid_{};
#endif
    };

} // namespace polymarket::memory
