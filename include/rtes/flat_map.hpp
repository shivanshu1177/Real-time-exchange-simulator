#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rtes {

// Fixed-capacity open-addressing hash map from uint64_t keys to V.
// All memory is allocated in the constructor; insert/find/erase never allocate.
// Linear probing with backward-shift deletion (no tombstones), so lookups stay
// short after many insert/erase cycles. insert() fails when the map holds
// max_size() entries (load factor 1/2).
template<typename V>
class FlatMap {
public:
    explicit FlatMap(size_t max_entries) {
        size_t cap = 16;
        while (cap < max_entries * 2) cap <<= 1;
        capacity_ = cap;
        mask_     = cap - 1;
        max_size_ = cap / 2;
        slots_    = std::make_unique<Slot[]>(cap);
    }

    size_t size() const noexcept { return size_; }
    size_t max_size() const noexcept { return max_size_; }
    bool   empty() const noexcept { return size_ == 0; }

    // Returns false if the key exists or the map is full.
    bool insert(uint64_t key, const V& value) noexcept {
        if (size_ >= max_size_) return false;
        size_t i = hash(key) & mask_;
        while (slots_[i].used) {
            if (slots_[i].key == key) return false;
            i = (i + 1) & mask_;
        }
        slots_[i].used  = true;
        slots_[i].key   = key;
        slots_[i].value = value;
        ++size_;
        return true;
    }

    V* find(uint64_t key) noexcept {
        size_t i = hash(key) & mask_;
        while (slots_[i].used) {
            if (slots_[i].key == key) return &slots_[i].value;
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    const V* find(uint64_t key) const noexcept {
        return const_cast<FlatMap*>(this)->find(key);
    }

    bool erase(uint64_t key) noexcept {
        size_t i = hash(key) & mask_;
        while (slots_[i].used) {
            if (slots_[i].key == key) {
                remove_at(i);
                return true;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    // Calls f(key, value&) for every entry. f must not insert or erase.
    template<typename F>
    void for_each(F&& f) {
        for (size_t i = 0; i < capacity_; ++i) {
            if (slots_[i].used) f(slots_[i].key, slots_[i].value);
        }
    }

    void clear() noexcept {
        for (size_t i = 0; i < capacity_; ++i) slots_[i].used = false;
        size_ = 0;
    }

private:
    struct Slot {
        uint64_t key{0};
        V        value{};
        bool     used{false};
    };

    std::unique_ptr<Slot[]> slots_;
    size_t capacity_{0};
    size_t mask_{0};
    size_t max_size_{0};
    size_t size_{0};

    static size_t hash(uint64_t x) noexcept {
        // splitmix64 finaliser
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27; x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<size_t>(x);
    }

    // Backward-shift deletion: move later entries of the probe chain into the
    // hole when their home slot is at or before it (cyclically).
    void remove_at(size_t hole) noexcept {
        slots_[hole].used = false;
        --size_;
        size_t i = (hole + 1) & mask_;
        while (slots_[i].used) {
            size_t home = hash(slots_[i].key) & mask_;
            // Distance from home to i, and from home to hole, cyclically.
            size_t dist_i    = (i - home) & mask_;
            size_t dist_hole = (hole - home) & mask_;
            if (dist_hole < dist_i) {
                slots_[hole] = slots_[i];
                slots_[i].used = false;
                hole = i;
            }
            i = (i + 1) & mask_;
        }
    }
};

} // namespace rtes
