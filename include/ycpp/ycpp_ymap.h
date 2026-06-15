// ycpp_ymap.h — root-level Y.Map with last-writer-wins per key semantics.
//
// W3+W4 ship the LWW Y.Map exclusively: every keyed insert lands an Item
// whose `parent_sub` is the map key; the YMap keeps a head pointer per key
// pointing at the currently-live Item. When a new Item arrives for the
// same key, the older one is marked deleted and the head pointer moves.
//
// Conflict resolution is purely Lamport: if two clients write the same key
// concurrently (neither sees the other), the Item with the larger
// `(client, clock)` Id wins. The loser is marked deleted but still walks
// the wire so peers converge.
//
// Y.Array / Y.Text / nested types require neighbour-walking integration
// (Yjs's full algorithm). Those land in a later wave. The spine's
// projection store needs LWW only.

#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_hashmap.h"
#include "ycpp_id.h"
#include "ycpp_item.h"

namespace ycpp {

namespace ymap_detail {

// Hash function for ByteView keys (FNV-1a). We need a stable hash so the
// head-pointer hashmap behaves deterministically across clients.
struct ByteViewHash {
    [[nodiscard]] std::size_t operator()(const ByteView& v) const noexcept {
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t h = 0xcbf29ce484222325ULL;
        for (std::size_t i = 0; i < v.size; ++i) {
            h = (h ^ static_cast<uint64_t>(v.data[i])) * kPrime;
        }
        return static_cast<std::size_t>(h);
    }
};

struct ByteViewEq {
    [[nodiscard]] bool operator()(const ByteView& a, const ByteView& b) const noexcept {
        if (a.size != b.size) return false;
        if (a.size == 0)      return true;
        return std::memcmp(a.data, b.data, a.size) == 0;
    }
};

} // namespace ymap_detail

template <Allocator A>
class YMap {
public:
    explicit YMap(A* alloc) noexcept
        : heads_(alloc) {
        assert(alloc != nullptr);
    }

    YMap(const YMap&)            = delete;
    YMap& operator=(const YMap&) = delete;
    YMap(YMap&&)                 = delete;
    YMap& operator=(YMap&&)      = delete;

    ~YMap() noexcept = default;

    // Returns the currently-live Item for `key`, or nullptr if no live entry.
    // Caller reads the value via `it->content_view` and `it->content_kind`.
    [[nodiscard]] Item* get(ByteView key) const noexcept {
        const auto* hit = heads_.find(key);
        if (hit == nullptr) return nullptr;
        Item* head = hit->value;
        if (head == nullptr) return nullptr;
        if ((head->flags & kFlagDeleted) != 0) return nullptr;
        return head;
    }

    // Integration hook: install `item` as the head for its parent_sub key
    // unless a competing live item with a larger (client, clock) Id already
    // owns the slot. Returns the item that lost (to be marked deleted) or
    // nullptr if the new item is itself the loser.
    //
    // Contract:
    //  - `item->parent_sub` is the map key (non-empty).
    //  - Caller has already inserted `item` into the StructStore.
    //  - Caller will mark whichever item the return value points at as
    //    deleted (flag + delete-set range).
    [[nodiscard]] Item* integrate(Item* item) noexcept {
        assert(item             != nullptr);
        assert(item->parent_sub.size > 0);
        auto* hit = heads_.find(item->parent_sub);
        if (hit == nullptr) {
            (void)heads_.insert(item->parent_sub, item);
            return nullptr;
        }
        Item* prior = hit->value;
        if (prior == nullptr) {
            hit->value = item;
            return nullptr;
        }
        // Tie-break by Lamport order: larger (client, clock) wins.
        if (prior->id < item->id) {
            hit->value = item;
            return prior;       // loser: caller marks deleted
        }
        return item;            // new item is the loser; caller marks it deleted
    }

    template <class Fn>
    void for_each(Fn&& fn) const noexcept {
        // Iteration order is hashmap-internal; callers that need a stable
        // order should sort by ByteView themselves.
        // We expose a min surface here; heads_ doesn't have a public
        // visitor today, so this is a TODO once heads_ gains one. For now,
        // a no-op iterator placeholder.
        (void)fn;
    }

    [[nodiscard]] std::size_t live_key_count() const noexcept {
        // TODO once HashMap exposes iteration; W3 tests inspect specific
        // keys directly via get().
        return heads_.size();
    }

private:
    HashMap<ByteView, Item*, A,
            ymap_detail::ByteViewHash, ymap_detail::ByteViewEq> heads_;
};

} // namespace ycpp
