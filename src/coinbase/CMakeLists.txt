add_executable(coinbase_harvester
    main.cpp
    CoinbaseFeed.cpp
)

target_include_directories(coinbase_harvester PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}      # BtcJournal.hpp, CoinbaseFeed.hpp
)

target_link_libraries(coinbase_harvester PRIVATE
    simdjson
    ixwebsocket
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
)
