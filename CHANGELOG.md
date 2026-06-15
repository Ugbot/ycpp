# Changelog

## 0.0.2 — collaborative text + RPC primitives

CRDT core:
- `YArray<A>` — sequence CRDT with **full YATA integration**
  (`itemsBeforeOrigin` + `conflicting` sets so concurrent inserts
  converge across peers).
- `YText<A>` — text facade over `YArray`; `byte_length()`,
  `copy_into(Writer&)`, `for_each_chunk(fn)`.
- `Doc<A>` routing: items whose `parent_sub` is empty land in a root
  `YArray` (or are surfaced through `YText`). `parent_sub`-keyed items
  still land in a root `YMap`.
- Local-edit helpers: `Doc::array_insert_at`, `Doc::text_append`,
  `Doc::array_delete_at`.
- Decoder now follows Yjs's parent-inheritance convention: when an
  incoming Item omits parent info (origin is set), the parent is
  inherited from the predecessor chain.

RPC + protocol layer:
- `ycpp::Envelope` — `{ kind, request_id, length-prefixed payload }`
  framing for any transport.
- `ycpp::MessageKind` — reserved slots for sync (`kSyncStep1/2/Update`),
  awareness (`kAwarenessUpdate`), auth (`kAuthChallenge/Reply`), and
  app-defined RPC (`kCustomRequest/Reply/Event`).
- `ycpp_protocol.h` — `emit_sync_step1`, `emit_sync_step2`,
  `emit_sync_update`, `apply_sync_message`.
- `Awareness<A>` — per-client `(clock, payload)` map with `publish`,
  `apply` (LWW by clock), `encode_all`, `get`, `for_each`. Empty
  payload encodes "peer offline".

Docs:
- README rewritten for shipping audience.
- This CHANGELOG.
- `LIMITATIONS.md` enumerating what isn't yet supported.

Test surface: 17 test suites, 50+ cases. All green on MSVC `/W4 /WX`.

## 0.0.1 — initial scaffold (W1 + W2)

W1 primitives:
- `ByteView`, `Status`, branchless `varint` codec (LEB128 + zigzag),
  bounded `Reader` / `Writer`.
- `DefaultArenaAllocator` (fixed-block bump, oversize path),
  `Pool<T, A>` (type-stable free-list), `SpscRing<T, A>` (bounded SPSC,
  cache-line-padded), `HashMap<K, V, A>` (SwissTable-shape, tombstone
  reclaim).

W2 CRDT primitives:
- `Id` Lamport pair, `Item` POD + `ContentKind` + Yjs info-byte helpers.
- `DeleteSet<A>` (RLE per-client ranges + adjacency-merge),
  `StructStore<A>` (per-client sorted vectors + state-vector query).
- `decode_update_v1` parses Yjs updateV1 framing.

W3 CRDT runtime:
- `Doc<A>`, `YMap<A>` last-writer-wins, `StateVector<A>`,
  `apply_update_v1`, `encode_diff_v1`.
- Decoder + encoder bodies inline in headers so non-default allocators
  instantiate at the call site.

Zero external runtime deps. MIT license. Built clean on MSVC `/W4 /WX`.
