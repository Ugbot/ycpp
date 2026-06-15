// ycpp_protocol.h — Yjs-style sync protocol over the ycpp envelope.
//
// Two peers reconcile their Docs by exchanging three message kinds:
//
//   1. kSyncStep1   — initiator emits its StateVector, asking
//                     "what am I missing?"
//   2. kSyncStep2   — responder emits an updateV1 containing only the
//                     structs the initiator's SV didn't cover.
//   3. kSyncUpdate  — either peer broadcasts a fresh local edit as it
//                     happens (also an updateV1).
//
// A typical session:
//
//   peer A sends    kSyncStep1{SV_A}
//   peer B replies  kSyncStep2{diff(A_sv)} + kSyncStep1{SV_B}
//   peer A replies  kSyncStep2{diff(B_sv)}
//   ... thereafter both peers broadcast kSyncUpdate as edits happen ...
//
// Both kSyncStep2 and kSyncUpdate payloads are wire-compatible with
// ycpp_doc.h's `apply_update_v1` — peers just decode the envelope and
// hand the payload off.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_arena.h"
#include "ycpp_byteview.h"
#include "ycpp_doc.h"
#include "ycpp_envelope.h"
#include "ycpp_state_vector.h"
#include "ycpp_status.h"
#include "ycpp_writer.h"

namespace ycpp {

// ----- producer helpers ----------------------------------------------------

// Encode a kSyncStep1 envelope carrying the doc's current StateVector.
// `scratch` is a fresh per-call allocator used only to build the SV — it
// does NOT pollute the doc's own arena. Callers typically construct one
// per outbound message (e.g. a stack-scoped DefaultArenaAllocator).
template <Allocator A, Allocator Scratch>
[[nodiscard]] inline Status emit_sync_step1(const Doc<A>& doc,
                                             Scratch&      scratch,
                                             uint64_t      request_id,
                                             Writer*       out) noexcept {
    assert(out != nullptr);
    YCPP_TRY(out->u8(static_cast<uint8_t>(MessageKind::kSyncStep1)));
    YCPP_TRY(out->varint_u64(request_id));

    StateVector<Scratch> sv{&scratch};
    YCPP_TRY(doc.state_vector(&sv));

    // Counting/staging buffer for the SV payload. Bounded by the SV's
    // own kMaxClients * (≤10 + ≤10) varint bytes — well under 128 KiB.
    uint8_t staging[131072];
    Writer  sw{staging, sizeof(staging)};
    YCPP_TRY(sv.encode(sw));
    return out->length_prefixed(staging, sw.pos());
}

// Encode a kSyncStep2 envelope carrying a doc diff aimed at the peer
// whose SV is `peer_sv` (the SV the peer sent in step 1).
template <Allocator A>
[[nodiscard]] inline Status emit_sync_step2(const Doc<A>&         doc,
                                             const StateVector<A>* peer_sv,
                                             uint64_t              request_id,
                                             Writer*               out) noexcept {
    assert(out != nullptr);
    YCPP_TRY(out->u8(static_cast<uint8_t>(MessageKind::kSyncStep2)));
    YCPP_TRY(out->varint_u64(request_id));

    uint8_t scratch[16384];
    Writer  sw{scratch, sizeof(scratch)};
    YCPP_TRY(encode_diff_v1<A>(doc, peer_sv, &sw));
    return out->length_prefixed(scratch, sw.pos());
}

// Encode a kSyncUpdate envelope for broadcasting a fresh update_v1.
// (Caller already has the update bytes in hand — e.g. they emitted them
// via encode_diff_v1 over the prior SV.)
[[nodiscard]] inline Status emit_sync_update(ByteView update_v1,
                                              uint64_t request_id,
                                              Writer*  out) noexcept {
    return encode_envelope(MessageKind::kSyncUpdate, request_id, update_v1, *out);
}

// ----- consumer helper -----------------------------------------------------

// Apply a decoded envelope to `dst`. If the envelope carries a
// kSyncStep1, decode the peer's SV into `*peer_sv_out` so the caller can
// respond with `emit_sync_step2`. Otherwise (`kSyncStep2`, `kSyncUpdate`)
// hand the payload to `dst.apply_update_v1`.
//
// `peer_sv_out` may be nullptr when the caller doesn't care about
// recording the peer's SV (one-way subscribers).
template <Allocator A>
[[nodiscard]] inline Status apply_sync_message(Doc<A>&         dst,
                                                const Envelope& env,
                                                StateVector<A>* peer_sv_out) noexcept {
    switch (env.kind) {
        case MessageKind::kSyncStep1: {
            if (peer_sv_out == nullptr) return Status::kOk;
            Reader r{env.payload};
            return peer_sv_out->decode(r);
        }
        case MessageKind::kSyncStep2:
        case MessageKind::kSyncUpdate:
            return dst.apply_update_v1(env.payload);
        default:
            return Status::kUnsupportedFormat;
    }
}

} // namespace ycpp
