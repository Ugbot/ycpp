// ycpp_unicode.h — minimal UTF-8 ↔ UTF-16 helpers.
//
// Yjs measures Y.Text length in UTF-16 code units (the JavaScript string
// `.length` semantics). On the wire the content is UTF-8 bytes. ycpp's
// per-Item `length` field must therefore be derived from the UTF-8
// payload at decode time so the per-client clock arithmetic matches what
// Yjs JS would compute locally.
//
// We do not store UTF-16 anywhere — just count code units. The mapping
// is fully determined by the UTF-8 lead byte:
//
//   0xxx_xxxx  →  1-byte UTF-8  → 1 UTF-16 code unit  (ASCII)
//   110x_xxxx  →  2-byte UTF-8  → 1 UTF-16 code unit  (Latin / BMP)
//   1110_xxxx  →  3-byte UTF-8  → 1 UTF-16 code unit  (BMP)
//   1111_0xxx  →  4-byte UTF-8  → 2 UTF-16 code units (surrogate pair)
//
// Continuation bytes (10xx_xxxx) are skipped — they don't start a new
// code unit. Truncated / invalid UTF-8 is treated leniently: each
// unrecognised byte counts as 1 unit, the walker advances by 1 byte.
// We never throw or fail; the worst case is a slightly-wrong length
// against a malformed input.

#pragma once

#include <cstddef>
#include <cstdint>

#include "ycpp_byteview.h"

namespace ycpp {

// Count the UTF-16 code units that decoding `bytes` (treated as UTF-8)
// would produce. Linear scan, no allocations, no exceptions.
[[nodiscard]] inline std::uint64_t utf16_length_of_utf8(ByteView bytes) noexcept {
    std::uint64_t units = 0;
    std::size_t   i     = 0;
    while (i < bytes.size) {
        const uint8_t b = bytes.data[i];
        if      ((b & 0x80U) == 0x00U) { ++units; i += 1; }    // 0xxxxxxx
        else if ((b & 0xE0U) == 0xC0U) { ++units; i += 2; }    // 110xxxxx
        else if ((b & 0xF0U) == 0xE0U) { ++units; i += 3; }    // 1110xxxx
        else if ((b & 0xF8U) == 0xF0U) { units += 2; i += 4; } // 11110xxx → surrogate pair
        else                            { ++units; i += 1; }    // invalid lead; lenient
    }
    return units;
}

// Byte offset that corresponds to UTF-16 code unit position `target`.
// Useful when splitting a kString Item at an interior CRDT clock.
//
// Returns `bytes.size` if `target` exceeds the string's UTF-16 length.
[[nodiscard]] inline std::size_t utf8_byte_offset_for_utf16_units(
    ByteView bytes, std::uint64_t target) noexcept {
    std::uint64_t units = 0;
    std::size_t   i     = 0;
    while (i < bytes.size && units < target) {
        const uint8_t b = bytes.data[i];
        if      ((b & 0x80U) == 0x00U) { ++units; i += 1; }
        else if ((b & 0xE0U) == 0xC0U) { ++units; i += 2; }
        else if ((b & 0xF0U) == 0xE0U) { ++units; i += 3; }
        else if ((b & 0xF8U) == 0xF0U) { units += 2; i += 4; }
        else                            { ++units; i += 1; }
    }
    return i > bytes.size ? bytes.size : i;
}

} // namespace ycpp
