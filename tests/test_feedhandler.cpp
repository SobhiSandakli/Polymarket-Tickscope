//
// test_feedhandler.cpp — Feedhandler JSON parse → RingBuffer round-trip test
//
// Tests
// ──────
//  1. Happy-path price_change (single leg) — checks all fields including
//     the new best_bid, best_ask, event_type.
//  2. Multi-leg price_change — two Ticks, field-level check on both.
//  3. "book" snapshot (bare array) — silently dropped.
//  4. Malformed JSON — silently dropped.
//  5. Missing required field in a leg — that leg skipped.
//  6. last_trade_price event — flat object, one Tick, event_type==1,
//     best_bid/best_ask remain 0.0.
//

#include <polymarket/feedhandler/Feedhandler.hpp>
#include <polymarket/core/Tick.hpp>
#include <polymarket/memory/RingBuffer.hpp>

#include <simdjson.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

static constexpr double EPS = 1e-9;

static bool near(double a, double b) { return std::abs(a - b) < EPS; }

static std::string make_padded(const char *json)
{
    std::string buf(json);
    buf.resize(buf.size() + simdjson::SIMDJSON_PADDING, '\0');
    return buf;
}

static std::string_view live_view(const std::string &padded)
{
    return {padded.data(), padded.size() - simdjson::SIMDJSON_PADDING};
}

// ---------------------------------------------------------------------------
// Test 1 — Happy-path: single-leg price_change, all new fields verified
// ---------------------------------------------------------------------------
static bool test_happy_path()
{
    std::puts("[test_happy_path]");

    static const char *JSON =
        "{"
        "\"event_type\":\"price_change\","
        "\"market\":\"0xdeadbeef\","
        "\"timestamp\":\"1700000000000\","
        "\"price_changes\":["
        "{"
        "\"asset_id\":\"71321045679252212594626385532706912750332728571942532289631379312455583992563\","
        "\"price\":\"0.52\","
        "\"size\":\"100.5\","
        "\"side\":\"BUY\","
        "\"hash\":\"abc123\","
        "\"best_bid\":\"0.51\","
        "\"best_ask\":\"0.53\""
        "}"
        "]"
        "}";

    static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> ring;
    polymarket::feedhandler::parse_and_push(live_view(make_padded(JSON)), ring);

    polymarket::core::Tick tick{};
    CHECK(ring.consume(tick), "Expected one Tick");

    polymarket::core::Tick extra{};
    CHECK(!ring.consume(extra), "Ring should be empty");

    const char *expected_id =
        "71321045679252212594626385532706912750332728571942532289631379312455583992563";
    CHECK(std::strcmp(tick.asset_id, expected_id) == 0, "asset_id mismatch");
    CHECK(near(tick.price, 0.52), "price mismatch");
    CHECK(near(tick.size, 100.5), "size mismatch");
    CHECK(near(tick.best_bid, 0.51), "best_bid mismatch");
    CHECK(near(tick.best_ask, 0.53), "best_ask mismatch");
    CHECK(tick.side == 0u, "side mismatch (expected BID=0)");
    CHECK(tick.event_type == 0u, "event_type mismatch (expected price_change=0)");
    CHECK(tick.timestamp == 1700000000000ULL, "timestamp mismatch");

    std::printf("  price=%.2f  size=%.1f  best_bid=%.2f  best_ask=%.2f  "
                "side=%u  event_type=%u  [OK]\n",
                tick.price, tick.size, tick.best_bid, tick.best_ask,
                tick.side, tick.event_type);
    std::puts("  => PASS");
    return true;
}

