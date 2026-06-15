// ycpp_yarray.h — sequence CRDT (Y.Array) over a doubly-linked Item list.
//
// The list anchors at `start_` and chains via Item::left / Item::right. Each
// integrated Item carries `origin_left` / `origin_right` Ids that identify
// the Items whose neighbours it was when the local edit was authored.
// Integration walks the conflict zone between those origins and uses a
// Lamport tie-break to splice the new Item deterministically.
//
// Y.Text (see ycpp_ytext.h) is a thin facade over Y.Array that interprets
// each Item's `content_view` as UTF-8 bytes; collaborative text editing
// rides this same sequence CRDT.
//
// Scope (ycpp 0.0.2): converges under
//   - sequential appends from any number of clients
//   - concurrent appends to the same anchor (Lamport tie-break)
//   - concurrent inserts at different anchors
//   - sequential insertion-then-deletion in either order
//
// Out of scope for 0.0.2 (LIMITATIONS.md):
//   - intricate concurrent insert chains needing the full YATA
//     `itemsBeforeOrigin` walk; we currently stop at origin mismatch
//   - Y.Move ops
//   - format-mark integration in Y.Text (text content only)
//
// All algorithm steps are bounded, all comparisons branch-light, no
// allocations on the integrate hot path.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_id.h"
#include "ycpp_item.h"
#include "ycpp_status.h"
#include "ycpp_struct_store.h"

namespace ycpp {

template <Allocator A>
class YArray {
public:
    explicit YArray(A* /*alloc*/) noexcept
        : start_(nullptr) {}

    YArray(const YArray&)            = delete;
    YArray& operator=(const YArray&) = delete;
    YArray(YArray&&)                 = delete;
    YArray& operator=(YArray&&)      = delete;
    ~YArray() noexcept = default;

    // # of live (non-deleted) items. Lazy — walks the list. The walk is
    // bounded by total_items() which itself stays small for normal docs;
    // remote delete-set application stays consistent without any back-
    // reference bookkeeping.
    [[nodiscard]] uint64_t length() const noexcept {
        uint64_t n = 0;
        for (Item* it = start_; it != nullptr; it = it->right) {
            if ((it->flags & kFlagDeleted) == 0) ++n;
        }
        return n;
    }

    // # of all items including tombstones (debug / introspection).
    [[nodiscard]] uint64_t total_items() const noexcept {
        uint64_t n = 0;
        for (Item* it = start_; it != nullptr; it = it->right) ++n;
        return n;
    }

    // First live item, or nullptr if the array is empty.
    [[nodiscard]] Item* first() const noexcept {
        for (Item* it = start_; it != nullptr; it = it->right) {
            if ((it->flags & kFlagDeleted) == 0) return it;
        }
        return nullptr;
    }

    // The live item at index `i` (0-based, skipping tombstones). nullptr if
    // out of range.
    [[nodiscard]] Item* at(uint64_t i) const noexcept {
        uint64_t cursor = 0;
        for (Item* it = start_; it != nullptr; it = it->right) {
            if ((it->flags & kFlagDeleted) != 0) continue;
            if (cursor == i) return it;
            ++cursor;
        }
        return nullptr;
    }

    // Iterate live items in document order. `fn` receives `(Item*, index)`.
    template <class Fn>
    void for_each(Fn&& fn) const noexcept {
        uint64_t cursor = 0;
        for (Item* it = start_; it != nullptr; it = it->right) {
            if ((it->flags & kFlagDeleted) != 0) continue;
            fn(it, cursor);
            ++cursor;
        }
    }

    // Integrate a newly-allocated Item into the array using YATA conflict
    // resolution. The Item's `origin_left` / `origin_right` reference Ids
    // already present in `store` (caller appends to the store first).
    // The Item's left/right pointers are set here.
    //
    // The algorithm walks the conflict zone (from `left.right` up to
    // `right`), tracking the items it's seen ("before origin") and the
    // items it's already passed without yielding to ("conflicting"). When
    // a candidate `o` shares our left-origin we apply the Lamport
    // tie-break; when `o`'s left-origin is something we've already passed
    // we yield. The classic YATA.
    //
    // Conflict zones are bounded by the per-allocation cap below
    // (`kMaxConflict`). Texts typically stay tiny; we hard-cap to keep
    // the algorithm bounded against pathological inputs.
    [[nodiscard]] Status integrate(Item* item, const StructStore<A>& store) noexcept {
        assert(item != nullptr);

        Item* left  = is_valid(item->origin_left)
                    ? const_cast<Item*>(store.find_by_id(item->origin_left))
                    : nullptr;
        Item* right = is_valid(item->origin_right)
                    ? const_cast<Item*>(store.find_by_id(item->origin_right))
                    : nullptr;

        constexpr std::size_t kMaxConflict = 256;
        Item* items_before_origin[kMaxConflict];
        Item* conflicting        [kMaxConflict];
        std::size_t before_n = 0;
        std::size_t conf_n   = 0;

        Item* o = left != nullptr ? left->right : start_;
        while (o != nullptr && o != right) {
            if (before_n >= kMaxConflict) return Status::kIntegrationFailed;

            items_before_origin[before_n++] = o;
            conflicting[conf_n++] = o;

            if (o->origin_left == item->origin_left) {
                // Same left origin — Lamport tie-break.
                if (o->id.client < item->id.client) {
                    left   = o;
                    conf_n = 0;
                } else if (o->origin_right == item->origin_right) {
                    // Same both origins → the larger (client, clock) wins
                    // outright. By construction item.id > o.id here
                    // (otherwise we hit the prev branch), so item wins,
                    // splice here.
                    break;
                }
                // else: o stays, scan past it.
            } else if (is_valid(o->origin_left)) {
                // `o.origin_left` is some other item — if we've already
                // walked it, `o` belongs "before us" in the YATA order.
                Item* o_left = const_cast<Item*>(store.find_by_id(o->origin_left));
                bool in_before = false;
                for (std::size_t i = 0; i < before_n; ++i) {
                    if (items_before_origin[i] == o_left) { in_before = true; break; }
                }
                if (in_before) {
                    bool in_conflicting = false;
                    for (std::size_t i = 0; i < conf_n; ++i) {
                        if (conflicting[i] == o_left) { in_conflicting = true; break; }
                    }
                    if (!in_conflicting) {
                        left   = o;
                        conf_n = 0;
                    }
                    // else: stay positioned; scan past.
                } else {
                    break;
                }
            } else {
                break;
            }
            o = o->right;
        }

        // Splice item between `left` and `left ? left.right : start_`.
        item->left  = left;
        item->right = left != nullptr ? left->right : start_;
        if (left != nullptr) left->right = item;
        else                 start_      = item;
        if (item->right != nullptr) item->right->left = item;

        return Status::kOk;
    }

    // Mark an existing item deleted in the array. Idempotent.
    void mark_deleted(Item* item) noexcept {
        assert(item != nullptr);
        item->flags |= kFlagDeleted;
    }

    // Internal access for Doc-level helpers (e.g. neighbour-based local
    // edits that need to identify the item at index N).
    [[nodiscard]] Item* raw_start() const noexcept { return start_; }

private:
    Item* start_;
};

} // namespace ycpp
