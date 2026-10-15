// W4.5 compact() gate. Items marked deleted are converted to kSkip;
// content_view bytes freed (size == 0); clock arithmetic + live text
// view unchanged.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_doc.h"
#include "ycpp/ycpp_item.h"
#include "ycpp/ycpp_state_vector.h"
#include "ycpp/ycpp_status.h"

using ycpp::ByteView;
using ycpp::DefaultArenaAllocator;
using ycpp::ContentKind;
using ycpp::Status;
using DocA = ycpp::Doc<DefaultArenaAllocator>;
using SvA  = ycpp::StateVector<DefaultArenaAllocator>;

namespace {

std::string text_of(DocA& d, const char* root) {
    std::string out;
    d.get_or_create_text(root).for_each_chunk(
        [&](ByteView b) noexcept {
            out.append(reinterpret_cast<const char*>(b.data), b.size);
        });
    return out;
}

} // namespace

TEST(YcppCompact, ConvertsDeletedItemsToSkip) {
    DocA doc{1};
    ASSERT_EQ(doc.text_append("t", "alpha"),  Status::kOk);
    ASSERT_EQ(doc.text_append("t", "beta"),   Status::kOk);
    ASSERT_EQ(doc.text_append("t", "gamma"),  Status::kOk);
    ASSERT_EQ(doc.array_delete_at("t", 2),    Status::kOk);  // drop unreferenced tail "gamma"

    EXPECT_EQ(text_of(doc, "t"), std::string{"alphabeta"});

    const std::size_t compacted = doc.compact();
    EXPECT_GE(compacted, 1U);

    // Text view unchanged.
    EXPECT_EQ(text_of(doc, "t"), std::string{"alphabeta"});

    // Walk the per-client items; the deleted ones should be kSkip with
    // empty content_view.
    bool found_skip = false;
    doc.store().for_each_client(
        [&](uint64_t /*client*/, const ycpp::Item* const* items,
            std::size_t count) noexcept {
            for (std::size_t i = 0; i < count; ++i) {
                if (items[i]->content_kind == ContentKind::kSkip) {
                    found_skip = true;
                    EXPECT_EQ(items[i]->content_view.size, 0U);
                }
            }
        });
    EXPECT_TRUE(found_skip);
}

TEST(YcppCompact, StateVectorUnchanged) {
    DocA doc{42};
    ASSERT_EQ(doc.text_append("t", "AB"),  Status::kOk);
    ASSERT_EQ(doc.text_append("t", "CD"),  Status::kOk);
    ASSERT_EQ(doc.array_delete_at("t", 0), Status::kOk);

    DefaultArenaAllocator a;
    SvA sv_before{&a};
    ASSERT_EQ(doc.state_vector(&sv_before), Status::kOk);
    const uint64_t before_clock = sv_before.get(42);

    (void)doc.compact();

    DefaultArenaAllocator a2;
    SvA sv_after{&a2};
    ASSERT_EQ(doc.state_vector(&sv_after), Status::kOk);
    EXPECT_EQ(sv_after.get(42), before_clock);
}

TEST(YcppCompact, PreservesLastClockOriginsThroughDeletedMapOverwriteChain) {
    DocA doc{42};
    ASSERT_EQ(doc.map_set_string("slots", "worker", "0123456789abcdef"), Status::kOk);
    ASSERT_EQ(doc.map_set_string("slots", "worker", "second-value"), Status::kOk);
    ASSERT_EQ(doc.map_set_string("slots", "worker", "final-value"), Status::kOk);
    // Both superseded strings remain structural anchors, including the
    // deleted referring middle item. Dropping either loses root/key on replay.
    EXPECT_EQ(doc.compact(), 0U);
    const auto* first=doc.store().find_by_id({42,0}); ASSERT_NE(first,nullptr);
    EXPECT_EQ(first->content_kind,ContentKind::kString);
    std::uint8_t bytes[4096]{}; ycpp::Writer wire{bytes,sizeof(bytes)};
    ASSERT_EQ(ycpp::encode_diff_v1(doc,static_cast<const SvA*>(nullptr),&wire),Status::kOk);
    DocA clone{42}; ASSERT_EQ(clone.apply_update_v1({bytes,wire.pos()}),Status::kOk);
    EXPECT_EQ(clone.next_clock(),doc.next_clock());
    const auto* slots=clone.find_map("slots"); ASSERT_NE(slots,nullptr);
    const auto* item=slots->get(ycpp::ByteView{reinterpret_cast<const std::uint8_t*>("worker"),6}); ASSERT_NE(item,nullptr);
    EXPECT_EQ(item->content_kind,ContentKind::kString);
    ASSERT_EQ(item->content_view.size,std::strlen("final-value"));
    EXPECT_EQ(std::memcmp(item->content_view.data,"final-value",item->content_view.size),0);
    EXPECT_EQ(clone.compact(),0U);
}
