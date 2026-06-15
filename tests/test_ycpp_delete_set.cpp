// W2 DeleteSet gate. Range insertion + adjacency-merge + binary-search
// contains query + encode/decode round-trip.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_delete_set.h"
#include "ycpp/ycpp_id.h"
#include "ycpp/ycpp_reader.h"
#include "ycpp/ycpp_writer.h"

using ycpp::ByteView;
using ycpp::DefaultArenaAllocator;
using ycpp::DeleteSet;
using ycpp::Id;
using ycpp::Reader;
using ycpp::Status;
using ycpp::Writer;

TEST(YcppDeleteSet, AddAndContainsBasic) {
    DefaultArenaAllocator a;
    DeleteSet<DefaultArenaAllocator> ds{&a};
    ASSERT_EQ(ds.add(7, 10, 3), Status::kOk);  // [10..13)
    EXPECT_TRUE (ds.contains(Id{7, 10}));
    EXPECT_TRUE (ds.contains(Id{7, 12}));
    EXPECT_FALSE(ds.contains(Id{7, 13}));
    EXPECT_FALSE(ds.contains(Id{7, 9}));
    EXPECT_FALSE(ds.contains(Id{8, 11}));
}

TEST(YcppDeleteSet, MergesAdjacentRanges) {
    DefaultArenaAllocator a;
    DeleteSet<DefaultArenaAllocator> ds{&a};
    ASSERT_EQ(ds.add(1, 0, 5),  Status::kOk);  // [0..5)
    ASSERT_EQ(ds.add(1, 5, 3),  Status::kOk);  // [5..8)  — adjacent
    ASSERT_EQ(ds.add(1, 12, 2), Status::kOk);  // [12..14)
    ASSERT_EQ(ds.add(1, 8, 4),  Status::kOk);  // [8..12)  — bridges into [0..14)

    // After merge, exactly one range covering [0..14).
    std::size_t count = 0;
    ds.for_each_client([&](uint64_t client, const ycpp::DeleteRange* rs,
                           std::size_t n) noexcept {
        EXPECT_EQ(client, 1U);
        count = n;
        ASSERT_EQ(n, 1U);
        EXPECT_EQ(rs[0].clock_start, 0U);
        EXPECT_EQ(rs[0].length,      14U);
    });
    EXPECT_EQ(count, 1U);
    EXPECT_TRUE (ds.contains(Id{1, 13}));
    EXPECT_FALSE(ds.contains(Id{1, 14}));
}

TEST(YcppDeleteSet, EncodeDecodeRoundTrip) {
    DefaultArenaAllocator a;
    DeleteSet<DefaultArenaAllocator> ds{&a};
    ASSERT_EQ(ds.add(1, 0,   3), Status::kOk);
    ASSERT_EQ(ds.add(1, 100, 5), Status::kOk);
    ASSERT_EQ(ds.add(7, 42,  1), Status::kOk);

    std::array<uint8_t, 128> buf{};
    Writer w{buf.data(), buf.size()};
    ASSERT_EQ(ds.encode(w), Status::kOk);

    DefaultArenaAllocator a2;
    DeleteSet<DefaultArenaAllocator> ds2{&a2};
    Reader r{ByteView{buf.data(), w.pos()}};
    ASSERT_EQ(ds2.decode(r), Status::kOk);

    EXPECT_TRUE (ds2.contains(Id{1, 0}));
    EXPECT_TRUE (ds2.contains(Id{1, 2}));
    EXPECT_FALSE(ds2.contains(Id{1, 3}));
    EXPECT_TRUE (ds2.contains(Id{1, 104}));
    EXPECT_FALSE(ds2.contains(Id{1, 105}));
    EXPECT_TRUE (ds2.contains(Id{7, 42}));
    EXPECT_EQ(ds2.client_count(), 2U);
}

TEST(YcppDeleteSet, DecodeRejectsZeroLengthRange) {
    // Hand-crafted: 1 client, client=3, 1 range, clock=0, length=0.
    std::array<uint8_t, 8> bad{ 0x01U, 0x03U, 0x01U, 0x00U, 0x00U };
    DefaultArenaAllocator a;
    DeleteSet<DefaultArenaAllocator> ds{&a};
    Reader r{ByteView{bad.data(), 5}};
    EXPECT_EQ(ds.decode(r), Status::kCorruptInput);
}
