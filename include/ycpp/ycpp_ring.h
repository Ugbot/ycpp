// ycpp_ring.h — bounded SPSC ring buffer.
//
// Single-producer / single-consumer. Capacity is a power of two so the
// modulo collapses to a bitwise AND. Head and tail are cache-line padded so
// the producer and consumer never share a line.
//
// Used by the pending-update queue (W3 integration) and the observer
// fan-out (W3+). The ring stores trivially-copyable `T`s by value — for
// non-trivial payloads, store a pointer into the pool.

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "ycpp_arena.h"

namespace ycpp {

inline constexpr std::size_t kRingCacheLine = 64;

template <class T, Allocator A>
class SpscRing {
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing<T>: T must be trivially copyable; box non-POD via pointer");

    // Construct an empty (no storage) ring. Call init() before use.
    explicit SpscRing(A* alloc) noexcept
        : alloc_(alloc), buf_(nullptr), mask_(0) {
        assert(alloc != nullptr);
    }

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&)                 = delete;
    SpscRing& operator=(SpscRing&&)      = delete;

    ~SpscRing() noexcept = default;  // arena owns the buffer

    // Allocate storage for `capacity` slots. `capacity` MUST be a power of
    // two and ≥2 (we use one slot as the "full" sentinel so the usable
    // count is capacity - 1).
    [[nodiscard]] bool init(std::size_t capacity) noexcept {
        assert(capacity >= 2);
        assert((capacity & (capacity - 1)) == 0 && "capacity must be a power of two");
        if (buf_ != nullptr) return false;       // already initialised
        void* raw = alloc_->alloc(sizeof(T) * capacity, alignof(T));
        if (raw == nullptr) return false;
        buf_     = static_cast<T*>(raw);
        mask_    = capacity - 1;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

    // Producer side. Returns false when full.
    [[nodiscard]] bool try_push(const T& v) noexcept {
        assert(buf_ != nullptr);
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        if (((h + 1) & mask_) == (t & mask_)) return false;
        buf_[h & mask_] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when empty.
    [[nodiscard]] bool try_pop(T* out) noexcept {
        assert(buf_ != nullptr);
        assert(out  != nullptr);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        if ((t & mask_) == (h & mask_)) return false;
        *out = buf_[t & mask_];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Approximate size for observability (precise only when no producer/consumer is active).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & mask_;
    }

    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

private:
    // 64B-padded head/tail to prevent false sharing between the two ends.
    alignas(kRingCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kRingCacheLine) std::atomic<std::size_t> tail_{0};

    A*          alloc_;
    T*          buf_;
    std::size_t mask_;
};

} // namespace ycpp
