#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <deque>
#include <functional>
#include <memory>

namespace rtes {

// Thread safety annotations
// Clang thread-safety analysis attributes; GCC does not support them.
#if defined(__clang__)
#define THREAD_ANNOTATION(x) __attribute__((x))
#else
#define THREAD_ANNOTATION(x)
#endif
#define GUARDED_BY(x) THREAD_ANNOTATION(guarded_by(x))
#define REQUIRES(x) THREAD_ANNOTATION(requires_capability(x))
#define EXCLUDES(x) THREAD_ANNOTATION(locks_excluded(x))
#define ACQUIRE(x) THREAD_ANNOTATION(acquire_capability(x))
#define RELEASE(x) THREAD_ANNOTATION(release_capability(x))

// Deadlock detection
class DeadlockDetector {
public:
    static DeadlockDetector& instance();
    
    void register_lock_acquisition(std::thread::id tid, void* lock_addr);
    void register_lock_release(std::thread::id tid, void* lock_addr);
    bool would_cause_deadlock(std::thread::id tid, void* lock_addr);
    
private:
    std::mutex detector_mutex_;
    std::unordered_map<std::thread::id, std::vector<void*>> thread_locks_;
    std::unordered_map<void*, std::thread::id> lock_owners_;
};

// Scoped lock with deadlock detection
template<typename... Mutexes>
class scoped_lock {
public:
    explicit scoped_lock(Mutexes&... mutexes) : mutexes_(mutexes...) {
        auto tid = std::this_thread::get_id();
        
        // Check for potential deadlocks
        std::apply([this, tid](auto&... mtx) {
            ((check_deadlock(tid, &mtx)), ...);
        }, mutexes_);
        
        // Acquire locks in consistent order
        std::lock(mutexes...);
        
        // Register acquisitions
        std::apply([this, tid](auto&... mtx) {
            ((register_acquisition(tid, &mtx)), ...);
        }, mutexes_);
    }
    
    ~scoped_lock() {
        auto tid = std::this_thread::get_id();
        std::apply([this, tid](auto&... mtx) {
            ((register_release(tid, &mtx)), ...);
        }, mutexes_);
    }
    
    scoped_lock(const scoped_lock&) = delete;
    scoped_lock& operator=(const scoped_lock&) = delete;

private:
    std::tuple<Mutexes&...> mutexes_;
    
    void check_deadlock(std::thread::id tid, void* lock_addr) {
        if (DeadlockDetector::instance().would_cause_deadlock(tid, lock_addr)) {
            throw std::runtime_error("Potential deadlock detected");
        }
    }
    
    void register_acquisition(std::thread::id tid, void* lock_addr) {
        DeadlockDetector::instance().register_lock_acquisition(tid, lock_addr);
    }
    
    void register_release(std::thread::id tid, void* lock_addr) {
        DeadlockDetector::instance().register_lock_release(tid, lock_addr);
    }
};

// Atomic wrapper with memory ordering
template<typename T>
class atomic_wrapper {
public:
    atomic_wrapper() = default;
    atomic_wrapper(T value) : value_(value) {}
    
    T load(std::memory_order order = std::memory_order_seq_cst) const {
        return value_.load(order);
    }
    
    void store(T value, std::memory_order order = std::memory_order_seq_cst) {
        value_.store(value, order);
    }
    
    T exchange(T value, std::memory_order order = std::memory_order_seq_cst) {
        return value_.exchange(value, order);
    }
    
    bool compare_exchange_weak(T& expected, T desired, 
                              std::memory_order success = std::memory_order_seq_cst,
                              std::memory_order failure = std::memory_order_seq_cst) {
        return value_.compare_exchange_weak(expected, desired, success, failure);
    }
    
    bool compare_exchange_strong(T& expected, T desired,
                                std::memory_order success = std::memory_order_seq_cst,
                                std::memory_order failure = std::memory_order_seq_cst) {
        return value_.compare_exchange_strong(expected, desired, success, failure);
    }
    
    explicit operator bool() const requires std::is_same_v<T, bool> { return load(); }
    operator T() const { return load(); }

private:
    std::atomic<T> value_;
};

// Condition variable with predicate checking
class condition_variable_safe {
public:
    template<typename Predicate>
    void wait(std::unique_lock<std::mutex>& lock, Predicate pred) {
        cv_.wait(lock, pred);
    }
    
    template<typename Rep, typename Period, typename Predicate>
    bool wait_for(std::unique_lock<std::mutex>& lock,
                  const std::chrono::duration<Rep, Period>& timeout,
                  Predicate pred) {
        return cv_.wait_for(lock, timeout, pred);
    }
    
    void notify_one() { cv_.notify_one(); }
    void notify_all() { cv_.notify_all(); }

private:
    std::condition_variable cv_;
};

// Shutdown coordination
class ShutdownManager {
public:
    static ShutdownManager& instance();
    
    void register_component(const std::string& name, std::function<void()> shutdown_fn);
    void initiate_shutdown();
    void wait_for_shutdown();
    bool is_shutdown_requested() const;
    
private:
    mutable std::shared_mutex mutex_;
    atomic_wrapper<bool> shutdown_requested_{false};
    std::unordered_map<std::string, std::function<void()>> components_ GUARDED_BY(mutex_);
    condition_variable_safe shutdown_complete_;
    std::mutex shutdown_mutex_;
    atomic_wrapper<int> active_components_{0};
};

// Work drainage helper
class WorkDrainer {
public:
    WorkDrainer(const std::string& name) : name_(name) {
        ShutdownManager::instance().register_component(name_, [this]() { drain_work(); });
    }
    
    void add_work_item(std::function<void()> work) {
        std::unique_lock lock(work_mutex_);
        if (draining_.load()) return;
        work_queue_.push_back(std::move(work));
        work_cv_.notify_one();
    }
    
    void process_work() {
        std::unique_lock lock(work_mutex_);
        work_cv_.wait(lock, [this] { return !work_queue_.empty() || draining_; });
        
        while (!work_queue_.empty() && !draining_) {
            auto work = std::move(work_queue_.front());
            work_queue_.pop_front();
            lock.unlock();
            work();
            lock.lock();
        }
    }

private:
    std::string name_;
    std::mutex work_mutex_;
    condition_variable_safe work_cv_;
    std::deque<std::function<void()>> work_queue_ GUARDED_BY(work_mutex_);
    atomic_wrapper<bool> draining_{false};
    
    void drain_work() {
        draining_.store(true);
        work_cv_.notify_all();
        
        std::unique_lock lock(work_mutex_);
        work_queue_.clear();
    }
};

// Lock ordering validator
class LockOrderValidator {
public:
    static LockOrderValidator& instance();
    
    void register_lock_order(void* first, void* second);
    bool validate_lock_order(const std::vector<void*>& locks);
    
private:
    std::mutex validator_mutex_;
    std::unordered_map<void*, std::vector<void*>> lock_graph_ GUARDED_BY(validator_mutex_);
};

// Race condition detector
class RaceDetector {
public:
    static RaceDetector& instance();
    
    void register_memory_access(void* addr, bool is_write);
    void register_synchronization_point();
    bool has_race_condition() const;
    
private:
    struct MemoryAccess {
        std::thread::id thread_id;
        bool is_write;
        std::chrono::steady_clock::time_point timestamp;
    };
    
    mutable std::shared_mutex detector_mutex_;
    std::unordered_map<void*, std::vector<MemoryAccess>> memory_accesses_ GUARDED_BY(detector_mutex_);
    atomic_wrapper<bool> race_detected_{false};
};

} // namespace rtes