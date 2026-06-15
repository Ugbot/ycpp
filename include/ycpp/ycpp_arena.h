// ycpp_arena.h — fixed-block arena allocator + the Allocator concept.
//
// Two things live here because they're inseparable in the public API:
//
//   1. The `Allocator` C++20 concept that every hot-path type templates on.
//   2. `DefaultArenaAllocator` — a bump allocator backed by 64 KiB blocks,
//      satisfying the concept. The default for `ycpp::Doc`.
//
// y-bolt (Gestalt2-internal) supplies its own `BoltArenaAllocator` over
// `bolt::Arena`, also satisfying the concept. ycpp never knows.
//
// Lifetime model:
//   - `alloc(n, align)` returns a pointer or nullptr. No exceptions.
//   - `free(p, n)` is allowed to be a no-op (arena style); the contract is
//     "the allocator MUST tolerate the call" — not "this byte run is now
//     reusable." Pools handle per-object reuse.
//   - `reset()` (optional on the policy) frees every outstanding allocation.
//     Used between transactions when scratch buffers are scoped.
//
// Tiger Style:
//   - No std::vector — blocks are a singly-linked list of owned chunks.
//   - Bounded block count; assert + return null past the cap.
//   - Allocations align to the requested boundary via integer arithmetic
//     (no UB on uintptr_t cast). Default 8B alignment for unaligned asks.

#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

namespace ycpp {

// ----- Allocator concept ----------------------------------------------------

template <class A>
concept Allocator = requires(A& a, const A& ca, void* p, std::size_t n,
                             std::size_t align) {
    { a.alloc(n, align) }    -> std::convertible_to<void*>;
    { a.free(p, n)      }    -> std::same_as<void>;
    { ca.bytes_in_use() }    -> std::convertible_to<std::size_t>;
};

// ----- DefaultArenaAllocator ------------------------------------------------

class DefaultArenaAllocator {
public:
    static constexpr std::size_t kDefaultBlockBytes = 64 * 1024;
    static constexpr std::size_t kMaxBlocks         = 4096;  // 256 MiB at 64KB
    static constexpr std::size_t kDefaultAlign      = 8;

    explicit DefaultArenaAllocator(std::size_t block_bytes = kDefaultBlockBytes) noexcept
        : block_bytes_(block_bytes), head_(nullptr), used_(0), n_blocks_(0) {
        assert(block_bytes >= 64);
        assert((block_bytes & (block_bytes - 1)) == 0 && "block_bytes must be power-of-two");
    }

    // Move-only; copying an arena would double-free.
    DefaultArenaAllocator(const DefaultArenaAllocator&)            = delete;
    DefaultArenaAllocator& operator=(const DefaultArenaAllocator&) = delete;
    DefaultArenaAllocator(DefaultArenaAllocator&& o) noexcept
        : block_bytes_(o.block_bytes_), head_(o.head_),
          used_(o.used_), n_blocks_(o.n_blocks_) {
        o.head_ = nullptr; o.used_ = 0; o.n_blocks_ = 0;
    }
    DefaultArenaAllocator& operator=(DefaultArenaAllocator&& o) noexcept {
        if (this != &o) {
            reset();
            block_bytes_ = o.block_bytes_;
            head_        = o.head_;
            used_        = o.used_;
            n_blocks_    = o.n_blocks_;
            o.head_ = nullptr; o.used_ = 0; o.n_blocks_ = 0;
        }
        return *this;
    }

    ~DefaultArenaAllocator() noexcept { reset(); }

