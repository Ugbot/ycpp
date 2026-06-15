# Using ycpp

This is the practical recipes file. The [README](../README.md) is the
elevator pitch; [LIMITATIONS](../LIMITATIONS.md) is what's not yet
shipped.

## 1. Sync two Docs

```cpp
#include <ycpp/ycpp.h>

using Doc = ycpp::Doc<ycpp::DefaultArenaAllocator>;
using SV  = ycpp::StateVector<ycpp::DefaultArenaAllocator>;

Doc alice{1};
Doc bob  {2};

alice.map_set_string("doc", "title", "Hello, world");
bob  .map_set_string("doc", "editor", "bob");

// Bob asks "what am I missing?" — sends his SV.
ycpp::DefaultArenaAllocator scratch;
SV bob_sv{&scratch};
bob.state_vector(&bob_sv);

// Alice replies with the diff.
uint8_t  buf[4096];
ycpp::Writer w{buf, sizeof(buf)};
ycpp::encode_diff_v1(alice, &bob_sv, &w);
bob.apply_update_v1({buf, w.pos()});

// Alice does the symmetric thing to catch up on Bob.
```

## 2. Collaborative text

```cpp
Doc doc{/*client_id=*/42};

doc.text_append("body", "Hello, ");
doc.text_append("body", "world!");

// Read concatenated text back out.
auto text = doc.get_or_create_text("body");
text.for_each_chunk([](ycpp::ByteView chunk) noexcept {
    fwrite(chunk.data, 1, chunk.size, stdout);
});

// Concurrent peers exchange diffs the same way — YATA integration
// converges concurrent appends and inserts.
```

## 3. Sync protocol over your transport

`ycpp_protocol.h` wraps the sync handshake on top of the Envelope. The
emitter writes bytes you ship over WebSocket / TCP / MQTT / whatever;
the receiver decodes the Envelope and hands the payload to ycpp.

```cpp
// Peer A initiates:
uint8_t  out[4096];
ycpp::Writer w{out, sizeof(out)};
ycpp::DefaultArenaAllocator scratch;
ycpp::emit_sync_step1(alice, scratch, /*request_id=*/1, &w);
// (ship `out[0..w.pos()]` over the wire to peer B)

// Peer B receives bytes:
ycpp::Envelope env{};
ycpp::decode_envelope({wire_bytes, wire_len}, &env);
if (env.kind == ycpp::MessageKind::kSyncStep1) {
    SV peer_sv{&scratch};
    ycpp::apply_sync_message(bob, env, &peer_sv);
    // Now reply with kSyncStep2:
    ycpp::Writer rw{out, sizeof(out)};
    ycpp::emit_sync_step2(bob, &peer_sv, env.request_id, &rw);
    // (ship the reply back to peer A)
}
```

## 4. Building your own RPC on the Envelope

The Envelope reserves `kCustomRequest` / `kCustomReply` / `kCustomEvent`
slots whose payloads are opaque to ycpp. You define your own format
inside; ycpp gives you correlation via `request_id` for free.

```cpp
// Request side:
uint8_t  out[1024];
ycpp::Writer w{out, sizeof(out)};
const char* body = R"({"method":"hello","arg":"world"})";
ycpp::encode_envelope(
    ycpp::MessageKind::kCustomRequest, /*request_id=*/123,
    ycpp::ByteView{reinterpret_cast<const uint8_t*>(body),
                   std::strlen(body)},
    w);
// ship out[0..w.pos()]

// Reply side:
ycpp::Envelope env{};
ycpp::decode_envelope({wire_bytes, wire_len}, &env);
if (env.kind == ycpp::MessageKind::kCustomRequest) {
    // env.request_id == 123; env.payload is your JSON body
    // process, then write a kCustomReply with the same request_id
}
```

## 5. Awareness (presence + cursors)

```cpp
ycpp::Awareness<ycpp::DefaultArenaAllocator> aw{&scratch};

// Local peer publishes its state. Clock auto-increments.
const char* my_state = R"({"cursor":[5,13],"selecting":false})";
aw.publish(/*client_id=*/42, ycpp::ByteView{
    reinterpret_cast<const uint8_t*>(my_state), std::strlen(my_state)});

// Encode for broadcast.
uint8_t buf[1024];
ycpp::Writer w{buf, sizeof(buf)};
aw.encode_all(w);
// ship buf[0..w.pos()] inside a kAwarenessUpdate envelope (or however
// you frame it). Peers who receive it call `apply()`.
ycpp::Awareness<...> remote_aw{&scratch};
remote_aw.apply({wire_bytes, wire_len});
auto* peer = remote_aw.get(42);
// peer->state is the JSON above; peer->clock is monotonic per client.

// Mark yourself offline:
aw.publish(42, ycpp::ByteView{});  // empty payload, higher clock
```

## 6. Custom allocator (production binding)

Implement the `ycpp::Allocator` concept and you instantiate every
template against your storage:

```cpp
class MyAllocator {
public:
    void*       alloc(size_t n, size_t align) noexcept { /* ... */ }
    void        free (void* p, size_t n)       noexcept { /* ... */ }
    size_t      bytes_in_use() const           noexcept { /* ... */ }
};
static_assert(ycpp::Allocator<MyAllocator>);

ycpp::Doc<MyAllocator> doc{client_id, MyAllocator{}};
```

See [`bolt::ybolt`](https://github.com/Ugbot/bolt/tree/gestalt2-substrate/ybolt)
for the canonical example over `bolt::Arena`.
