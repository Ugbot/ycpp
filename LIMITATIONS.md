# What's not yet in ycpp 0.0.2

ycpp is alpha. The core CRDT runtime + sync + RPC framing all work and
are tested, but the Yjs surface is large and a handful of pieces are
explicitly deferred. This file is the honest inventory.

## Yjs JS interop — what works today

`tools/yjs_interop/test.js` drives the bundled `ycpp_cli` (and
`bolt::ybolt`'s `ybolt_cli`) against the real `yjs` npm package.
**13/13 assertions green** via Node 22:

- **Y.Map with primitive values**: round-trips cleanly. Yjs encodes
  these as `kAny` (lib0/any binary format); ycpp's decoder walks
  lib0/any structurally and stores the payload bytes verbatim so the
  encoder can echo them back to a fresh Y.Doc.
- **Y.Text short content**: round-trips byte-for-byte.
- **Y.Text length-N items**: multi-char inserts ("Hello, world") now
  decode with the correct UTF-16 code unit count so subsequent appends
  don't surface `kPendingReference`. Sequential `Y.Text.insert` from
  Yjs JS, applied through ycpp, then applied back to a fresh Yjs Doc,
  produces the same text. Verified for "Hello, world!".
- **Y.Text concurrent edits inside a length-N item** (origin pointing
  into the interior of a stored item). ycpp's multi-pass apply
  defers structs whose origin isn't yet in the store and retries until
  fixedpoint, so the wire-emitter-side split chain (which Yjs emits
  when concurrently editing inside a string) resolves correctly.
  Verified that Bob inserting `", reader"` at index 5 of Alice's
  "Hello world" produces "Hello, reader world" on both sides.
- **Concurrent appends → Yjs JS canonical merge**: ycpp produces the
  same text as Yjs's reference merge of two diverged docs.

The decoder also tolerates `kFormat`, `kType`, `kDoc`, `kMove` content
kinds — skipper logic walks the structural payload and captures it
verbatim so the encoder can echo it back. A Yjs doc that uses rich
text format ranges or nested types round-trips through ycpp without
loss of bytes.

## Still on the deferred list

- **Y.Text rich-text rendering**: format marks (`kFormat`) round-trip
  on the wire but ycpp doesn't surface a `delta()`-style API. Consumers
  see the format Items as opaque entries; building a rich-text view
  layer is up to the caller.
- **Sub-doc materialization**: `kDoc` content survives round-trip but
  ycpp does not materialize the nested `Doc`.
- **Y.Move re-anchoring**: `kMove` survives round-trip; the move
  semantics (relocating an item to a new position) are not applied.
- **UndoManager**: not shipped; callers maintain their own undo stack.
- **GC / compaction**: long-lived docs accumulate tombstones.
- **updateV2 wire format**: ycpp speaks updateV1 only. Yjs's default
  `encodeStateAsUpdate` is v1; v2 is opt-in via `encodeStateAsUpdateV2`.

## Non-Yjs limits worth knowing

- **`StateVector::kMaxClients = 4096`** — `set()` returns
  `kCapacityExceeded` past this. Multi-million peer scenarios need a
  higher cap.
- **`Awareness::kMaxStateBytes = 64 KiB`** per peer entry.
- **YATA conflict zone bounded at 256 items** — pathological histories
  return `kIntegrationFailed`. Typical text edits stay well under this.
- **Multi-pass apply caps at decoded.size() + 1 passes** — circular
  dependencies (impossible in well-formed updates) bail out as
  `kPendingReference`.
- **Single-threaded per `Doc`.** All `Doc` methods assume the caller
  has serialized access. Multi-thread orchestration is the caller's
  problem.

## What the tests actually prove

55+ test cases under MSVC `/W4 /permissive- /WX`, including:

- Two-Doc bidirectional convergence (`test_ycpp_convergence`,
  `test_ycpp_yarray_text`)
- Concurrent same-key writes resolve by Lamport
  (`test_ycpp_convergence`)
- Concurrent same-position inserts converge across peers
  (`test_ycpp_yarray_text`)
- Sync protocol over the Envelope round-trips peers (`test_ycpp_protocol`)
- Awareness LWW + offline marker (`test_ycpp_awareness`)
- Envelope rejects malformed inputs (`test_ycpp_envelope`)

Plus **13/13 assertions against the real Yjs JS library** via
`tools/yjs_interop/test.js` — see the top of this file.
