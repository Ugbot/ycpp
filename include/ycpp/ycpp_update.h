// ycpp_update.h — Yjs updateV1 codec (decoder lives here; encoder is in
// ycpp_doc.h since it walks a Doc).
//
// The decoder body is inline in this header so any `Allocator` policy
// instantiates the parser at the call site — including the production
// `bolt::ybolt::BoltArenaAllocator` binding, which never crosses ycpp's
// static library boundary. The header stays compact because the parser
// itself is small (~200 LOC including helpers).

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_arena.h"   // for Allocator concept
#include "ycpp_byteview.h"
#include "ycpp_delete_set.h"
#include "ycpp_id.h"
#include "ycpp_item.h"
#include "ycpp_reader.h"
#include "ycpp_status.h"
#include "ycpp_unicode.h"

namespace ycpp {

// One decoded struct from the wire. POD; ByteView fields alias the source
// update buffer — integration must clone before persistence.
struct DecodedStruct {
    Id            id;
    Id            origin_left;
    Id            origin_right;
    ParentRef     parent_ref;
    Id            parent_id;
    ByteView      parent_name;
    ByteView      parent_sub;
    ContentKind   content_kind;
    uint64_t      length;
    ByteView      content_view;
};

// A decoded update = (struct vector, delete-set). Storage from the
// supplied allocator.
template <Allocator A>
class DecodedUpdate {
public:
    explicit DecodedUpdate(A* alloc) noexcept
        : alloc_(alloc), structs_(nullptr), size_(0), cap_(0),
          delete_set_(alloc) {
        assert(alloc != nullptr);
    }

    DecodedUpdate(const DecodedUpdate&)            = delete;
    DecodedUpdate& operator=(const DecodedUpdate&) = delete;
    DecodedUpdate(DecodedUpdate&&)                 = delete;
    DecodedUpdate& operator=(DecodedUpdate&&)      = delete;

    ~DecodedUpdate() noexcept = default;

    [[nodiscard]] Status push(const DecodedStruct& s) noexcept {
        YCPP_TRY(ensure_cap(size_ + 1));
        structs_[size_++] = s;
        return Status::kOk;
    }

    [[nodiscard]] std::size_t          size () const noexcept { return size_; }
    [[nodiscard]] const DecodedStruct* data () const noexcept { return structs_; }
    [[nodiscard]] const DecodedStruct& at   (std::size_t i) const noexcept {
        assert(i < size_);
        return structs_[i];
    }

    [[nodiscard]] DeleteSet<A>&       delete_set()       noexcept { return delete_set_; }
    [[nodiscard]] const DeleteSet<A>& delete_set() const noexcept { return delete_set_; }

private:
    static constexpr std::size_t kInitialCap = 32;
    static constexpr std::size_t kMaxStructs = 1U << 26;

    [[nodiscard]] Status ensure_cap(std::size_t need) noexcept {
        if (need <= cap_) return Status::kOk;
        if (need > kMaxStructs) return Status::kCapacityExceeded;
        std::size_t new_cap = cap_ == 0 ? kInitialCap : cap_ * 2;
        while (new_cap < need) new_cap *= 2;
        if (new_cap > kMaxStructs) new_cap = kMaxStructs;
        auto* fresh = static_cast<DecodedStruct*>(alloc_->alloc(
            sizeof(DecodedStruct) * new_cap, alignof(DecodedStruct)));
        if (fresh == nullptr) return Status::kOutOfMemory;
        if (size_ != 0) {
            for (std::size_t i = 0; i < size_; ++i) fresh[i] = structs_[i];
        }
        structs_ = fresh;
        cap_     = new_cap;
        return Status::kOk;
    }

