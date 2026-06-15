// ycpp_update_decode_v1.cpp — Yjs updateV1 wire-format decoder.
//
// Wire layout (Yjs updateV1, see y-crdt/yjs-tools sources):
//
//   update := struct_section + delete_set
//
//   struct_section:
//     numClients : varint_u64
//     for each client in [0..numClients):
//       numStructs : varint_u64
//       client     : varint_u64
//       startClock : varint_u64
//       for each struct in [0..numStructs):
//         info  : u8                         ; ContentKind | origin/parent flags
//         (if info_has_left_origin)
//           leftOrigin : (varint_u64 client, varint_u64 clock)
//         (if info_has_right_origin)
//           rightOrigin: (varint_u64 client, varint_u64 clock)
//         (if info_has_parent_info)
//           parentKindTag : u8 (1 = parentId, 0 = parentName)
//           if parentKindTag == 0: parentName = length-prefixed UTF-8
//           if parentKindTag == 1: parentId   = (varint_u64 client, varint_u64 clock)
//         (if info_has_parent_sub)
//           parentSub : length-prefixed UTF-8 (Y.Map key)
//         content per ContentKind:
//           kGc / kSkip  → length : varint_u64           (no payload)
//           kDeleted     → length : varint_u64           (placeholder run)
//           kString /
//           kJson        → length-prefixed UTF-8         (content = the bytes)
//           kBinary      → length-prefixed bytes         (content = the bytes)
//           kEmbed /
//           kAny /
//           kFormat /
//           kType /
//           kDoc /
//           kMove        → opaque payload, byte-counted   (W2: ByteView slice)
//
//   delete_set: (see DeleteSet::decode in ycpp_delete_set.h)
//
// W2 scope: parse the framing + the per-Item header (Id, origins, parent,
// content kind, length). For opaque content kinds (kEmbed, kAny, kFormat,
// kType, kDoc, kMove) we record a ByteView slice the size of the payload
// and skip past it without interpreting. Full type integration is W4.
//
// We bound the content-byte slice on opaque kinds via a payload-prefix
// length read; the Yjs source emits these prefixes as part of its encoder
// so the decoder can pick them up unambiguously.

#include "ycpp/ycpp_update.h"

#include <cassert>
#include <cstdint>

#include "ycpp/ycpp_byteview.h"
#include "ycpp/ycpp_delete_set.h"
#include "ycpp/ycpp_id.h"
#include "ycpp/ycpp_item.h"
#include "ycpp/ycpp_reader.h"
#include "ycpp/ycpp_status.h"

