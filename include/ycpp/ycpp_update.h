// ycpp_update.h — public surface of the update wire-format codecs.
//
// W2 ships `decode_update_v1`: parse a Yjs updateV1 blob into a flat
// vector of `DecodedStruct`s + a DeleteSet. Integration (W3) consumes
// these to splice Items into the Doc; types (W4) interpret the content
// payloads.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_byteview.h"
#include "ycpp_delete_set.h"
#include "ycpp_id.h"
#include "ycpp_item.h"
#include "ycpp_status.h"

namespace ycpp {

// One decoded struct from the wire. This is a lightweight POD — the
// caller-supplied buffer for the update remains the storage backing every
// ByteView field. The decoded vector is meant to be drained by integration
// in W3 (where Items get cloned into Doc-owned storage).
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
    // Raw content bytes — interpretation depends on `content_kind`:
    //   kString / kJson      → UTF-8 text
    //   kBinary              → opaque bytes
    //   kGc / kSkip / kDeleted → empty (length carries the run width)
    //   everything else      → opaque; parsed in W4
    ByteView      content_view;
};

// A decoded update is just a pair of (struct vector, delete-set). The
// vector storage comes from the supplied allocator.
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

    ~DecodedUpdate() noexcept = default;  // storage owned by arena

    [[nodiscard]] Status push(const DecodedStruct& s) noexcept {
        YCPP_TRY(ensure_cap(size_ + 1));
        structs_[size_++] = s;
        return Status::kOk;
    }

    [[nodiscard]] std::size_t          size  () const noexcept { return size_; }
    [[nodiscard]] const DecodedStruct* data  () const noexcept { return structs_; }
    [[nodiscard]] const DecodedStruct& at    (std::size_t i) const noexcept {
        assert(i < size_);
        return structs_[i];
    }

    [[nodiscard]] DeleteSet<A>&        delete_set()       noexcept { return delete_set_; }
    [[nodiscard]] const DeleteSet<A>&  delete_set() const noexcept { return delete_set_; }

private:
    static constexpr std::size_t kInitialCap = 32;
    static constexpr std::size_t kMaxStructs = 1U << 26;  // 64M structs cap

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

// Parse a Yjs updateV1 binary blob `bytes` into `out`. The DecodedUpdate's
// ByteView fields alias `bytes`; the caller must keep `bytes` alive while
// `out` is used.
template <Allocator A>
[[nodiscard]] Status decode_update_v1(ByteView bytes, DecodedUpdate<A>* out) noexcept;

// Extern instantiation for the default allocator — keeps consumers who use
// the default Doc from re-compiling the whole decoder template.
extern template Status decode_update_v1<DefaultArenaAllocator>(
    ByteView, DecodedUpdate<DefaultArenaAllocator>*) noexcept;

} // namespace ycpp