    A*             alloc_;
    DecodedStruct* structs_;
    std::size_t    size_;
    std::size_t    cap_;
    DeleteSet<A>   delete_set_;
};

// ----- decoder ---------------------------------------------------------------

namespace detail_decode {

inline constexpr std::size_t kMaxClientsPerUpdate = 4096;
inline constexpr std::size_t kMaxStructsPerClient = 1U << 24;
inline constexpr int         kMaxAnyDepth         = 64;

// Skip one lib0/any value, advancing the reader. lib0/any is Yjs's
// JSON-like binary container for Y.Map values, Y.Array entries, and
// Y.Embed payloads. Returns kOk on success, kCorruptInput / kOutOfBounds
// on malformed input, kIntegrationFailed on excessive nesting depth.
//
// Type tags (from yjs/lib0/any):
//    119 string         varString
//    125 int32          varInt (zigzag)
//    124 float32        4 bytes
//    123 float64        8 bytes
//    122 bigint64       8 bytes
//    121 false          (no payload)
//    120 true           (no payload)
//    127 undefined      (no payload)
//    126 null           (no payload)
//    118 object         varUint count + count × (varString key, any value)
//    117 array          varUint count + count × any value
//    116 Uint8Array     varString-shaped (varUint byteLen + bytes)
[[nodiscard]] inline Status skip_any(Reader& r, int depth) noexcept {
    if (depth >= kMaxAnyDepth) return Status::kIntegrationFailed;
    uint8_t type = 0;
    YCPP_TRY(r.u8(&type));
    switch (type) {
        case 127: case 126: case 121: case 120:
            return Status::kOk;
        case 125: { int64_t  v = 0; return r.varint_i64(&v); }
        case 124: return r.skip(4);
        case 123: return r.skip(8);
        case 122: return r.skip(8);
        case 119: case 116: { ByteView v{}; return r.length_prefixed(&v); }
        case 118: {
            uint64_t n = 0;
            YCPP_TRY(r.varint_u64(&n));
            if (n > 65536) return Status::kCapacityExceeded;
            for (uint64_t i = 0; i < n; ++i) {
                ByteView key{};
                YCPP_TRY(r.length_prefixed(&key));
                YCPP_TRY(skip_any(r, depth + 1));
            }
            return Status::kOk;
        }
        case 117: {
            uint64_t n = 0;
            YCPP_TRY(r.varint_u64(&n));
            if (n > 65536) return Status::kCapacityExceeded;
            for (uint64_t i = 0; i < n; ++i) YCPP_TRY(skip_any(r, depth + 1));
            return Status::kOk;
        }
        default:
            return Status::kUnsupportedFormat;
    }
}

// Skip a ContentAny payload (varUint count + count × lib0/any). Returns
// the consumed byte slice via `*out` and the element count via `*out_count`.
[[nodiscard]] inline Status read_any_array(Reader& r, ByteView* out,
                                            uint64_t* out_count) noexcept {
    assert(out       != nullptr);
    assert(out_count != nullptr);
    const std::size_t start = r.pos();
    uint64_t n = 0;
    YCPP_TRY(r.varint_u64(&n));
    if (n > 65536) return Status::kCapacityExceeded;
    for (uint64_t i = 0; i < n; ++i) YCPP_TRY(skip_any(r, 0));
    const std::size_t consumed = r.pos() - start;
    *out       = ByteView{r.tail().data - consumed, consumed};
    *out_count = n;
    return Status::kOk;
}

// Skip a ContentJSON payload (varUint count + count × varString). Returns
// the byte slice + element count.
[[nodiscard]] inline Status read_json_array(Reader& r, ByteView* out,
                                             uint64_t* out_count) noexcept {
    assert(out       != nullptr);
    assert(out_count != nullptr);
    const std::size_t start = r.pos();
    uint64_t n = 0;
    YCPP_TRY(r.varint_u64(&n));
    if (n > 65536) return Status::kCapacityExceeded;
    for (uint64_t i = 0; i < n; ++i) {
        ByteView s{};
        YCPP_TRY(r.length_prefixed(&s));
    }
    const std::size_t consumed = r.pos() - start;
    *out       = ByteView{r.tail().data - consumed, consumed};
    *out_count = n;
    return Status::kOk;
}

// ContentFormat = varString key + varString value-as-JSON. Captures the
// full payload as opaque bytes.
[[nodiscard]] inline Status read_format(Reader& r, ByteView* out) noexcept {
    assert(out != nullptr);
    const std::size_t start = r.pos();
    ByteView key{}, val{};
    YCPP_TRY(r.length_prefixed(&key));
    YCPP_TRY(r.length_prefixed(&val));
    const std::size_t consumed = r.pos() - start;
    *out = ByteView{r.tail().data - consumed, consumed};
    return Status::kOk;
}

// ContentType = varUint type-ref. We capture the bytes verbatim so the
// encoder can echo them.
[[nodiscard]] inline Status read_type_ref(Reader& r, ByteView* out) noexcept {
    assert(out != nullptr);
    const std::size_t start = r.pos();
    uint64_t ref = 0;
    YCPP_TRY(r.varint_u64(&ref));
    const std::size_t consumed = r.pos() - start;
    *out = ByteView{r.tail().data - consumed, consumed};
    return Status::kOk;
}

// ContentDoc = varString guid + lib0/any opts.
[[nodiscard]] inline Status read_doc(Reader& r, ByteView* out) noexcept {
    assert(out != nullptr);
    const std::size_t start = r.pos();
    ByteView guid{};
    YCPP_TRY(r.length_prefixed(&guid));
    YCPP_TRY(skip_any(r, 0));
    const std::size_t consumed = r.pos() - start;
    *out = ByteView{r.tail().data - consumed, consumed};
    return Status::kOk;
}

// ContentMove = startID + endID + uint8 priority. Both IDs are
// (varUint client, varUint clock).
[[nodiscard]] inline Status read_move(Reader& r, ByteView* out) noexcept {
    assert(out != nullptr);
    const std::size_t start = r.pos();
    uint64_t a = 0, b = 0, c = 0, d = 0;
    YCPP_TRY(r.varint_u64(&a));  // startID.client
    YCPP_TRY(r.varint_u64(&b));  // startID.clock
    YCPP_TRY(r.varint_u64(&c));  // endID.client
    YCPP_TRY(r.varint_u64(&d));  // endID.clock
    uint8_t prio = 0;
    YCPP_TRY(r.u8(&prio));
    const std::size_t consumed = r.pos() - start;
    *out = ByteView{r.tail().data - consumed, consumed};
    return Status::kOk;
}

[[nodiscard]] inline Status read_id(Reader& r, Id* out) noexcept {
    assert(out != nullptr);
    uint64_t client = 0, clock = 0;
    YCPP_TRY(r.varint_u64(&client));
    YCPP_TRY(r.varint_u64(&clock));
    *out = Id{client, clock};
    return Status::kOk;
}

[[nodiscard]] inline Status read_content_payload(Reader& r, ContentKind kind,
                                                  uint64_t* length,
                                                  ByteView* payload) noexcept {
    assert(length  != nullptr);
    assert(payload != nullptr);
    *length  = 1;
    *payload = ByteView{};
    switch (kind) {
        case ContentKind::kGc:
        case ContentKind::kSkip:
        case ContentKind::kDeleted:
            YCPP_TRY(r.varint_u64(length));
            if (*length == 0) return Status::kCorruptInput;
            return Status::kOk;

        case ContentKind::kString:
            // varString: varUint byteLen + UTF-8 bytes. The CRDT length
            // is the UTF-16 code unit count of the decoded string (Yjs
            // semantics) — derive from the bytes.
            YCPP_TRY(r.length_prefixed(payload));
            *length = utf16_length_of_utf8(*payload);
            if (*length == 0) *length = 1;  // empty-string Item still occupies 1 slot
            return Status::kOk;

        case ContentKind::kBinary:
        case ContentKind::kEmbed:
            // Single varString-shaped value. CRDT length = 1.
            YCPP_TRY(r.length_prefixed(payload));
            *length = 1;
            return Status::kOk;

        case ContentKind::kJson:
            // varUint count + count × varString. CRDT length = count.
            YCPP_TRY(read_json_array(r, payload, length));
            if (*length == 0) *length = 1;
            return Status::kOk;

        case ContentKind::kAny:
            // varUint count + count × lib0/any value. CRDT length = count.
            YCPP_TRY(read_any_array(r, payload, length));
            if (*length == 0) *length = 1;
            return Status::kOk;

        case ContentKind::kFormat:
            YCPP_TRY(read_format(r, payload));
            *length = 1;
            return Status::kOk;

        case ContentKind::kType:
            YCPP_TRY(read_type_ref(r, payload));
            *length = 1;
            return Status::kOk;

        case ContentKind::kDoc:
            YCPP_TRY(read_doc(r, payload));
            *length = 1;
            return Status::kOk;

        case ContentKind::kMove:
            YCPP_TRY(read_move(r, payload));
            *length = 1;
            return Status::kOk;
    }
    return Status::kUnsupportedFormat;
}

[[nodiscard]] inline Status decode_one_struct(Reader& r, uint64_t client,
                                              uint64_t clock,
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
        // Yjs convention: tag is a varUint. 1 = root type identified by
        // name string; 0 = parent is the Item referenced by the trailing
        // (client, clock) Id.
        uint64_t parent_tag = 0;
        YCPP_TRY(r.varint_u64(&parent_tag));
        if (parent_tag == 1) {
            out->parent_ref = ParentRef::kRootName;
            YCPP_TRY(r.length_prefixed(&out->parent_name));
        } else if (parent_tag == 0) {
            out->parent_ref = ParentRef::kId;
            YCPP_TRY(read_id(r, &out->parent_id));
        } else {
            return Status::kCorruptInput;
        }
    }
    if (info_has_parent_sub(info)) {
        YCPP_TRY(r.length_prefixed(&out->parent_sub));
    }

