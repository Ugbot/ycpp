// ycpp_hashmap.h — open-addressing hash map (SwissTable-shape).
//
// Slots are grouped in fixed-width buckets; per-slot metadata bytes encode
// `empty`, `tombstone`, or `H2(key)` (top 7 hash bits). Probe is linear
// over groups; the scalar group-probe path is the default. SSE2/NEON
// acceleration is a follow-up (W7-era; left as a hook).
//
// Hash + equality + value semantics are all customisable via template
// parameters so a UInt64 -> Item* map and a string-view -> RootType* map
// can share the same body. Storage comes from the templated Allocator.
//
// Tiger Style: bounded capacity, bounded rehash (one grow doubles the
// table; we never shrink), no exceptions, every fn noexcept.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>

#include "ycpp_arena.h"

namespace ycpp {

namespace hashmap_detail {

inline constexpr uint8_t kCtrlEmpty     = 0x80;  // 1000 0000
inline constexpr uint8_t kCtrlTombstone = 0xFE;  // 1111 1110
// Live entries store H2(hash) in bits [0..6]; the high bit is 0.

[[nodiscard]] inline constexpr uint8_t hash_h2(uint64_t full) noexcept {
    return static_cast<uint8_t>((full >> 57) & 0x7FU);
}

[[nodiscard]] inline constexpr std::size_t round_up_pow2(std::size_t n) noexcept {
    std::size_t v = 1;
    while (v < n) v <<= 1;
    return v;
}

} // namespace hashmap_detail

template <class K, class V, Allocator A,
          class Hash  = std::hash<K>,
          class Equal = std::equal_to<K>>
class HashMap {
public:
    static_assert(std::is_nothrow_destructible_v<K>, "HashMap<K>: K must be nothrow-destructible");
    static_assert(std::is_nothrow_destructible_v<V>, "HashMap<V>: V must be nothrow-destructible");

    static constexpr std::size_t kMinCap       = 16;
    static constexpr std::size_t kMaxCap       = 1U << 24;  // 16M slots cap
    static constexpr float       kLoadFactor   = 0.75f;     // grow when size > cap * 0.75

    struct Entry { K key; V value; };

    explicit HashMap(A* alloc) noexcept
        : alloc_(alloc), ctrl_(nullptr), entries_(nullptr),
          cap_(0), size_(0), tombstones_(0),
          hash_(Hash{}), eq_(Equal{}) {
        assert(alloc != nullptr);
    }

    HashMap(const HashMap&)            = delete;
    HashMap& operator=(const HashMap&) = delete;
    HashMap(HashMap&&)                 = delete;
    HashMap& operator=(HashMap&&)      = delete;

    ~HashMap() noexcept {
        destroy_all_entries();
        // Storage lives in the arena; we don't free the slabs.
    }

    [[nodiscard]] std::size_t size()     const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
    [[nodiscard]] bool        empty()    const noexcept { return size_ == 0; }

    // Pre-size the table. Returns false on OOM or if capacity would exceed kMaxCap.
    [[nodiscard]] bool reserve(std::size_t want) noexcept {
        if (want <= cap_) return true;
        const std::size_t target = hashmap_detail::round_up_pow2(
            want < kMinCap ? kMinCap : want);
        if (target > kMaxCap) return false;
        return rehash_to(target);
    }

    // Find: returns &Entry on hit, nullptr on miss.
    [[nodiscard]] Entry* find(const K& k) noexcept {
        if (cap_ == 0) return nullptr;
        const uint64_t h     = static_cast<uint64_t>(hash_(k));
        const uint8_t  h2    = hashmap_detail::hash_h2(h);
        std::size_t    idx   = static_cast<std::size_t>(h) & (cap_ - 1);
        for (std::size_t step = 0; step < cap_; ++step) {
            const uint8_t c = ctrl_[idx];
            if (c == hashmap_detail::kCtrlEmpty) return nullptr;
            if (c == h2 && eq_(entries_[idx].key, k)) return &entries_[idx];
            idx = (idx + 1) & (cap_ - 1);
        }
        return nullptr;
    }

    [[nodiscard]] const Entry* find(const K& k) const noexcept {
        return const_cast<HashMap*>(this)->find(k);
    }

    // Insert; returns {Entry*, inserted}. Returns {nullptr, false} on OOM /
    // capacity-cap.
    template <class KK, class VV>
    [[nodiscard]] std::pair<Entry*, bool> insert(KK&& k, VV&& v) noexcept {
        if (cap_ == 0 && !rehash_to(kMinCap)) return {nullptr, false};
        if (needs_grow() && !rehash_to(cap_ * 2)) return {nullptr, false};
        return insert_unchecked(std::forward<KK>(k), std::forward<VV>(v));
    }

    // Walk every live entry in slot order, calling `fn(entry)` for each.
    // `fn` should accept `Entry&` or `const Entry&`. Skips empty +
    // tombstoned slots via the control byte. Iteration order is
    // hashmap-internal (the open-addressing layout); callers that need
    // a stable order sort the keys themselves.
    //
    // Bounded by cap_; no allocation; ≥2 assertions per Tiger Style.
    template <class Fn>
    void for_each(Fn&& fn) noexcept {
        assert(cap_ == 0 || ctrl_ != nullptr);
        assert(cap_ == 0 || entries_ != nullptr);
        for (std::size_t i = 0; i < cap_; ++i) {
            const std::uint8_t c = ctrl_[i];
            if (c == hashmap_detail::kCtrlEmpty) continue;
            if (c == hashmap_detail::kCtrlTombstone) continue;
            fn(entries_[i]);
        }
    }

