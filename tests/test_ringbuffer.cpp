//
// test_ringbuffer.cpp — MPSC RingBuffer stress test
//
// Scenario
// ────────
//   10 producer std::threads each write 100,000 integers (their producer ID)
//   into a shared RingBuffer.  A single consumer thread reads until it has
//   consumed exactly 1,000,000 items.
//
// Correctness checks
// ──────────────────
//   1. Total items consumed == NUM_PRODUCERS * ITEMS_PER_PRODUCER.
//   2. Each producer ID was seen exactly ITEMS_PER_PRODUCER times
//      (no lost writes, no duplicates).
//   3. No deadlock: the test exits naturally (no join timeout here, but the
//      logic is structurally deadlock-free because every produce() will
//      eventually find a free slot as the consumer drains the ring).
//
// What this test does NOT do
// ──────────────────────────
//   • It does NOT verify ordering between different producers — MPSC gives
//     no cross-producer ordering guarantee; only within a single producer's
//     stream is FIFO preserved.
//   • It does NOT benchmark; for latency numbers run with perf/rdtsc.
//

#include <polymarket/memory/RingBuffer.hpp> // also pulls in cpu_pause()

#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

// ---------------------------------------------------------------------------
// Test parameters
// ---------------------------------------------------------------------------
static constexpr int NUM_PRODUCERS = 10;
static constexpr int ITEMS_PER_PRODUCER = 100'000;
static constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

// Ring capacity: power-of-two, large enough to absorb producer bursts.
// 2^17 = 131,072 slots.  sizeof(Slot<int>) = 64 bytes → 8 MB of SRAM.
static constexpr std::size_t RING_CAPACITY = 1 << 17;

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

// The ring buffer under test.
// Declared static to place it in BSS (zero-overhead startup, avoids
// exhausting the default 8 MB thread stack).
static polymarket::memory::RingBuffer<int, RING_CAPACITY> g_ring;

// Start-gun: producers spin on this before hammering the ring simultaneously.
// This maximises contention and stress-tests the fetch_add claim path.
static std::atomic<bool> g_start{false};

// Producers decrement this on completion; consumer watches it to know
// when it is safe to declare the buffer fully drained.
static std::atomic<int> g_producers_active{NUM_PRODUCERS};

// ---------------------------------------------------------------------------
// Producer
// ---------------------------------------------------------------------------
static void producer_fn(int id)
{
    // Spin until the start-gun fires.  _mm_pause() keeps us from burning
    // cache-coherence bandwidth while we wait.
    while (!g_start.load(std::memory_order_acquire))
    {
        polymarket::memory::cpu_pause();
    }

    for (int i = 0; i < ITEMS_PER_PRODUCER; ++i)
    {
        g_ring.produce(id);
    }

    // Signal completion.  Release so the consumer's acquire sees all writes.
    g_producers_active.fetch_sub(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------
// Returns a counts array: counts[i] = number of times producer i was seen.
static std::array<int, NUM_PRODUCERS> consumer_fn()
{
    std::array<int, NUM_PRODUCERS> counts{};
    int total = 0;

    while (total < TOTAL_ITEMS)
    {
        int val;
        if (g_ring.consume(val))
        {
            // Bounds-check: a corrupted value outside [0, NUM_PRODUCERS)
            // would immediately surface here.
            assert(val >= 0 && val < NUM_PRODUCERS);
            ++counts[val];
            ++total;
        }
        // No else-branch sleep/yield: the Tickerplant runs a tight loop.
        // _mm_pause() would go here in production to reduce power.
    }

    return counts;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    std::puts("=== polymarket RingBuffer stress test ===");
    std::printf("  Producers : %d\n", NUM_PRODUCERS);
    std::printf("  Per-prod  : %d\n", ITEMS_PER_PRODUCER);
    std::printf("  Total     : %d\n", TOTAL_ITEMS);
    std::printf("  Ring cap  : %zu slots\n", RING_CAPACITY);

    // Launch producer threads.
    std::array<std::thread, NUM_PRODUCERS> producers;
    for (int i = 0; i < NUM_PRODUCERS; ++i)
    {
        producers[i] = std::thread(producer_fn, i);
    }

    // Launch the consumer thread.
    std::array<int, NUM_PRODUCERS> counts{};
    std::thread consumer_thread([&counts]()
                                { counts = consumer_fn(); });

    // Fire the start-gun — all producers begin simultaneously.
    const auto t0 = std::chrono::steady_clock::now();
    g_start.store(true, std::memory_order_release);

    // Join producers first, then consumer.
    for (auto &p : producers)
    {
        p.join();
    }
    consumer_thread.join();
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double throughput_mops = TOTAL_ITEMS / (elapsed_ms * 1'000.0);

    // -----------------------------------------------------------------------
    // Verification
    // -----------------------------------------------------------------------
    bool pass = true;

    // Check 1: every producer was seen exactly ITEMS_PER_PRODUCER times.
    for (int i = 0; i < NUM_PRODUCERS; ++i)
    {
        if (counts[i] != ITEMS_PER_PRODUCER)
        {
            std::printf("[FAIL] Producer %d: expected %d items, got %d\n",
                        i, ITEMS_PER_PRODUCER, counts[i]);
            pass = false;
        }
    }

    // Check 2: sum equals TOTAL_ITEMS (redundant but explicit).
    int total_consumed = 0;
    for (int c : counts)
    {
        total_consumed += c;
    }
    if (total_consumed != TOTAL_ITEMS)
    {
        std::printf("[FAIL] Total consumed %d, expected %d\n",
                    total_consumed, TOTAL_ITEMS);
        pass = false;
    }

    // -----------------------------------------------------------------------
    // Report
    // -----------------------------------------------------------------------
    std::puts("");
    std::printf("  Result    : %s\n", pass ? "PASS" : "FAIL");
    std::printf("  Elapsed   : %.2f ms\n", elapsed_ms);
    std::printf("  Throughput: %.2f M ops/s\n", throughput_mops);

    if (pass)
    {
        std::puts("  All 1,000,000 items consumed with zero loss.");
    }

    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
