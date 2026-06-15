// W4 Y.Array + Y.Text gate. Local edits round-trip; bidirectional sync
// converges; concurrent appends to the same anchor resolve by Lamport.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
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

std::string text_of(const ycpp::YText<DefaultArenaAllocator>& t) {
    std::string out;
    t.for_each_chunk([&](ByteView b) noexcept {
        out.append(reinterpret_cast<const char*>(b.data), b.size);
    });
    return out;
}

Status sync(const DocA& src, DocA& dst) noexcept {
    DefaultArenaAllocator a;
    SvA dst_sv{&a};
    if (Status s = dst.state_vector(&dst_sv); s != Status::kOk) return s;
    std::array<uint8_t, 8192> buf{};
    Writer w{buf.data(), buf.size()};
    if (Status s = encode_diff_v1<DefaultArenaAllocator>(src, &dst_sv, &w);
        s != Status::kOk) return s;
    return dst.apply_update_v1(ByteView{buf.data(), w.pos()});
}

} // namespace

TEST(YcppYArray, LocalAppendsThenIterate) {
    DocA doc{1};
    ASSERT_EQ(doc.text_append("words", "hello "), Status::kOk);
    ASSERT_EQ(doc.text_append("words", "world!"), Status::kOk);

    auto text = doc.get_or_create_text("words");
    EXPECT_EQ(text_of(text), std::string{"hello world!"});
    EXPECT_EQ(text.length(), 2U);   // two chunks
    EXPECT_EQ(text.byte_length(), std::strlen("hello world!"));
}

TEST(YcppYArray, InsertAtIndex) {
    DocA doc{1};
    ASSERT_EQ(doc.array_insert_at("arr", 0, "b"), Status::kOk);
    ASSERT_EQ(doc.array_insert_at("arr", 0, "a"), Status::kOk);  // prepend
    ASSERT_EQ(doc.array_insert_at("arr", 2, "c"), Status::kOk);  // append at end

    auto* arr = doc.get_or_create_array("arr");
    ASSERT_EQ(arr->length(), 3U);
    EXPECT_EQ(arr->at(0)->content_view.size, 1U);
    EXPECT_EQ(arr->at(0)->content_view.data[0], 'a');
    EXPECT_EQ(arr->at(1)->content_view.data[0], 'b');
    EXPECT_EQ(arr->at(2)->content_view.data[0], 'c');
}

TEST(YcppYArray, DeleteAtRemovesFromLiveView) {
    DocA doc{1};
    ASSERT_EQ(doc.text_append("t", "alpha"), Status::kOk);
    ASSERT_EQ(doc.text_append("t", "beta"),  Status::kOk);
    ASSERT_EQ(doc.text_append("t", "gamma"), Status::kOk);

    ASSERT_EQ(doc.array_delete_at("t", 1), Status::kOk);  // drops "beta"

    auto text = doc.get_or_create_text("t");
    EXPECT_EQ(text_of(text), std::string{"alphagamma"});
    EXPECT_EQ(text.length(), 2U);
}

TEST(YcppYArray, BidirectionalCollabTextConverges) {
    DocA alice{1};
    DocA bob  {2};

    // Alice authors a short greeting.
    ASSERT_EQ(alice.text_append("doc", "hello, "), Status::kOk);
    ASSERT_EQ(alice.text_append("doc", "world"),   Status::kOk);

    // Bob simultaneously authors a different intro.
    ASSERT_EQ(bob.text_append("doc", "greetings"), Status::kOk);

    // Exchange.
    ASSERT_EQ(sync(alice, bob),   Status::kOk);
    ASSERT_EQ(sync(bob,   alice), Status::kOk);

    // Both peers see all three chunks. Order depends on Lamport on
    // shared origins (here all-anchored-on-empty), so we don't assert
    // strict ordering — just that all content is present and identical
    // across peers.
    const std::string a_text = text_of(alice.get_or_create_text("doc"));
    const std::string b_text = text_of(bob  .get_or_create_text("doc"));
    EXPECT_EQ(a_text, b_text);
    EXPECT_NE(a_text.find("hello, "),  std::string::npos);
    EXPECT_NE(a_text.find("world"),    std::string::npos);
    EXPECT_NE(a_text.find("greetings"), std::string::npos);
}

TEST(YcppYArray, ConcurrentAppendsResolveByLamport) {
    DocA low {1};   // lower client id
    DocA high{99};  // higher client id

    // Both peers append a token onto empty array (origin_left = invalid).
    ASSERT_EQ(low .text_append("seq", "L"), Status::kOk);   // (1, 0)
    ASSERT_EQ(high.text_append("seq", "H"), Status::kOk);   // (99, 0)

    ASSERT_EQ(sync(low,  high), Status::kOk);
    ASSERT_EQ(sync(high, low),  Status::kOk);

    // YATA-style integration: when two items share the same origin_left
    // (kInvalid), the one with the smaller (client, clock) ends up first.
    const std::string a = text_of(low .get_or_create_text("seq"));
    const std::string b = text_of(high.get_or_create_text("seq"));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, std::string{"LH"});   // low-client first per Lamport
}

TEST(YcppYArray, DeleteSyncsBetweenPeers) {
    DocA alice{1};
    DocA bob  {2};
    ASSERT_EQ(alice.text_append("t", "AAA"), Status::kOk);
    ASSERT_EQ(alice.text_append("t", "BBB"), Status::kOk);
    ASSERT_EQ(sync(alice, bob), Status::kOk);
    EXPECT_EQ(text_of(bob.get_or_create_text("t")), std::string{"AAABBB"});

    ASSERT_EQ(alice.array_delete_at("t", 0), Status::kOk);
    ASSERT_EQ(sync(alice, bob), Status::kOk);
    EXPECT_EQ(text_of(bob.get_or_create_text("t")), std::string{"BBB"});
    EXPECT_EQ(text_of(alice.get_or_create_text("t")), std::string{"BBB"});
}
