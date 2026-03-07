# ---------------------------------------------------------------------------
# polymarket_tickerplant — static library
#
# Encapsulates pthread affinity and POSIX file I/O.
# The PUBLIC include path propagates polymarket headers to dependents.
# Threads::Threads pulls in -lpthread automatically.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(polymarket_tickerplant STATIC
    Tickerplant.cpp
)

target_include_directories(polymarket_tickerplant PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(polymarket_tickerplant PUBLIC
    polymarket_compile_opts  # -O3 / -march=native / ASAN flags
    Threads::Threads        # -lpthread (for pthread_setaffinity_np)
)
