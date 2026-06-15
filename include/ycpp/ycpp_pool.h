// ycpp_pool.h — type-stable free-list pool, templated on the Allocator.
//
// Per-object reuse for types that are allocated and recycled frequently
// (Item nodes during CRDT integration; pending-update entries; observer
// records). The pool owns slabs of `T`s, hands out one at a time, and
// keeps a single-linked free list of returned slots.
//
// Trade-off vs raw arena: pools cost one pointer per slot for the freelist
// thread, but make `release(T*)` O(1) — important for the per-Item churn
// during garbage collection (W8). The arena alone cannot reclaim individual
// objects.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "ycpp_arena.h"

namespace ycpp {

template <class T, Allocator A>
class Pool {
public:
    static_assert(std::is_nothrow_destructible_v<T>, "Pool<T>: T must be nothrow-destructible");
    static_assert(sizeof(T) >= sizeof(void*), "Pool<T>: T must be at least pointer-sized");

    static constexpr std::size_t kDefaultSlabSlots = 1024;
    static constexpr std::size_t kMaxSlabs         = 4096;

    explicit Pool(A* alloc, std::size_t slab_slots = kDefaultSlabSlots) noexcept
        : alloc_(alloc), slab_slots_(slab_slots),
          slabs_(nullptr), free_head_(nullptr),
          live_(0), n_slabs_(0) {
        assert(alloc      != nullptr);
        assert(slab_slots != 0);
    }

    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;

    // Move stays simple: take the slab list + freelist verbatim. The destination
    // must outlive the source's slabs, which is true for moves into a containing
    // object since both arena and pool share the doc's lifetime.
    Pool(Pool&& o) noexcept
        : alloc_(o.alloc_), slab_slots_(o.slab_slots_),
          slabs_(o.slabs_), free_head_(o.free_head_),
          live_(o.live_), n_slabs_(o.n_slabs_) {
        o.slabs_ = nullptr; o.free_head_ = nullptr;
        o.live_ = 0; o.n_slabs_ = 0;
    }
    Pool& operator=(Pool&& o) noexcept {
        if (this != &o) {
            // Caller is responsible for releasing live nodes before move-assign;
            // we cannot enumerate them generically. assert in debug.
            assert(live_ == 0 && "move-assigning into a Pool with live objects");
            alloc_      = o.alloc_;
            slab_slots_ = o.slab_slots_;
            slabs_      = o.slabs_;
            free_head_  = o.free_head_;
            live_       = o.live_;
            n_slabs_    = o.n_slabs_;
            o.slabs_ = nullptr; o.free_head_ = nullptr;
            o.live_ = 0; o.n_slabs_ = 0;
        }
        return *this;
    }

    ~Pool() noexcept = default;  // arena owns the storage; slabs free at arena reset

    // Acquire one slot, default-constructed in place. Returns nullptr if the
    // allocator is exhausted (out-of-memory) or we exceed kMaxSlabs.
    template <class... Args>
    [[nodiscard]] T* acquire(Args&&... args) noexcept {
        T* slot = pop_free();
        if (slot == nullptr) {
            slot = grow_one();
            if (slot == nullptr) return nullptr;
        }
        ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
        ++live_;
        return slot;
    }

    // Return a previously-acquired slot. Destroys T and pushes onto the freelist.
    void release(T* p) noexcept {
        assert(p     != nullptr);
        assert(live_ != 0);
        p->~T();
        FreeNode* n = reinterpret_cast<FreeNode*>(p);
        n->next     = free_head_;
        free_head_  = n;
        --live_;
    }

    [[nodiscard]] std::size_t live()  const noexcept { return live_; }
    [[nodiscard]] std::size_t slabs() const noexcept { return n_slabs_; }

private:
    struct FreeNode { FreeNode* next; };

    struct Slab {
        Slab*       next;
        std::size_t slots;
        // payload follows in place; storage is alignof(T)-aligned
    };

    static constexpr std::size_t kSlabHeaderBytes =
        (sizeof(Slab) + alignof(T) - 1) & ~(alignof(T) - 1);

    [[nodiscard]] T* pop_free() noexcept {
        if (free_head_ == nullptr) return nullptr;
        FreeNode* n = free_head_;
        free_head_  = n->next;
        return reinterpret_cast<T*>(n);
    }

    [[nodiscard]] T* grow_one() noexcept {
        if (n_slabs_ >= kMaxSlabs) return nullptr;
        const std::size_t bytes = kSlabHeaderBytes + sizeof(T) * slab_slots_;
        void* raw = alloc_->alloc(bytes, alignof(T));
        if (raw == nullptr) return nullptr;
        auto* s   = static_cast<Slab*>(raw);
        s->next   = slabs_;
        s->slots  = slab_slots_;
        slabs_    = s;
        ++n_slabs_;
        // Push every slot onto the freelist EXCEPT the first one, which we
        // return as the "newly-acquired" slot.
        auto* base = reinterpret_cast<uint8_t*>(s) + kSlabHeaderBytes;
        for (std::size_t i = slab_slots_ - 1; i >= 1; --i) {
            auto* fn   = reinterpret_cast<FreeNode*>(base + i * sizeof(T));
            fn->next   = free_head_;
            free_head_ = fn;
        }
        return reinterpret_cast<T*>(base);
    }

    A*          alloc_;
    std::size_t slab_slots_;
    Slab*       slabs_;
    FreeNode*   free_head_;
    std::size_t live_;
    std::size_t n_slabs_;
};

} // namespace ycpp
