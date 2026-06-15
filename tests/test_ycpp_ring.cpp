// W1 ring gate. SPSC push/pop, wrap-around, full/empty edges.

#include <gtest/gtest.h>

#include <cstdint>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_ring.h"

using ycpp::DefaultArenaAllocator;
using ycpp::SpscRing;

TEST(YcppRing, PushPopFifo) {
    DefaultArenaAllocator a;
    SpscRing<uint32_t, DefaultArenaAllocator> r{&a};
    ASSERT_TRUE(r.init(8));
    EXPECT_EQ(r.capacity(), 8U);
    EXPECT_TRUE(r.empty_approx());

    for (uint32_t i = 0; i < 7; ++i) {
        ASSERT_TRUE(r.try_push(i));
    }
    EXPECT_EQ(r.size_approx(), 7U);
    // 8th push fails — we reserve one slot for the full sentinel.
    EXPECT_FALSE(r.try_push(99));

    uint32_t out = 0;
    for (uint32_t expected = 0; expected < 7; ++expected) {
        ASSERT_TRUE(r.try_pop(&out));
        EXPECT_EQ(out, expected);
    }
    EXPECT_TRUE(r.empty_approx());
    EXPECT_FALSE(r.try_pop(&out));
}

TEST(YcppRing, WrapAroundPreservesOrder) {
    DefaultArenaAllocator a;
    SpscRing<uint32_t, DefaultArenaAllocator> r{&a};
    ASSERT_TRUE(r.init(4));

    uint32_t scratch = 0;
    for (uint32_t round = 0; round < 8; ++round) {
        ASSERT_TRUE(r.try_push(round * 100U + 1U));
        ASSERT_TRUE(r.try_push(round * 100U + 2U));
        ASSERT_TRUE(r.try_pop(&scratch));
        EXPECT_EQ(scratch, round * 100U + 1U);
        ASSERT_TRUE(r.try_pop(&scratch));
        EXPECT_EQ(scratch, round * 100U + 2U);
    }
}

TEST(YcppRing, DoubleInitRejected) {
    DefaultArenaAllocator a;
    SpscRing<uint32_t, DefaultArenaAllocator> r{&a};
    ASSERT_TRUE (r.init(4));
    EXPECT_FALSE(r.init(8));  // already populated
}
