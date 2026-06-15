// W4.5 UndoManager gate. capture / undo / redo over a tracked origin.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string_view>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_doc.h"
#include "ycpp/ycpp_status.h"
#include "ycpp/ycpp_undo.h"

using ycpp::ByteView;
using ycpp::DefaultArenaAllocator;
using ycpp::Status;
using ycpp::UndoManager;
using DocA = ycpp::Doc<DefaultArenaAllocator>;

namespace {

ByteView sv(const char* s) noexcept {
    return ByteView{reinterpret_cast<const uint8_t*>(s), std::strlen(s)};
}

} // namespace

TEST(YcppUndo, TrackedCaptureUndoRedoCycle) {
    DocA doc{/*client_id=*/1};
    UndoManager<DefaultArenaAllocator> um{doc};
    ASSERT_EQ(um.track(/*origin=*/0xAA), Status::kOk);

    ASSERT_EQ(um.capture_begin(0xAA),                Status::kOk);
    ASSERT_EQ(doc.text_append("body", "Hello, "),    Status::kOk);
    ASSERT_EQ(doc.text_append("body", "world!"),     Status::kOk);
    ASSERT_EQ(um.capture_end(),                      Status::kOk);

    EXPECT_TRUE(um.can_undo());
    EXPECT_FALSE(um.can_redo());
    EXPECT_EQ(um.undo_depth(), 1U);

    // Pre-undo: text reads "Hello, world!"
    {
        std::string got;
        doc.get_or_create_text("body").for_each_chunk(
            [&](ByteView b) noexcept {
                got.append(reinterpret_cast<const char*>(b.data), b.size);
            });
        EXPECT_EQ(got, std::string{"Hello, world!"});
    }

    // Undo: text becomes empty.
    ASSERT_EQ(um.undo(), Status::kOk);
    EXPECT_TRUE(um.can_redo());
    {
        std::string got;
        doc.get_or_create_text("body").for_each_chunk(
            [&](ByteView b) noexcept {
                got.append(reinterpret_cast<const char*>(b.data), b.size);
            });
        EXPECT_TRUE(got.empty()) << "got=" << got;
    }

    // Redo: text comes back.
    ASSERT_EQ(um.redo(), Status::kOk);
    EXPECT_FALSE(um.can_redo());
    {
        std::string got;
        doc.get_or_create_text("body").for_each_chunk(
            [&](ByteView b) noexcept {
                got.append(reinterpret_cast<const char*>(b.data), b.size);
            });
        EXPECT_EQ(got, std::string{"Hello, world!"});
    }
}

TEST(YcppUndo, UntrackedOriginRejected) {
    DocA doc{1};
    UndoManager<DefaultArenaAllocator> um{doc};
    EXPECT_EQ(um.capture_begin(0xFF), Status::kInvalidArgument);
}

TEST(YcppUndo, CaptureWithNoEditsLeavesStackEmpty) {
    DocA doc{1};
    UndoManager<DefaultArenaAllocator> um{doc};
    ASSERT_EQ(um.track(7),            Status::kOk);
    ASSERT_EQ(um.capture_begin(7),    Status::kOk);
    ASSERT_EQ(um.capture_end(),       Status::kOk);
    EXPECT_FALSE(um.can_undo());
}

TEST(YcppUndo, MultipleFramesUnstack) {
    DocA doc{1};
    UndoManager<DefaultArenaAllocator> um{doc};
    ASSERT_EQ(um.track(1), Status::kOk);

    ASSERT_EQ(um.capture_begin(1),                 Status::kOk);
    ASSERT_EQ(doc.text_append("t", "alpha"),       Status::kOk);
    ASSERT_EQ(um.capture_end(),                    Status::kOk);

    ASSERT_EQ(um.capture_begin(1),                 Status::kOk);
    ASSERT_EQ(doc.text_append("t", "beta"),        Status::kOk);
    ASSERT_EQ(um.capture_end(),                    Status::kOk);

    EXPECT_EQ(um.undo_depth(), 2U);
    ASSERT_EQ(um.undo(), Status::kOk);  // beta gone
    ASSERT_EQ(um.undo(), Status::kOk);  // alpha gone

    std::string got;
    doc.get_or_create_text("t").for_each_chunk(
        [&](ByteView b) noexcept {
            got.append(reinterpret_cast<const char*>(b.data), b.size);
        });
    EXPECT_TRUE(got.empty()) << "got=" << got;
}
