// W3 Doc gate. Local edits land in Y.Map; encode_diff_v1 round-trips; a
// second Doc applying the diff converges to the same observable state.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_byteview.h"
#include "ycpp/ycpp_doc.h"
#include "ycpp/ycpp_state_vector.h"
#include "ycpp/ycpp_status.h"
#include "ycpp/ycpp_writer.h"

using ycpp::ByteView;
using ycpp::DefaultArenaAllocator;
using ycpp::Status;
using ycpp::Writer;
using DocA = ycpp::Doc<DefaultArenaAllocator>;
using SvA  = ycpp::StateVector<DefaultArenaAllocator>;

namespace {

ByteView sv(const char* s) noexcept {
    return ByteView{reinterpret_cast<const uint8_t*>(s), std::strlen(s)};
}

std::string_view as_sv(ByteView b) noexcept {
    return {reinterpret_cast<const char*>(b.data), b.size};
}

} // namespace

TEST(YcppDoc, LocalSetThenGetReturnsValue) {
    DocA doc{1};
    ASSERT_EQ(doc.map_set_string("root", "title", "Hello"), Status::kOk);

    auto* m = doc.get_or_create_map("root");
    ASSERT_NE(m, nullptr);
    auto* it = m->get(sv("title"));
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(as_sv(it->content_view), std::string_view{"Hello"});
    // next_clock is the UTF-16 length of the inserted value.
    EXPECT_EQ(doc.next_clock(), 5U);
}

TEST(YcppDoc, OverwriteSameKeyMarksPriorDeleted) {
    DocA doc{1};
    ASSERT_EQ(doc.map_set_string("r", "k", "first"),  Status::kOk);
    ASSERT_EQ(doc.map_set_string("r", "k", "second"), Status::kOk);

    auto* m = doc.get_or_create_map("r");
    auto* it = m->get(sv("k"));
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(as_sv(it->content_view), std::string_view{"second"});
    // Prior (client=1, clock=0) is in the delete set so peers converge.
    EXPECT_TRUE(doc.delete_set().contains(ycpp::Id{1, 0}));
}

TEST(YcppDoc, EncodeDiffRoundTripsThroughApply) {
    DocA alice{1};
    ASSERT_EQ(alice.map_set_string("r", "k1", "v1"), Status::kOk);
    ASSERT_EQ(alice.map_set_string("r", "k2", "v2"), Status::kOk);
    ASSERT_EQ(alice.map_set_string("r", "k3", "v3"), Status::kOk);

    std::array<uint8_t, 512> buf{};
    Writer w{buf.data(), buf.size()};
    ASSERT_EQ(encode_diff_v1<DefaultArenaAllocator>(alice, nullptr, &w), Status::kOk);

    DocA bob{2};
    ASSERT_EQ(bob.apply_update_v1(ByteView{buf.data(), w.pos()}), Status::kOk);

    auto* m = bob.get_or_create_map("r");
    ASSERT_NE(m, nullptr);
    ASSERT_NE(m->get(sv("k1")), nullptr);
    ASSERT_NE(m->get(sv("k2")), nullptr);
    ASSERT_NE(m->get(sv("k3")), nullptr);
    EXPECT_EQ(as_sv(m->get(sv("k1"))->content_view), std::string_view{"v1"});
    EXPECT_EQ(as_sv(m->get(sv("k2"))->content_view), std::string_view{"v2"});
    EXPECT_EQ(as_sv(m->get(sv("k3"))->content_view), std::string_view{"v3"});
}

TEST(YcppDoc, ApplyIsIdempotent) {
    DocA alice{1};
    ASSERT_EQ(alice.map_set_string("r", "k", "v"), Status::kOk);

    std::array<uint8_t, 256> buf{};
    Writer w{buf.data(), buf.size()};
    ASSERT_EQ(encode_diff_v1<DefaultArenaAllocator>(alice, nullptr, &w), Status::kOk);
    const ByteView update{buf.data(), w.pos()};

    DocA bob{2};
    ASSERT_EQ(bob.apply_update_v1(update), Status::kOk);
    ASSERT_EQ(bob.apply_update_v1(update), Status::kOk);  // replay = noop

    auto* m = bob.get_or_create_map("r");
    EXPECT_EQ(as_sv(m->get(sv("k"))->content_view), std::string_view{"v"});
    EXPECT_EQ(bob.store().total_items(), 1U);
}

TEST(YcppDoc, EncodeDiffSinceStateVectorOmitsKnownStructs) {
    // Alice makes 2 edits; full sync to Bob.
    DocA alice{1};
    DocA bob  {2};
    ASSERT_EQ(alice.map_set_string("r", "a", "1"), Status::kOk);  // (1, 0)
    ASSERT_EQ(alice.map_set_string("r", "b", "2"), Status::kOk);  // (1, 1)

    {
        DefaultArenaAllocator a;
        SvA empty{&a};
        std::array<uint8_t, 256> buf{};
        Writer w{buf.data(), buf.size()};
        ASSERT_EQ(encode_diff_v1<DefaultArenaAllocator>(alice, &empty, &w), Status::kOk);
        ASSERT_EQ(bob.apply_update_v1(ByteView{buf.data(), w.pos()}), Status::kOk);
    }

    // Alice makes a 3rd edit; encode-diff against bob's actual SV must
    // carry ONLY the new struct (clock=2).
    ASSERT_EQ(alice.map_set_string("r", "c", "3"), Status::kOk);  // (1, 2)

    DefaultArenaAllocator a;
    SvA bob_sv{&a};
    ASSERT_EQ(bob.state_vector(&bob_sv), Status::kOk);
    EXPECT_EQ(bob_sv.get(1), 2U);  // bob has seen client=1 up to clock 2

    std::array<uint8_t, 256> buf{};
    Writer w{buf.data(), buf.size()};
    ASSERT_EQ(encode_diff_v1<DefaultArenaAllocator>(alice, &bob_sv, &w), Status::kOk);
    ASSERT_EQ(bob.apply_update_v1(ByteView{buf.data(), w.pos()}), Status::kOk);

    auto* m = bob.get_or_create_map("r");
    ASSERT_NE(m->get(sv("c")), nullptr);
    EXPECT_EQ(as_sv(m->get(sv("c"))->content_view), std::string_view{"3"});
    // Alice's earlier items already present from the initial sync.
    EXPECT_NE(m->get(sv("a")), nullptr);
    EXPECT_NE(m->get(sv("b")), nullptr);
}

TEST(YcppDoc, StateVectorReflectsAppliedStructs) {
    DocA doc{42};
    ASSERT_EQ(doc.map_set_string("r", "k1", "v1"), Status::kOk);
    ASSERT_EQ(doc.map_set_string("r", "k2", "v2"), Status::kOk);
    ASSERT_EQ(doc.map_set_string("r", "k3", "v3"), Status::kOk);

    DefaultArenaAllocator a;
    SvA out_sv{&a};
    ASSERT_EQ(doc.state_vector(&out_sv), Status::kOk);
    // Each map_set_string advances the clock by UTF-16 length of value.
    // "v1" "v2" "v3" are each 2 chars → 6 total.
    EXPECT_EQ(out_sv.get(42), 6U);
}
