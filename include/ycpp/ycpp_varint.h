// ycpp_varint.h — Yjs-compatible variable-length integer codec.
//
// Yjs uses LEB128 (unsigned) and zigzag-then-LEB128 (signed). Each varint
// is at most 10 bytes for a 64-bit value (continuation bit drains 7 data
// bits per byte). Both encode and decode are branch-light bounded loops.
//
// All entry points are noexcept and bounded by `kMaxVarintBytes` so a
// hostile/corrupt input cannot run the parser unbounded.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_status.h"

namespace ycpp {

inline constexpr size_t kMaxVarintBytes = 10;  // ⌈64 / 7⌉

// ----- unsigned ------------------------------------------------------------

// Encode `v` into `out[0..cap)`. Writes `*written` bytes on success.
// Returns kOutOfBounds when `cap < bytes_required(v)`. Bounded ≤10 iters.
[[nodiscard]] inline Status varint_encode_u64(uint64_t v, uint8_t* out, size_t cap,
                                              size_t* written) noexcept {
    assert(out     != nullptr);
    assert(written != nullptr);
    size_t i = 0;
    for (; i < kMaxVarintBytes; ++i) {
        const uint8_t low7 = static_cast<uint8_t>(v & 0x7FU);
        v >>= 7;
        if (v == 0) {
            if (i >= cap) return Status::kOutOfBounds;
            out[i] = low7;
            *written = i + 1;
            return Status::kOk;
        }
        if (i >= cap) return Status::kOutOfBounds;
        out[i] = static_cast<uint8_t>(low7 | 0x80U);
    }
    // unreachable: u64 fits in ≤10 bytes by construction
    assert(false && "varint_encode_u64 ran past kMaxVarintBytes");
    return Status::kInternal;
}

// Decode a varint at `in[0..len)`. Writes value to `*out` and bytes consumed
// to `*consumed`. Returns kOutOfBounds when the stream ends mid-varint and
// kCorruptInput when the encoded value overflows u64.
[[nodiscard]] inline Status varint_decode_u64(const uint8_t* in, size_t len,
                                              uint64_t* out, size_t* consumed) noexcept {
    assert(in       != nullptr || len == 0);
    assert(out      != nullptr);
    assert(consumed != nullptr);
    uint64_t acc   = 0;
    uint32_t shift = 0;
    for (size_t i = 0; i < kMaxVarintBytes; ++i) {
        if (i >= len) return Status::kOutOfBounds;
        const uint8_t b = in[i];
        // The 10th byte (i==9) of a u64 varint carries only 1 data bit;
        // upper bits MUST be zero or the value overflows u64.
        if (i == kMaxVarintBytes - 1 && (b & 0x7EU) != 0) {
            return Status::kCorruptInput;
        }
        acc   |= static_cast<uint64_t>(b & 0x7FU) << shift;
        shift += 7;
        if ((b & 0x80U) == 0) {
            *out      = acc;
            *consumed = i + 1;
            return Status::kOk;
        }
    }
    return Status::kCorruptInput;  // continuation bit never cleared
}

[[nodiscard]] inline size_t varint_size_u64(uint64_t v) noexcept {
    size_t n = 1;
    while (v >= 0x80U) { v >>= 7; ++n; }
    return n;
}

// ----- signed (zigzag) ------------------------------------------------------

[[nodiscard]] inline constexpr uint64_t zigzag_encode_i64(int64_t v) noexcept {
    // (v << 1) ^ (v >> 63) — branchless map to u64.
    return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}
[[nodiscard]] inline constexpr int64_t zigzag_decode_i64(uint64_t v) noexcept {
    return static_cast<int64_t>((v >> 1) ^ (~(v & 1U) + 1U));
}

[[nodiscard]] inline Status varint_encode_i64(int64_t v, uint8_t* out, size_t cap,
                                              size_t* written) noexcept {
    assert(out     != nullptr);
    assert(written != nullptr);
    return varint_encode_u64(zigzag_encode_i64(v), out, cap, written);
}

[[nodiscard]] inline Status varint_decode_i64(const uint8_t* in, size_t len,
                                              int64_t* out, size_t* consumed) noexcept {
    assert(in       != nullptr || len == 0);
    assert(out      != nullptr);
    assert(consumed != nullptr);
    uint64_t u = 0;
    const Status s = varint_decode_u64(in, len, &u, consumed);
    if (s != Status::kOk) return s;
    *out = zigzag_decode_i64(u);
    return Status::kOk;
}

[[nodiscard]] inline size_t varint_size_i64(int64_t v) noexcept {
    return varint_size_u64(zigzag_encode_i64(v));
}

} // namespace ycpp
