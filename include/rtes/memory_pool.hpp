#pragma once

#include "types.hpp"
#include <atomic>
#include <vector>
#include <cstddef>

namespace rtes {

template<typename T>
class MemoryPool {
    // Each node holds an object as its first member so that
    // reinterpret_cast<Node*>(T*) is valid for standard-layout types.
    struct alignas(64) Node {
        T       object;
        Node*   next{nullptr};
    };

    std::vector<Node>          storage_;
    alignas(64) std::atomic<Node*> head_{nullptr};

public:
    explicit MemoryPool(size_t capacity) : storage_(capacity) {
        for (size_t i = 0; i + 1 < capacity; ++i)
            storage_[i].next = &storage_[i + 1];
        storage_[capacity - 1].next = nullptr;
        head_.store(&storage_[0], std::memory_order_relaxed);
    }

    // Lock-free acquire via CAS on the free-list head. Returns nullptr when
    // the pool is exhausted — no heap fallback on the hot path.
    T* acquire() noexcept {
        Node* node = head_.load(std::memory_order_acquire);
        while (node) {
            if (head_.compare_exchange_weak(node, node->next,
                    std::memory_order_release,
                    std::memory_order_acquire))
                return &node->object;
        }
        return nullptr;
    }

    // Number of free slots. Walks the free list: for tests and diagnostics
    // only, and only when no other thread is using the pool.
    size_t available() const noexcept {
        size_t n = 0;
        for (Node* node = head_.load(std::memory_order_acquire); node; node = node->next) ++n;
        return n;
    }
    size_t capacity() const noexcept { return storage_.size(); }

    // Lock-free release — pushes the node back onto the free-list.
    void release(T* ptr) noexcept {
        if (!ptr) return;
        // Safe: object is the first member of the standard-layout Node struct.
        Node* node = reinterpret_cast<Node*>(ptr);
        Node* head = head_.load(std::memory_order_relaxed);
        do { node->next = head; }
        while (!head_.compare_exchange_weak(head, node,
                std::memory_order_release,
                std::memory_order_relaxed));
    }
};

using OrderPool = MemoryPool<Order>;

} // namespace rtes
