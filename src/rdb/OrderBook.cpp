// OrderBook.cpp
//
// This translation unit exists so the build system has a concrete .cpp file to
// compile into polymarket_rdb.  All the logic lives in the header (inline /
// template), so this file only provides the mandatory compilation unit that
// CMake needs to produce a STATIC library.
//
// Nothing to define here: every method is inlined in OrderBook.hpp.

#include "polymarket/rdb/OrderBook.hpp"