// ---------------------------------------------------------------------------
// Test 2 — Multi-leg price_change: two legs, two Ticks
// ---------------------------------------------------------------------------
static bool test_multi_leg()
{
    std::puts("[test_multi_leg]");

    static const char *JSON =
        "{"
        "\"event_type\":\"price_change\","
        "\"market\":\"0xdeadbeef\","
        "\"timestamp\":\"1700000001000\","
        "\"price_changes\":["
        "{"
        "\"asset_id\":\"11111111111111111111111111111111111111111111111111111111111111111111\","
        "\"price\":\"0.48\","
        "\"size\":\"50.0\","
        "\"side\":\"SELL\","
        "\"hash\":\"aaa\","
        "\"best_bid\":\"0.47\","
        "\"best_ask\":\"0.49\""
        "},"
        "{"
        "\"asset_id\":\"22222222222222222222222222222222222222222222222222222222222222222222\","
        "\"price\":\"0.52\","
        "\"size\":\"50.0\","
        "\"side\":\"BUY\","
        "\"hash\":\"bbb\","
        "\"best_bid\":\"0.51\","
        "\"best_ask\":\"0.53\""
        "}"
        "]"
        "}";

    static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> ring;
    polymarket::feedhandler::parse_and_push(live_view(make_padded(JSON)), ring);

    polymarket::core::Tick t1{};
    CHECK(ring.consume(t1), "Expected first Tick (leg 1)");
    CHECK(t1.side == 1u, "leg1 side mismatch (expected ASK=1)");
    CHECK(t1.event_type == 0u, "leg1 event_type mismatch");
    CHECK(near(t1.price, 0.48), "leg1 price mismatch");
    CHECK(near(t1.best_bid, 0.47), "leg1 best_bid mismatch");
    CHECK(near(t1.best_ask, 0.49), "leg1 best_ask mismatch");
    CHECK(t1.timestamp == 1700000001000ULL, "leg1 timestamp mismatch");
    std::printf("  leg1  side=ASK  price=%.2f  best_bid=%.2f  best_ask=%.2f  [OK]\n",
                t1.price, t1.best_bid, t1.best_ask);

    polymarket::core::Tick t2{};
    CHECK(ring.consume(t2), "Expected second Tick (leg 2)");
    CHECK(t2.side == 0u, "leg2 side mismatch (expected BID=0)");
    CHECK(t2.event_type == 0u, "leg2 event_type mismatch");
    CHECK(near(t2.price, 0.52), "leg2 price mismatch");
    CHECK(near(t2.best_bid, 0.51), "leg2 best_bid mismatch");
    CHECK(near(t2.best_ask, 0.53), "leg2 best_ask mismatch");
    CHECK(std::strcmp(t2.asset_id,
                      "22222222222222222222222222222222222222222222222222222222222222222222") == 0,
          "leg2 asset_id mismatch");
    std::printf("  leg2  side=BID  price=%.2f  best_bid=%.2f  best_ask=%.2f  [OK]\n",
                t2.price, t2.best_bid, t2.best_ask);

    polymarket::core::Tick extra{};
    CHECK(!ring.consume(extra), "Ring should be empty after both legs");

    std::puts("  => PASS");
    return true;
}

// ---------------------------------------------------------------------------
// Test 3 — "book" snapshot (bare JSON array) is silently dropped
// ---------------------------------------------------------------------------
static bool test_book_snapshot_dropped()
{
    std::puts("[test_book_snapshot_dropped]");

    static const char *JSON =
        "[{\"market\":\"0xdeadbeef\","
        "\"asset_id\":\"71321045679252212594626385532706912750332728571942532289631379312455583992563\","
        "\"timestamp\":\"1700000000000\","
        "\"hash\":\"abc\","
        "\"bids\":[{\"price\":\"0.51\",\"size\":\"100\"}],"
        "\"asks\":[{\"price\":\"0.52\",\"size\":\"200\"}]}]";

    static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> ring;
    polymarket::feedhandler::parse_and_push(live_view(make_padded(JSON)), ring);

    polymarket::core::Tick tick{};
    CHECK(!ring.consume(tick), "book snapshot must not produce a Tick");
    std::puts("  Ring correctly empty after book snapshot  [OK]");
    std::puts("  => PASS");
    return true;
}

// ---------------------------------------------------------------------------
// Test 4 — Malformed JSON is silently dropped
// ---------------------------------------------------------------------------
static bool test_malformed_json()
{
    std::puts("[test_malformed_json]");

    static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> ring;
    polymarket::feedhandler::parse_and_push(live_view(make_padded("{bad json{{{{")), ring);

    polymarket::core::Tick tick{};
    CHECK(!ring.consume(tick), "Malformed JSON must not produce a Tick");
    std::puts("  Ring correctly empty after bad JSON  [OK]");
    std::puts("  => PASS");
    return true;
}

