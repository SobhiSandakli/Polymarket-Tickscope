# ---------------------------------------------------------------------------
# polymarket_feedhandler — static library
#
# Encapsulates the simdjson dependency: only this .cpp file includes
# simdjson.h, keeping the heavy macro machinery out of all other TUs.
# The PUBLIC include path exposes polymarket headers to dependents.
# ---------------------------------------------------------------------------
add_library(polymarket_feedhandler STATIC
    Feedhandler.cpp
)

target_include_directories(polymarket_feedhandler PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(polymarket_feedhandler PUBLIC
    polymarket_compile_opts  # -O3 / -march=native / ASAN flags
    simdjson                # SIMD JSON parser
    spdlog::spdlog          # async overflow warning logs
)
