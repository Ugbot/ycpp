# ycpp — Working Agreement

ycpp is a standalone, open-source, dependency-free C++20 Yjs CRDT
runtime. This file is the merge gate. Code that violates these rules
does not land.

## Non-negotiable rules

### Dependencies
- **No external runtime dependencies.** Only the C++20 standard library.
  GoogleTest is permitted in `tests/` only, via FetchContent.
- **No internal-to-Gestalt2 dependencies.** ycpp never includes
  `<bolt/…>`, `<marbledb/…>`, `<chukonu/…>`, `<boltapi/…>`. Production
  bindings live in a separate `y-bolt` library outside this repo.
- **Reimplement, don't import.** Ring buffers, arenas, pools,
  SwissTable-shape maps — we ship our own. MIT-licensed, written fresh.

### Tiger Style
- **Allocate at startup, never on the hot path.** Arenas for scratch,
  pools for ownership, ring buffers for queues. Every buffer carries a
  `constexpr` cap.
- **No `std::string` / `std::vector` / `std::map`** in hot structs or
  hot loops. POD records, `ByteView`, fixed-capacity arrays.
- **No smart pointers.** Raw pointers; ownership is lexical.
- **No exceptions, no RTTI across the public API.** Every public
  function is `noexcept`. Internal errors return `ycpp::Status`.
- **`≥2 assertions` per non-trivial function.** Preconditions +
  postconditions, positive and negative space. No side effects inside
  `assert(...)`.
- **Bounded everything.** Every loop has a fixed upper bound; recursion
  is rejected on the hot path (iterative integration only).
- **Functions < 70 lines. Headers ≤ 300 lines. .cpp ≤ 1500 lines.**
- **Explicit integer sizes** (`uint64_t`, `int32_t`); avoid `size_t` in
  serialisation paths where the wire format pins width.
- **Zero compiler warnings** at `/W4 /permissive-` (MSVC) and
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (clang/gcc). Fix,
  don't suppress.

### Policy injection — templates, not vtables
Allocator + observer plumbing is policy-based:

```cpp
template <Allocator A> class Doc;
using DefaultDoc = Doc<DefaultArenaAllocator>;
```

`Allocator` is a C++20 concept (`alloc`, `free`, `bytes_in_use`). Every
allocation site is monomorphised + inlined by the compiler. No vtables,
no virtual calls on the per-`Item` hot path.

### Wire compatibility
ycpp must round-trip byte-identical against JS Yjs for updateV1
(legacy read), updateV2 (default read and write), and state vectors.
The interop gate (`tests/test_ycpp_yjs_interop.cpp`) lands in W9 and
fixtures regenerate via a small Node script.

### Portability
Builds on Windows MSVC + clang-cl, macOS clang, Linux clang + gcc.
**Never MinGW.** No GCC-isms in source. No `pthread.h` / `sys/mman.h` /
`unistd.h` in public headers — `<thread>`, `<atomic>`, `<chrono>`,
`<bit>`.

## File structure

```
include/ycpp/        public headers (one file per concept)
src/                compiled TUs (one file per module)
tests/              gtest (one file per module; `test_ycpp_<area>.cpp`)
benchmarks/         perf gates against y-crdt
cmake/              YcppCompileOptions, YcppGTest
docs/               ALGORITHM.md, WIRE_FORMAT.md, PERFORMANCE.md
```

Headers are top-level under `include/ycpp/`. Names are prefixed
(`ycpp_arena.h`, `ycpp_doc.h`) so consumers who pull the whole tree get
unambiguous includes.

## Build & test

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

## Workflow

- Commit / push to ycpp's own `main`. ycpp is a standalone repo; it is
  consumed by Gestalt2 as a submodule (pointer bumped after each green
  ycpp commit).
- Every change ships with its test. A feature without a test is not done.
- New `.cpp` files allocate ONLY through the template `Allocator A`
  parameter — never via `new`, `malloc`, or `std::allocator`. A
  one-line grep in CI rejects offenders.
