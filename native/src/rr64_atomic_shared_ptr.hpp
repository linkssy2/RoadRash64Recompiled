#pragma once

// RR64-MAC: an atomically replaced std::shared_ptr.
//
// C++20's std::atomic<std::shared_ptr<T>> exists in MSVC's STL and GCC's
// libstdc++, but no libc++ implements it (Apple's or LLVM's, as of libc++ 20),
// so the macOS builds cannot use it. Where the standard library has it, this
// is exactly that type; elsewhere a mutex guards the pointer. Only load(),
// store() and exchange() are offered, which is all the game needs; the memory
// order arguments are accepted for the same call syntax (a mutex is at least
// as strong as any of them).

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace rr64 {

#if defined(__cpp_lib_atomic_shared_ptr)

template <typename T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;

#else

template <typename T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() noexcept = default;
    AtomicSharedPtr(std::shared_ptr<T> value) noexcept : value_(std::move(value)) {}
    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

    std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    void store(std::shared_ptr<T> value, std::memory_order = std::memory_order_seq_cst) noexcept {
        std::shared_ptr<T> previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = std::exchange(value_, std::move(value));
        }
        // The old object (if this was its last owner) is destroyed here,
        // outside the lock.
    }

    std::shared_ptr<T> exchange(std::shared_ptr<T> value, std::memory_order = std::memory_order_seq_cst) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(value_, std::move(value));
    }

    operator std::shared_ptr<T>() const noexcept { return load(); }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<T> value_;
};

#endif

} // namespace rr64
