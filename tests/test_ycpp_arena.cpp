// W1 arena gate. Allocation arithmetic, alignment, oversize path, reset.

#include <gtest/gtest.h>

#include <cstdint>

#include "ycpp/ycpp_arena.h"

using ycpp::DefaultArenaAllocator;

TEST(YcppArena, BasicAllocReturnsAlignedNonNull) {
    DefaultArenaAllocator a;
    void* p = a.alloc(64, 8);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8U, 0U);
    EXPECT_EQ(a.bytes_in_use(), 64U);
}

TEST(YcppArena, ZeroBytesReturnsNull) {
    DefaultArenaAllocator a;
    EXPECT_EQ(a.alloc(0, 8), nullptr);
}

TEST(YcppArena, AlignmentRespected) {
    DefaultArenaAllocator a;
    // Burn a byte so the next allocation must pad.
    (void)a.alloc(1, 1);
    void* p = a.alloc(16, 16);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16U, 0U);
}

TEST(YcppArena, ChainsToNewBlockWhenFull) {
    DefaultArenaAllocator a(64);
    void* a1 = a.alloc(32, 1);
    void* a2 = a.alloc(32, 1);
    void* a3 = a.alloc(16, 1);  // must trigger a new 64B block (only 0B free)
    ASSERT_NE(a1, nullptr);
    ASSERT_NE(a2, nullptr);
    ASSERT_NE(a3, nullptr);
    EXPECT_GE(a.block_count(), 2U);
}

TEST(YcppArena, OversizeAllocationGoesToItsOwnBlock) {
    DefaultArenaAllocator a(128);
    void* p = a.alloc(4096, 16);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16U, 0U);
    EXPECT_GE(a.block_count(), 1U);
}

TEST(YcppArena, ResetClearsBlocks) {
    DefaultArenaAllocator a(128);
    for (int i = 0; i < 10; ++i) (void)a.alloc(64, 1);
    EXPECT_GT(a.block_count(), 0U);
    a.reset();
    EXPECT_EQ(a.block_count(),  0U);
    EXPECT_EQ(a.bytes_in_use(), 0U);
    // After reset, the arena must still be usable.
    EXPECT_NE(a.alloc(32, 1), nullptr);
}

TEST(YcppArena, FreeDecreasesBytesInUse) {
    DefaultArenaAllocator a;
    void* p = a.alloc(128, 8);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(a.bytes_in_use(), 128U);
    a.free(p, 128);
    EXPECT_EQ(a.bytes_in_use(), 0U);
}
