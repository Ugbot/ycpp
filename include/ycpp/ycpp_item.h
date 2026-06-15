// ycpp_item.h — Item / GC struct + Yjs info-byte decoding helpers.
//
// A `Struct` in Yjs is one of:
//   - Item  — a CRDT operation: insert (with optional left/right origin and
//             parent reference) carrying typed content.
//   - GC    — a "garbage-collected" length-N region representing dropped
//             items whose Ids still anchor state-vector accounting.
//   - Skip  — same shape as GC but emitted by encoders when reframing
//             output; treated identically by decoders.
//
// The Yjs wire format encodes the kind in the LOW 5 BITS of the info byte
// (0 = GC, 10 = Skip, 1..9 and 11 = Item content kinds). The HIGH BITS
// encode origin / parent-info flags. See `info_byte` helpers below.

#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>

#include "ycpp_byteview.h"
#include "ycpp_id.h"

namespace ycpp {

// ----- Content kind (low 5 bits of info byte) ------------------------------

enum class ContentKind : uint8_t {
    kGc       = 0,   // length-N run of dropped items
    kDeleted  = 1,   // Item placeholder, no payload (length carries width)
    kJson     = 2,   // length-prefixed UTF-8 JSON text
    kBinary   = 3,   // length-prefixed bytes
    kString   = 4,   // length-prefixed UTF-8 text
    kEmbed    = 5,   // single "lib0/any" value (opaque ByteView at W2)
    kFormat   = 6,   // (key, value) format mark for Y.Text rich runs
    kType     = 7,   // sub-type ref (Y.Map / Y.Array / Y.Text / Y.XmlFragment / Y.XmlElement / Y.XmlHook / Y.XmlText)
    kAny      = 8,   // sequence of "lib0/any" values (opaque ByteView at W2)
    kDoc      = 9,   // sub-doc (guid + options)
    kSkip     = 10,  // alias of GC for encoder reframing
    kMove     = 11,  // Y.Move op (referenced struct + run length)
};

[[nodiscard]] constexpr bool content_kind_is_known(uint8_t raw) noexcept {
    return raw <= static_cast<uint8_t>(ContentKind::kMove);
}

[[nodiscard]] constexpr const char* content_kind_name(ContentKind k) noexcept {
    switch (k) {
        case ContentKind::kGc:      return "gc";
        case ContentKind::kDeleted: return "deleted";
        case ContentKind::kJson:    return "json";
        case ContentKind::kBinary:  return "binary";
        case ContentKind::kString:  return "string";
        case ContentKind::kEmbed:   return "embed";
        case ContentKind::kFormat:  return "format";
        case ContentKind::kType:    return "type";
        case ContentKind::kAny:     return "any";
        case ContentKind::kDoc:     return "doc";
        case ContentKind::kSkip:    return "skip";
        case ContentKind::kMove:    return "move";
    }
    return "unknown";
}

// ----- Info byte layout -----------------------------------------------------
//
// Yjs (see y-crdt updateV1 encoder):
//   bits [0..4]: ContentKind
//   bit  5:     has parent-sub key (the Y.Map key)
//   bit  6:     has right origin
//   bit  7:     has left origin
//
// Parent reference is signalled by ContentKind kType; otherwise the parent
// is implicit (the doc root by name, sent in a separate string after the
// origin fields). The "parent-info present" flag is encoded by neither
// origin bit being set when the content kind is not GC/Skip — Yjs's
// `cantCopyParentInfo` rule.

inline constexpr uint8_t kInfoMaskContentKind = 0x1FU;  // bits 0..4
inline constexpr uint8_t kInfoFlagHasParentSub = 1U << 5;
inline constexpr uint8_t kInfoFlagHasRightOrigin = 1U << 6;
inline constexpr uint8_t kInfoFlagHasLeftOrigin  = 1U << 7;

[[nodiscard]] constexpr ContentKind info_content_kind(uint8_t info) noexcept {
    return static_cast<ContentKind>(info & kInfoMaskContentKind);
}
[[nodiscard]] constexpr bool info_has_left_origin (uint8_t info) noexcept {
    return (info & kInfoFlagHasLeftOrigin)  != 0;
}
[[nodiscard]] constexpr bool info_has_right_origin(uint8_t info) noexcept {
    return (info & kInfoFlagHasRightOrigin) != 0;
}
[[nodiscard]] constexpr bool info_has_parent_sub  (uint8_t info) noexcept {
    return (info & kInfoFlagHasParentSub)   != 0;
}
// Yjs convention: when neither origin bit is set and the content kind is
// neither GC nor Skip, the encoder writes an explicit parent reference
// (either a root-name string or a parent Id, signalled by a 1-byte tag).
[[nodiscard]] constexpr bool info_has_parent_info(uint8_t info) noexcept {
    return !info_has_left_origin(info) && !info_has_right_origin(info)
        && info_content_kind(info) != ContentKind::kGc
        && info_content_kind(info) != ContentKind::kSkip;
}

// ----- Parent reference (sub-doc tag) --------------------------------------

enum class ParentRef : uint8_t {
    kNone     = 0,
    kRootName = 1,  // the parent is a root-type identified by a string name
    kId       = 2,  // the parent is the Item with the given Id
};

// ----- Item / GC POD -------------------------------------------------------

// `Item` is the in-memory representation of a single CRDT struct as
// produced by the W2 decoder. Pointer fields stay null at W2 (no
// integration yet); W3 fills them as part of splicing the new struct
// into its parent's doubly-linked list.
//
// `content_view` aliases the source update buffer for opaque kinds; the
// owning caller (decoder result) must keep the update alive while these
// items exist. Once W3 lands and Items are persisted into the StructStore,
// owned copies of the content will be made via the Doc's allocator.

struct Item {
    Id            id;
    Id            origin_left;
    Id            origin_right;

    ParentRef     parent_ref;
    Id            parent_id;       // valid when parent_ref == kId
    ByteView      parent_name;     // valid when parent_ref == kRootName

    ByteView      parent_sub;      // Y.Map key, optional (empty if absent)
    ContentKind   content_kind;
    uint64_t      length;          // run length (always 1 for non-GC at W2)
    ByteView      content_view;    // opaque slice into the source buffer

    uint16_t      flags;           // populated later (kDeleted / kKeep / …)

    // W3 pointer fields — kept zero by the decoder.
    Item*         left;
    Item*         right;

    constexpr Item() noexcept
        : id{}, origin_left{kInvalidId}, origin_right{kInvalidId},
          parent_ref{ParentRef::kNone}, parent_id{kInvalidId},
          parent_name{}, parent_sub{},
          content_kind{ContentKind::kDeleted}, length{0},
          content_view{}, flags{0}, left{nullptr}, right{nullptr} {}
};

// Flag bits (mirrors Yjs Item flags; only the first two land at W2).
inline constexpr uint16_t kFlagDeleted   = 1U << 0;
inline constexpr uint16_t kFlagKeep      = 1U << 1;
inline constexpr uint16_t kFlagCountable = 1U << 2;
inline constexpr uint16_t kFlagMarker    = 1U << 3;

static_assert(std::is_trivially_copyable_v<ContentKind>, "ContentKind must be trivially copyable");
// Item carries ByteViews (which are trivially copyable) and pointers; it
// is *not* trivially default-constructible due to the in-class initialisers,
// but it IS trivially copyable, which is what the pool requires.
static_assert(std::is_trivially_copyable_v<Item>, "Item must stay trivially copyable for the pool");

} // namespace ycpp
