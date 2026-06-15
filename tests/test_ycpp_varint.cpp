// W1 varint gate. Tiger Style + LEB128: round trip every interesting value
// and confirm the size predictions match the actual encoder output.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

#include "ycpp/ycpp_varint.h"

using ycpp::Status;
using ycpp::varint_decode_i64;
using ycpp::varint_decode_u64;
using ycpp::varint_encode_i64;
using ycpp::varint_encode_u64;
using ycpp::varint_size_i64;
using ycpp::varint_size_u64;

namespace {

void round_trip_u64(uint64_t v) {
    std::array<uint8_t, ycpp::kMaxVarintBytes> buf{};
    size_t written = 0;
    ASSERT_EQ(varint_encode_u64(v, buf.data(), buf.size(), &written), Status::kOk);
    EXPECT_EQ(written, varint_size_u64(v));

    uint64_t back = 0;
    size_t   consumed = 0;
    ASSERT_EQ(varint_decode_u64(buf.data(), written, &back, &consumed), Status::kOk);
    EXPECT_EQ(consumed, written);
    EXPECT_EQ(back, v);
}

void round_trip_i64(int64_t v) {
    std::array<uint8_t, ycpp::kMaxVarintBytes> buf{};
    size_t written = 0;
    ASSERT_EQ(varint_encode_i64(v, buf.data(), buf.size(), &written), Status::kOk);
    EXPECT_EQ(written, varint_size_i64(v));

    int64_t back = 0;
    size_t  consumed = 0;
    ASSERT_EQ(varint_decode_i64(buf.data(), written, &back, &consumed), Status::kOk);
    EXPECT_EQ(consumed, written);
    EXPECT_EQ(back, v);
}

} // namespace

TEST(YcppVarint, UnsignedRoundTripBoundaryValues) {
    constexpr uint64_t cases[] = {
        0U, 1U, 0x7FU, 0x80U, 0x3FFFU, 0x4000U,
        0x1FFFFFU, 0x200000U,
        0xFFFFFFFFFFFFFFFFU - 1,
        0xFFFFFFFFFFFFFFFFU,
    };
    for (auto v : cases) round_trip_u64(v);
}

TEST(YcppVarint, SignedRoundTripBoundaryValues) {
    constexpr int64_t cases[] = {
        0, 1, -1, 63, -64, 64, -65,
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::max(),
    };
    for (auto v : cases) round_trip_i64(v);
}

TEST(YcppVarint, OverflowDuringDecodeReturnsCorruptInput) {
    // 10 continuation bytes with high data bits — overflows u64.
    uint8_t bad[ycpp::kMaxVarintBytes];
    for (size_t i = 0; i < ycpp::kMaxVarintBytes - 1; ++i) bad[i] = 0xFFU;
    bad[ycpp::kMaxVarintBytes - 1] = 0x7FU;  // upper 6 bits non-zero
    uint64_t out = 0;
    size_t   consumed = 0;
    EXPECT_EQ(varint_decode_u64(bad, sizeof(bad), &out, &consumed),
              Status::kCorruptInput);
}

TEST(YcppVarint, TruncatedStreamReturnsOutOfBounds) {
    // 0xFF without a following byte: continuation set, stream ends.
    uint8_t partial[] = { 0xFFU };
    uint64_t out = 0;
    size_t   consumed = 0;
    EXPECT_EQ(varint_decode_u64(partial, sizeof(partial), &out, &consumed),
              Status::kOutOfBounds);
}

TEST(YcppVarint, EncoderRespectsCap) {
    uint8_t small[1];
    size_t  written = 0;
    EXPECT_EQ(varint_encode_u64(0x80U, small, sizeof(small), &written),
              Status::kOutOfBounds);
}

TEST(YcppVarint, SizeMatchesActualEncodedLength) {
    for (uint64_t v = 0; v < (1U << 21); v += 1023) {
        std::array<uint8_t, ycpp::kMaxVarintBytes> buf{};
        size_t written = 0;
        ASSERT_EQ(varint_encode_u64(v, buf.data(), buf.size(), &written), Status::kOk);
        ASSERT_EQ(written, varint_size_u64(v));
    }
}
