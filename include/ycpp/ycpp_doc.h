// ycpp_doc.h — Doc<A>: the orchestrator.
//
// A Doc owns:
//   - an allocator policy `A` (templated; default = DefaultArenaAllocator,
//     production gestaltd uses ybolt::BoltArenaAllocator over bolt::Arena)
//   - a pool of Item nodes (per-object reuse during GC; W8)
//   - a StructStore<A> (per-client sorted vectors)
//   - a DeleteSet<A> (per-client RLE delete ranges)
//   - a hashmap of root YMap<A>* keyed by root name
//   - a Lamport clock counter (this peer's next clock)
//
// Public surface (W3+W5):
//   apply_update_v1   — integrate an incoming wire update
//   state_vector      — fill out this Doc's state vector
//   encode_diff_v1    — emit a v1 update carrying everything `since` is
//                       missing
//   get_or_create_map — root-level Y.Map by name
//
// Non-thread-safe per Doc. Caller serialises concurrent access.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_delete_set.h"
#include "ycpp_hashmap.h"
#include "ycpp_id.h"
#include "ycpp_item.h"
#include "ycpp_pool.h"
#include "ycpp_state_vector.h"
#include "ycpp_status.h"
#include "ycpp_struct_store.h"
#include "ycpp_update.h"
#include "ycpp_ymap.h"

namespace ycpp {

template <Allocator A>
class Doc {
public:
    explicit Doc(uint64_t client_id, A allocator = A{}) noexcept
        : alloc_(std::move(allocator)),
          item_pool_(&alloc_, 256),
          ymap_pool_(&alloc_, 8),
          store_(&alloc_),
          deletes_(&alloc_),
          roots_(&alloc_),
          client_id_(client_id),
          next_clock_(0) {}

    Doc(const Doc&)            = delete;
    Doc& operator=(const Doc&) = delete;
    Doc(Doc&&)                 = delete;
    Doc& operator=(Doc&&)      = delete;

    ~Doc() noexcept = default;  // arena owns the storage

    [[nodiscard]] uint64_t client_id () const noexcept { return client_id_; }
    [[nodiscard]] uint64_t next_clock() const noexcept { return next_clock_; }

    // --- root Y.Map access ----------------------------------------------------
    [[nodiscard]] YMap<A>* get_or_create_map(ByteView name) noexcept {
        auto* hit = roots_.find(name);
        if (hit != nullptr) return hit->value;
        // Copy the name into arena-owned storage so the hashmap key outlives
        // the caller's buffer.
        ByteView owned = clone_view(name);
        if (owned.data == nullptr && name.size != 0) return nullptr;
        YMap<A>* m = ymap_pool_.acquire(&alloc_);
        if (m == nullptr) return nullptr;
        auto [entry, inserted] = roots_.insert(owned, m);
        if (!inserted) {
            ymap_pool_.release(m);
            return entry->value;
        }
        return m;
    }

    [[nodiscard]] YMap<A>* get_or_create_map(const char* name) noexcept {
        return get_or_create_map(ByteView{
            reinterpret_cast<const uint8_t*>(name),
            name == nullptr ? 0 : std::char_traits<char>::length(name)
        });
    }

    // --- local edits ---------------------------------------------------------
    //
    // Emit a kString Item for `key = value` on the named root map. The
    // Item lands in the StructStore + the root map's head pointer.
    // Returns the Item's Id on success.
    [[nodiscard]] Status map_set_string(ByteView root_name, ByteView key,
                                        ByteView value, Id* out_id = nullptr) noexcept {
        YMap<A>* m = get_or_create_map(root_name);
        if (m == nullptr) return Status::kOutOfMemory;

        Item* it = item_pool_.acquire();
        if (it == nullptr) return Status::kOutOfMemory;
        ByteView owned_name  = clone_view(root_name);
        ByteView owned_key   = clone_view(key);
        ByteView owned_value = clone_view(value);
        if ((root_name.size  != 0 && owned_name.data  == nullptr) ||
            (key.size        != 0 && owned_key.data   == nullptr) ||
            (value.size      != 0 && owned_value.data == nullptr)) {
            item_pool_.release(it);
            return Status::kOutOfMemory;
        }
        it->id            = Id{client_id_, next_clock_};
        it->origin_left   = kInvalidId;
        it->origin_right  = kInvalidId;
        it->parent_ref    = ParentRef::kRootName;
        it->parent_name   = owned_name;
        it->parent_sub    = owned_key;
        it->content_kind  = ContentKind::kString;
        it->length        = 1;
        it->content_view  = owned_value;
        it->flags         = 0;

        YCPP_TRY(install_item(it, m));
        if (out_id != nullptr) *out_id = it->id;
        ++next_clock_;
        return Status::kOk;
    }

    // --- wire-format I/O -----------------------------------------------------
    [[nodiscard]] Status state_vector(StateVector<A>* out) const noexcept {
        assert(out != nullptr);
        Status acc = Status::kOk;
        store_.for_each_client(
            [&](uint64_t client, const Item* const* /*items*/, std::size_t /*n*/) noexcept {
                if (acc != Status::kOk) return;
                const Status s = out->set(client, store_.state(client));
                if (s != Status::kOk) acc = s;
            });
        return acc;
    }

