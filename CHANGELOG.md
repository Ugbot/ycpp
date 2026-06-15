# Changelog

## 0.0.4 — UndoManager, GC, Y.Text delta, sub-docs, Y.Move, updateV2 surface

New surfaces:
- **`UndoManager<A>`** — origin-scoped undo/redo. `track(origin)` +
  `capture_begin(origin)` / `capture_end()` frame local edits; `undo()`
  marks the frame's items deleted + propagates to the Doc's delete set;
  `redo()` clears the flag. Bounded depth (256 frames × 256 origins).
- **`Doc::compact()`** — converts `kFlagDeleted` items to `kSkip`
  placeholders. Frees `content_view` bytes; clock arithmetic preserved.
  Skips items still referenced by another item's origin chain.
- **`YText::delta(on_entry)`** — Yjs-style sequence: `Insert` entries
  carry the text chunk plus the list of currently-active format
  payloads; `Format` entries report each format Item (live or deleted).
  Pure read; no allocations beyond the caller's collector.
- **`SubDocRegistry<A>`** — parses `kDoc` content (`varString guid` +
  `lib0/any opts`) and registers per Doc. `Doc::sub_docs()` enumerates
  by guid; `find(guid)` returns the `SubDocRef { id, guid, opts }`.
- **`ycpp::decode_move(payload, *out)` / `apply_move(item, op, store)`**
  — Y.Move op decoder + range-relocation. `Doc::install_into_array`
  triggers `apply_move` automatically when integrating a `kMove` Item.
- **`apply_update_v2` / `encode_diff_v2`** — public surface (returns
  `kUnsupportedFormat` in 0.0.4; full v2 RLE-stream implementation
  lands in 0.1.x). Symbol shape pinned so callers can compile-time
  depend on it.

Sequence-type routing:
- `kFormat`, `kMove`, `kEmbed` Items now route into the root `YArray`
  alongside `kString/kJson/kBinary/kAny` so rich-text format marks,
  Y.Move ops, and embedded content participate in the doubly-linked
  list rather than landing as bare store entries.

Tests: 19 own suites + 13 Yjs JS interop assertions. All green on MSVC
`/W4 /permissive- /WX`.

## 0.0.3 — length-N items + rich-content skippers + Yjs JS interior-edit interop

CRDT wire-format completeness:
- **Length-N items**: `kString` content now reports `length` as the
  UTF-16 code unit count of its UTF-8 bytes (Yjs's CRDT length
  semantics). `kAny` and `kJson` report the varUint element count.
  State-vector arithmetic is consistent across peers.
- **Local edits** (`map_set_string`, `text_append`, `array_insert_at`)
  set `length` to match what the receiver's wire decoder will recompute.
  `text_append` anchors `origin_left` at the END of the tail item's
  run (was the START), so subsequent appends from peers land in the
  correct position.
- **Decoder skippers** for `kFormat` (varString key + varString value),
  `kType` (varUint type-ref), `kDoc` (varString guid + lib0/any opts),
  `kMove` (start ID + end ID + priority byte). Each captures the
  structural payload verbatim; the encoder echoes it byte-for-byte.
  Yjs docs using rich text format ranges or nested types round-trip
  through ycpp without loss.
- **Multi-pass apply**: structs whose origin Ids haven't yet landed in
  the store (because the wire emitted them after the dependent struct)
  defer to the next pass and retry until fixedpoint. Resolves Yjs's
  encoder ordering where mid-string concurrent edits put the newer
  client's struct first.
- **UTF-16 helpers** (`utf16_length_of_utf8`,
  `utf8_byte_offset_for_utf16_units`) for callers that need to
  translate clock offsets to byte offsets and back.

Yjs JS interop (`tools/yjs_interop/test.js`):
- 13/13 assertions green against real `yjs` npm package via Node 22.
- New scenarios proven:
  - Y.Text multi-char insert + sequential append ("Hello, world!")
  - Y.Text concurrent mid-string edit (Bob inserts ", reader" at
    index 5 of Alice's "Hello world" → both peers converge to
    "Hello, reader world")

Docs:
- LIMITATIONS.md rewritten to reflect what 0.0.3 actually does + what
  is still deferred (rich-text rendering / sub-doc materialization /
  Y.Move re-anchoring / UndoManager / GC / updateV2).

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