    [[nodiscard]] void* alloc(std::size_t n, std::size_t align = kDefaultAlign) noexcept {
        assert(align > 0);
        assert((align & (align - 1)) == 0 && "align must be power-of-two");
        if (n == 0) return nullptr;

        // Big-allocation path: anything that won't fit in a fresh block
        // gets its own oversized block. Same lifetime, same free path.
        if (n + align > block_bytes_) {
            return alloc_oversize_block(n, align);
        }
        // Try the current block, then chain a fresh one if needed.
        if (head_ != nullptr) {
            void* p = bump_from(head_, n, align);
            if (p != nullptr) return p;
        }
        if (n_blocks_ >= kMaxBlocks) return nullptr;
        Block* nb = allocate_block(block_bytes_);
        if (nb == nullptr) return nullptr;
        nb->next = head_;
        head_    = nb;
        ++n_blocks_;
        return bump_from(head_, n, align);
    }

    // free() is a no-op for the arena; per-object reuse lives in the pool.
    // We still update `used_` so bytes_in_use() reports the *live* size for
    // the rare case the caller tracks allocations individually.
    void free(void* /*p*/, std::size_t n) noexcept {
        if (n <= used_) used_ -= n; else used_ = 0;
    }

    void reset() noexcept {
        Block* b = head_;
        while (b != nullptr) {
            Block* next = b->next;
            std::free(b);
            b = next;
        }
        head_     = nullptr;
        used_     = 0;
        n_blocks_ = 0;
    }

    [[nodiscard]] std::size_t bytes_in_use() const noexcept { return used_; }
    [[nodiscard]] std::size_t block_count()  const noexcept { return n_blocks_; }
    [[nodiscard]] std::size_t block_bytes()  const noexcept { return block_bytes_; }

private:
    struct Block {
        Block*       next;
        std::size_t  cap;
        std::size_t  off;
        // payload follows in-place
    };

    static constexpr std::size_t kBlockHeaderBytes =
        sizeof(Block) <= 32 ? 32 : sizeof(Block);  // round up so payload aligns

    [[nodiscard]] static Block* allocate_block(std::size_t cap) noexcept {
        // std::malloc gives ≥ alignof(std::max_align_t) which covers our
        // ≤16B alignment requirements without a platform-specific aligned
        // allocator. Asserts catch the corner case at debug time.
        void* raw = std::malloc(kBlockHeaderBytes + cap);
        if (raw == nullptr) return nullptr;
        auto* b = static_cast<Block*>(raw);
        b->next = nullptr;
        b->cap  = cap;
        b->off  = 0;
        return b;
    }

    [[nodiscard]] void* bump_from(Block* b, std::size_t n, std::size_t align) noexcept {
        assert(b != nullptr);
        auto* base    = reinterpret_cast<uint8_t*>(b) + kBlockHeaderBytes;
        auto  raw_off = b->off;
        auto  cur_addr = reinterpret_cast<std::uintptr_t>(base + raw_off);
        auto  aligned = (cur_addr + (align - 1)) & ~static_cast<std::uintptr_t>(align - 1);
        std::size_t pad = static_cast<std::size_t>(aligned - cur_addr);
        if (raw_off + pad + n > b->cap) return nullptr;
        b->off = raw_off + pad + n;
        used_ += n;
        return reinterpret_cast<void*>(aligned);
    }

    [[nodiscard]] void* alloc_oversize_block(std::size_t n, std::size_t align) noexcept {
        if (n_blocks_ >= kMaxBlocks) return nullptr;
        const std::size_t cap = n + align;
        Block* b = allocate_block(cap);
        if (b == nullptr) return nullptr;
        // Place the oversize block AFTER the current head so we don't waste
        // residual capacity in the active block on subsequent small allocs.
        if (head_ != nullptr) { b->next = head_->next; head_->next = b; }
        else                  { b->next = nullptr;     head_ = b;       }
        ++n_blocks_;
        return bump_from(b, n, align);
    }

    std::size_t block_bytes_;
    Block*      head_;
    std::size_t used_;
    std::size_t n_blocks_;
};

static_assert(Allocator<DefaultArenaAllocator>,
              "DefaultArenaAllocator must satisfy ycpp::Allocator");

} // namespace ycpp
