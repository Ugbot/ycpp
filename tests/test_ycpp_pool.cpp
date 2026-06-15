// W1 pool gate. Acquire/release reuses slots; live counter is correct;
// grows beyond one slab.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_pool.h"

using ycpp::DefaultArenaAllocator;
using ycpp::Pool;

namespace {

struct Node {
    uint64_t x = 0;
    uint64_t y = 0;
    uint64_t z = 0;
    explicit Node(uint64_t v = 0) noexcept : x(v), y(v * 2), z(v * 3) {}
};

} // namespace

TEST(YcppPool, AcquireConstructsAndCountsLive) {
    DefaultArenaAllocator a;
    Pool<Node, DefaultArenaAllocator> p{&a, 8};

    Node* n = p.acquire(42);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->x, 42U);
    EXPECT_EQ(n->y, 84U);
    EXPECT_EQ(p.live(), 1U);

    p.release(n);
    EXPECT_EQ(p.live(), 0U);
}

TEST(YcppPool, ReleaseRecyclesSlot) {
    DefaultArenaAllocator a;
    Pool<Node, DefaultArenaAllocator> p{&a, 4};
    Node* first = p.acquire(1);
    p.release(first);
    Node* again = p.acquire(2);
    EXPECT_EQ(again, first);  // freelist LIFO returns the just-released slot
    p.release(again);
}

TEST(YcppPool, GrowsBeyondInitialSlab) {
    DefaultArenaAllocator a;
    Pool<Node, DefaultArenaAllocator> p{&a, 4};
    std::vector<Node*> live;
    for (int i = 0; i < 16; ++i) {
        Node* n = p.acquire(static_cast<uint64_t>(i));
        ASSERT_NE(n, nullptr);
        live.push_back(n);
    }
    EXPECT_EQ(p.live(),  16U);
    EXPECT_GE(p.slabs(), 4U);
    for (auto* n : live) p.release(n);
    EXPECT_EQ(p.live(), 0U);
}