    return read_content_payload(r, out->content_kind, &out->length, &out->content_view);
}

} // namespace detail_decode

// Parse a Yjs updateV1 binary blob `bytes` into `out`. ByteView fields in
// `out` alias `bytes`; the caller must keep `bytes` alive while reading
// from `out`. Integration into a Doc clones into Doc-owned storage.
template <Allocator A>
[[nodiscard]] inline Status decode_update_v1(ByteView bytes,
                                              DecodedUpdate<A>* out) noexcept {
    assert(out != nullptr);
    Reader r{bytes};

    uint64_t n_clients = 0;
    YCPP_TRY(r.varint_u64(&n_clients));
    if (n_clients > detail_decode::kMaxClientsPerUpdate) return Status::kCapacityExceeded;

    for (uint64_t ci = 0; ci < n_clients; ++ci) {
        uint64_t n_structs = 0;
        uint64_t client    = 0;
        uint64_t clock     = 0;
        YCPP_TRY(r.varint_u64(&n_structs));
        if (n_structs > detail_decode::kMaxStructsPerClient) return Status::kCapacityExceeded;
        YCPP_TRY(r.varint_u64(&client));
        YCPP_TRY(r.varint_u64(&clock));
        for (uint64_t si = 0; si < n_structs; ++si) {
            DecodedStruct s{};
            YCPP_TRY(detail_decode::decode_one_struct(r, client, clock, &s));
            YCPP_TRY(out->push(s));
            clock += s.length == 0 ? 1 : s.length;
        }
    }

    YCPP_TRY(out->delete_set().decode(r));
    if (!r.eof()) return Status::kCorruptInput;
    return Status::kOk;
}

} // namespace ycpp
