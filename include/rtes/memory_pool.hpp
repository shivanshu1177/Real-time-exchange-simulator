#pragma once

#include "types.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace rtes {

// Fixed-capacity object pool with a single owner thread.
//
// It is NOT thread-safe, by design: every matching engine owns its own pool,
// and only that engine's thread acquires and releases. Orders cross threads
// by value (OrderRequest), never as pool pointers, so no lock-free free list
// (and no ABA problem) is needed. Debug builds assert the single-owner rule
// and catch double releases. See docs/design/memory.md.
template<typename T>
class MemoryPool {
public:
    explicit MemoryPool(size_t capacity)
        : capacity_(capacity),
          storage_(std::make_unique<T[]>(capacity)),
          free_(std::make_unique<uint32_t[]>(capacity)),
          in_use_(std::make_unique<bool[]>(capacity)) {
        // Hand out low indices first.
        for (size_t i = 0; i < capacity; ++i) free_[i] = static_cast<uint32_t>(capacity - 1 - i);
        free_count_ = capacity;
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // Returns nullptr when the pool is exhausted — no heap fallback.
    T* acquire() noexcept {
        check_owner();
        if (free_count_ == 0) return nullptr;
        const uint32_t i = free_[--free_count_];
        in_use_[i] = true;
        return &storage_[i];
    }

    void release(T* p) noexcept {
        if (!p) return;
        check_owner();
        const size_t i = static_cast<size_t>(p - storage_.get());
        assert(i < capacity_ && "pointer does not belong to this pool");
        assert(in_use_[i] && "double release");
        if (i >= capacity_ || !in_use_[i]) return;
        in_use_[i] = false;
        free_[free_count_++] = static_cast<uint32_t>(i);
    }

    size_t available() const noexcept { return free_count_; }
    size_t capacity() const noexcept { return capacity_; }

private:
    size_t                      capacity_;
    std::unique_ptr<T[]>        storage_;
    std::unique_ptr<uint32_t[]> free_;       // stack of free indices
    std::unique_ptr<bool[]>     in_use_;
    size_t                      free_count_{0};

#ifndef NDEBUG
    std::thread::id owner_{};
    void check_owner() noexcept {
        // The first thread to use the pool owns it.
        if (owner_ == std::thread::id{}) owner_ = std::this_thread::get_id();
        assert(owner_ == std::this_thread::get_id() && "MemoryPool used from two threads");
    }
#else
    void check_owner() noexcept {}
#endif
};

using OrderPool = MemoryPool<Order>;

} // namespace rtes
