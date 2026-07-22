# ---------------------------------------------------------------------------
# polymarket_rdb — header-only INTERFACE library
#
# The RDB OrderBook is defined entirely in the header (inline / templated), so
# there is no translation unit to compile. An INTERFACE library propagates the
# include path and compile options to consumers (the tickerplant and the
# order-book test) without producing an empty static archive — which previously
# tripped ranlib's "table of contents is empty" warning.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(polymarket_rdb INTERFACE)

target_include_directories(polymarket_rdb INTERFACE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(polymarket_rdb INTERFACE
    polymarket_compile_opts   # -O3 / -march=native in Release; ASAN in Debug
    Threads::Threads
)
