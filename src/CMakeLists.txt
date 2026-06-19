# Shared libraries (used by harvester and bot)
add_subdirectory(feedhandler)
add_subdirectory(tickerplant)
add_subdirectory(gateway)
add_subdirectory(rdb)

# Executables
add_subdirectory(harvester)     # polymarket_harvester — Polymarket tick data collector
add_subdirectory(coinbase)       # coinbase_harvester    — Coinbase BTC price collector
