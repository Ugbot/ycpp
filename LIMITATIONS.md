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

## Still on the deferred list (0.1.x targets)

- **updateV2 wire format** — deliberately skipped, not a TODO. ycpp
  0.0.4 ships the `apply_update_v2` / `encode_diff_v2` public surface
  (so callers can compile-time depend on it) but the bodies return
  `kUnsupportedFormat`. Rationale:
  - Yjs JS defaults to v1 (`Y.encodeStateAsUpdate` is v1; v2 is
    opt-in via `Y.encodeStateAsUpdateV2`).
  - v2's only benefit is wire-size compression (~30–50% on typical
    text workloads) — no new CRDT semantics, no new content kinds.
  - Implementing it means ~1500 LOC of bit-fiddly stream encoders
    (RLE / IntDiffOptRle / UintOptRle / StringEncoder) plus a
    multi-stream cursor abstraction — no behavioural payoff while the
    v1-default ecosystem covers our consumers.
  - The symbol shape is pinned, so future v2 implementation lands
    without breaking callers. We'll do it when a real use case
    shows up. If you need v2 today, open an issue.
- **Sub-doc full materialization**: `SubDocRegistry` parses guid + opts
  bytes. Spawning a child `Doc<A>` instance that shares clock-space and
  sync semantics with the parent is the remaining piece.
- **Y.Move under concurrent moves**: ycpp applies a single Move op by
  re-linking the source range after the carrier. Yjs's full algorithm
  uses the move's `priority` byte to tie-break concurrent moves of
  overlapping ranges; we currently honour the first move applied.
- **Y.Text delta with structured attributes**: `YText::delta()` emits
  format payloads as raw bytes (varString key + varString value-JSON);
  decoding the JSON value into an attribute map is left to the caller
  so ycpp stays JSON-decoder-free.

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
