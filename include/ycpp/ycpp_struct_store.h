// ycpp_struct_store.h — per-client sorted vector of Items.
//
// Every CRDT struct in a Doc lives in this store, keyed by `client`. The
// per-client vector is sorted by `id.clock` so a lookup is a binary search.
// State-vector queries (the next clock per client) come from the tail of
// each vector.
//
// `Item*` ownership lives in the Doc's pool (W3). The store just holds
// stable pointers — the pool guarantees Item addresses are pinned for the
// Doc's lifetime.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ycpp_arena.h"
#include "ycpp_hashmap.h"
#include "ycpp_id.h"
#include "ycpp_item.h"
#include "ycpp_status.h"

namespace ycpp {

template <Allocator A>
class StructStore {
public:
    static constexpr std::size_t kInitialPerClientCap = 8;
    static constexpr std::size_t kMaxPerClientItems   = 1U << 24;  // 16M items/client cap
    static constexpr std::size_t kMaxClients          = 4096;

    explicit StructStore(A* alloc) noexcept
        : alloc_(alloc), per_client_(alloc) {
        assert(alloc != nullptr);
    }

    StructStore(const StructStore&)            = delete;
    StructStore& operator=(const StructStore&) = delete;
    StructStore(StructStore&&)                 = delete;
    StructStore& operator=(StructStore&&)      = delete;

    ~StructStore() noexcept = default;

    // Append the Item at the tail of its client's vector. The Item's clock
    // MUST equal the current next-clock for that client (i.e. dense,
    // monotonic) — appending in-order is the load-bearing invariant. Out-of-
    // order inserts come back as kPendingReference and queue at the integration
    // layer (W3).
    [[nodiscard]] Status append(Item* item) noexcept {
        assert(item != nullptr);
        ClientVec* vec = get_or_create(item->id.client);
        if (vec == nullptr) return Status::kOutOfMemory;
        const uint64_t expected_clock = (vec->size == 0)
            ? 0
            : id_after(vec->data[vec->size - 1]->id,
                       vec->data[vec->size - 1]->length).clock;
        if (item->id.clock != expected_clock) return Status::kPendingReference;
        YCPP_TRY(ensure_cap(vec, vec->size + 1));
        vec->data[vec->size++] = item;
        return Status::kOk;
    }

    [[nodiscard]] Item* find_by_id(Id id) noexcept {
        ClientVec* vec = find_vec(id.client);
        if (vec == nullptr) return nullptr;
        // Binary search for the Item whose [clock, clock+length) contains id.clock.
        std::size_t lo = 0, hi = vec->size;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            const Item* it = vec->data[mid];
            if (it->id.clock <= id.clock) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) return nullptr;
        Item* it = vec->data[lo - 1];
        const uint64_t end = it->id.clock + (it->length == 0 ? 1 : it->length);
        return id.clock < end ? it : nullptr;
    }

    [[nodiscard]] const Item* find_by_id(Id id) const noexcept {
        return const_cast<StructStore*>(this)->find_by_id(id);
    }

    // The "next clock to be emitted" for a client — i.e. the state-vector
    // entry. Returns 0 for unknown clients.
    [[nodiscard]] uint64_t state(uint64_t client) const noexcept {
        const ClientVec* vec = find_vec_const(client);
        if (vec == nullptr || vec->size == 0) return 0;
        const Item* tail = vec->data[vec->size - 1];
        return tail->id.clock + (tail->length == 0 ? 1 : tail->length);
    }

    // Iteration in insertion order over the known clients.
    template <class Fn>
    void for_each_client(Fn&& fn) const noexcept {
        for (std::size_t i = 0; i < client_order_size_; ++i) {
            const uint64_t client = client_order_[i];
            const ClientVec* vec  = find_vec_const(client);
            assert(vec != nullptr);
            fn(client, const_cast<const Item* const*>(vec->data), vec->size);
        }
    }

    [[nodiscard]] std::size_t client_count() const noexcept { return client_order_size_; }

    // Total item count across all clients. Linear scan; intended for tests
    // + diagnostics, not the hot path.
    [[nodiscard]] std::size_t total_items() const noexcept {
        std::size_t total = 0;
        for (std::size_t i = 0; i < client_order_size_; ++i) {
            const ClientVec* vec = find_vec_const(client_order_[i]);
            if (vec != nullptr) total += vec->size;
        }
        return total;
    }

private:
    struct ClientVec {
        Item**      data;
        std::size_t size;
        std::size_t cap;
    };

    A* alloc_;
    HashMap<uint64_t, ClientVec, A> per_client_;
    uint64_t                        client_order_[kMaxClients]{};
    std::size_t                     client_order_size_ = 0;

    [[nodiscard]] ClientVec* get_or_create(uint64_t client) noexcept {
        auto* hit = per_client_.find(client);
        if (hit != nullptr) return &hit->value;
        if (client_order_size_ >= kMaxClients) return nullptr;
        ClientVec init{nullptr, 0, 0};
        auto [entry, inserted] = per_client_.insert(client, init);
        if (!inserted) return nullptr;
        client_order_[client_order_size_++] = client;
        return &entry->value;
    }

    [[nodiscard]] ClientVec* find_vec(uint64_t client) noexcept {
        auto* hit = per_client_.find(client);
        return hit != nullptr ? &hit->value : nullptr;
    }

    [[nodiscard]] const ClientVec* find_vec_const(uint64_t client) const noexcept {
        const auto* hit = per_client_.find(client);
        return hit != nullptr ? &hit->value : nullptr;
    }

    [[nodiscard]] Status ensure_cap(ClientVec* vec, std::size_t need) noexcept {
        if (need <= vec->cap) return Status::kOk;
        if (need > kMaxPerClientItems) return Status::kCapacityExceeded;
        std::size_t new_cap = vec->cap == 0 ? kInitialPerClientCap : vec->cap * 2;
        while (new_cap < need) new_cap *= 2;
        if (new_cap > kMaxPerClientItems) new_cap = kMaxPerClientItems;
        auto* fresh = static_cast<Item**>(alloc_->alloc(
            sizeof(Item*) * new_cap, alignof(Item*)));
        if (fresh == nullptr) return Status::kOutOfMemory;
        if (vec->size != 0) {
            std::memcpy(fresh, vec->data, vec->size * sizeof(Item*));
        }
        vec->data = fresh;
        vec->cap  = new_cap;
        return Status::kOk;
    }
};

} // namespace ycpp
