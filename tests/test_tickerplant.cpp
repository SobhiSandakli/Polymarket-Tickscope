//
// test_tickerplant.cpp — Tickerplant journal write/read + rotation tests
//
// Test 1: run_full_test
// ─────────────────────
//  100,000 ticks through a single journal file (large rotation interval so
//  no rotation triggers).  Verifies tick count, file size, field integrity.
//
// Test 2: run_rotation_test
// ─────────────────────────
//  Push ticks, force_rotate(), push more ticks, verify two distinct files
//  exist with correct combined tick count.
//

#include <polymarket/tickerplant/Tickerplant.hpp>
#include <polymarket/memory/RingBuffer.hpp>
#include <polymarket/core/Tick.hpp>
#include <polymarket/rdb/OrderBook.hpp>

#include <cmath> // std::abs
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <dirent.h>   // opendir, readdir
#include <sys/stat.h> // stat
#include <unistd.h>   // rmdir, unlink

// Always-on check macro (not disabled by -DNDEBUG).
#define CHECK(cond, msg)                             \
    do                                               \
    {                                                \
        if (!(cond))                                 \
        {                                            \
            std::printf("  [FAIL] %s\n  at %s:%d\n", \
                        (msg), __FILE__, __LINE__);  \
            return false;                            \
        }                                            \
    } while (0)

static constexpr int NUM_TICKS = 100'000;
static constexpr double DOUBLE_EPS = 1e-9;

// ---------------------------------------------------------------------------
// make_tick — deterministic tick factory
// ---------------------------------------------------------------------------
static polymarket::core::Tick make_tick(int i)
{
    polymarket::core::Tick t{};
    t.timestamp = static_cast<uint64_t>(1'700'000'000'000ULL + i);
    t.price = 0.01 * static_cast<double>(i % 100);
    t.size = static_cast<double>(i) + 0.5;
    t.side = static_cast<uint8_t>(i % 2);
    std::snprintf(t.asset_id, sizeof(t.asset_id), "ASSET_%08d", i);
    return t;
}

