cmake_minimum_required(VERSION 3.25)

project(polymarket
    VERSION 0.1.0
    DESCRIPTION "Ultra-low latency HFT infrastructure for Polymarket"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Global C++20 settings — no compiler extensions, strict standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Export compile commands so clangd can find all headers (simdjson, ixwebsocket, spdlog, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# HFT compiler flags
#   Release: maximum throughput, native ISA (AVX-512 / AVX2 on target host)
#   Debug:   sanitizers for correctness checking during development
# ---------------------------------------------------------------------------
add_library(polymarket_compile_opts INTERFACE)

target_compile_options(polymarket_compile_opts INTERFACE
    $<$<CONFIG:Release>:
        -O3
        -march=native
        -funroll-loops
        -ffast-math
        -DNDEBUG
    >
    $<$<CONFIG:Debug>:
        -O0
        -g3
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    >
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

target_link_options(polymarket_compile_opts INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# ---------------------------------------------------------------------------
# Dependencies via FetchContent (no system install required)
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)          # show download progress

# --- simdjson ---------------------------------------------------------------
# Zero-copy, SIMD-accelerated JSON parser — used in every WebSocket worker.
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.10.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(simdjson)

# --- spdlog -----------------------------------------------------------------
# Compiled (non-header-only) build for faster incremental recompilation.
# Async sink is used in main.cpp; background I/O thread is initialised once
# at startup and never touched again in the hot path.
set(SPDLOG_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL          OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL     OFF CACHE BOOL "" FORCE)   # use bundled {fmt}

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)

# --- OpenSSL ----------------------------------------------------------------
# Required by IXWebSocket for wss:// TLS connections.
find_package(OpenSSL REQUIRED)

# --- cpp-httplib ------------------------------------------------------------
# Header-only HTTPS client used during the boot sequence to fetch active
# token IDs from the Polymarket Gamma REST API.  NOT used in the hot path.
# HTTPLIB_USE_OPENSSL_IF_AVAILABLE=ON (default) picks up the OpenSSL already
# found above and sets CPPHTTPLIB_OPENSSL_SUPPORT on the httplib::httplib target.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE ON  CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE                  OFF CACHE BOOL "" FORCE)  # keep header-only
set(HTTPLIB_INSTALL                  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    cpp-httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cpp-httplib)

# --- IXWebSocket ------------------------------------------------------------
# Production-grade C++ WebSocket client used by the Live Network Gateway.
# uWebSockets' ClientApp is an unfinshed stub; IXWebSocket is the correct
# client-mode library with full TLS support via OpenSSL.
set(USE_TLS             ON  CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.6
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ixwebsocket)

# ---------------------------------------------------------------------------
# Sub-projects
# ---------------------------------------------------------------------------
# enable_testing() must be called from the root scope so CTest discovers
# tests registered in subdirectories via add_test().
enable_testing()

add_subdirectory(src)
add_subdirectory(src/bot)
add_subdirectory(tests)

# Note: src/harvester, src/coinbase, and src/bot are added via src/CMakeLists.txt
# and the root-level add_subdirectory(src/bot) above.
