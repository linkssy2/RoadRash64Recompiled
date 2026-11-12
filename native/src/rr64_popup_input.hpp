#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
namespace rr64::popup_input {
inline std::atomic_bool wait_for_release{false};
inline std::atomic_bool discard_menu_action{false};
inline void closed() {
    wait_for_release.store(true, std::memory_order_release);
    discard_menu_action.store(true, std::memory_order_release);
}
inline bool suppress(bool ui_capturing, bool input_available, std::uint16_t buttons, float x,
                     float y) {
    if (!wait_for_release.load(std::memory_order_acquire))
        return false;
    // A UI-blocked poll returns artificial zeros, not a physical release.
    if (!ui_capturing && input_available && buttons == 0 && std::isfinite(x) && std::isfinite(y) &&
        std::abs(x) < 0.25f && std::abs(y) < 0.25f)
        wait_for_release.store(false, std::memory_order_release);
    return true; // Also consume the release poll; require a fresh action.
}
inline bool consume_menu_action() {
    const bool stale = discard_menu_action.exchange(false, std::memory_order_acq_rel);
    return stale || wait_for_release.load(std::memory_order_acquire);
}
}
