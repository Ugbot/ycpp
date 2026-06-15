// W4 awareness gate. Per-client LWW; encode/decode round-trip; stale
// updates dropped; offline marker (empty state).

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_awareness.h"
#include "ycpp/ycpp_byteview.h"
#include "ycpp/ycpp_reader.h"
#include "ycpp/ycpp_status.h"
#include "ycpp/ycpp_writer.h"

using ycpp::Awareness;
using ycpp::ByteView;
using ycpp::DefaultArenaAllocator;
using ycpp::Reader;
using ycpp::Status;
using ycpp::Writer;

namespace {

ByteView sv(const char* s) noexcept {
    return ByteView{reinterpret_cast<const uint8_t*>(s), std::strlen(s)};
}

std::string_view as_sv(ByteView b) noexcept {
    return {reinterpret_cast<const char*>(b.data), b.size};
}

} // namespace

TEST(YcppAwareness, PublishAndGet) {
    DefaultArenaAllocator a;
    Awareness<DefaultArenaAllocator> aw{&a};
    ASSERT_EQ(aw.publish(1, sv("{cursor:5}")), Status::kOk);
    auto* e = aw.get(1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(as_sv(e->state), std::string_view{"{cursor:5}"});
    EXPECT_EQ(e->clock, 1U);
    EXPECT_EQ(aw.get(2), nullptr);
}

TEST(YcppAwareness, RepublishBumpsClock) {
    DefaultArenaAllocator a;
    Awareness<DefaultArenaAllocator> aw{&a};
    ASSERT_EQ(aw.publish(1, sv("v0")), Status::kOk);
    ASSERT_EQ(aw.publish(1, sv("v1")), Status::kOk);
    auto* e = aw.get(1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->clock, 2U);
    EXPECT_EQ(as_sv(e->state), std::string_view{"v1"});
}

TEST(YcppAwareness, EncodeAllRoundTripsViaApply) {
    DefaultArenaAllocator a;
    Awareness<DefaultArenaAllocator> alice{&a};
    ASSERT_EQ(alice.publish(1, sv("alice-state")), Status::kOk);
    ASSERT_EQ(alice.publish(7, sv("delegate-7")),  Status::kOk);

    std::array<uint8_t, 256> buf{};
    Writer w{buf.data(), buf.size()};
    ASSERT_EQ(alice.encode_all(w), Status::kOk);

    DefaultArenaAllocator a2;
    Awareness<DefaultArenaAllocator> bob{&a2};
    ASSERT_EQ(bob.apply(ByteView{buf.data(), w.pos()}), Status::kOk);
    auto* e1 = bob.get(1);
    auto* e7 = bob.get(7);
    ASSERT_NE(e1, nullptr);
    ASSERT_NE(e7, nullptr);
    EXPECT_EQ(as_sv(e1->state), std::string_view{"alice-state"});
    EXPECT_EQ(as_sv(e7->state), std::string_view{"delegate-7"});
}

TEST(YcppAwareness, StaleApplyIsDropped) {
    DefaultArenaAllocator a;
    Awareness<DefaultArenaAllocator> aw{&a};
    ASSERT_EQ(aw.publish(1, sv("v3")), Status::kOk);  // sets clock 1
    ASSERT_EQ(aw.publish(1, sv("v3")), Status::kOk);  // clock 2
    ASSERT_EQ(aw.publish(1, sv("v3")), Status::kOk);  // clock 3

    // Synthesize an awareness payload with clock 2 — stale.
    std::array<uint8_t, 64> buf{};
    Writer w{buf.data(), buf.size()};
    ASSERT_EQ(w.varint_u64(1),     Status::kOk);  // numClients
    ASSERT_EQ(w.varint_u64(1),     Status::kOk);  // client_id
    ASSERT_EQ(w.varint_u64(2),     Status::kOk);  // clock (< local clock 3)
    ASSERT_EQ(w.length_prefixed(reinterpret_cast<const uint8_t*>("stale"), 5),
              Status::kOk);
    ASSERT_EQ(aw.apply(ByteView{buf.data(), w.pos()}), Status::kOk);

    // Local clock + state unchanged.
    auto* e = aw.get(1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->clock, 3U);
    EXPECT_EQ(as_sv(e->state), std::string_view{"v3"});
}

TEST(YcppAwareness, EmptyStateMarksPeerOffline) {
    DefaultArenaAllocator a;
    Awareness<DefaultArenaAllocator> aw{&a};
    ASSERT_EQ(aw.publish(5, sv("online")), Status::kOk);
    ASSERT_EQ(aw.publish(5, ByteView{}),   Status::kOk);  // clock bumps, state empty
    auto* e = aw.get(5);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->state.size, 0U);
    EXPECT_EQ(e->clock, 2U);
}
