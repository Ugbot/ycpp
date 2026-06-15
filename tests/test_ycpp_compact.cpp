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
    ASSERT_EQ(doc.array_delete_at("t", 1),    Status::kOk);  // drop "beta"

    EXPECT_EQ(text_of(doc, "t"), std::string{"alphagamma"});

    const std::size_t compacted = doc.compact();
    EXPECT_GE(compacted, 1U);

    // Text view unchanged.
    EXPECT_EQ(text_of(doc, "t"), std::string{"alphagamma"});

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
