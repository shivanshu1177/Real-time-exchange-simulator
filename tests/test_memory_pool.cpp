// MemoryPool: fixed capacity, single owner, no duplicates.
#include <gtest/gtest.h>
#include "rtes/memory_pool.hpp"
#include <set>
#include <vector>

using namespace rtes;

TEST(MemoryPool, ExhaustionReturnsNull) {
    OrderPool pool(64);
    std::vector<Order*> got;
    for (int i = 0; i < 64; ++i) {
        Order* o = pool.acquire();
        ASSERT_NE(o, nullptr);
        got.push_back(o);
    }
    EXPECT_EQ(pool.acquire(), nullptr);
    EXPECT_EQ(pool.available(), 0u);
    pool.release(got.back());
    EXPECT_EQ(pool.available(), 1u);
    EXPECT_EQ(pool.acquire(), got.back());   // the freed slot is the one handed out
}

TEST(MemoryPool, NoDuplicatesAcrossAcquireReleaseCycles) {
    OrderPool pool(256);
    std::vector<Order*> live;
    std::set<Order*> live_set;
    uint64_t seed = 12345;
    auto next = [&] { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return seed >> 33; };
    for (int step = 0; step < 100000; ++step) {
        if (live.empty() || (next() % 2 == 0 && live.size() < 256)) {
            Order* o = pool.acquire();
            ASSERT_NE(o, nullptr);
            ASSERT_TRUE(live_set.insert(o).second) << "slot handed out twice";
            live.push_back(o);
        } else {
            size_t i = next() % live.size();
            pool.release(live[i]);
            live_set.erase(live[i]);
            live[i] = live.back();
            live.pop_back();
        }
        ASSERT_EQ(pool.available() + live.size(), pool.capacity());
    }
    for (Order* o : live) pool.release(o);
    EXPECT_EQ(pool.available(), pool.capacity());
}

TEST(MemoryPool, ReleaseNullIsANoOp) {
    OrderPool pool(4);
    pool.release(nullptr);
    EXPECT_EQ(pool.available(), 4u);
}

#ifndef NDEBUG
// Debug builds enforce the single-owner rule and catch double releases.
TEST(MemoryPoolDeathTest, DoubleReleaseAsserts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({
        OrderPool pool(4);
        Order* o = pool.acquire();
        pool.release(o);
        pool.release(o);
    }, "double release");
}

TEST(MemoryPoolDeathTest, UseFromASecondThreadAsserts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({
        OrderPool pool(4);
        Order* o = pool.acquire();
        std::thread t([&] { pool.release(o); });
        t.join();
    }, "two threads");
}
#endif
