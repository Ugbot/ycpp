// ycpp_id.h — Lamport identifier (client, clock).
//
// Every CRDT struct in a ycpp Doc carries an Id. `client` is the peer's
// random 64-bit identifier (assigned at Doc construction); `clock` is the
// monotonic per-client counter that starts at 0 and bumps by the length
// of every emitted Item. The pair (client, clock) is globally unique by
// construction.
//
// kInvalidId is the sentinel for "no origin" / "absent reference".

#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>

namespace ycpp {

struct Id {
    uint64_t client;
    uint64_t clock;

    constexpr Id() noexcept : client(0), clock(0) {}
    constexpr Id(uint64_t c, uint64_t k) noexcept : client(c), clock(k) {}

    [[nodiscard]] constexpr bool operator==(const Id& rhs) const noexcept {
        return client == rhs.client && clock == rhs.clock;
    }
    [[nodiscard]] constexpr bool operator!=(const Id& rhs) const noexcept {
        return !(*this == rhs);
    }

    // Lexicographic by (client, clock). The total order ycpp imposes on Ids
    // is used by the integration tie-break in W3.
    [[nodiscard]] constexpr bool operator<(const Id& rhs) const noexcept {
        if (client != rhs.client) return client < rhs.client;
        return clock < rhs.clock;
    }
};

inline constexpr Id kInvalidId{
    std::numeric_limits<uint64_t>::max(),
    std::numeric_limits<uint64_t>::max(),
};

[[nodiscard]] constexpr bool is_valid(Id id) noexcept {
    return id != kInvalidId;
}

// Returns the clock immediately after `id`'s last bit — the place a new
// Item from the same client would start. ycpp Items are length-1 today;
// length-N runs (W6 compaction) will pass the run length here.
[[nodiscard]] constexpr Id id_after(Id id, uint64_t len = 1) noexcept {
    assert(is_valid(id));
    assert(len > 0);
    return Id{ id.client, id.clock + len };
}

// FNV-1a 64-bit mix of the two 64-bit halves. Good enough hash for the
// HashMap; we don't need cryptographic strength here. Branchless.
struct IdHash {
    [[nodiscard]] constexpr std::size_t operator()(Id id) const noexcept {
        // Mix client and clock via xor-rotate. Avoids hash collisions when
        // client_id is uniform (random per peer) and clock is small.
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t h = 0xcbf29ce484222325ULL;
        h = (h ^ id.client) * kPrime;
        h = (h ^ id.clock)  * kPrime;
        return static_cast<std::size_t>(h);
    }
};

struct IdEq {
    [[nodiscard]] constexpr bool operator()(Id a, Id b) const noexcept {
        return a == b;
    }
};

static_assert(sizeof(Id) == 16,                                "Id must pack as two u64s");
static_assert(std::is_trivially_copyable_v<Id>,                "Id must be trivially copyable");

} // namespace ycpp
