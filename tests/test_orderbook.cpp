// test_orderbook.cpp
//
// Five focused tests for polymarket::rdb::OrderBook.
// No external test framework — binary returns EXIT_SUCCESS or EXIT_FAILURE.
//
// Tests
// ─────
//  1. test_routing            — multiple tokens get distinct indices
//  2. test_bid_update         — last BID tick wins
//  3. test_ask_update         — last ASK tick wins
//  4. test_two_assets_independent — interleaved ticks don't cross-contaminate
//  5. test_table_layout       — sizeof / alignof static guarantees

#include "polymarket/rdb/OrderBook.hpp"
#include "polymarket/core/Tick.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Minimal assertion helper (always compiled in, unlike assert with NDEBUG).
// ---------------------------------------------------------------------------
#define CHECK(expr)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            std::fprintf(stderr,                                             \
                         "[FAIL] %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            std::exit(EXIT_FAILURE);                                         \
        }                                                                    \
    } while (false)

// ---------------------------------------------------------------------------
// Helper: build a Tick with only the fields OrderBook cares about.
// ---------------------------------------------------------------------------
static polymarket::core::Tick make_tick(const char *asset_id,
                                        uint8_t side,
                                        double price,
                                        double size)
{
    polymarket::core::Tick t{};
    t.side = side;
    t.price = price;
    t.size = size;
    std::strncpy(t.asset_id, asset_id, sizeof(t.asset_id) - 1);
    t.asset_id[sizeof(t.asset_id) - 1] = '\0';
    return t;
}

// ---------------------------------------------------------------------------
// Test 1 — Routing: two distinct tokens get index 0 and index 1.
// ---------------------------------------------------------------------------
static void test_routing()
{
    polymarket::rdb::OrderBook ob;

    // 6 ticks: 3 on token A, 3 on token B (interleaved)
    ob.apply_tick(make_tick("TOKEN_A", 0, 0.50, 100.0));
    ob.apply_tick(make_tick("TOKEN_B", 1, 0.80, 200.0));
    ob.apply_tick(make_tick("TOKEN_A", 1, 0.51, 110.0));
    ob.apply_tick(make_tick("TOKEN_B", 0, 0.79, 190.0));
    ob.apply_tick(make_tick("TOKEN_A", 0, 0.52, 120.0));
    ob.apply_tick(make_tick("TOKEN_B", 1, 0.81, 210.0));

    // Should have created exactly 2 tokens
    CHECK(ob.token_count() == 2);

    uint16_t idx_a = 9999, idx_b = 9999;
    CHECK(ob.find_index("TOKEN_A", idx_a));
    CHECK(ob.find_index("TOKEN_B", idx_b));

    // First token seen gets idx 0, second gets idx 1
    CHECK(idx_a == 0);
    CHECK(idx_b == 1);

    std::puts("[PASS] test_routing");
}

// ---------------------------------------------------------------------------
// Test 2 — BID updates: last write wins.
// ---------------------------------------------------------------------------
static void test_bid_update()
{
    polymarket::rdb::OrderBook ob;

    ob.apply_tick(make_tick("TOK", 0, 0.50, 100.0));
    ob.apply_tick(make_tick("TOK", 0, 0.55, 150.0));
    ob.apply_tick(make_tick("TOK", 0, 0.60, 200.0));

    uint16_t idx = 0;
    CHECK(ob.find_index("TOK", idx));
    const auto &ms = ob.state(idx);

    CHECK(ms.best_bid_price == 0.60);
    CHECK(ms.best_bid_size == 200.0);
    // ASK fields untouched — still default 0
    CHECK(ms.best_ask_price == 0.0);
    CHECK(ms.best_ask_size == 0.0);

    std::puts("[PASS] test_bid_update");
}

// ---------------------------------------------------------------------------
// Test 3 — ASK updates: last write wins.
// ---------------------------------------------------------------------------
static void test_ask_update()
{
    polymarket::rdb::OrderBook ob;

    ob.apply_tick(make_tick("TOK", 1, 0.70, 300.0));
    ob.apply_tick(make_tick("TOK", 1, 0.65, 250.0));
    ob.apply_tick(make_tick("TOK", 1, 0.62, 220.0));

    uint16_t idx = 0;
    CHECK(ob.find_index("TOK", idx));
    const auto &ms = ob.state(idx);

    CHECK(ms.best_ask_price == 0.62);
    CHECK(ms.best_ask_size == 220.0);
    // BID fields untouched
    CHECK(ms.best_bid_price == 0.0);
    CHECK(ms.best_bid_size == 0.0);

    std::puts("[PASS] test_ask_update");
}

// ---------------------------------------------------------------------------
// Test 4 — Two assets are fully independent.
// ---------------------------------------------------------------------------
static void test_two_assets_independent()
{
    polymarket::rdb::OrderBook ob;

    // Token X: only BID ticks
    ob.apply_tick(make_tick("TOKEN_X", 0, 0.30, 500.0));
    ob.apply_tick(make_tick("TOKEN_X", 0, 0.31, 510.0));

    // Token Y: only ASK ticks
    ob.apply_tick(make_tick("TOKEN_Y", 1, 0.75, 300.0));
    ob.apply_tick(make_tick("TOKEN_Y", 1, 0.74, 290.0));

    uint16_t ix = 9999, iy = 9999;
    CHECK(ob.find_index("TOKEN_X", ix));
    CHECK(ob.find_index("TOKEN_Y", iy));
    CHECK(ix != iy);

    const auto &x = ob.state(ix);
    const auto &y = ob.state(iy);

    // X has BID, no ASK
    CHECK(x.best_bid_price == 0.31);
    CHECK(x.best_bid_size == 510.0);
    CHECK(x.best_ask_price == 0.0);
    CHECK(x.best_ask_size == 0.0);

    // Y has ASK, no BID
    CHECK(y.best_ask_price == 0.74);
    CHECK(y.best_ask_size == 290.0);
    CHECK(y.best_bid_price == 0.0);
    CHECK(y.best_bid_size == 0.0);

    std::puts("[PASS] test_two_assets_independent");
}

// ---------------------------------------------------------------------------
// Test 5 — Table layout: compile-time size/align guarantees at run time too.
// ---------------------------------------------------------------------------
static void test_table_layout()
{
    // These mirror the static_asserts in OrderBook.hpp — defensive belt-and-
    // suspenders check that the binary actually honours the struct layout.
    CHECK(sizeof(polymarket::rdb::MarketState) == 64);
    CHECK(alignof(polymarket::rdb::MarketState) == 64);

    std::puts("[PASS] test_table_layout");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main()
{
    test_routing();
    test_bid_update();
    test_ask_update();
    test_two_assets_independent();
    test_table_layout();

    std::puts("\nAll OrderBook tests passed.");
    return EXIT_SUCCESS;
}
