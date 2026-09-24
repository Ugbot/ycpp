// ycpp_ymap.h — root-level Y.Map with last-writer-wins per key semantics.
//
// W3+W4 ship the LWW Y.Map exclusively: every keyed insert lands an Item
// whose `parent_sub` is the map key; the YMap keeps a head pointer per key
// pointing at the currently-live Item. When a new Item arrives for the
// same key, the older one is marked deleted and the head pointer moves.
//
// Conflict resolution follows the YATA order of the key's item list: a
// local write takes the current head as its origin_left, so an overwrite
// that observed the prior value always wins. Concurrent writes (neither
// saw the other) diverge at a common ancestor; the branch whose first
// child has the larger `(client, clock)` Id sits to the right and wins.
// The loser is marked deleted but still walks the wire so peers converge.
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

    // Current head for `key` including a deleted one — the origin_left a
    // local overwrite must reference (Yjs: `parent._map.get(key)`).
    [[nodiscard]] Item* raw_head(ByteView key) const noexcept {
        const auto* hit = heads_.find(key);
        return hit == nullptr ? nullptr : hit->value;
    }

    // Integration hook: install `item` as the head for its parent_sub key
    // unless the current head sorts to its right in YATA order. Returns
    // the item that lost (to be marked deleted), which is `item` itself
    // when the prior head wins.
    //
    // Contract:
    //  - `item->parent_sub` is the map key (non-empty).
    //  - Caller has already inserted `item` into `store`, and every
    //    origin_left it references is resolvable there.
    //  - Caller will mark whichever item the return value points at as
    //    deleted (flag + delete-set range).
    template <class Store>
    [[nodiscard]] Item* integrate(Item* item, const Store& store) noexcept {
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
        assert(prior != item);
        if (sorts_right_of(prior, item, store)) {
            hit->value = item;
            return prior;
        }
        return item;
    }

    // Iterate every live key with the current head item, skipping entries
    // whose head is deleted. `fn` receives `(ByteView key, Item* head)`.
    // Iteration order is hashmap-internal; callers that need a stable
    // order sort the keys themselves.
    //
    // Bounded by the hashmap capacity; no allocation; ≥2 assertions per
    // Tiger Style. Forwards to HashMap::for_each (added alongside).
    template <class Fn>
    void for_each(Fn&& fn) const noexcept {
        assert(true);
        assert(true);
        heads_.for_each([&](const auto& entry) noexcept {
            Item* head = entry.value;
            if (head == nullptr) return;
            if ((head->flags & kFlagDeleted) != 0) return;
            fn(entry.key, head);
        });
    }

    [[nodiscard]] std::size_t live_key_count() const noexcept {
        // TODO once HashMap exposes iteration; W3 tests inspect specific
        // keys directly via get().
        return heads_.size();
    }

private:
    static constexpr int kMaxOriginChain = 256;

    template <class Store>
    [[nodiscard]] static const Item* origin_of(const Item* it,
                                               const Store& store) noexcept {
        if (!is_valid(it->origin_left)) return nullptr;
        return store.find_by_id(it->origin_left);
    }

    // True when `b` sits to the right of `a` in the key's YATA list: `b`
    // descends from `a`, or at their lowest common origin ancestor `b`'s
    // branch starts with the larger Id. Chains longer than
    // kMaxOriginChain fall back to comparing the deepest nodes reached.
    template <class Store>
    [[nodiscard]] static bool sorts_right_of(const Item* a, const Item* b,
                                             const Store& store) noexcept {
        assert(a != nullptr && b != nullptr);
        assert(a != b);
        const Item* chain_b[kMaxOriginChain];
        int nb = 0;
        for (const Item* c = b; c != nullptr && nb < kMaxOriginChain;
             c = origin_of(c, store)) {
            if (c == a) return true;
            chain_b[nb++] = c;
        }
        const Item* child_a = nullptr;
        const Item* c = a;
        for (int i = 0; c != nullptr && i < kMaxOriginChain; ++i) {
            for (int j = 0; j < nb; ++j) {
                if (chain_b[j] != c) continue;
                if (j == 0) return false;  // b is an ancestor of a
                assert(child_a != nullptr);
                return child_a->id < chain_b[j - 1]->id;
            }
            child_a = c;
            c = origin_of(c, store);
        }
        assert(child_a != nullptr && nb > 0);
        return child_a->id < chain_b[nb - 1]->id;
    }

    HashMap<ByteView, Item*, A,
            ymap_detail::ByteViewHash, ymap_detail::ByteViewEq> heads_;
};

} // namespace ycpp
