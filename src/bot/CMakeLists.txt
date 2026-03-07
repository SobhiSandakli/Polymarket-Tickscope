cmake_minimum_required(VERSION 3.25)

# polymarket_bot — standalone ConvergenceNo execution bot
#
# Dependencies: all FetchContent'd in the root CMakeLists.txt:
#   simdjson, spdlog, IXWebSocket, cpp-httplib, OpenSSL

add_executable(polymarket_bot
    main.cpp
    BookState.cpp
    BotDiscovery.cpp
    FeedHandler.cpp
    OrderGateway.cpp
)

target_include_directories(polymarket_bot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # own headers (BotConfig.hpp etc.)
    ${CMAKE_SOURCE_DIR}/include          # shared polymarket includes
    ${simdjson_SOURCE_DIR}/include
    ${spdlog_SOURCE_DIR}/include
    ${ixwebsocket_SOURCE_DIR}
)

target_link_libraries(polymarket_bot PRIVATE
    polymarket_compile_opts   # HFT-grade compiler flags from root
    simdjson
    spdlog::spdlog
    ixwebsocket
    httplib::httplib          # carries CPPHTTPLIB_OPENSSL_SUPPORT + include path
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

# Suppress warnings from httplib's use of deprecated OpenSSL calls.
target_compile_options(polymarket_bot PRIVATE
    -Wno-deprecated-declarations
)
