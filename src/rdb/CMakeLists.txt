find_package(Threads REQUIRED)

add_library(polymarket_rdb STATIC OrderBook.cpp)

target_include_directories(polymarket_rdb PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(polymarket_rdb PUBLIC
    polymarket_compile_opts   # -O3 / -march=native in Release; ASAN in Debug
    Threads::Threads
)
