// W2 updateV1 decoder gate.
//
// Real Yjs reference fixtures land in W9 (interop). These tests use a small
// hand-built encoder + the decoder under test to round-trip synthetic
// updates whose layout matches the Yjs wire format. This validates the
// framing (info byte, origins, parent info, content kind) end-to-end
// without depending on a Node fixture-generator at W2.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_byteview.h"
#include "ycpp/ycpp_id.h"
#include "ycpp/ycpp_item.h"
#include "ycpp/ycpp_status.h"
#include "ycpp/ycpp_update.h"
#include "ycpp/ycpp_writer.h"

using ycpp::ByteView;
using ycpp::ContentKind;
using ycpp::DecodedUpdate;
using ycpp::DefaultArenaAllocator;
using ycpp::decode_update_v1;
using ycpp::Id;
using ycpp::ParentRef;
using ycpp::Status;
using ycpp::Writer;
using ycpp::kInfoFlagHasLeftOrigin;
using ycpp::kInfoFlagHasParentSub;
using ycpp::kInfoFlagHasRightOrigin;

namespace {

// Tiny hand-encoder for one fixture per test. Writes:
//   numClients = 1, numStructs, client, startClock, then each struct.
// Returns the bytes consumed.
struct V1Builder {
    std::vector<uint8_t> buf;

    void emit_varint(uint64_t v) {
        uint8_t b[10];
        size_t n = 0;
        const auto s = ycpp::varint_encode_u64(v, b, sizeof(b), &n);
        ASSERT_EQ(s, Status::kOk);
        buf.insert(buf.end(), b, b + n);
    }

    void emit_u8(uint8_t v) { buf.push_back(v); }

    void emit_length_prefixed(ByteView v) {
        emit_varint(v.size);
        if (v.size != 0) buf.insert(buf.end(), v.data, v.data + v.size);
    }
};

} // namespace

TEST(YcppDecodeV1, SingleStringInsertWithRootParent) {
    V1Builder b;
    // Struct section: 1 client.
    b.emit_varint(1);
    // 1 struct, client=7, startClock=0.
    b.emit_varint(1);
    b.emit_varint(7);
    b.emit_varint(0);
    // Info byte: kString=4, no origins (parent info follows).
    b.emit_u8(static_cast<uint8_t>(ContentKind::kString));
    // Parent info present (no origin bits set, kind ≠ GC/Skip):
    //   tag 0 → root-name parent.
    b.emit_u8(1);  // tag=1 → root by name (Yjs convention)
    const char* root = "rootmap";
    b.emit_length_prefixed(ByteView{reinterpret_cast<const uint8_t*>(root),
                                    std::strlen(root)});
    // Content: length-prefixed UTF-8 "hello".
    const char* content = "hello";
    b.emit_length_prefixed(ByteView{reinterpret_cast<const uint8_t*>(content),
                                    std::strlen(content)});
    // Delete set: 0 clients.
    b.emit_varint(0);

    DefaultArenaAllocator a;
    DecodedUpdate<DefaultArenaAllocator> out{&a};
    const auto s = decode_update_v1(ByteView{b.buf.data(), b.buf.size()}, &out);
    ASSERT_EQ(s, Status::kOk);
    ASSERT_EQ(out.size(), 1U);
    const auto& d = out.at(0);
    EXPECT_EQ(d.id,           (Id{7, 0}));
    EXPECT_EQ(d.content_kind, ContentKind::kString);
    EXPECT_EQ(d.parent_ref,   ParentRef::kRootName);
    ASSERT_EQ(d.parent_name.size, std::strlen(root));
    EXPECT_EQ(std::memcmp(d.parent_name.data, root, d.parent_name.size), 0);
    ASSERT_EQ(d.content_view.size, std::strlen(content));
    EXPECT_EQ(std::memcmp(d.content_view.data, content, d.content_view.size), 0);
}

