// W4 envelope gate. Round-trip + truncation + unknown-kind rejection.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "ycpp/ycpp_byteview.h"
#include "ycpp/ycpp_envelope.h"
#include "ycpp/ycpp_status.h"
#include "ycpp/ycpp_writer.h"

using ycpp::ByteView;
using ycpp::Envelope;
using ycpp::MessageKind;
using ycpp::Status;
using ycpp::Writer;

TEST(YcppEnvelope, EncodeDecodeRoundTripWithPayload) {
    std::array<uint8_t, 128> buf{};
    Writer w{buf.data(), buf.size()};
    const char* payload = "ping";
    ASSERT_EQ(encode_envelope(MessageKind::kCustomRequest, 42,
                              ByteView{reinterpret_cast<const uint8_t*>(payload),
                                       std::strlen(payload)},
                              w),
              Status::kOk);

    Envelope env{};
    ASSERT_EQ(decode_envelope(ByteView{buf.data(), w.pos()}, &env), Status::kOk);
    EXPECT_EQ(env.kind, MessageKind::kCustomRequest);
    EXPECT_EQ(env.request_id, 42U);
    ASSERT_EQ(env.payload.size, std::strlen(payload));
    EXPECT_EQ(std::memcmp(env.payload.data, payload, env.payload.size), 0);
}

TEST(YcppEnvelope, EmptyPayloadOk) {
    std::array<uint8_t, 16> buf{};
    Writer w{buf.data(), buf.size()};
    ASSERT_EQ(encode_envelope(MessageKind::kCustomEvent, 0, ByteView{}, w),
              Status::kOk);
    Envelope env{};
    ASSERT_EQ(decode_envelope(ByteView{buf.data(), w.pos()}, &env), Status::kOk);
    EXPECT_EQ(env.kind, MessageKind::kCustomEvent);
    EXPECT_EQ(env.request_id, 0U);
    EXPECT_EQ(env.payload.size, 0U);
}

TEST(YcppEnvelope, UnknownKindRejected) {
    std::array<uint8_t, 8> buf{ 0xFFU, 0x01U, 0x00U };  // kind=0xFF, req=1, len=0
    Envelope env{};
    EXPECT_EQ(decode_envelope(ByteView{buf.data(), 3}, &env),
              Status::kUnsupportedFormat);
}

TEST(YcppEnvelope, TruncatedRejected) {
    std::array<uint8_t, 1> buf{ static_cast<uint8_t>(MessageKind::kSyncStep1) };
    Envelope env{};
    EXPECT_EQ(decode_envelope(ByteView{buf.data(), 1}, &env),
              Status::kOutOfBounds);
}

TEST(YcppEnvelope, TrailingGarbageRejected) {
    std::array<uint8_t, 16> buf{};
    Writer w{buf.data(), buf.size()};
    ASSERT_EQ(encode_envelope(MessageKind::kCustomEvent, 0, ByteView{}, w),
              Status::kOk);
    // Bump pos by one byte of trailing junk.
    buf[w.pos()] = 0xABU;
    Envelope env{};
    EXPECT_EQ(decode_envelope(ByteView{buf.data(), w.pos() + 1}, &env),
              Status::kCorruptInput);
}
