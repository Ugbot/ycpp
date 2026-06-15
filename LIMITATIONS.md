# What's not yet in ycpp 0.0.2

ycpp is alpha. The core CRDT runtime + sync + RPC framing all work and
are tested, but the Yjs surface is large and a handful of pieces are
explicitly deferred. This file is the honest inventory.

## Yjs JS interop — what works today

`tools/yjs_interop/test.js` drives the bundled `ycpp_cli` (and
`bolt::ybolt`'s `ybolt_cli`) against the real `yjs` npm package:

- **Y.Map with string / number / boolean / null values**: round-trips
  cleanly. Yjs encodes these as `kAny` (lib0/any binary format); ycpp's
  decoder skips lib0/any structurally and stores the payload bytes
  verbatim so the encoder can echo them back to a fresh Y.Doc.
- **Y.Text short content**: round-trips byte-for-byte.
- **Concurrent appends → Yjs JS canonical merge**: ycpp produces the
  same text as Yjs's reference merge of two diverged docs.

7/7 interop assertions green via Node 22.

## Yjs JS interop — what doesn't work yet

- **Rich-text format marks** (`kFormat` content kind), **nested types**
  (`kType`), **sub-docs** (`kDoc`), **Y.Move** (`kMove`): the decoder
  returns `kUnsupportedFormat` rather than guess at the wire shape. A
  Yjs doc using rich text or nested Y.Map/Y.Array hierarchies will fail
  to apply.
- **Length-N string Items.** Yjs's encoder collapses adjacent character
  inserts from the same client into one Item with `length = N` and a
  multi-char content payload. ycpp's decoder reads each Item as
  `length = 1`. Short text round-trips because each insert is its own
  Item; large pasted blobs from Yjs may surface `kPendingReference`.
- **UTF-16 vs UTF-8 char counting.** Yjs measures `Y.Text` length in
  UTF-16 code units (JS string semantics). ycpp counts bytes. Cursor
  positions translated naively will land mid-byte for non-ASCII text.
- **updateV2 wire format.** Yjs's default in recent versions is still
  updateV1; v2 is opt-in via `encodeStateAsUpdateV2`. ycpp ships v1 only.

These are tractable, just substantial. They land in 0.1.x.

## Y.Text — text only, no formatting

`YText<A>` is a text facade over `YArray`: each Item carries a UTF-8
byte run. **Format marks** (bold / italic / spans) are wire-recognized
by the decoder but not interpreted. `for_each_chunk` returns all
chunks; you cannot today get a "rich text delta" from a `YText`.

## YATA edge cases

The integration algorithm covers `itemsBeforeOrigin` and `conflicting`
sets (the classic YATA). It is bounded at 256 items per conflict zone —
larger conflict zones return `kIntegrationFailed`. Typical text edits
stay well under this.

Move operations (`Y.Move`) parse and survive a round-trip but do not
re-anchor the moved item — they are treated as opaque tombstones.

## Y.Map — last-writer-wins only

`YMap<A>` is LWW per key, anchored by Lamport order on `(client, clock)`
of the inserting Item. The full Y.Map semantics in Yjs preserve the
*entire* set of writes (so a `keys()` after concurrent set + delete is
deterministic). ycpp drops the loser into the delete set, which is
correct but doesn't expose the alternate-history queries some apps want.

## Sub-docs, XML, format

None of these are implemented:

- **Sub-docs** (`Y.Doc` as a content kind) — parses + survives round-trip
  but you cannot today materialize the sub-doc.
- **`Y.XmlElement` / `Y.XmlFragment` / `Y.XmlText` / `Y.XmlHook`** — no
  user driving this; can land if there's demand.
- **Format marks** in Y.Text (covered above).

## UndoManager + GC

Both are designed and reserved as `W7` / `W8` in the project plan but
not implemented. Callers maintain their own undo stack for now;
long-lived Docs accumulate tombstones (each delete leaves the Item in
the store with `kFlagDeleted` set).

## Non-Yjs limits worth knowing

- **`StateVector::kMaxClients = 4096`** — `set()` returns
  `kCapacityExceeded` past this. Multi-million peer scenarios need a
  higher cap.
- **`Awareness::kMaxStateBytes = 64 KiB`** per peer entry.
- **Single-threaded per `Doc`.** All `Doc` methods assume the caller
  has serialized access. Multi-thread orchestration is the caller's
  problem.

## What the tests actually prove

50+ test cases under MSVC `/W4 /permissive- /WX`, including:

- Two-Doc bidirectional convergence (`test_ycpp_convergence`,
  `test_ycpp_yarray_text`)
- Concurrent same-key writes resolve by Lamport
  (`test_ycpp_convergence`)
- Concurrent same-position inserts converge across peers
  (`test_ycpp_yarray_text`)
- Sync protocol over the Envelope round-trips peers (`test_ycpp_protocol`)
- Awareness LWW + offline marker (`test_ycpp_awareness`)
- Envelope rejects malformed inputs (`test_ycpp_envelope`)

What is **not** proven: behaviour against real Yjs JS fixtures. If
you're shipping ycpp behind a JS Yjs client, run your own fixture round-
trip before relying on the integration.
