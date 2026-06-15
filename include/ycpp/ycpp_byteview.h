// ycpp_byteview.h — non-owning view over a contiguous byte range.
//
// Layout is `{ const uint8_t* data; size_t size; }`, deliberately chosen to be
// reinterpret-compatible with `bolt::ByteView` for the y-bolt adapter. We
// never depend on bolt — this is just the layout contract documented at
// the seam.
//
// Tiger Style:
//   - Trivially copyable POD; no virtuals.
//   - All members noexcept, constexpr where possible.
//   - Asserts on every nontrivial accessor.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace ycpp {

struct ByteView {
    const uint8_t* data;
    size_t         size;

    constexpr ByteView() noexcept : data(nullptr), size(0) {}
    constexpr ByteView(const uint8_t* d, size_t n) noexcept : data(d), size(n) {
        // pre: either both empty or both populated. The wire reader uses
        // empty views as sentinels, so nullptr+0 is the only legal "empty".
        assert((d != nullptr) || (n == 0));
    }

    [[nodiscard]] constexpr bool          empty() const noexcept { return size == 0; }
    [[nodiscard]] constexpr const uint8_t* begin() const noexcept { return data; }
    [[nodiscard]] constexpr const uint8_t* end  () const noexcept {
        assert((data != nullptr) || (size == 0));
        return data + size;
    }

    [[nodiscard]] constexpr uint8_t operator[](size_t i) const noexcept {
        assert(data != nullptr);
        assert(i < size);
        return data[i];
    }

    // Returns the subrange [off, off+n). Asserts on out-of-range.
    [[nodiscard]] constexpr ByteView slice(size_t off, size_t n) const noexcept {
        assert(off <= size);
        assert(n <= size - off);
        return ByteView{ data + off, n };
    }

    [[nodiscard]] constexpr ByteView drop_front(size_t n) const noexcept {
        assert(n <= size);
        return ByteView{ data + n, size - n };
    }
};

// Sanity: the layout we promised to the adapter seam.
static_assert(sizeof(ByteView)  == 2 * sizeof(void*), "ByteView must be 2 pointers wide");
static_assert(alignof(ByteView) == alignof(void*),    "ByteView must align like a pointer");

} // namespace ycpp
