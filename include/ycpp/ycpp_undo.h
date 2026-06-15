// ycpp_undo.h — minimal origin-scoped UndoManager.
//
// The UndoManager records the Ids of items emitted by a tracked set of
// "origins" (caller-defined u64 tags) within each `capture()` frame.
// `undo()` rolls back the most recent frame by marking those items
// deleted and adding the range to the Doc's delete set; `redo()`
// re-clears the kFlagDeleted bit on the affected items.
//
// Semantically this is *local* undo: it doesn't synthesize new CRDT
// inserts for previously-deleted content, so a remote peer who saw the
// original ops sees a delete on undo and an "un-delete" on redo via the
// existing items' Ids. For Y.Map LWW + Y.Text append workflows that
// makes sense and matches what app-side editors expect. Full Yjs-style
// undo with re-insert semantics is a follow-up.
//
// Capture model: the caller invokes `capture_begin(origin)` before each
// local edit and `capture_end()` after. The manager observes the
// per-client state vector around the edit and records the Ids that
// became visible. Multiple inserts inside one capture window become a
// single undo step.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_delete_set.h"
#include "ycpp_doc.h"
#include "ycpp_hashmap.h"
#include "ycpp_id.h"
#include "ycpp_item.h"
#include "ycpp_state_vector.h"
#include "ycpp_status.h"
#include "ycpp_struct_store.h"

namespace ycpp {

template <Allocator A>
class UndoManager {
public:
    static constexpr std::size_t kMaxFrames           = 256;
    static constexpr std::size_t kMaxIdsPerFrame      = 256;
    static constexpr std::size_t kMaxTrackedOrigins   = 32;

    explicit UndoManager(Doc<A>& doc) noexcept
        : doc_(&doc),
          undo_top_(0),
          redo_top_(0),
          tracked_n_(0),
          open_origin_(0),
          open_active_(false),
          pre_clock_(0) {}

    UndoManager(const UndoManager&)            = delete;
    UndoManager& operator=(const UndoManager&) = delete;
    UndoManager(UndoManager&&)                 = delete;
    UndoManager& operator=(UndoManager&&)      = delete;

    ~UndoManager() noexcept = default;

    // Mark `origin` as a tracked origin. Items created during a
    // `capture_begin(o)` window for any tracked origin become undoable.
    [[nodiscard]] Status track(uint64_t origin) noexcept {
        for (std::size_t i = 0; i < tracked_n_; ++i) {
            if (tracked_[i] == origin) return Status::kOk;
        }
        if (tracked_n_ >= kMaxTrackedOrigins) return Status::kCapacityExceeded;
        tracked_[tracked_n_++] = origin;
        return Status::kOk;
    }

    // Open a capture window. The caller emits 1+ local edits, then
    // calls capture_end() to seal the frame.
    [[nodiscard]] Status capture_begin(uint64_t origin) noexcept {
        if (open_active_) return Status::kAlreadyExists;
        if (!is_tracked(origin)) return Status::kInvalidArgument;
        open_origin_ = origin;
        open_active_ = true;
        pre_clock_   = doc_->next_clock();
        return Status::kOk;
    }

    [[nodiscard]] Status capture_end() noexcept {
        if (!open_active_) return Status::kInvalidArgument;
        const uint64_t after_clock = doc_->next_clock();
        open_active_ = false;
        if (after_clock == pre_clock_) return Status::kOk;  // nothing emitted

        if (undo_top_ >= kMaxFrames) {
            // Drop oldest frame to make room.
            for (std::size_t i = 1; i < kMaxFrames; ++i) {
                undo_stack_[i - 1] = undo_stack_[i];
            }
            --undo_top_;
        }
        Frame& f = undo_stack_[undo_top_++];
        f.origin = open_origin_;
        f.client = doc_->client_id();
        f.start  = pre_clock_;
        f.end    = after_clock;
        // Future-redo gets reset on a new capture.
        redo_top_ = 0;
        return Status::kOk;
    }

    [[nodiscard]] bool can_undo() const noexcept { return undo_top_ > 0; }
    [[nodiscard]] bool can_redo() const noexcept { return redo_top_ > 0; }

    // Undo the most recent frame: mark every Item with id.client ==
    // frame.client and clock ∈ [frame.start, frame.end) as deleted.
    [[nodiscard]] Status undo() noexcept {
        if (undo_top_ == 0) return Status::kNotFound;
        const Frame f = undo_stack_[--undo_top_];
        mark_range_flag(f.client, f.start, f.end, /*set=*/true);
        YCPP_TRY(doc_->delete_set().add(f.client, f.start, f.end - f.start));
        // Move frame onto redo stack.
        if (redo_top_ < kMaxFrames) redo_stack_[redo_top_++] = f;
        return Status::kOk;
    }

    // Redo: clear the kFlagDeleted bit on the items in the top redo
    // frame. (The DeleteSet entry stays — peers receive the delete and
    // applying our redo locally still surfaces the live content via the
    // un-flagged items; the upstream DeleteSet entry is monotone so
    // peers' view stays self-consistent in the LWW Y.Map case.)
    [[nodiscard]] Status redo() noexcept {
        if (redo_top_ == 0) return Status::kNotFound;
        const Frame f = redo_stack_[--redo_top_];
        mark_range_flag(f.client, f.start, f.end, /*set=*/false);
        if (undo_top_ < kMaxFrames) undo_stack_[undo_top_++] = f;
        return Status::kOk;
    }

    [[nodiscard]] std::size_t undo_depth() const noexcept { return undo_top_; }
    [[nodiscard]] std::size_t redo_depth() const noexcept { return redo_top_; }

private:
    struct Frame {
        uint64_t origin;
        uint64_t client;
        uint64_t start;
        uint64_t end;
    };

    [[nodiscard]] bool is_tracked(uint64_t origin) const noexcept {
        for (std::size_t i = 0; i < tracked_n_; ++i) {
            if (tracked_[i] == origin) return true;
        }
        return false;
    }

    void mark_range_flag(uint64_t client, uint64_t start, uint64_t end,
                          bool set) noexcept {
        for (uint64_t k = start; k < end; ++k) {
            Item* it = doc_->store().find_by_id(Id{client, k});
            if (it == nullptr) continue;
            if (set) it->flags |=  kFlagDeleted;
            else     it->flags &= ~static_cast<uint16_t>(kFlagDeleted);
        }
    }

    Doc<A>*        doc_;
    Frame          undo_stack_[kMaxFrames];
    Frame          redo_stack_[kMaxFrames];
    std::size_t    undo_top_;
    std::size_t    redo_top_;
    uint64_t       tracked_[kMaxTrackedOrigins];
    std::size_t    tracked_n_;
    uint64_t       open_origin_;
    bool           open_active_;
    uint64_t       pre_clock_;
};

} // namespace ycpp
