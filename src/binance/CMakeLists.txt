add_executable(binance_harvester
    main.cpp
    BinanceFeed.cpp
)

target_include_directories(binance_harvester PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}      # BtcJournal.hpp, BinanceFeed.hpp
)

target_link_libraries(binance_harvester PRIVATE
    simdjson
    ixwebsocket
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
)
