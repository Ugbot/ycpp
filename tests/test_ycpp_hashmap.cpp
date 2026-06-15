// W1 hashmap gate. Insert / find / erase across load factors; tombstones
// don't permanently degrade lookups; reserve does what it claims.

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_set>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_hashmap.h"

using ycpp::DefaultArenaAllocator;
using ycpp::HashMap;

TEST(YcppHashMap, InsertFindEraseRoundTrip) {
    DefaultArenaAllocator a;
    HashMap<uint64_t, uint64_t, DefaultArenaAllocator> m{&a};
    auto [e1, ok1] = m.insert(1ULL, 100ULL);
    auto [e2, ok2] = m.insert(2ULL, 200ULL);
    ASSERT_TRUE(ok1);
    ASSERT_TRUE(ok2);
    ASSERT_NE(e1, nullptr);
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(m.size(), 2U);

    auto* hit = m.find(1ULL);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->value, 100ULL);

    EXPECT_TRUE (m.erase(1ULL));
    EXPECT_EQ   (m.find(1ULL), nullptr);
    EXPECT_EQ   (m.size(), 1U);
    EXPECT_FALSE(m.erase(1ULL));
}

TEST(YcppHashMap, DuplicateInsertReturnsExisting) {
    DefaultArenaAllocator a;
    HashMap<uint64_t, uint64_t, DefaultArenaAllocator> m{&a};
    auto [e1, ok1] = m.insert(42ULL, 1ULL);
    auto [e2, ok2] = m.insert(42ULL, 999ULL);
    EXPECT_TRUE (ok1);
    EXPECT_FALSE(ok2);
    EXPECT_EQ   (e1, e2);
    EXPECT_EQ   (e1->value, 1ULL);  // first insert wins; we don't overwrite
}

TEST(YcppHashMap, GrowsAndPreservesEntries) {
    DefaultArenaAllocator a;
    HashMap<uint64_t, uint64_t, DefaultArenaAllocator> m{&a};
    constexpr uint64_t kN = 2048;
    for (uint64_t i = 0; i < kN; ++i) {
        auto [e, inserted] = m.insert(i, i * 7ULL);
        ASSERT_TRUE(inserted) << "insert i=" << i << " failed";
        ASSERT_NE  (e, nullptr);
    }
    EXPECT_EQ(m.size(), kN);
    for (uint64_t i = 0; i < kN; ++i) {
        auto* hit = m.find(i);
        ASSERT_NE(hit, nullptr) << "missing i=" << i;
        EXPECT_EQ(hit->value, i * 7ULL);
    }
}

TEST(YcppHashMap, EraseLeavesTombstonesThatLookupTraverses) {
    DefaultArenaAllocator a;
    HashMap<uint64_t, uint64_t, DefaultArenaAllocator> m{&a};
    for (uint64_t i = 0; i < 64; ++i) (void)m.insert(i, i);
    for (uint64_t i = 0; i < 64; i += 2) EXPECT_TRUE(m.erase(i));
    for (uint64_t i = 1; i < 64; i += 2) {
        auto* hit = m.find(i);
        ASSERT_NE(hit, nullptr);
        EXPECT_EQ(hit->value, i);
    }
    for (uint64_t i = 0; i < 64; i += 2) {
        EXPECT_EQ(m.find(i), nullptr);
    }
}

TEST(YcppHashMap, ReserveGrowsCapacityWithoutBreakingEntries) {
    DefaultArenaAllocator a;
    HashMap<uint64_t, uint64_t, DefaultArenaAllocator> m{&a};
    (void)m.insert(7ULL, 70ULL);
    ASSERT_TRUE(m.reserve(1024));
    EXPECT_GE(m.capacity(), 1024U);
    auto* hit = m.find(7ULL);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->value, 70ULL);
}
