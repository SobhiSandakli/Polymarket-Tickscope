enable_testing()

# ---------------------------------------------------------------------------
# test_ringbuffer — MPSC RingBuffer stress test (Phase 2)
#
# No external test framework needed: the test binary returns EXIT_SUCCESS /
# EXIT_FAILURE, which CTest interprets as pass / fail.
# Link only polymarket_compile_opts — the RingBuffer is a header-only template.
# ---------------------------------------------------------------------------
add_executable(test_ringbuffer
    test_ringbuffer.cpp
)

target_include_directories(test_ringbuffer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_ringbuffer PRIVATE
    polymarket_compile_opts  # -O3 / -march=native / ASAN flags
)

add_test(
    NAME    RingBufferStressTest
    COMMAND test_ringbuffer
)

# Give the stress test a generous timeout (10 producers × 100k items).
set_tests_properties(RingBufferStressTest PROPERTIES TIMEOUT 60)

# ---------------------------------------------------------------------------
# test_feedhandler — Feedhandler JSON parse → RingBuffer round-trip (Phase 3)
#
# Links against polymarket_feedhandler (which pulls in simdjson transitively).
# ---------------------------------------------------------------------------
add_executable(test_feedhandler
    test_feedhandler.cpp
)

target_link_libraries(test_feedhandler PRIVATE
    polymarket_feedhandler   # simdjson + compile opts (via PUBLIC linkage)
)

add_test(
    NAME    FeedhandlerParseTest
    COMMAND test_feedhandler
)

set_tests_properties(FeedhandlerParseTest PROPERTIES TIMEOUT 10)

# ---------------------------------------------------------------------------
# test_tickerplant — end-to-end journal write/verify test (Phase 4)
#
# Pushes 100,000 ticks through the ring buffer, waits for the Tickerplant
# to flush them to /tmp/polymarket_test_journal.bin, then reads back and
# verifies every field of every record.
# ---------------------------------------------------------------------------
add_executable(test_tickerplant
    test_tickerplant.cpp
)

target_link_libraries(test_tickerplant PRIVATE
    polymarket_tickerplant   # Threads::Threads + compile opts (transitively)
)

add_test(
    NAME    TickerplantJournalTest
    COMMAND test_tickerplant
)

# 100k ticks through a 1024-slot ring: allow 30 s in a slow CI container.
set_tests_properties(TickerplantJournalTest PROPERTIES TIMEOUT 30)

# ---------------------------------------------------------------------------
# test_orderbook — In-memory RDB OrderBook unit tests (Phase 6)
#
# Five checks: routing, bid/ask last-write-wins, cross-token isolation,
# and struct layout.  Links only polymarket_rdb (header-only, but STATIC
# library gives CMake a concrete link target).
# ---------------------------------------------------------------------------
add_executable(test_orderbook
    test_orderbook.cpp
)

target_link_libraries(test_orderbook PRIVATE
    polymarket_rdb   # compile opts transitively
)

add_test(
    NAME    OrderBookTest
    COMMAND test_orderbook
)

set_tests_properties(OrderBookTest PROPERTIES TIMEOUT 10)
