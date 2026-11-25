#include <catch2/catch_test_macros.hpp>

#include "bigtop/SpscQueue.h"

using namespace bigtop;

// FIFO order, empty pop fails, and all Capacity slots are usable before push fails.
TEST_CASE("SpscQueue keeps order and reports full") {
    SpscQueue<int, 4> q; int v = 0;
    CHECK_FALSE(q.pop(v));
    for (int i = 0; i < 4; ++i) CHECK(q.push(i));
    CHECK_FALSE(q.push(99));
    for (int i = 0; i < 4; ++i) { REQUIRE(q.pop(v)); CHECK(v == i); }
}