// ---------------------------------------------------------------------------
// Test 5 — Missing required field in a leg → that leg skipped
// ---------------------------------------------------------------------------
static bool test_missing_field()
{
    std::puts("[test_missing_field]");

    // "price" absent from the only leg.
    static const char *JSON =
        "{"
        "\"event_type\":\"price_change\","
        "\"market\":\"0xdeadbeef\","
        "\"timestamp\":\"1700000000000\","
        "\"price_changes\":["
        "{"
        "\"asset_id\":\"71321045679252212594626385532706912750332728571942532289631379312455583992563\","
        "\"side\":\"BUY\","
        "\"size\":\"10.0\""
        "}"
        "]"
        "}";

    static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> ring;
    polymarket::feedhandler::parse_and_push(live_view(make_padded(JSON)), ring);

    polymarket::core::Tick tick{};
    CHECK(!ring.consume(tick), "Missing 'price' must not produce a Tick");
    std::puts("  Ring correctly empty after missing field  [OK]");
    std::puts("  => PASS");
    return true;
}

// ---------------------------------------------------------------------------
// Test 6 — last_trade_price: flat object, event_type==1, best_bid/ask==0.0
// ---------------------------------------------------------------------------
static bool test_last_trade_price()
{
    std::puts("[test_last_trade_price]");

    static const char *JSON =
        "{"
        "\"event_type\":\"last_trade_price\","
        "\"asset_id\":\"71321045679252212594626385532706912750332728571942532289631379312455583992563\","
        "\"market\":\"0xdeadbeef\","
        "\"timestamp\":\"1700000005000\","
        "\"price\":\"0.72\","
        "\"size\":\"500\","
        "\"side\":\"BUY\","
        "\"fee_rate_bps\":25"
        "}";

    static polymarket::memory::RingBuffer<polymarket::core::Tick, 65536> ring;
    polymarket::feedhandler::parse_and_push(live_view(make_padded(JSON)), ring);

    polymarket::core::Tick tick{};
    CHECK(ring.consume(tick), "Expected one Tick from last_trade_price");

    polymarket::core::Tick extra{};
    CHECK(!ring.consume(extra), "Ring should be empty after one consume");

    const char *expected_id =
        "71321045679252212594626385532706912750332728571942532289631379312455583992563";
    CHECK(std::strcmp(tick.asset_id, expected_id) == 0, "asset_id mismatch");
    CHECK(near(tick.price, 0.72), "price mismatch");
    CHECK(near(tick.size, 500.0), "size mismatch");
    CHECK(near(tick.best_bid, 0.0), "best_bid should be 0.0 for trade events");
    CHECK(near(tick.best_ask, 0.0), "best_ask should be 0.0 for trade events");
    CHECK(tick.side == 0u, "side mismatch (expected BID=0)");
    CHECK(tick.event_type == 1u, "event_type mismatch (expected last_trade_price=1)");
    CHECK(tick.timestamp == 1700000005000ULL, "timestamp mismatch");

    std::printf("  price=%.2f  size=%.0f  best_bid=%.2f  best_ask=%.2f  "
                "side=%u  event_type=%u  [OK]\n",
                tick.price, tick.size, tick.best_bid, tick.best_ask,
                tick.side, tick.event_type);
    std::puts("  => PASS");
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    std::puts("=== polymarket Feedhandler parse-and-push tests ===\n");

    bool pass = true;
    pass &= test_happy_path();
    std::putchar('\n');
    pass &= test_multi_leg();
    std::putchar('\n');
    pass &= test_book_snapshot_dropped();
    std::putchar('\n');
    pass &= test_malformed_json();
    std::putchar('\n');
    pass &= test_missing_field();
    std::putchar('\n');
    pass &= test_last_trade_price();
    std::putchar('\n');

    std::puts(pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
