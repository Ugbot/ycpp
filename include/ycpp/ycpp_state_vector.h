// ycpp_state_vector.h — sparse client→clock vector.
//
// A StateVector summarises "for each client I've heard of, the next clock
// I'd expect from them." Peers exchange state vectors to compute the
// minimal patch they're missing from each other (see encode_state_as_update).
//
// Wire format (Yjs SV v1):
//   numClients : varint_u64
//   for each client:
//     client : varint_u64
//     clock  : varint_u64

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_arena.h"
#include "ycpp_hashmap.h"
#include "ycpp_id.h"
#include "ycpp_reader.h"
#include "ycpp_status.h"
#include "ycpp_writer.h"

namespace ycpp {

template <Allocator A>
class StateVector {
public:
    static constexpr std::size_t kMaxClients = 4096;

    explicit StateVector(A* alloc) noexcept
        : alloc_(alloc), per_client_(alloc), client_order_size_(0) {
        assert(alloc != nullptr);
    }

    StateVector(const StateVector&)            = delete;
    StateVector& operator=(const StateVector&) = delete;
    StateVector(StateVector&&)                 = delete;
    StateVector& operator=(StateVector&&)      = delete;

    ~StateVector() noexcept = default;

    // Set the clock for a client. Inserts on first sight; updates otherwise.
    [[nodiscard]] Status set(uint64_t client, uint64_t clock) noexcept {
        auto* hit = per_client_.find(client);
        if (hit != nullptr) {
            hit->value = clock;
            return Status::kOk;
        }
        if (client_order_size_ >= kMaxClients) return Status::kCapacityExceeded;
        auto [entry, inserted] = per_client_.insert(client, clock);
        if (!inserted) return Status::kOutOfMemory;
        client_order_[client_order_size_++] = client;
        return Status::kOk;
    }

    // Returns the recorded clock, or 0 if unknown.
    [[nodiscard]] uint64_t get(uint64_t client) const noexcept {
        const auto* hit = per_client_.find(client);
        return hit != nullptr ? hit->value : 0;
    }

    [[nodiscard]] std::size_t client_count() const noexcept { return client_order_size_; }

    template <class Fn>
    void for_each(Fn&& fn) const noexcept {
        for (std::size_t i = 0; i < client_order_size_; ++i) {
            const uint64_t client = client_order_[i];
            const auto* hit = per_client_.find(client);
            assert(hit != nullptr);
            fn(client, hit->value);
        }
    }

    [[nodiscard]] Status encode(Writer& w) const noexcept {
        YCPP_TRY(w.varint_u64(static_cast<uint64_t>(client_order_size_)));
        for (std::size_t i = 0; i < client_order_size_; ++i) {
            const uint64_t client = client_order_[i];
            const auto* hit = per_client_.find(client);
            assert(hit != nullptr);
            YCPP_TRY(w.varint_u64(client));
            YCPP_TRY(w.varint_u64(hit->value));
        }
        return Status::kOk;
    }

    [[nodiscard]] Status decode(Reader& r) noexcept {
        uint64_t n = 0;
        YCPP_TRY(r.varint_u64(&n));
        if (n > kMaxClients) return Status::kCapacityExceeded;
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t client = 0;
            uint64_t clock  = 0;
            YCPP_TRY(r.varint_u64(&client));
            YCPP_TRY(r.varint_u64(&clock));
            YCPP_TRY(set(client, clock));
        }
        return Status::kOk;
    }

private:
    A*                              alloc_;
    HashMap<uint64_t, uint64_t, A>  per_client_;
    uint64_t                        client_order_[kMaxClients]{};
    std::size_t                     client_order_size_;
};

} // namespace ycpp
