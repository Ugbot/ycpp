// ycpp_subdoc.h — sub-doc registry.
//
// Yjs's `ContentDoc` carries:
//   - varString guid
//   - lib0/any opts
//
// ycpp doesn't materialize a nested Doc instance (sub-doc lifecycle
// management has implications for state vectors + sync that warrant a
// proper design pass). What this header provides is:
//
//   - `SubDocRef` — a parsed view of a sub-doc Item: its Id, its guid,
//     and the raw opts bytes (caller decodes lib0/any).
//   - A registry keyed by guid for enumeration / lookup.
//
// The registry is populated by the Doc apply path when it encounters
// a `ContentKind::kDoc` Item.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_hashmap.h"
#include "ycpp_id.h"
#include "ycpp_reader.h"
#include "ycpp_status.h"
#include "ycpp_update.h"
#include "ycpp_ymap.h"  // for ByteViewHash / ByteViewEq

namespace ycpp {

struct SubDocRef {
    Id        id;          // the kDoc Item's Id
    ByteView  guid;         // arena-owned bytes
    ByteView  opts_payload; // arena-owned bytes (the raw lib0/any blob)
};

template <Allocator A>
class SubDocRegistry {
public:
    static constexpr std::size_t kMaxSubDocs = 1024;

    explicit SubDocRegistry(A* alloc) noexcept
        : alloc_(alloc), by_guid_(alloc), order_n_(0) {
        assert(alloc != nullptr);
    }

    SubDocRegistry(const SubDocRegistry&)            = delete;
    SubDocRegistry& operator=(const SubDocRegistry&) = delete;
    SubDocRegistry(SubDocRegistry&&)                 = delete;
    SubDocRegistry& operator=(SubDocRegistry&&)      = delete;

    ~SubDocRegistry() noexcept = default;

    // Parse the kDoc content payload (varString guid + lib0/any opts)
    // and register an entry. Idempotent on duplicate guids.
    //
    // `content_view` is the bytes captured by the decoder for the
    // entire kDoc payload, including the varUint length prefix of guid
    // and the lib0/any structure of opts.
    [[nodiscard]] Status install(Id id, ByteView content_view) noexcept {
        Reader r{content_view};
        ByteView guid_view{};
        YCPP_TRY(r.length_prefixed(&guid_view));
        const std::size_t guid_end = r.pos();
        const std::size_t total    = content_view.size;

        // Opts is the remaining bytes (a single lib0/any value);
        // capture as opaque slice.
        ByteView opts_view{content_view.data + guid_end,
                            total - guid_end};

        ByteView owned_guid = clone(guid_view);
        ByteView owned_opts = clone(opts_view);
        if ((guid_view.size != 0 && owned_guid.data == nullptr) ||
            (opts_view.size != 0 && owned_opts.data == nullptr)) {
            return Status::kOutOfMemory;
        }

        auto* hit = by_guid_.find(owned_guid);
        if (hit != nullptr) return Status::kOk;  // already registered

        if (order_n_ >= kMaxSubDocs) return Status::kCapacityExceeded;
        SubDocRef ref{id, owned_guid, owned_opts};
        auto [entry, inserted] = by_guid_.insert(owned_guid, ref);
        if (!inserted) return Status::kOutOfMemory;
        order_[order_n_++] = owned_guid;
        return Status::kOk;
    }

    [[nodiscard]] const SubDocRef* find(ByteView guid) const noexcept {
        const auto* hit = by_guid_.find(guid);
        return hit != nullptr ? &hit->value : nullptr;
    }

    [[nodiscard]] std::size_t count() const noexcept { return order_n_; }

    template <class Fn>
    void for_each(Fn&& fn) const noexcept {
        for (std::size_t i = 0; i < order_n_; ++i) {
            const auto* hit = by_guid_.find(order_[i]);
            assert(hit != nullptr);
            fn(hit->value);
        }
    }

private:
    [[nodiscard]] ByteView clone(ByteView v) noexcept {
        if (v.size == 0) return ByteView{};
        auto* dst = static_cast<uint8_t*>(alloc_->alloc(v.size, 1));
        if (dst == nullptr) return ByteView{};
        for (std::size_t i = 0; i < v.size; ++i) dst[i] = v.data[i];
        return ByteView{dst, v.size};
    }

    A* alloc_;
    HashMap<ByteView, SubDocRef, A,
            ymap_detail::ByteViewHash,
            ymap_detail::ByteViewEq>  by_guid_;
    ByteView                          order_[kMaxSubDocs];
    std::size_t                       order_n_;
};

} // namespace ycpp
