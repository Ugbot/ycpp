// ycpp_update_encode_v1.cpp — symmetric counterpart to decode_update_v1.
//
// Walks the Doc's StructStore + DeleteSet and emits a Yjs updateV1-shaped
// blob containing only the structs the peer is missing (clock ≥ since[client]
// for each client we know about). The DeleteSet is always emitted in full
// because deletion is monotone: a peer that already has a delete is a noop
// when re-applied.
//
// The encoder is byte-symmetric with the decoder: a Doc that calls
// encode_diff_v1(empty_sv) and feeds the output into a fresh Doc's
// apply_update_v1 must converge them to the same observable state.

#include "ycpp/ycpp_doc.h"

#include <cassert>
#include <cstdint>

#include "ycpp/ycpp_arena.h"
#include "ycpp/ycpp_byteview.h"
#include "ycpp/ycpp_delete_set.h"
#include "ycpp/ycpp_id.h"
#include "ycpp/ycpp_item.h"
#include "ycpp/ycpp_state_vector.h"
#include "ycpp/ycpp_status.h"
#include "ycpp/ycpp_struct_store.h"
#include "ycpp/ycpp_writer.h"

namespace ycpp {

namespace {

[[nodiscard]] Status write_id(Writer& w, Id id) noexcept {
    YCPP_TRY(w.varint_u64(id.client));
    YCPP_TRY(w.varint_u64(id.clock));
    return Status::kOk;
}

[[nodiscard]] uint8_t build_info_byte(const Item* it) noexcept {
    uint8_t info = static_cast<uint8_t>(it->content_kind) & kInfoMaskContentKind;
    if (is_valid(it->origin_left )) info |= kInfoFlagHasLeftOrigin;
    if (is_valid(it->origin_right)) info |= kInfoFlagHasRightOrigin;
    if (it->parent_sub.size > 0)    info |= kInfoFlagHasParentSub;
    return info;
}

[[nodiscard]] Status write_content_payload(Writer& w, const Item* it) noexcept {
    switch (it->content_kind) {
        case ContentKind::kGc:
        case ContentKind::kSkip:
        case ContentKind::kDeleted:
            return w.varint_u64(it->length == 0 ? 1 : it->length);

        case ContentKind::kString:
        case ContentKind::kJson:
        case ContentKind::kBinary:
        case ContentKind::kEmbed:
        case ContentKind::kAny:
        case ContentKind::kFormat:
        case ContentKind::kType:
        case ContentKind::kDoc:
        case ContentKind::kMove:
            return w.length_prefixed(it->content_view.data, it->content_view.size);
    }
    return Status::kUnsupportedFormat;
}

[[nodiscard]] Status write_struct(Writer& w, const Item* it) noexcept {
    assert(it != nullptr);
    const uint8_t info = build_info_byte(it);
    YCPP_TRY(w.u8(info));

    if (info_has_left_origin (info)) YCPP_TRY(write_id(w, it->origin_left));
    if (info_has_right_origin(info)) YCPP_TRY(write_id(w, it->origin_right));
    if (info_has_parent_info (info)) {
        if (it->parent_ref == ParentRef::kId) {
            YCPP_TRY(w.u8(1));
            YCPP_TRY(write_id(w, it->parent_id));
        } else if (it->parent_ref == ParentRef::kRootName) {
            YCPP_TRY(w.u8(0));
            YCPP_TRY(w.length_prefixed(it->parent_name.data, it->parent_name.size));
        } else {
            return Status::kInternal;  // info bits implied parent but Item has none
        }
    }
    if (info_has_parent_sub(info)) {
        YCPP_TRY(w.length_prefixed(it->parent_sub.data, it->parent_sub.size));
    }
    return write_content_payload(w, it);
}

// First pass: count clients with at least one struct above the since-SV
// and the per-client struct count. We need both for the v1 prefix
// (numClients, then per-client { numStructs, client, startClock }) before
// we start emitting structs.
template <Allocator A>
[[nodiscard]] Status count_clients_to_emit(const Doc<A>& doc,
                                            const StateVector<A>* since,
                                            uint64_t* out_n_clients) noexcept {
    uint64_t n = 0;
    doc.store().for_each_client(
        [&](uint64_t client, const Item* const* items, std::size_t count) noexcept {
            const uint64_t since_clock = (since != nullptr) ? since->get(client) : 0U;
            for (std::size_t i = 0; i < count; ++i) {
                if (items[i]->id.clock >= since_clock) { ++n; return; }
            }
        });
    *out_n_clients = n;
    return Status::kOk;
}

} // namespace

template <Allocator A>
Status encode_diff_v1(const Doc<A>& doc, const StateVector<A>* since,
                       Writer* out) noexcept {
    assert(out != nullptr);

    uint64_t n_clients_to_emit = 0;
    YCPP_TRY(count_clients_to_emit(doc, since, &n_clients_to_emit));
    YCPP_TRY(out->varint_u64(n_clients_to_emit));

    Status acc = Status::kOk;
    doc.store().for_each_client(
        [&](uint64_t client, const Item* const* items, std::size_t count) noexcept {
            if (acc != Status::kOk) return;
            const uint64_t since_clock = (since != nullptr) ? since->get(client) : 0U;

            // Locate the first struct whose clock ≥ since_clock — binary search
            // would be tighter but linear walk is bounded by `count` and we're
            // not on the per-item hot path.
            std::size_t start = 0;
            while (start < count && items[start]->id.clock < since_clock) ++start;
            const std::size_t emit_count = count - start;
            if (emit_count == 0) return;

            const Status s_num_structs = out->varint_u64(static_cast<uint64_t>(emit_count));
            if (s_num_structs != Status::kOk) { acc = s_num_structs; return; }
            const Status s_client = out->varint_u64(client);
            if (s_client != Status::kOk) { acc = s_client; return; }
            const Status s_clock  = out->varint_u64(items[start]->id.clock);
            if (s_clock  != Status::kOk) { acc = s_clock; return; }

            for (std::size_t i = start; i < count; ++i) {
                const Status s = write_struct(*out, items[i]);
                if (s != Status::kOk) { acc = s; return; }
            }
        });
    if (acc != Status::kOk) return acc;

    // DeleteSet — always full.
    return doc.delete_set().encode(*out);
}

template Status encode_diff_v1<DefaultArenaAllocator>(
    const Doc<DefaultArenaAllocator>&,
    const StateVector<DefaultArenaAllocator>*,
    Writer*) noexcept;

} // namespace ycpp
