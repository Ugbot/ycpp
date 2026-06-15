// W1 reader/writer gate. Confirms round-trip + out-of-bounds + length-prefix.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "ycpp/ycpp_reader.h"
#include "ycpp/ycpp_writer.h"

using ycpp::ByteView;
using ycpp::Reader;
using ycpp::Status;
using ycpp::Writer;

TEST(YcppWriterReader, FixedWidthRoundTrip) {
    std::array<uint8_t, 32> buf{};
    Writer w{buf.data(), buf.size()};
    EXPECT_EQ(w.u8 (0xABU),                Status::kOk);
    EXPECT_EQ(w.u16_le(0xBEEFU),           Status::kOk);
    EXPECT_EQ(w.u32_le(0xDEADBEEFU),       Status::kOk);
    EXPECT_EQ(w.u64_le(0x0102030405060708ULL), Status::kOk);
    EXPECT_EQ(w.pos(), static_cast<size_t>(1 + 2 + 4 + 8));

    Reader r{ByteView{buf.data(), w.pos()}};
    uint8_t  v8  = 0;
    uint16_t v16 = 0;
    uint32_t v32 = 0;
    uint64_t v64 = 0;
    ASSERT_EQ(r.u8 (&v8 ),    Status::kOk);
    ASSERT_EQ(r.u16_le(&v16), Status::kOk);
    ASSERT_EQ(r.u32_le(&v32), Status::kOk);
    ASSERT_EQ(r.u64_le(&v64), Status::kOk);
    EXPECT_EQ(v8,  0xABU);
    EXPECT_EQ(v16, 0xBEEFU);
    EXPECT_EQ(v32, 0xDEADBEEFU);
    EXPECT_EQ(v64, 0x0102030405060708ULL);
    EXPECT_TRUE(r.eof());
}

TEST(YcppWriterReader, VarintRoundTrip) {
    std::array<uint8_t, 32> buf{};
    Writer w{buf.data(), buf.size()};
    EXPECT_EQ(w.varint_u64(0U),       Status::kOk);
    EXPECT_EQ(w.varint_u64(0x4000U),  Status::kOk);
    EXPECT_EQ(w.varint_i64(-42),      Status::kOk);

    Reader r{ByteView{buf.data(), w.pos()}};
    uint64_t a = 1;
    uint64_t b = 1;
    int64_t  c = 0;
    ASSERT_EQ(r.varint_u64(&a), Status::kOk);
    ASSERT_EQ(r.varint_u64(&b), Status::kOk);
    ASSERT_EQ(r.varint_i64(&c), Status::kOk);
    EXPECT_EQ(a, 0U);
    EXPECT_EQ(b, 0x4000U);
    EXPECT_EQ(c, -42);
    EXPECT_TRUE(r.eof());
}

TEST(YcppWriterReader, LengthPrefixedRoundTrip) {
    std::array<uint8_t, 64> buf{};
    Writer w{buf.data(), buf.size()};
    const char* msg = "hello, ycpp";
    EXPECT_EQ(w.length_prefixed(reinterpret_cast<const uint8_t*>(msg),
                                std::strlen(msg)),
              Status::kOk);

    Reader r{ByteView{buf.data(), w.pos()}};
    ByteView out{};
    ASSERT_EQ(r.length_prefixed(&out), Status::kOk);
    ASSERT_EQ(out.size, std::strlen(msg));
    EXPECT_EQ(std::memcmp(out.data, msg, out.size), 0);
    EXPECT_TRUE(r.eof());
}

TEST(YcppWriterReader, WriterOverflowReturnsOutOfBounds) {
    uint8_t small[2];
    Writer  w{small, sizeof(small)};
    EXPECT_EQ(w.u32_le(0x12345678U), Status::kOutOfBounds);
    EXPECT_EQ(w.pos(), 0U);  // failed write must not advance
}

TEST(YcppWriterReader, ReaderUnderflowReturnsOutOfBounds) {
    uint8_t one[1] = { 0x12U };
    Reader  r{ByteView{one, 1}};
    uint16_t out = 0;
    EXPECT_EQ(r.u16_le(&out), Status::kOutOfBounds);
}

TEST(YcppWriterReader, LengthPrefixRejectsTruncatedPayload) {
    // Encode length=5 but provide only 2 bytes of payload.
    std::array<uint8_t, 3> bad{ 0x05U, 'a', 'b' };
    Reader r{ByteView{bad.data(), bad.size()}};
    ByteView out{};
    EXPECT_EQ(r.length_prefixed(&out), Status::kOutOfBounds);
}
