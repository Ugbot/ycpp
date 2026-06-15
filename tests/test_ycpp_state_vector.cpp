// W3 StateVector gate. Set / get / encode / decode round-trip.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_byteview.h"
#include "ycpp/ycpp_reader.h"
#include "ycpp/ycpp_state_vector.h"
#include "ycpp/ycpp_writer.h"

using ycpp::ByteView;
using ycpp::DefaultArenaAllocator;
using ycpp::Reader;
using ycpp::StateVector;
using ycpp::Status;
using ycpp::Writer;

TEST(YcppStateVector, SetAndGetBasic) {
    DefaultArenaAllocator a;
    StateVector<DefaultArenaAllocator> sv{&a};
    ASSERT_EQ(sv.set(1, 5), Status::kOk);
    ASSERT_EQ(sv.set(7, 9), Status::kOk);
    EXPECT_EQ(sv.get(1), 5U);
    EXPECT_EQ(sv.get(7), 9U);
    EXPECT_EQ(sv.get(2), 0U);  // unknown clients default to 0
    EXPECT_EQ(sv.client_count(), 2U);
}

TEST(YcppStateVector, UpdateOverwrites) {
    DefaultArenaAllocator a;
    StateVector<DefaultArenaAllocator> sv{&a};
    ASSERT_EQ(sv.set(1, 5), Status::kOk);
    ASSERT_EQ(sv.set(1, 8), Status::kOk);
    EXPECT_EQ(sv.get(1), 8U);
    EXPECT_EQ(sv.client_count(), 1U);
}

TEST(YcppStateVector, EncodeDecodeRoundTrip) {
    DefaultArenaAllocator a;
    StateVector<DefaultArenaAllocator> sv{&a};
    ASSERT_EQ(sv.set(11,  4), Status::kOk);
    ASSERT_EQ(sv.set(22, 19), Status::kOk);
    ASSERT_EQ(sv.set(33,  0), Status::kOk);  // explicit zero

    std::array<uint8_t, 64> buf{};
    Writer w{buf.data(), buf.size()};
    ASSERT_EQ(sv.encode(w), Status::kOk);

    DefaultArenaAllocator a2;
    StateVector<DefaultArenaAllocator> sv2{&a2};
    Reader r{ByteView{buf.data(), w.pos()}};
    ASSERT_EQ(sv2.decode(r), Status::kOk);

    EXPECT_EQ(sv2.get(11),  4U);
    EXPECT_EQ(sv2.get(22), 19U);
    EXPECT_EQ(sv2.get(33),  0U);
    EXPECT_EQ(sv2.client_count(), 3U);
}

TEST(YcppStateVector, ForEachIteratesAllEntries) {
    DefaultArenaAllocator a;
    StateVector<DefaultArenaAllocator> sv{&a};
    ASSERT_EQ(sv.set(1, 100), Status::kOk);
    ASSERT_EQ(sv.set(2, 200), Status::kOk);
    ASSERT_EQ(sv.set(3, 300), Status::kOk);

    uint64_t sum_client = 0, sum_clock = 0;
    sv.for_each([&](uint64_t c, uint64_t k) noexcept {
        sum_client += c;
        sum_clock  += k;
    });
    EXPECT_EQ(sum_client,   6U);
    EXPECT_EQ(sum_clock,  600U);
}
