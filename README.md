# ycpp

> A Tiger-Style, **dependency-free** C++20 [Yjs](https://github.com/yjs/yjs)
> CRDT runtime — built for servers, sync brokers, embedded apps,
> collaborative text editors, and as a small framing toolkit for your
> own RPC.

```
zero external runtime deps · header-mostly · open-source MIT
Doc / Y.Map / Y.Array / Y.Text · YATA integration · state-vector sync
Awareness · pluggable Envelope/Protocol layer
allocator-policy templates · no vtables on the hot path
```

## What you get

- **CRDT runtime.** `Doc<A>` orchestrates a `StructStore`, a `DeleteSet`,
  and the root types: `YMap<A>` (last-writer-wins per key), `YArray<A>`
  (sequence CRDT with YATA integration), `YText<A>` (text facade over
  `YArray`).
- **Wire protocol.** `apply_update_v1(bytes)` and `encode_diff_v1(doc,
  since_state_vector, writer)` close the read/write loop. Peers
  exchange minimal state-vector-bounded diffs and converge.
- **RPC primitives.** A generic `Envelope { kind, request_id, payload }`
  framing format you can carry over any transport (TCP, WebSocket, MQTT,
  unix socket), with a Yjs-style sync protocol (`kSyncStep1` /
  `kSyncStep2` / `kSyncUpdate`) layered on top, plus an `Awareness<A>`
  per-client presence map.
- **Allocator policy via C++20 concept-constrained templates.** Every
  hot-path allocation is monomorphised + inlined at the call site — no
  vtable indirection. Default = `DefaultArenaAllocator`. Production
  adapters (e.g.
  [bolt::ybolt](https://github.com/Ugbot/bolt/tree/gestalt2-substrate/ybolt)
  over `bolt::Arena`) just supply their own policy.
- **Tiger Style.** No exceptions, no RTTI, no `std::string` / `std::vector`
  / `std::map` in hot structs, bounded loops, ≥2 asserts/fn, ≤300-line
  headers, ≤1500-line `.cpp`. Built clean on MSVC `/W4 /permissive- /WX`.

## Building

```sh
cmake --preset release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Windows MSVC:

```pwsh
cmake --preset msvc
cmake --build build/msvc --config Release
ctest --test-dir build/msvc -C Release --output-on-failure
```

## Five-line example — sync two Docs

```cpp
#include <ycpp/ycpp.h>

ycpp::Doc<ycpp::DefaultArenaAllocator> alice{/*client_id=*/1};
ycpp::Doc<ycpp::DefaultArenaAllocator> bob  {/*client_id=*/2};

// Local edits.
alice.map_set_string("doc", "title", "Hello, world");
bob  .text_append   ("body", "First post.");

// Build a state-vector-bounded diff and apply.
uint8_t buf[4096];
ycpp::Writer w{buf, sizeof(buf)};
ycpp::DefaultArenaAllocator scratch;
ycpp::StateVector<ycpp::DefaultArenaAllocator> bob_sv{&scratch};
bob.state_vector(&bob_sv);
ycpp::encode_diff_v1(alice, &bob_sv, &w);
bob.apply_update_v1({buf, w.pos()});

// Bob now sees both keys; Alice can do the symmetric exchange.
auto* title = bob.get_or_create_map("doc")->get(
    ycpp::ByteView{reinterpret_cast<const uint8_t*>("title"), 5});
```

## RPC framing

The `Envelope` is the small framing primitive every ycpp protocol sits on:

```
{ kind: u8, request_id: varint_u64, payload: length-prefixed bytes }
```

`MessageKind` covers the built-in protocols (sync, awareness, auth) and
reserves `kCustomRequest` / `kCustomReply` / `kCustomEvent` for
app-defined RPC. See `include/ycpp/ycpp_envelope.h` for details and
`include/ycpp/ycpp_protocol.h` for the sync protocol on top.

## Public API surface

```
ycpp_byteview.h      ycpp_status.h       ycpp_id.h           ycpp_varint.h
ycpp_reader.h        ycpp_writer.h       ycpp_arena.h        ycpp_pool.h
ycpp_ring.h          ycpp_hashmap.h
ycpp_item.h          ycpp_delete_set.h   ycpp_struct_store.h
ycpp_state_vector.h  ycpp_update.h       ycpp_doc.h
ycpp_ymap.h          ycpp_yarray.h       ycpp_ytext.h
ycpp_envelope.h      ycpp_protocol.h     ycpp_awareness.h
```

One `#include <ycpp/ycpp.h>` pulls everything.

## Status

ycpp **0.0.2** — alpha. The W1 + W2 + W3 + W4 core lands (17 test suites
under MSVC `/W4 /WX`). See [`LIMITATIONS.md`](LIMITATIONS.md) for the
parts of Yjs's full surface that aren't shipped yet (updateV2 encoder,
full Yjs JS interop verification, format ranges in Y.Text, Y.Move,
sub-docs, XML types, UndoManager, GC).

## License

[MIT](LICENSE).
