# ---------------------------------------------------------------------------
# polymarket_gateway — static library
#
# Encapsulates the IXWebSocket client, thread affinity, and the JSON-to-ring
# bridge.  IXWebSocket headers are isolated to WebSocketClient.cpp and never
# leak into our public include tree.
#
# Transitive dependencies via PUBLIC linkage:
#   polymarket_feedhandler → simdjson, polymarket_compile_opts
#   ixwebsocket           → OpenSSL (when USE_TLS=ON)
#   Threads::Threads      → -lpthread (for pthread_setaffinity_np)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(polymarket_gateway STATIC
    WebSocketClient.cpp
    MarketDiscovery.cpp
)

target_include_directories(polymarket_gateway PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(polymarket_gateway PUBLIC
    polymarket_compile_opts   # -O3 / -march=native / ASAN flags
    polymarket_feedhandler    # parse_and_push + simdjson (transitive)
    ixwebsocket              # WebSocket client + OpenSSL TLS (transitive)
    httplib::httplib         # HTTPS boot-sequence client (header-only, SSL-enabled)
    OpenSSL::SSL             # httplib SSLClient needs these explicitly on some linkers
    OpenSSL::Crypto
    spdlog::spdlog           # diagnostic logging
    Threads::Threads         # -lpthread
)
