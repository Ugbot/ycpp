// ycpp_envelope.h — generic framed-message envelope.
//
// The envelope is the small framing primitive every ycpp-flavoured RPC,
// sync protocol, awareness exchange, or app-defined message sits on:
//
//   { kind: u8, request_id: varint_u64, payload: length-prefixed bytes }
//
// `kind` namespaces the protocol stack:
//
//   kSyncStep1 / kSyncStep2 / kSyncUpdate   ycpp_protocol.h
//   kAwarenessUpdate                        ycpp_awareness.h
//   kAuthChallenge / kAuthReply             reserved
//   kCustomRequest / kCustomReply / kCustomEvent
//                                            free-form, app-defined
//
// `request_id` is the correlation token. Use 0 when the message has no
// reply (events, broadcasts). Servers reply by echoing the request_id.
//
// Wire encoding is intentionally tiny — `kind` is 1 byte, `request_id`
// is a varint (1 byte for typical low ids), and the payload prefix is
// itself a varint. Most envelopes fit in <16 bytes plus payload.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ycpp_byteview.h"
#include "ycpp_reader.h"
#include "ycpp_status.h"
#include "ycpp_writer.h"

namespace ycpp {

enum class MessageKind : uint8_t {
    // Yjs sync (ycpp_protocol.h)
    kSyncStep1       = 0x01,   // SV: "this is what I have"
    kSyncStep2       = 0x02,   // update: "here's what you're missing"
    kSyncUpdate      = 0x03,   // update: "here's a new edit"

    // Awareness (ycpp_awareness.h)
    kAwarenessUpdate = 0x10,

    // Auth (reserved)
    kAuthChallenge   = 0x20,
    kAuthReply       = 0x21,

    // App-defined RPC slots — opaque payload, app interprets.
    kCustomRequest   = 0x80,
    kCustomReply     = 0x81,
    kCustomEvent     = 0x82,
};

[[nodiscard]] constexpr bool message_kind_is_known(uint8_t raw) noexcept {
    switch (static_cast<MessageKind>(raw)) {
        case MessageKind::kSyncStep1:
        case MessageKind::kSyncStep2:
        case MessageKind::kSyncUpdate:
        case MessageKind::kAwarenessUpdate:
        case MessageKind::kAuthChallenge:
        case MessageKind::kAuthReply:
        case MessageKind::kCustomRequest:
        case MessageKind::kCustomReply:
        case MessageKind::kCustomEvent:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr const char* message_kind_name(MessageKind k) noexcept {
    switch (k) {
        case MessageKind::kSyncStep1:       return "sync_step1";
        case MessageKind::kSyncStep2:       return "sync_step2";
        case MessageKind::kSyncUpdate:      return "sync_update";
        case MessageKind::kAwarenessUpdate: return "awareness_update";
        case MessageKind::kAuthChallenge:   return "auth_challenge";
        case MessageKind::kAuthReply:       return "auth_reply";
        case MessageKind::kCustomRequest:   return "custom_request";
        case MessageKind::kCustomReply:     return "custom_reply";
        case MessageKind::kCustomEvent:     return "custom_event";
    }
    return "unknown";
}

struct Envelope {
    MessageKind kind;
    uint64_t    request_id;   // 0 = no reply expected
    ByteView    payload;      // length-prefixed on the wire; aliases input buffer on decode
};

[[nodiscard]] inline Status encode_envelope(MessageKind kind, uint64_t request_id,
                                             ByteView payload, Writer& w) noexcept {
    YCPP_TRY(w.u8(static_cast<uint8_t>(kind)));
    YCPP_TRY(w.varint_u64(request_id));
    return w.length_prefixed(payload.data, payload.size);
}

[[nodiscard]] inline Status decode_envelope(ByteView bytes, Envelope* out) noexcept {
    assert(out != nullptr);
    Reader r{bytes};
    uint8_t raw_kind = 0;
    YCPP_TRY(r.u8(&raw_kind));
    if (!message_kind_is_known(raw_kind)) return Status::kUnsupportedFormat;
    out->kind = static_cast<MessageKind>(raw_kind);
    YCPP_TRY(r.varint_u64(&out->request_id));
    YCPP_TRY(r.length_prefixed(&out->payload));
    if (!r.eof()) return Status::kCorruptInput;
    return Status::kOk;
}

} // namespace ycpp
