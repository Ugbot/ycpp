// W2 Id gate. Comparators, kInvalidId sentinel, hash distribution sanity.

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_set>

#include "ycpp/ycpp_id.h"

using ycpp::Id;
using ycpp::IdEq;
using ycpp::IdHash;
using ycpp::id_after;
using ycpp::is_valid;
using ycpp::kInvalidId;

TEST(YcppId, EqualityAndOrdering) {
    Id a{1, 5};
    Id b{1, 5};
    Id c{1, 6};
    Id d{2, 0};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_TRUE(a < c);
    EXPECT_TRUE(c < d);
    EXPECT_FALSE(d < a);
}

TEST(YcppId, KInvalidIsSentinel) {
    EXPECT_FALSE(is_valid(kInvalidId));
    EXPECT_TRUE (is_valid(Id{0, 0}));
}

TEST(YcppId, IdAfterAdvancesClock) {
    Id base{42, 100};
    EXPECT_EQ(id_after(base, 1), (Id{42, 101}));
    EXPECT_EQ(id_after(base, 7), (Id{42, 107}));
}

TEST(YcppId, HashDistributesAcrossClientsAndClocks) {
    IdHash h;
    std::unordered_set<std::size_t> seen;
    for (uint64_t client = 1; client <= 8; ++client) {
        for (uint64_t clock = 0; clock < 8; ++clock) {
            seen.insert(h(Id{client, clock}));
        }
    }
    // 64 distinct IDs — collisions should be rare; require ≥ 60 unique buckets.
    EXPECT_GE(seen.size(), 60U);
}

TEST(YcppId, EqFunctorMatchesOperator) {
    IdEq eq;
    EXPECT_TRUE (eq(Id{1, 2}, Id{1, 2}));
    EXPECT_FALSE(eq(Id{1, 2}, Id{1, 3}));
}
