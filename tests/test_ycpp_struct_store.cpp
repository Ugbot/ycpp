// W2 StructStore gate. Append-in-order, find_by_id binary search, state(),
// non-monotonic append rejection.

#include <gtest/gtest.h>

#include <cstdint>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_id.h"
#include "ycpp/ycpp_item.h"
#include "ycpp/ycpp_pool.h"
#include "ycpp/ycpp_struct_store.h"

using ycpp::DefaultArenaAllocator;
using ycpp::Id;
using ycpp::Item;
using ycpp::Pool;
using ycpp::Status;
using ycpp::StructStore;

namespace {

Item make_item(uint64_t client, uint64_t clock, uint64_t len = 1) {
    Item it{};
    it.id     = Id{client, clock};
    it.length = len;
    return it;
}

} // namespace

TEST(YcppStructStore, AppendThenFindRoundTrip) {
    DefaultArenaAllocator a;
    Pool<Item, DefaultArenaAllocator> pool{&a, 16};
    StructStore<DefaultArenaAllocator> store{&a};

    Item* i0 = pool.acquire(make_item(1, 0));
    Item* i1 = pool.acquire(make_item(1, 1));
    Item* i2 = pool.acquire(make_item(1, 2));
    ASSERT_NE(i0, nullptr);
    ASSERT_NE(i1, nullptr);
    ASSERT_NE(i2, nullptr);
    ASSERT_EQ(store.append(i0), Status::kOk);
    ASSERT_EQ(store.append(i1), Status::kOk);
    ASSERT_EQ(store.append(i2), Status::kOk);

    EXPECT_EQ(store.find_by_id(Id{1, 0}), i0);
    EXPECT_EQ(store.find_by_id(Id{1, 1}), i1);
    EXPECT_EQ(store.find_by_id(Id{1, 2}), i2);
    EXPECT_EQ(store.find_by_id(Id{1, 3}), nullptr);
    EXPECT_EQ(store.find_by_id(Id{2, 0}), nullptr);

    EXPECT_EQ(store.state(1), 3U);
    EXPECT_EQ(store.state(2), 0U);
    EXPECT_EQ(store.total_items(), 3U);
}

TEST(YcppStructStore, OutOfOrderAppendIsPending) {
    DefaultArenaAllocator a;
    Pool<Item, DefaultArenaAllocator> pool{&a, 4};
    StructStore<DefaultArenaAllocator> store{&a};

    Item* i0 = pool.acquire(make_item(7, 0));
    Item* i5 = pool.acquire(make_item(7, 5));   // skips 1..4
    ASSERT_EQ(store.append(i0), Status::kOk);
    EXPECT_EQ(store.append(i5), Status::kPendingReference);
}

TEST(YcppStructStore, MultiClientStateIsolated) {
    DefaultArenaAllocator a;
    Pool<Item, DefaultArenaAllocator> pool{&a, 8};
    StructStore<DefaultArenaAllocator> store{&a};

    ASSERT_EQ(store.append(pool.acquire(make_item(1, 0))), Status::kOk);
    ASSERT_EQ(store.append(pool.acquire(make_item(1, 1))), Status::kOk);
    ASSERT_EQ(store.append(pool.acquire(make_item(2, 0))), Status::kOk);

    EXPECT_EQ(store.state(1), 2U);
    EXPECT_EQ(store.state(2), 1U);
    EXPECT_EQ(store.client_count(), 2U);
}

TEST(YcppStructStore, FindByIdHandlesRunLengths) {
    DefaultArenaAllocator a;
    Pool<Item, DefaultArenaAllocator> pool{&a, 4};
    StructStore<DefaultArenaAllocator> store{&a};

    // A GC of length 5 starting at clock 0 covers ids 0..4.
    Item* gc = pool.acquire(make_item(3, 0, 5));
    ASSERT_EQ(store.append(gc), Status::kOk);
    for (uint64_t clk = 0; clk < 5; ++clk) {
        EXPECT_EQ(store.find_by_id(Id{3, clk}), gc) << "clock=" << clk;
    }
    EXPECT_EQ(store.find_by_id(Id{3, 5}), nullptr);
    EXPECT_EQ(store.state(3), 5U);
}
