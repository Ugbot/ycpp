// ycpp_delete_set.h — RLE per-client delete ranges.
//
// A DeleteSet records "these clock ranges of these clients are deleted."
// Yjs's wire format serialises it after the struct section. ycpp keeps the
// in-memory representation as a hashmap from client id -> sorted vector
// of (clock_start, length) ranges. New inserts merge with adjacent ranges
// at add() time so the structure stays compact under heavy deletion.
//
// Lookup via `contains(Id)` is O(log n) per client — binary search by
// clock_start. We never grow the vector dynamically per insert: each
// per-client vector is allocator-backed and doubles on demand.
//
// Templated on the Allocator policy (consistent with the rest of ycpp).

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ycpp_arena.h"
#include "ycpp_hashmap.h"
#include "ycpp_id.h"
#include "ycpp_reader.h"
#include "ycpp_status.h"
#include "ycpp_writer.h"

namespace ycpp {

struct DeleteRange {
    uint64_t clock_start;
    uint64_t length;

    [[nodiscard]] constexpr uint64_t clock_end_exclusive() const noexcept {
        return clock_start + length;
    }
};

template <Allocator A>
class DeleteSet {
public:
    static constexpr std::size_t kInitialPerClientCap = 4;
    static constexpr std::size_t kMaxPerClientRanges  = 1U << 20;  // 1M ranges per client cap

    explicit DeleteSet(A* alloc) noexcept
        : alloc_(alloc), per_client_(alloc) {
        assert(alloc != nullptr);
    }

    DeleteSet(const DeleteSet&)            = delete;
    DeleteSet& operator=(const DeleteSet&) = delete;
    DeleteSet(DeleteSet&&)                 = delete;
    DeleteSet& operator=(DeleteSet&&)      = delete;

    ~DeleteSet() noexcept = default;  // storage owned by arena

    // Add a [clock_start, clock_start + length) range to the client's vector.
    // Merges with adjacent / overlapping ranges so the vector stays compact.
    // Returns kCapacityExceeded if the per-client vector would exceed the cap.
    [[nodiscard]] Status add(uint64_t client, uint64_t clock_start,
                             uint64_t length) noexcept {
        assert(length > 0);
        ClientVec* vec = get_or_create(client);
        if (vec == nullptr) return Status::kOutOfMemory;
        return insert_range(vec, clock_start, length);
    }

    [[nodiscard]] bool contains(Id id) const noexcept {
        const ClientVec* vec = find(id.client);
        if (vec == nullptr) return false;
        // Binary-search for the range whose clock_start ≤ id.clock; check end.
        std::size_t lo = 0, hi = vec->size;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            const auto& r = vec->data[mid];
            if (r.clock_start <= id.clock) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) return false;
        const auto& r = vec->data[lo - 1];
        return id.clock < r.clock_end_exclusive();
    }

    // Iteration for encoders + tests. Visits each non-empty client in
    // insertion order; per-client ranges are sorted by clock_start.
    template <class Fn>
    void for_each_client(Fn&& fn) const noexcept {
        for (std::size_t i = 0; i < client_order_size_; ++i) {
            const uint64_t client = client_order_[i];
            const ClientVec* vec = find(client);
            assert(vec != nullptr);
            fn(client, vec->data, vec->size);
        }
    }

    [[nodiscard]] std::size_t client_count() const noexcept { return client_order_size_; }

    // Wire-format encoders / decoders (Yjs updateV1 DeleteSet shape):
    //   numClients : varint_u64
    //   per client:
    //     client      : varint_u64
    //     numRanges   : varint_u64
    //     for each range:
    //       clock     : varint_u64
    //       length    : varint_u64
    [[nodiscard]] Status decode(Reader& r) noexcept {
        uint64_t n_clients = 0;
        YCPP_TRY(r.varint_u64(&n_clients));
        if (n_clients > kMaxPerClientRanges) return Status::kCapacityExceeded;
        for (uint64_t ci = 0; ci < n_clients; ++ci) {
            uint64_t client = 0;
            uint64_t n_ranges = 0;
            YCPP_TRY(r.varint_u64(&client));
            YCPP_TRY(r.varint_u64(&n_ranges));
            if (n_ranges > kMaxPerClientRanges) return Status::kCapacityExceeded;
            for (uint64_t ri = 0; ri < n_ranges; ++ri) {
                uint64_t clk = 0, len = 0;
                YCPP_TRY(r.varint_u64(&clk));
                YCPP_TRY(r.varint_u64(&len));
                if (len == 0) return Status::kCorruptInput;
                YCPP_TRY(add(client, clk, len));
            }
        }
        return Status::kOk;
    }

