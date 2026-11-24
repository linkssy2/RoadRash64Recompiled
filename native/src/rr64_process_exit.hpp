#pragma once

// macOS: Apple's libc has no C11 quick_exit/at_quick_exit, so libc++ does not
// declare std::quick_exit or std::at_quick_exit there. N64ModernRuntime already
// falls back to std::_Exit on Apple; these wrappers keep that exit semantics
// (no static destructors, no stdio flush) while still running the handlers RR64
// registers, in reverse registration order as quick_exit would.

#include <cstdlib>

#if defined(__APPLE__)
#include <array>
#include <atomic>

namespace rr64::process {
inline std::array<void (*)(), 32> quick_exit_handlers{};
inline std::atomic<unsigned> quick_exit_handler_count{0};

inline int at_quick_exit(void (*handler)()) noexcept {
    const unsigned slot = quick_exit_handler_count.fetch_add(1);
    if (slot >= quick_exit_handlers.size()) {
        return -1;
    }
    quick_exit_handlers[slot] = handler;
    return 0;
}

[[noreturn]] inline void quick_exit(int status) noexcept {
    unsigned count = quick_exit_handler_count.load();
    if (count > quick_exit_handlers.size()) {
        count = static_cast<unsigned>(quick_exit_handlers.size());
    }
    while (count > 0) {
        if (auto handler = quick_exit_handlers[--count]) {
            handler();
        }
    }
    std::_Exit(status);
}
} // namespace rr64::process
#else
namespace rr64::process {
inline int at_quick_exit(void (*handler)()) noexcept {
    return std::at_quick_exit(handler);
}

[[noreturn]] inline void quick_exit(int status) noexcept {
    std::quick_exit(status);
}
} // namespace rr64::process
#endif
