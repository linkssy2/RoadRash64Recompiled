#pragma once
#include "rr64_msvc_crt_compat.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace rr64::presentation_options {
// The frontend applies persisted values before boot. During gameplay it saves
// changes for restart so an actor/terrain traversal cannot change policy midway.
inline std::atomic_int max_lod{-1};
inline std::atomic_int world_distance{-1};
// This radius is sampled by each terrain pass, so it can be tuned live.
inline std::atomic_int draw_distance{100};
inline bool environment_enabled(const char *name) {
    char *text = nullptr;
    size_t bytes = 0;
    const bool enabled = _dupenv_s(&text, &bytes, name) == 0 && text && std::strcmp(text, "1") == 0;
    std::free(text);
    return enabled;
}
inline bool enabled(const std::atomic_int &configured, bool fallback) {
    const int value = configured.load(std::memory_order_relaxed);
    return value < 0 ? fallback : value != 0;
}
}
