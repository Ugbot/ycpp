// ycpp_doc.h — Doc<A>: the orchestrator + encode_diff_v1 (the write path).
//
// A Doc owns:
//   - an allocator policy `A` (concept-constrained; default = DefaultArenaAllocator)
//   - typed Item pool + YMap pool (per-object reuse)
//   - StructStore<A> (per-client sorted Item* vectors)
//   - DeleteSet<A>   (per-client RLE delete ranges)
//   - root YMap<A>* hashmap keyed by root name (arena-owned ByteView keys)
//   - this peer's Lamport clock counter
//
// W3 LWW Y.Map model: insertions whose `parent_ref == kRootName` and
// `content_kind in {kString, kJson, kBinary}` route into the named root
// YMap; the head pointer per key follows last-writer-wins by Lamport
// order. Anything else lives in the store but isn't routed into a type
// (W4+ adds Y.Array / Y.Text / nested types).
//
// Single-threaded per Doc.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

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
#include "ycpp_writer.h"
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

    ~Doc() noexcept = default;

    [[nodiscard]] uint64_t client_id () const noexcept { return client_id_; }
    [[nodiscard]] uint64_t next_clock() const noexcept { return next_clock_; }

    // --- root Y.Map access ---------------------------------------------------
    [[nodiscard]] YMap<A>* get_or_create_map(ByteView name) noexcept {
        auto* hit = roots_.find(name);
        if (hit != nullptr) return hit->value;
        ByteView owned = clone_view(name);
        if (owned.data == nullptr && name.size != 0) return nullptr;
        YMap<A>* m = ymap_pool_.acquire(&alloc_);
        if (m == nullptr) return nullptr;
        auto [entry, inserted] = roots_.insert(owned, m);
        if (!inserted) { ymap_pool_.release(m); return entry->value; }
        return m;
    }

    [[nodiscard]] YMap<A>* get_or_create_map(const char* name) noexcept {
        if (name == nullptr) return nullptr;
        std::size_t n = 0;
        while (name[n] != '\0') ++n;
        return get_or_create_map(ByteView{reinterpret_cast<const uint8_t*>(name), n});
    }

    // --- local edits ---------------------------------------------------------
    [[nodiscard]] Status map_set_string(ByteView root_name, ByteView key,
                                        ByteView value, Id* out_id = nullptr) noexcept {
        YMap<A>* m = get_or_create_map(root_name);
        if (m == nullptr) return Status::kOutOfMemory;

        Item* it = item_pool_.acquire();
        if (it == nullptr) return Status::kOutOfMemory;

        ByteView owned_name  = clone_view(root_name);
        ByteView owned_key   = clone_view(key);
        ByteView owned_value = clone_view(value);
        if ((root_name.size != 0 && owned_name.data  == nullptr) ||
            (key.size       != 0 && owned_key.data   == nullptr) ||
            (value.size     != 0 && owned_value.data == nullptr)) {
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

        YCPP_TRY(install_into_map(it, m));
        if (out_id != nullptr) *out_id = it->id;
        return Status::kOk;
    }

    [[nodiscard]] Status map_set_string(const char* root, const char* key,
                                        const char* value, Id* out_id = nullptr) noexcept {
        return map_set_string(cstr_view(root), cstr_view(key), cstr_view(value), out_id);
    }

    // --- wire I/O ------------------------------------------------------------
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

    [[nodiscard]] StructStore<A>&       store      ()       noexcept { return store_; }
    [[nodiscard]] const StructStore<A>& store      () const noexcept { return store_; }
    [[nodiscard]] DeleteSet<A>&         delete_set ()       noexcept { return deletes_; }
    [[nodiscard]] const DeleteSet<A>&   delete_set () const noexcept { return deletes_; }
    [[nodiscard]] A&                    allocator  ()       noexcept { return alloc_; }

private:
    [[nodiscard]] Status apply_one(const DecodedStruct& d) noexcept {
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
        if ((d.parent_name.size  != 0 && it->parent_name.data  == nullptr) ||
            (d.parent_sub.size   != 0 && it->parent_sub.data   == nullptr) ||
            (d.content_view.size != 0 && it->content_view.data == nullptr)) {
            item_pool_.release(it);
            return Status::kOutOfMemory;
        }

        const bool is_map_value =
            (d.parent_ref   == ParentRef::kRootName) &&
            (d.parent_sub.size > 0) &&
            (d.content_kind == ContentKind::kString  ||
             d.content_kind == ContentKind::kJson    ||
             d.content_kind == ContentKind::kBinary);

        if (is_map_value) {
            YMap<A>* m = get_or_create_map(it->parent_name);
            if (m == nullptr) { item_pool_.release(it); return Status::kOutOfMemory; }
            return install_into_map(it, m);
        }
        return install_bare(it);
    }

    [[nodiscard]] Status install_bare(Item* it) noexcept {
        const Status s = store_.append(it);
        if (s != Status::kOk) { item_pool_.release(it); return s; }
        bump_clock_for(it);
        return Status::kOk;
    }

    [[nodiscard]] Status install_into_map(Item* it, YMap<A>* m) noexcept {
        const Status s = store_.append(it);
        if (s != Status::kOk) { item_pool_.release(it); return s; }
        Item* loser = m->integrate(it);
        if (loser != nullptr) {
            loser->flags |= kFlagDeleted;
            YCPP_TRY(deletes_.add(loser->id.client, loser->id.clock,
                                  loser->length == 0 ? 1 : loser->length));
        }
        bump_clock_for(it);
        return Status::kOk;
    }

    void bump_clock_for(const Item* it) noexcept {
        if (it->id.client == client_id_) {
            const uint64_t end = it->id.clock + (it->length == 0 ? 1 : it->length);
            if (end > next_clock_) next_clock_ = end;
        }
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

    [[nodiscard]] static ByteView cstr_view(const char* s) noexcept {
        if (s == nullptr) return ByteView{};
        std::size_t n = 0;
        while (s[n] != '\0') ++n;
        return ByteView{reinterpret_cast<const uint8_t*>(s), n};
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

// ----- encode_diff_v1 -------------------------------------------------------

namespace detail_encode {

[[nodiscard]] inline Status write_id(Writer& w, Id id) noexcept {
    YCPP_TRY(w.varint_u64(id.client));
    YCPP_TRY(w.varint_u64(id.clock));
    return Status::kOk;
}

[[nodiscard]] inline uint8_t build_info_byte(const Item* it) noexcept {
    uint8_t info = static_cast<uint8_t>(it->content_kind) & kInfoMaskContentKind;
    if (is_valid(it->origin_left )) info |= kInfoFlagHasLeftOrigin;
    if (is_valid(it->origin_right)) info |= kInfoFlagHasRightOrigin;
    if (it->parent_sub.size > 0)    info |= kInfoFlagHasParentSub;
    return info;
}

[[nodiscard]] inline Status write_content_payload(Writer& w, const Item* it) noexcept {
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

[[nodiscard]] inline Status write_struct(Writer& w, const Item* it) noexcept {
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
            return Status::kInternal;
        }
    }
    if (info_has_parent_sub(info)) {
        YCPP_TRY(w.length_prefixed(it->parent_sub.data, it->parent_sub.size));
    }
    return write_content_payload(w, it);
}

} // namespace detail_encode

// Emit a Yjs updateV1 carrying only the structs whose clock is >=
// since[client], plus the full DeleteSet (deletions are monotone).
template <Allocator A>
[[nodiscard]] inline Status encode_diff_v1(const Doc<A>&         doc,
                                            const StateVector<A>* since,
                                            Writer*               out) noexcept {
    assert(out != nullptr);

    uint64_t n_clients_to_emit = 0;
    doc.store().for_each_client(
        [&](uint64_t client, const Item* const* items, std::size_t count) noexcept {
            const uint64_t since_clock = (since != nullptr) ? since->get(client) : 0U;
            for (std::size_t i = 0; i < count; ++i) {
                if (items[i]->id.clock >= since_clock) { ++n_clients_to_emit; return; }
            }
        });
    YCPP_TRY(out->varint_u64(n_clients_to_emit));

    Status acc = Status::kOk;
    doc.store().for_each_client(
        [&](uint64_t client, const Item* const* items, std::size_t count) noexcept {
            if (acc != Status::kOk) return;
            const uint64_t since_clock = (since != nullptr) ? since->get(client) : 0U;

            std::size_t start = 0;
            while (start < count && items[start]->id.clock < since_clock) ++start;
            const std::size_t emit_count = count - start;
            if (emit_count == 0) return;

            Status s = out->varint_u64(static_cast<uint64_t>(emit_count));
            if (s != Status::kOk) { acc = s; return; }
            s = out->varint_u64(client);
            if (s != Status::kOk) { acc = s; return; }
            s = out->varint_u64(items[start]->id.clock);
            if (s != Status::kOk) { acc = s; return; }
            for (std::size_t i = start; i < count; ++i) {
                s = detail_encode::write_struct(*out, items[i]);
                if (s != Status::kOk) { acc = s; return; }
            }
        });
    if (acc != Status::kOk) return acc;

    return doc.delete_set().encode(*out);
}

} // namespace ycpp
