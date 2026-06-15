// ycpp_status.h — uniform return type for every fallible operation.
//
// ycpp does not throw across the public API. Every non-void function returns
// either `Status` or a `T` paired with a `Status` out-param. The status
// values are intentionally small (a u16 fits in a register) and cover the
// failure cases the algorithm + wire formats actually produce.

#pragma once

#include <cstdint>

namespace ycpp {

enum class Status : uint16_t {
    kOk                = 0,
    kInvalidArgument   = 1,  // caller passed something nonsensical
    kOutOfBounds       = 2,  // reader/writer underflow or overflow
    kCorruptInput      = 3,  // wire format malformed (length lies, bad tag, etc.)
    kUnsupportedFormat = 4,  // a version / tag we don't speak
    kOutOfMemory       = 5,  // allocator returned nullptr
    kCapacityExceeded  = 6,  // a bounded structure (pool, ring, sv) is full
    kNotFound          = 7,  // lookup miss
    kAlreadyExists     = 8,  // unique-insert conflict
    kPendingReference  = 9,  // structural dependency not yet integrated
    kIntegrationFailed = 10, // CRDT integration could not splice (unexpected)
    kInternal          = 11, // invariant broken (programmer error surfaced)
};

[[nodiscard]] constexpr bool is_ok(Status s) noexcept {
    return s == Status::kOk;
}

[[nodiscard]] constexpr const char* status_name(Status s) noexcept {
    switch (s) {
        case Status::kOk:                return "ok";
        case Status::kInvalidArgument:   return "invalid_argument";
        case Status::kOutOfBounds:       return "out_of_bounds";
        case Status::kCorruptInput:      return "corrupt_input";
        case Status::kUnsupportedFormat: return "unsupported_format";
        case Status::kOutOfMemory:       return "out_of_memory";
        case Status::kCapacityExceeded:  return "capacity_exceeded";
        case Status::kNotFound:          return "not_found";
        case Status::kAlreadyExists:     return "already_exists";
        case Status::kPendingReference:  return "pending_reference";
        case Status::kIntegrationFailed: return "integration_failed";
        case Status::kInternal:          return "internal";
    }
    return "unknown";
}

// One-line wrapper that mirrors the bolt style: prefer YCPP_TRY over manual
// `if (auto s = …; !is_ok(s)) return s;` chains.
#define YCPP_TRY(expr)                                                 \
    do {                                                              \
        const ::ycpp::Status _ycpp_s_ = (expr);                         \
        if (_ycpp_s_ != ::ycpp::Status::kOk) return _ycpp_s_;            \
    } while (0)

} // namespace ycpp