    [[nodiscard]] Status apply_update_v1(ByteView bytes) noexcept {
        DecodedUpdate<A> decoded{&alloc_};
        YCPP_TRY(decode_update_v1<A>(bytes, &decoded));
        for (std::size_t i = 0; i < decoded.size(); ++i) {
            YCPP_TRY(apply_one(decoded.at(i)));
        }
        // Merge incoming delete set + actually flag the targets.
        Status acc = Status::kOk;
        decoded.delete_set().for_each_client(
            [&](uint64_t client, const DeleteRange* rs, std::size_t n) noexcept {
                for (std::size_t i = 0; i < n; ++i) {
                    if (acc != Status::kOk) return;
                    const Status s = deletes_.add(client, rs[i].clock_start, rs[i].length);
                    if (s != Status::kOk) { acc = s; return; }
                    mark_range_deleted(client, rs[i].clock_start, rs[i].length);
                }
            });
        return acc;
    }

    // Access for the encoder. Internals exposed via friends / accessors so the
    // encoder TU can iterate without crossing the public surface.
    [[nodiscard]] StructStore<A>&       store      ()       noexcept { return store_; }
    [[nodiscard]] const StructStore<A>& store      () const noexcept { return store_; }
    [[nodiscard]] DeleteSet<A>&         delete_set ()       noexcept { return deletes_; }
    [[nodiscard]] const DeleteSet<A>&   delete_set () const noexcept { return deletes_; }
    [[nodiscard]] A&                    allocator  ()       noexcept { return alloc_; }

private:
    [[nodiscard]] Status apply_one(const DecodedStruct& d) noexcept {
        // Drop structs whose Id is already in our store (idempotent replay).
        if (store_.find_by_id(d.id) != nullptr) return Status::kOk;

        Item* it = item_pool_.acquire();
        if (it == nullptr) return Status::kOutOfMemory;
        it->id            = d.id;
        it->origin_left   = d.origin_left;
        it->origin_right  = d.origin_right;
        it->parent_ref    = d.parent_ref;
        it->parent_id     = d.parent_id;
        it->parent_name   = clone_view(d.parent_name);
        it->parent_sub    = clone_view(d.parent_sub);
        it->content_kind  = d.content_kind;
        it->length        = d.length;
        it->content_view  = clone_view(d.content_view);
        it->flags         = 0;
        // Validate that the owned copies didn't fail mid-clone.
        if ((d.parent_name.size   != 0 && it->parent_name.data   == nullptr) ||
            (d.parent_sub.size    != 0 && it->parent_sub.data    == nullptr) ||
            (d.content_view.size  != 0 && it->content_view.data  == nullptr)) {
            item_pool_.release(it);
            return Status::kOutOfMemory;
        }

        // For W3+W4 LWW, only root-Y.Map inserts are integrated into a YMap.
        // Other content kinds (GC, Deleted, anything without a root parent
        // name) land in the store and become visible via direct find_by_id
        // but don't enter any type's view.
        if (d.parent_ref == ParentRef::kRootName && d.parent_sub.size > 0
            && (d.content_kind == ContentKind::kString
             || d.content_kind == ContentKind::kJson
             || d.content_kind == ContentKind::kBinary)) {
            YMap<A>* m = get_or_create_map(it->parent_name);
            if (m == nullptr) { item_pool_.release(it); return Status::kOutOfMemory; }
            YCPP_TRY(install_item(it, m));
        } else {
            // Not in a known type — just record in the store; W4+ will route.
            const Status s = store_.append(it);
            if (s != Status::kOk) {
                item_pool_.release(it);
                return s;
            }
            // Keep the local clock ahead of any client we've heard from.
            if (it->id.client == client_id_ && it->id.clock + it->length > next_clock_) {
                next_clock_ = it->id.clock + it->length;
            }
        }
        return Status::kOk;
    }

    [[nodiscard]] Status install_item(Item* it, YMap<A>* m) noexcept {
        const Status s = store_.append(it);
        if (s != Status::kOk) {
            item_pool_.release(it);
            return s;
        }
        Item* loser = m->integrate(it);
        if (loser != nullptr) {
            loser->flags |= kFlagDeleted;
            YCPP_TRY(deletes_.add(loser->id.client, loser->id.clock,
                                 loser->length == 0 ? 1 : loser->length));
        }
        if (it->id.client == client_id_ && it->id.clock + it->length > next_clock_) {
            next_clock_ = it->id.clock + it->length;
        }
        return Status::kOk;
    }

    void mark_range_deleted(uint64_t client, uint64_t clock_start,
                            uint64_t length) noexcept {
        for (uint64_t off = 0; off < length; ++off) {
            Item* it = store_.find_by_id(Id{client, clock_start + off});
            if (it != nullptr) it->flags |= kFlagDeleted;
        }
    }

    [[nodiscard]] ByteView clone_view(ByteView v) noexcept {
        if (v.size == 0) return ByteView{};
        auto* dst = static_cast<uint8_t*>(alloc_.alloc(v.size, 1));
        if (dst == nullptr) return ByteView{};
        std::memcpy(dst, v.data, v.size);
        return ByteView{dst, v.size};
    }

    A                                   alloc_;
    Pool<Item, A>                       item_pool_;
    Pool<YMap<A>, A>                    ymap_pool_;
    StructStore<A>                      store_;
    DeleteSet<A>                        deletes_;
    HashMap<ByteView, YMap<A>*, A,
            ymap_detail::ByteViewHash,
            ymap_detail::ByteViewEq>    roots_;
    uint64_t                            client_id_;
    uint64_t                            next_clock_;
};

// Encoder lives in its own TU; declared here for callers.
template <Allocator A>
[[nodiscard]] Status encode_diff_v1(const Doc<A>& doc,
                                     const StateVector<A>* since,
                                     Writer* out) noexcept;

extern template Status encode_diff_v1<DefaultArenaAllocator>(
    const Doc<DefaultArenaAllocator>&,
    const StateVector<DefaultArenaAllocator>*,
    Writer*) noexcept;

} // namespace ycpp