// ---------------------------------------------------------------------------
// wait_for_ticks — spin until the tickerplant has written exactly `n` ticks.
// ---------------------------------------------------------------------------
static bool wait_for_ticks(
    const polymarket::tickerplant::Tickerplant &tp,
    uint64_t n,
    int timeout_ms = 10'000)
{
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);

    while (tp.ticks_written() < n)
    {
        polymarket::memory::cpu_pause();
        if (std::chrono::steady_clock::now() > deadline)
        {
            std::printf("  [TIMEOUT] Only %llu / %llu ticks written\n",
                        (unsigned long long)tp.ticks_written(),
                        (unsigned long long)n);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Helper: count .bin files and total bytes in a directory
// ---------------------------------------------------------------------------
static int count_bin_files(const char *dir, long &total_bytes)
{
    int count = 0;
    total_bytes = 0;

    DIR *d = ::opendir(dir);
    if (!d)
        return 0;

    struct dirent *entry;
    while ((entry = ::readdir(d)) != nullptr)
    {
        const char *name = entry->d_name;
        const std::size_t len = std::strlen(name);
        if (len > 4 && std::strcmp(name + len - 4, ".bin") == 0)
        {
            ++count;
            // Get file size
            std::string full = std::string(dir) + "/" + name;
            struct stat st{};
            if (::stat(full.c_str(), &st) == 0)
            {
                total_bytes += st.st_size;
            }
        }
    }
    ::closedir(d);
    return count;
}

// ---------------------------------------------------------------------------
// Helper: remove all files in a directory and the directory itself
// ---------------------------------------------------------------------------
static void cleanup_dir(const char *dir)
{
    DIR *d = ::opendir(dir);
    if (!d)
        return;

    struct dirent *entry;
    while ((entry = ::readdir(d)) != nullptr)
    {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;
        std::string full = std::string(dir) + "/" + entry->d_name;
        ::unlink(full.c_str());
    }
    ::closedir(d);
    ::rmdir(dir);
}

// ---------------------------------------------------------------------------
// Test 1: run_full_test — 100k ticks, single journal (no rotation)
// ---------------------------------------------------------------------------
static bool run_full_test()
{
    std::puts("[test_tickerplant_journal]");

    const char *journal_dir = "/tmp/polymarket_test_tp";
    cleanup_dir(journal_dir);

    // ── Generate reference ticks ─────────────────────────────────────────
    std::vector<polymarket::core::Tick> reference(NUM_TICKS);
    for (int i = 0; i < NUM_TICKS; ++i)
    {
        reference[i] = make_tick(i);
    }

    // ── Produce → consume → journal (nested scope destroys Tickerplant) ─
    double elapsed_ms = 0.0;
    {
        static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> ring;

        // rotation_minutes=0 disables rotation — single file for the test.
        polymarket::tickerplant::Tickerplant tp(
            ring, journal_dir, /*core_id=*/0, /*rotation_minutes=*/0);
        tp.start();

        const auto t0 = std::chrono::steady_clock::now();

        for (int i = 0; i < NUM_TICKS; ++i)
        {
            ring.produce(reference[i]);
        }

        CHECK(wait_for_ticks(tp, NUM_TICKS),
              "Timed out: tickerplant did not write all 100,000 ticks");

        elapsed_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count();

        tp.stop();
        // <<< tp destructor runs here: thread.join() + fclose() + fflush() >>>
    }

    std::printf("  %d ticks produced+journalled in %.2f ms (%.2f M ticks/s)\n",
                NUM_TICKS, elapsed_ms,
                NUM_TICKS / (elapsed_ms * 1'000.0));

    // ── Verify total bytes across all .bin files ─────────────────────────
    long total_bytes = 0;
    const int nfiles = count_bin_files(journal_dir, total_bytes);
    CHECK(nfiles >= 1, "Expected at least 1 journal .bin file");

    const long expected_bytes =
        static_cast<long>(NUM_TICKS) *
        static_cast<long>(sizeof(polymarket::core::Tick));

    std::printf("  Files: %d, total size: %ld bytes (expected %ld)\n",
                nfiles, total_bytes, expected_bytes);
    CHECK(total_bytes == expected_bytes, "Journal total size mismatch");

    // ── Read back from the single file and verify every field ────────────
    std::puts("  Verifying field-level integrity for all 100,000 ticks...");
    {
        // Find the .bin file
        DIR *d = ::opendir(journal_dir);
        CHECK(d != nullptr, "Cannot open journal dir for reading");

        std::string bin_path;
        struct dirent *entry;
        while ((entry = ::readdir(d)) != nullptr)
        {
            const char *name = entry->d_name;
            const std::size_t len = std::strlen(name);
            if (len > 4 && std::strcmp(name + len - 4, ".bin") == 0)
            {
                bin_path = std::string(journal_dir) + "/" + name;
                break;
            }
        }
        ::closedir(d);
        CHECK(!bin_path.empty(), "No .bin file found in journal dir");

        std::FILE *f = std::fopen(bin_path.c_str(), "rb");
        CHECK(f != nullptr, "Cannot open journal for verification");

        polymarket::core::Tick read_tick{};
        int mismatches = 0;

        for (int i = 0; i < NUM_TICKS; ++i)
        {
            const std::size_t n =
                std::fread(&read_tick, sizeof(polymarket::core::Tick), 1, f);

            if (n != 1)
            {
                std::printf("  [FAIL] fread returned 0 at tick index %d\n", i);
                ++mismatches;
                break;
            }

            const polymarket::core::Tick &ref = reference[i];

            if (read_tick.timestamp != ref.timestamp)
            {
                std::printf("  [FAIL] tick[%d].timestamp: got %llu, want %llu\n",
                            i, (unsigned long long)read_tick.timestamp,
                            (unsigned long long)ref.timestamp);
                ++mismatches;
            }
            if (std::abs(read_tick.price - ref.price) > DOUBLE_EPS)
            {
                std::printf("  [FAIL] tick[%d].price: got %.10f, want %.10f\n",
                            i, read_tick.price, ref.price);
                ++mismatches;
            }
            if (std::abs(read_tick.size - ref.size) > DOUBLE_EPS)
            {
                std::printf("  [FAIL] tick[%d].size: got %.10f, want %.10f\n",
                            i, read_tick.size, ref.size);
                ++mismatches;
            }
            if (read_tick.side != ref.side)
            {
                std::printf("  [FAIL] tick[%d].side: got %u, want %u\n",
                            i, read_tick.side, ref.side);
                ++mismatches;
            }
            if (std::strcmp(read_tick.asset_id, ref.asset_id) != 0)
            {
                std::printf("  [FAIL] tick[%d].asset_id: got '%s', want '%s'\n",
                            i, read_tick.asset_id, ref.asset_id);
                ++mismatches;
            }

            if (mismatches >= 5)
            {
                std::puts("  (stopping after 5 mismatches)");
                break;
            }
        }

        std::fclose(f);
        CHECK(mismatches == 0, "One or more field mismatches found in journal");
    }

    std::printf("  All %d ticks verified field-by-field  [OK]\n", NUM_TICKS);
    std::puts("  => PASS");

    cleanup_dir(journal_dir);
    return true;
}

// ---------------------------------------------------------------------------
// Test 2: run_rotation_test — force_rotate() produces multiple files
// ---------------------------------------------------------------------------
static bool run_rotation_test()
{
    std::puts("[test_tickerplant_rotation]");

    const char *journal_dir = "/tmp/polymarket_test_rotation";
    cleanup_dir(journal_dir);

    static constexpr int TICKS_BEFORE = 500;
    static constexpr int TICKS_AFTER = 300;
    static constexpr int TOTAL_TICKS = TICKS_BEFORE + TICKS_AFTER;

    {
        static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> ring;

        // Use rotation_minutes=15 (won't trigger naturally in <1 s).
        polymarket::tickerplant::Tickerplant tp(
            ring, journal_dir, /*core_id=*/0, /*rotation_minutes=*/15);
        tp.start();

        // Push first batch.
        for (int i = 0; i < TICKS_BEFORE; ++i)
        {
            ring.produce(make_tick(i));
        }
        CHECK(wait_for_ticks(tp, TICKS_BEFORE),
              "Timed out waiting for first batch");

        // Force rotation — opens a new file.
        tp.force_rotate();

        // Push second batch.
        for (int i = TICKS_BEFORE; i < TOTAL_TICKS; ++i)
        {
            ring.produce(make_tick(i));
        }
        CHECK(wait_for_ticks(tp, TOTAL_TICKS),
              "Timed out waiting for second batch");

        tp.stop();
    }

    // ── Verify: exactly 2 .bin files, combined size = TOTAL_TICKS × 128 ─
    long total_bytes = 0;
    const int nfiles = count_bin_files(journal_dir, total_bytes);

    std::printf("  Files: %d, total bytes: %ld\n", nfiles, total_bytes);

    CHECK(nfiles == 2, "Expected exactly 2 journal files after force_rotate()");

    const long expected =
        static_cast<long>(TOTAL_TICKS) *
        static_cast<long>(sizeof(polymarket::core::Tick));
    CHECK(total_bytes == expected,
          "Combined journal size mismatch after rotation");

    std::printf("  %d ticks across %d files, total %ld bytes  [OK]\n",
                TOTAL_TICKS, nfiles, total_bytes);
    std::puts("  => PASS");

    cleanup_dir(journal_dir);
    return true;
}

// ---------------------------------------------------------------------------
// Test 3: run_rdb_test — the Tickerplant maintains a live RDB top-of-book
//
// Pushes known bid/ask updates for 3 distinct tokens through the ring and
// verifies the Tickerplant's in-memory rdb::OrderBook reflects the latest
// top-of-book for each. Also pushes one trade event (event_type 1) and
// verifies it is journaled but does NOT alter the book (book-level events only).
// ---------------------------------------------------------------------------
static bool run_rdb_test()
{
    std::puts("[test_tickerplant_rdb]");

    const char *journal_dir = "/tmp/polymarket_test_rdb";
    cleanup_dir(journal_dir);

    struct Expect
    {
        const char *id;
        double bid_px, bid_sz, ask_px, ask_sz;
    };
    const Expect ex[] = {
        {"TOKEN_AAA", 0.42, 100.0, 0.45, 120.0},
        {"TOKEN_BBB", 0.10, 50.0, 0.12, 55.0},
        {"TOKEN_CCC", 0.90, 10.0, 0.93, 12.0},
    };
    constexpr int NTOK = 3;

    {
        static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> ring;
        polymarket::tickerplant::Tickerplant tp(
            ring, journal_dir, /*core_id=*/0, /*rotation_minutes=*/0);
        tp.start();

        uint64_t produced = 0;
        for (int t = 0; t < NTOK; ++t)
        {
            polymarket::core::Tick bid{}; // event_type 0 (price_change) via zero-init
            bid.side = 0;                 // BID
            bid.price = ex[t].bid_px;
            bid.size = ex[t].bid_sz;
            std::snprintf(bid.asset_id, sizeof(bid.asset_id), "%s", ex[t].id);
            ring.produce(bid);
            ++produced;

            polymarket::core::Tick ask{};
            ask.side = 1; // ASK
            ask.price = ex[t].ask_px;
            ask.size = ex[t].ask_sz;
            std::snprintf(ask.asset_id, sizeof(ask.asset_id), "%s", ex[t].id);
            ring.produce(ask);
            ++produced;
        }

        // A trade on TOKEN_AAA — journaled, but must NOT touch the book.
        polymarket::core::Tick trade{};
        trade.event_type = 1; // last_trade_price
        trade.side = 0;
        trade.price = 0.99;
        trade.size = 999.0;
        std::snprintf(trade.asset_id, sizeof(trade.asset_id), "%s", ex[0].id);
        ring.produce(trade);
        ++produced;

        CHECK(wait_for_ticks(tp, produced),
              "Timed out waiting for RDB ticks to journal");

        // ticks_written == produced ⇒ every book-level event has been applied
        // (apply_tick runs before the counter increment) and the ring is drained,
        // so the consumer thread is idle — these reads race nothing.
        const auto &book = tp.book();
        CHECK(book.token_count() == NTOK,
              "RDB token_count should equal the 3 distinct tokens seen");

        for (int t = 0; t < NTOK; ++t)
        {
            uint16_t idx = 0;
            CHECK(book.find_index(ex[t].id, idx), "RDB should have indexed each token");
            const auto &ms = book.state(idx);
            CHECK(std::abs(ms.best_bid_price - ex[t].bid_px) < DOUBLE_EPS, "best_bid_price mismatch");
            CHECK(std::abs(ms.best_bid_size - ex[t].bid_sz) < DOUBLE_EPS, "best_bid_size mismatch");
            CHECK(std::abs(ms.best_ask_price - ex[t].ask_px) < DOUBLE_EPS, "best_ask_price mismatch");
            CHECK(std::abs(ms.best_ask_size - ex[t].ask_sz) < DOUBLE_EPS, "best_ask_size mismatch");
        }

        // The trade event must not have overwritten TOKEN_AAA's best bid.
        uint16_t aidx = 0;
        CHECK(book.find_index(ex[0].id, aidx), "TOKEN_AAA must be indexed");
        CHECK(std::abs(book.state(aidx).best_bid_price - ex[0].bid_px) < DOUBLE_EPS,
              "trade event (event_type 1) must not modify the book");

        tp.stop();
    }

    std::puts("  RDB live top-of-book verified for 3 tokens; trade event ignored  [OK]");
    std::puts("  => PASS");

    cleanup_dir(journal_dir);
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    std::puts("=== polymarket Tickerplant journal + rotation tests ===\n");

    bool all_pass = true;

    all_pass &= run_full_test();
    std::putchar('\n');

    all_pass &= run_rotation_test();
    std::putchar('\n');

    all_pass &= run_rdb_test();
    std::putchar('\n');

    std::puts(all_pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return all_pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