    template <class Fn>
    void for_each(Fn&& fn) const noexcept {
        assert(cap_ == 0 || ctrl_ != nullptr);
        assert(cap_ == 0 || entries_ != nullptr);
        for (std::size_t i = 0; i < cap_; ++i) {
            const std::uint8_t c = ctrl_[i];
            if (c == hashmap_detail::kCtrlEmpty) continue;
            if (c == hashmap_detail::kCtrlTombstone) continue;
            fn(entries_[i]);
        }
    }

    // Erase by key; returns true if the key was present.
    bool erase(const K& k) noexcept {
        Entry* e = find(k);
        if (e == nullptr) return false;
        const std::size_t idx = static_cast<std::size_t>(e - entries_);
        e->key.~K();
        e->value.~V();
        ctrl_[idx] = hashmap_detail::kCtrlTombstone;
        --size_;
        ++tombstones_;
        return true;
    }

private:
    [[nodiscard]] bool needs_grow() const noexcept {
        // Grow when (size + tombstones) load factor exceeds the threshold —
        // tombstones must be reclaimed before the table degrades to linear.
        return static_cast<float>(size_ + tombstones_) >
               static_cast<float>(cap_) * kLoadFactor;
    }

    [[nodiscard]] bool rehash_to(std::size_t new_cap) noexcept {
        assert(new_cap >= kMinCap);
        assert((new_cap & (new_cap - 1)) == 0 && "new_cap must be power of two");
        if (new_cap > kMaxCap) return false;

        const std::size_t ctrl_bytes    = new_cap;
        const std::size_t entries_bytes = sizeof(Entry) * new_cap;

        auto* new_ctrl    = static_cast<uint8_t*>(alloc_->alloc(ctrl_bytes,    1));
        auto* new_entries = static_cast<Entry*>  (alloc_->alloc(entries_bytes, alignof(Entry)));
        if (new_ctrl == nullptr || new_entries == nullptr) return false;

        std::memset(new_ctrl, hashmap_detail::kCtrlEmpty, ctrl_bytes);

        // Move existing live entries into the new layout.
        const std::size_t old_cap     = cap_;
        uint8_t* const    old_ctrl    = ctrl_;
        Entry*   const    old_entries = entries_;

        ctrl_       = new_ctrl;
        entries_    = new_entries;
        cap_        = new_cap;
        size_       = 0;
        tombstones_ = 0;

        for (std::size_t i = 0; i < old_cap; ++i) {
            const uint8_t c = old_ctrl[i];
            if (c == hashmap_detail::kCtrlEmpty || c == hashmap_detail::kCtrlTombstone) continue;
            (void)insert_unchecked(std::move(old_entries[i].key),
                                   std::move(old_entries[i].value));
            old_entries[i].key.~K();
            old_entries[i].value.~V();
        }
        // Old storage stays in the arena; not freed.
        return true;
    }

    template <class KK, class VV>
    [[nodiscard]] std::pair<Entry*, bool> insert_unchecked(KK&& k, VV&& v) noexcept {
        assert(cap_ != 0);
        const uint64_t h   = static_cast<uint64_t>(hash_(k));
        const uint8_t  h2  = hashmap_detail::hash_h2(h);
        std::size_t    idx = static_cast<std::size_t>(h) & (cap_ - 1);
        std::size_t    first_tomb = static_cast<std::size_t>(-1);

        for (std::size_t step = 0; step < cap_; ++step) {
            const uint8_t c = ctrl_[idx];
            if (c == hashmap_detail::kCtrlEmpty) {
                // If we walked past a tombstone, prefer it (keeps probe length tight).
                const std::size_t target = first_tomb != static_cast<std::size_t>(-1)
                                         ? first_tomb : idx;
                if (target == idx) {}             // empty slot
                else                              { --tombstones_; }
                ctrl_[target] = h2;
                ::new (static_cast<void*>(&entries_[target].key  )) K(std::forward<KK>(k));
                ::new (static_cast<void*>(&entries_[target].value)) V(std::forward<VV>(v));
                ++size_;
                return {&entries_[target], true};
            }
            if (c == hashmap_detail::kCtrlTombstone) {
                if (first_tomb == static_cast<std::size_t>(-1)) first_tomb = idx;
            } else if (c == h2 && eq_(entries_[idx].key, k)) {
                return {&entries_[idx], false};
            }
            idx = (idx + 1) & (cap_ - 1);
        }
        // Full table; caller must rehash first. We assert at debug time.
        assert(false && "HashMap insert_unchecked walked the entire table");
        return {nullptr, false};
    }

    void destroy_all_entries() noexcept {
        if (ctrl_ == nullptr) return;
        for (std::size_t i = 0; i < cap_; ++i) {
            const uint8_t c = ctrl_[i];
            if (c == hashmap_detail::kCtrlEmpty || c == hashmap_detail::kCtrlTombstone) continue;
            entries_[i].key.~K();
            entries_[i].value.~V();
        }
    }

    A*          alloc_;
    uint8_t*    ctrl_;
    Entry*      entries_;
    std::size_t cap_;
    std::size_t size_;
    std::size_t tombstones_;
    Hash        hash_;
    Equal       eq_;
};

} // namespace ycpp