namespace ycpp {

namespace {

inline constexpr std::size_t kMaxClientsPerUpdate = 4096;
inline constexpr std::size_t kMaxStructsPerClient = 1U << 24;

[[nodiscard]] Status read_id(Reader& r, Id* out) noexcept {
    assert(out != nullptr);
    uint64_t client = 0;
    uint64_t clock  = 0;
    YCPP_TRY(r.varint_u64(&client));
    YCPP_TRY(r.varint_u64(&clock));
    *out = Id{client, clock};
    return Status::kOk;
}

// Read the content payload, returning the underlying bytes consumed via
// `*payload`. The exact framing depends on the kind.
[[nodiscard]] Status read_content_payload(Reader& r, ContentKind kind,
                                          uint64_t* length, ByteView* payload) noexcept {
    assert(length  != nullptr);
    assert(payload != nullptr);
    *length  = 1;
    *payload = ByteView{};

    switch (kind) {
        case ContentKind::kGc:
        case ContentKind::kSkip:
            // No payload, length is the run width. The wire stores it as a varint.
            YCPP_TRY(r.varint_u64(length));
            if (*length == 0) return Status::kCorruptInput;
            return Status::kOk;

        case ContentKind::kDeleted:
            YCPP_TRY(r.varint_u64(length));
            if (*length == 0) return Status::kCorruptInput;
            return Status::kOk;

        case ContentKind::kString:
        case ContentKind::kJson:
        case ContentKind::kBinary:
            // Length-prefixed payload; the bytes are the content.
            YCPP_TRY(r.length_prefixed(payload));
            *length = 1;  // CRDT length for these kinds is the item count (1 per Item)
            return Status::kOk;

        case ContentKind::kEmbed:
        case ContentKind::kAny:
        case ContentKind::kFormat:
        case ContentKind::kType:
        case ContentKind::kDoc:
        case ContentKind::kMove:
            // Opaque to W2; we capture a length-prefixed slice. W4 / W6
            // re-encode through type-specific paths.
            YCPP_TRY(r.length_prefixed(payload));
            *length = 1;
            return Status::kOk;
    }
    return Status::kUnsupportedFormat;
}

[[nodiscard]] Status decode_one_struct(Reader& r, uint64_t client, uint64_t clock,
                                       DecodedStruct* out) noexcept {
    assert(out != nullptr);
    uint8_t info = 0;
    YCPP_TRY(r.u8(&info));
    if (!content_kind_is_known(info & kInfoMaskContentKind)) {
        return Status::kUnsupportedFormat;
    }

    out->id            = Id{client, clock};
    out->origin_left   = kInvalidId;
    out->origin_right  = kInvalidId;
    out->parent_ref    = ParentRef::kNone;
    out->parent_id     = kInvalidId;
    out->parent_name   = ByteView{};
    out->parent_sub    = ByteView{};
    out->content_kind  = info_content_kind(info);
    out->length        = 1;
    out->content_view  = ByteView{};

    if (info_has_left_origin (info)) YCPP_TRY(read_id(r, &out->origin_left));
    if (info_has_right_origin(info)) YCPP_TRY(read_id(r, &out->origin_right));
    if (info_has_parent_info (info)) {
        uint8_t parent_kind_tag = 0;
        YCPP_TRY(r.u8(&parent_kind_tag));
        if (parent_kind_tag == 1) {
            out->parent_ref = ParentRef::kId;
            YCPP_TRY(read_id(r, &out->parent_id));
        } else if (parent_kind_tag == 0) {
            out->parent_ref = ParentRef::kRootName;
            YCPP_TRY(r.length_prefixed(&out->parent_name));
        } else {
            return Status::kCorruptInput;
        }
    }
    if (info_has_parent_sub(info)) {
        YCPP_TRY(r.length_prefixed(&out->parent_sub));
    }

    return read_content_payload(r, out->content_kind, &out->length, &out->content_view);
}

template <Allocator A>
[[nodiscard]] Status decode_struct_section(Reader& r, DecodedUpdate<A>* out) noexcept {
    uint64_t n_clients = 0;
    YCPP_TRY(r.varint_u64(&n_clients));
    if (n_clients > kMaxClientsPerUpdate) return Status::kCapacityExceeded;

    for (uint64_t ci = 0; ci < n_clients; ++ci) {
        uint64_t n_structs = 0;
        uint64_t client    = 0;
        uint64_t clock     = 0;
        YCPP_TRY(r.varint_u64(&n_structs));
        if (n_structs > kMaxStructsPerClient) return Status::kCapacityExceeded;
        YCPP_TRY(r.varint_u64(&client));
        YCPP_TRY(r.varint_u64(&clock));

        for (uint64_t si = 0; si < n_structs; ++si) {
            DecodedStruct s{};
            YCPP_TRY(decode_one_struct(r, client, clock, &s));
            YCPP_TRY(out->push(s));
            // Advance the running clock by this struct's length so the next
            // struct in the run carries the correct Id.
            clock += s.length == 0 ? 1 : s.length;
        }
    }
    return Status::kOk;
}

} // namespace

template <Allocator A>
Status decode_update_v1(ByteView bytes, DecodedUpdate<A>* out) noexcept {
    assert(out != nullptr);
    Reader r{bytes};
    YCPP_TRY(decode_struct_section(r, out));
    YCPP_TRY(out->delete_set().decode(r));
    // Per Yjs convention, any extra trailing bytes are protocol garbage.
    if (!r.eof()) return Status::kCorruptInput;
    return Status::kOk;
}

// Explicit instantiation for the default allocator.
template Status decode_update_v1<DefaultArenaAllocator>(
    ByteView, DecodedUpdate<DefaultArenaAllocator>*) noexcept;

} // namespace ycpp