TEST(YcppDecodeV1, TwoStructsWithOriginsAndParentSub) {
    V1Builder b;
    b.emit_varint(1);      // 1 client
    b.emit_varint(2);      // 2 structs
    b.emit_varint(11);     // client
    b.emit_varint(0);      // startClock

    // Struct 0: kString, root parent, parent_sub="k".
    {
        b.emit_u8(static_cast<uint8_t>(ContentKind::kString)
                  | kInfoFlagHasParentSub);
        b.emit_u8(1);  // tag=1 → root by name (Yjs convention)
        const char* root = "doc";
        b.emit_length_prefixed(ByteView{reinterpret_cast<const uint8_t*>(root),
                                        std::strlen(root)});
        const char* sub = "k";
        b.emit_length_prefixed(ByteView{reinterpret_cast<const uint8_t*>(sub),
                                        std::strlen(sub)});
        const char* content = "v0";
        b.emit_length_prefixed(ByteView{reinterpret_cast<const uint8_t*>(content),
                                        std::strlen(content)});
    }
    // Struct 1: kString, left-origin = (11, 0), no parent info, parent_sub="k".
    {
        b.emit_u8(static_cast<uint8_t>(ContentKind::kString)
                  | kInfoFlagHasLeftOrigin
                  | kInfoFlagHasParentSub);
        b.emit_varint(11); b.emit_varint(0);  // origin_left = (11, 0)
        const char* sub = "k";
        b.emit_length_prefixed(ByteView{reinterpret_cast<const uint8_t*>(sub),
                                        std::strlen(sub)});
        const char* content = "v1";
        b.emit_length_prefixed(ByteView{reinterpret_cast<const uint8_t*>(content),
                                        std::strlen(content)});
    }
    b.emit_varint(0);  // empty delete set

    DefaultArenaAllocator a;
    DecodedUpdate<DefaultArenaAllocator> out{&a};
    ASSERT_EQ(decode_update_v1(ByteView{b.buf.data(), b.buf.size()}, &out),
              Status::kOk);
    ASSERT_EQ(out.size(), 2U);
    // Each kString item has length = UTF-16 code units of its content.
    // "v0" is 2 chars → first item is at clock 0..1, second at clock 2.
    EXPECT_EQ(out.at(0).id, (Id{11, 0}));
    EXPECT_EQ(out.at(1).id, (Id{11, 2}));
    EXPECT_EQ(out.at(1).origin_left, (Id{11, 0}));
    EXPECT_EQ(out.at(1).parent_ref,   ParentRef::kNone);
    ASSERT_EQ(out.at(1).parent_sub.size, 1U);
    EXPECT_EQ(out.at(1).parent_sub.data[0], 'k');
}

TEST(YcppDecodeV1, GcStructEncodesLength) {
    V1Builder b;
    b.emit_varint(1);       // 1 client
    b.emit_varint(1);       // 1 struct
    b.emit_varint(2);       // client
    b.emit_varint(0);       // startClock
    b.emit_u8(static_cast<uint8_t>(ContentKind::kGc));
    b.emit_varint(5);       // GC run length
    b.emit_varint(0);       // empty delete set

    DefaultArenaAllocator a;
    DecodedUpdate<DefaultArenaAllocator> out{&a};
    ASSERT_EQ(decode_update_v1(ByteView{b.buf.data(), b.buf.size()}, &out),
              Status::kOk);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out.at(0).content_kind, ContentKind::kGc);
    EXPECT_EQ(out.at(0).length,       5U);
}

TEST(YcppDecodeV1, DecodeSurfacesDeleteSet) {
    V1Builder b;
    b.emit_varint(0);       // 0 client struct runs
    // Delete set: 1 client, client=42, 1 range (clock=10, length=4).
    b.emit_varint(1);
    b.emit_varint(42);
    b.emit_varint(1);
    b.emit_varint(10);
    b.emit_varint(4);

    DefaultArenaAllocator a;
    DecodedUpdate<DefaultArenaAllocator> out{&a};
    ASSERT_EQ(decode_update_v1(ByteView{b.buf.data(), b.buf.size()}, &out),
              Status::kOk);
    EXPECT_EQ(out.size(), 0U);
    EXPECT_TRUE (out.delete_set().contains(Id{42, 10}));
    EXPECT_TRUE (out.delete_set().contains(Id{42, 13}));
    EXPECT_FALSE(out.delete_set().contains(Id{42, 14}));
}

TEST(YcppDecodeV1, TrailingGarbageIsRejected) {
    V1Builder b;
    b.emit_varint(0);
    b.emit_varint(0);
    b.buf.push_back(0xFFU);  // one extra byte after a valid empty update

    DefaultArenaAllocator a;
    DecodedUpdate<DefaultArenaAllocator> out{&a};
    EXPECT_EQ(decode_update_v1(ByteView{b.buf.data(), b.buf.size()}, &out),
              Status::kCorruptInput);
}

TEST(YcppDecodeV1, UnknownContentKindReturnsUnsupportedFormat) {
    V1Builder b;
    b.emit_varint(1);
    b.emit_varint(1);
    b.emit_varint(0);
    b.emit_varint(0);
    b.emit_u8(0x1FU);  // content kind 31 — not in the table

    DefaultArenaAllocator a;
    DecodedUpdate<DefaultArenaAllocator> out{&a};
    EXPECT_EQ(decode_update_v1(ByteView{b.buf.data(), b.buf.size()}, &out),
              Status::kUnsupportedFormat);
}
