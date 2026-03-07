add_executable(polymarket_harvester
    main.cpp
)

target_include_directories(polymarket_harvester PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(polymarket_harvester PRIVATE
    polymarket_feedhandler   # feedhandler + simdjson (encapsulated)
    polymarket_tickerplant   # tickerplant + pthread
    polymarket_gateway       # WebSocket client + IXWebSocket + OpenSSL
    spdlog::spdlog           # Compiled async logger
)
