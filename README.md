# ycpp

> A Tiger-Style, dependency-free C++20 [Yjs](https://github.com/yjs/yjs)
> CRDT runtime. Wire-compatible with JS Yjs; aimed at the same kind of
> servers, sync brokers, and embedded apps that today reach for
> [y-crdt (Rust)](https://github.com/y-crdt/y-crdt) but want to avoid the
> Rust toolchain and FFI hop.

## Why

You have a JavaScript Yjs client and you need the server side to:

- **apply updates into an authoritative document** (not just relay blobs),
- **compute state-vector diffs** so peers only fetch what they're missing,
- **consolidate updates into snapshots** without round-tripping through a
  browser,
- **observe document changes** and react in C++ — without GC pauses, FFI
  hops, or per-operation allocations.

ycpp does that, in pure C++20, with **no runtime dependencies** beyond the
standard library. Tests pull GoogleTest via FetchContent — that is the
only external code anywhere near the project.

## Status

Pre-alpha. Wave 1 (this commit) ships the scaffold and the binary
primitives (varint, reader/writer, arena, pool, ring buffer,
SwissTable-shaped hash map). See `docs/` for the roadmap.

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

## Engineering standard

Tiger Style. Bounded everything. Allocate at startup, never on the hot
path. `≥2 asserts/fn`. No exceptions, no RTTI across the public API. See
[`CLAUDE.md`](CLAUDE.md) for the working agreement.

## License

[MIT](LICENSE).
