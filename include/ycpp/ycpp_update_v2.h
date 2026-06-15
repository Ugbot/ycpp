// ycpp_update_v2.h — Yjs updateV2 surface (stubs).
//
// V2 is Yjs's compressed wire format: multiple parallel encoder streams
// (RLE on client IDs, integer-diff RLE on clocks, optional-RLE on
// lengths, RLE on info bytes, separate string pool). It produces 30-50%
// smaller updates than v1 on typical workloads.
//
// ycpp 0.0.4 ships the public API at this header but the bodies return
// `kUnsupportedFormat` — implementation lands in 0.1.x. Callers can:
//
//   - Compile-time depend on the symbol shape.
//   - Receive a clear, structured failure when they pass v2 bytes; the
//     fallback in most Yjs JS clients (`encodeStateAsUpdate`) already
//     emits v1.
//
// Migration to v2 is opt-in inside Yjs JS (`encodeStateAsUpdateV2`); the
// ycpp wire path stays v1-only until 0.1.x.

#pragma once

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_doc.h"
#include "ycpp_state_vector.h"
#include "ycpp_status.h"
#include "ycpp_writer.h"

namespace ycpp {

// Apply a Yjs updateV2 binary blob. Returns kUnsupportedFormat in
// 0.0.4 (decoder body TBD in 0.1.x).
template <Allocator A>
[[nodiscard]] inline Status apply_update_v2(Doc<A>& /*dst*/,
                                             ByteView /*bytes*/) noexcept {
    return Status::kUnsupportedFormat;
}

// Encode a v2 diff (since the peer's state vector). Returns
// kUnsupportedFormat in 0.0.4.
template <Allocator A>
[[nodiscard]] inline Status encode_diff_v2(const Doc<A>&         /*doc*/,
                                            const StateVector<A>* /*since*/,
                                            Writer*               /*out*/) noexcept {
    return Status::kUnsupportedFormat;
}

} // namespace ycpp
