// ycpp_awareness.h — per-client ephemeral state map (presence / cursors).
//
// Awareness is a small CRDT distinct from the document: each peer publishes
// an opaque payload (their presence record, cursor position, current tool,
// etc.) tagged with a monotonic clock per peer. Higher clock wins; remote
// updates that don't increase a peer's clock are dropped.
//
// The Awareness map is held in memory only — it's intentionally NOT
// persisted in the Doc. Peers republish their state on reconnect.
//
// Wire format (payload of a kAwarenessUpdate envelope):
//
//   numClients : varint_u64
//   for each:
//     client_id : varint_u64
//     clock     : varint_u64
//     state     : length-prefixed bytes (opaque to ycpp)
//
// To remove a peer (signaling "I went offline"), publish an empty `state`
// at a higher clock. Receivers see size == 0 and treat the entry as gone.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_hashmap.h"
#include "ycpp_reader.h"
#include "ycpp_status.h"
#include "ycpp_writer.h"

namespace ycpp {

template <Allocator A>
class Awareness {
public:
    static constexpr std::size_t kMaxClients = 4096;
    static constexpr std::size_t kMaxStateBytes = 1U << 16;  // 64 KiB cap per peer

    struct Entry {
        uint64_t clock;
        ByteView state;  // empty = peer offline
    };

    explicit Awareness(A* alloc) noexcept
        : alloc_(alloc), entries_(alloc), client_order_size_(0) {
        assert(alloc != nullptr);
    }

    Awareness(const Awareness&)            = delete;
    Awareness& operator=(const Awareness&) = delete;
    Awareness(Awareness&&)                 = delete;
    Awareness& operator=(Awareness&&)      = delete;

    ~Awareness() noexcept = default;

    // Publish (or update) this peer's state. The local peer is the only
    // one allowed to call this for its own client_id; the API doesn't
    // enforce that — caller is responsible. Auto-increments the clock.
    [[nodiscard]] Status publish(uint64_t client_id, ByteView state) noexcept {
        if (state.size > kMaxStateBytes) return Status::kCapacityExceeded;
        ByteView owned = clone_view(state);
        if (owned.data == nullptr && state.size != 0) return Status::kOutOfMemory;

        auto* hit = entries_.find(client_id);
        if (hit != nullptr) {
            hit->value.clock += 1;
            hit->value.state  = owned;
            return Status::kOk;
        }
        if (client_order_size_ >= kMaxClients) return Status::kCapacityExceeded;
        auto [entry, inserted] = entries_.insert(client_id, Entry{1, owned});
        if (!inserted) return Status::kOutOfMemory;
        client_order_[client_order_size_++] = client_id;
        return Status::kOk;
    }

    // Apply a remote update (typically the payload of a kAwarenessUpdate
    // envelope). Only entries with a strictly higher clock for a known
    // client overwrite the local view; lower-clock entries are dropped.
    [[nodiscard]] Status apply(ByteView wire) noexcept {
        Reader r{wire};
        uint64_t n = 0;
        YCPP_TRY(r.varint_u64(&n));
        if (n > kMaxClients) return Status::kCapacityExceeded;
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t client = 0, clock = 0;
            ByteView state{};
            YCPP_TRY(r.varint_u64(&client));
            YCPP_TRY(r.varint_u64(&clock));
            YCPP_TRY(r.length_prefixed(&state));
            YCPP_TRY(merge_remote(client, clock, state));
        }
        if (!r.eof()) return Status::kCorruptInput;
        return Status::kOk;
    }

    // Encode the entire local view into `w`. Used to bootstrap a fresh
    // peer who just connected.
    [[nodiscard]] Status encode_all(Writer& w) const noexcept {
        YCPP_TRY(w.varint_u64(static_cast<uint64_t>(client_order_size_)));
        for (std::size_t i = 0; i < client_order_size_; ++i) {
            const uint64_t cid = client_order_[i];
            const auto* hit = entries_.find(cid);
            assert(hit != nullptr);
            YCPP_TRY(w.varint_u64(cid));
            YCPP_TRY(w.varint_u64(hit->value.clock));
            YCPP_TRY(w.length_prefixed(hit->value.state.data, hit->value.state.size));
        }
        return Status::kOk;
    }

    // Look up a peer's current state (or nullptr if unknown / offline).
    [[nodiscard]] const Entry* get(uint64_t client_id) const noexcept {
        const auto* hit = entries_.find(client_id);
        return hit != nullptr ? &hit->value : nullptr;
    }

    [[nodiscard]] std::size_t client_count() const noexcept { return client_order_size_; }

    template <class Fn>
    void for_each(Fn&& fn) const noexcept {
        for (std::size_t i = 0; i < client_order_size_; ++i) {
            const uint64_t cid = client_order_[i];
            const auto* hit = entries_.find(cid);
            assert(hit != nullptr);
            fn(cid, hit->value);
        }
    }

private:
    [[nodiscard]] Status merge_remote(uint64_t client_id, uint64_t clock,
                                       ByteView state) noexcept {
        auto* hit = entries_.find(client_id);
        if (hit != nullptr) {
            if (clock <= hit->value.clock) return Status::kOk;  // stale
            ByteView owned = clone_view(state);
            if (owned.data == nullptr && state.size != 0) return Status::kOutOfMemory;
            hit->value.clock = clock;
            hit->value.state = owned;
            return Status::kOk;
        }
        if (client_order_size_ >= kMaxClients) return Status::kCapacityExceeded;
        ByteView owned = clone_view(state);
        if (owned.data == nullptr && state.size != 0) return Status::kOutOfMemory;
        auto [entry, inserted] = entries_.insert(client_id, Entry{clock, owned});
        if (!inserted) return Status::kOutOfMemory;
        client_order_[client_order_size_++] = client_id;
        return Status::kOk;
    }

    [[nodiscard]] ByteView clone_view(ByteView v) noexcept {
        if (v.size == 0) return ByteView{};
        auto* dst = static_cast<uint8_t*>(alloc_->alloc(v.size, 1));
        if (dst == nullptr) return ByteView{};
        std::memcpy(dst, v.data, v.size);
        return ByteView{dst, v.size};
    }

    A*                              alloc_;
    HashMap<uint64_t, Entry, A>     entries_;
    uint64_t                        client_order_[kMaxClients]{};
    std::size_t                     client_order_size_;
};

} // namespace ycpp