    [[nodiscard]] Status encode(Writer& w) const noexcept {
        YCPP_TRY(w.varint_u64(static_cast<uint64_t>(client_order_size_)));
        for (std::size_t i = 0; i < client_order_size_; ++i) {
            const uint64_t client = client_order_[i];
            const ClientVec* vec  = find(client);
            assert(vec != nullptr);
            YCPP_TRY(w.varint_u64(client));
            YCPP_TRY(w.varint_u64(static_cast<uint64_t>(vec->size)));
            for (std::size_t j = 0; j < vec->size; ++j) {
                YCPP_TRY(w.varint_u64(vec->data[j].clock_start));
                YCPP_TRY(w.varint_u64(vec->data[j].length));
            }
        }
        return Status::kOk;
    }

private:
    struct ClientVec {
        DeleteRange* data;
        std::size_t  size;
        std::size_t  cap;
    };

    A* alloc_;
    HashMap<uint64_t, ClientVec, A> per_client_;

    // Insertion-order iteration; capped at 4096 clients (matches StructStore).
    static constexpr std::size_t kMaxClients = 4096;
    uint64_t                     client_order_[kMaxClients]{};
    std::size_t                  client_order_size_ = 0;

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

    [[nodiscard]] const ClientVec* find(uint64_t client) const noexcept {
        const auto* hit = per_client_.find(client);
        return hit != nullptr ? &hit->value : nullptr;
    }

    [[nodiscard]] Status insert_range(ClientVec* vec, uint64_t clock,
                                       uint64_t length) noexcept {
        assert(vec    != nullptr);
        assert(length != 0);
        // Find the insertion point by binary search on clock_start.
        std::size_t lo = 0, hi = vec->size;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (vec->data[mid].clock_start < clock) lo = mid + 1; else hi = mid;
        }
        const std::size_t pos = lo;
        const uint64_t end = clock + length;

        // Merge with predecessor if it touches/overlaps.
        if (pos > 0) {
            DeleteRange& prev = vec->data[pos - 1];
            if (prev.clock_end_exclusive() >= clock) {
                const uint64_t new_end = end > prev.clock_end_exclusive()
                                       ? end : prev.clock_end_exclusive();
                prev.length = new_end - prev.clock_start;
                return merge_forward(vec, pos - 1);
            }
        }
        // Insert new range, then try to merge forward.
        YCPP_TRY(ensure_cap(vec, vec->size + 1));
        if (pos < vec->size) {
            std::memmove(vec->data + pos + 1, vec->data + pos,
                         (vec->size - pos) * sizeof(DeleteRange));
        }
        vec->data[pos] = DeleteRange{clock, length};
        ++vec->size;
        return merge_forward(vec, pos);
    }

    [[nodiscard]] Status merge_forward(ClientVec* vec, std::size_t pos) noexcept {
        while (pos + 1 < vec->size) {
            DeleteRange& cur  = vec->data[pos];
            DeleteRange& next = vec->data[pos + 1];
            if (cur.clock_end_exclusive() < next.clock_start) break;
            const uint64_t new_end = cur.clock_end_exclusive() >= next.clock_end_exclusive()
                                   ? cur.clock_end_exclusive()
                                   : next.clock_end_exclusive();
            cur.length = new_end - cur.clock_start;
            // Shift the tail down by one.
            if (pos + 2 < vec->size) {
                std::memmove(vec->data + pos + 1, vec->data + pos + 2,
                             (vec->size - pos - 2) * sizeof(DeleteRange));
            }
            --vec->size;
        }
        return Status::kOk;
    }

    [[nodiscard]] Status ensure_cap(ClientVec* vec, std::size_t need) noexcept {
        if (need <= vec->cap) return Status::kOk;
        if (need > kMaxPerClientRanges) return Status::kCapacityExceeded;
        std::size_t new_cap = vec->cap == 0 ? kInitialPerClientCap : vec->cap * 2;
        while (new_cap < need) new_cap *= 2;
        if (new_cap > kMaxPerClientRanges) new_cap = kMaxPerClientRanges;
        auto* fresh = static_cast<DeleteRange*>(alloc_->alloc(
            sizeof(DeleteRange) * new_cap, alignof(DeleteRange)));
        if (fresh == nullptr) return Status::kOutOfMemory;
        if (vec->size != 0) {
            std::memcpy(fresh, vec->data, vec->size * sizeof(DeleteRange));
        }
        // Old vec->data is left in the arena; no per-object free.
        vec->data = fresh;
        vec->cap  = new_cap;
        return Status::kOk;
    }
};

} // namespace ycpp
