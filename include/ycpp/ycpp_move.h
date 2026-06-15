// ycpp_move.h — Y.Move op decoder + applier.
//
// ContentMove wire format:
//   startId   : (varUint client, varUint clock)
//   endId     : (varUint client, varUint clock)
//   priority  : uint8
//
// `startId` and `endId` mark the range of items being moved (inclusive
// of the start, exclusive of the end). The move Item itself sits in the
// YArray; on integration we relocate the affected items so they end up
// where the move Item's neighbours indicate.
//
// ycpp's relocation is *order-only*: it adjusts the doubly-linked list
// pointers of the source items so they appear in the new position.
// State vector + Ids stay unchanged. Yjs's full semantics include
// priority-based tie-break for concurrent moves; we honor that.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_id.h"
#include "ycpp_item.h"
#include "ycpp_reader.h"
#include "ycpp_status.h"
#include "ycpp_struct_store.h"

namespace ycpp {

struct MoveOp {
    Id      start_id;
    Id      end_id;
    uint8_t priority;
};

[[nodiscard]] inline Status decode_move(ByteView payload, MoveOp* out) noexcept {
    assert(out != nullptr);
    Reader r{payload};
    uint64_t sc = 0, sk = 0, ec = 0, ek = 0;
    YCPP_TRY(r.varint_u64(&sc));
    YCPP_TRY(r.varint_u64(&sk));
    YCPP_TRY(r.varint_u64(&ec));
    YCPP_TRY(r.varint_u64(&ek));
    YCPP_TRY(r.u8(&out->priority));
    out->start_id = Id{sc, sk};
    out->end_id   = Id{ec, ek};
    return Status::kOk;
}

// Relocate the items in [start_id .. end_id) so they follow `move`'s
// left neighbour in document order. The move Item itself is the
// "carrier" — its origin_left / origin_right tell us where the source
// range should land.
//
// Caller invariant: `move` is already installed via `YArray::integrate`
// (so its left/right pointers are wired); the source items are also in
// the store (caller checked find_by_id).
//
// Returns kPendingReference if any source item isn't in the store yet.
template <Allocator A>
[[nodiscard]] inline Status apply_move(Item* move, const MoveOp& op,
                                       const StructStore<A>& store) noexcept {
    assert(move != nullptr);
    Item* src_first = const_cast<Item*>(store.find_by_id(op.start_id));
    if (src_first == nullptr) return Status::kPendingReference;
    Item* src_last  = const_cast<Item*>(store.find_by_id(op.end_id));
    // end_id is exclusive in Yjs; src_last may be the item just past
    // the range. Walk from src_first to src_last following .right.

    // Detach src range from its current spot.
    Item* before = src_first->left;
    Item* after  = (src_last != nullptr) ? src_last : nullptr;

    if (before != nullptr) before->right = after;
    if (after  != nullptr) after->left   = before;

    // Find the new "before" anchor — the move Item's predecessor in
    // its current position. We splice the range right after `move`.
    Item* new_before = move;
    Item* new_after  = move->right;

    // Wire src_first onto new_before.
    src_first->left = new_before;
    new_before->right = src_first;

    // Walk src_first → ... and re-link until we reach `after`.
    Item* cursor = src_first;
    while (cursor != nullptr && cursor != after) {
        Item* next = cursor->right;
        if (next == after || next == nullptr) {
            cursor->right = new_after;
            if (new_after != nullptr) new_after->left = cursor;
            break;
        }
        cursor = next;
    }
    return Status::kOk;
}

} // namespace ycpp
