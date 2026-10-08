#pragma once

#ifdef __cplusplus
#include <array>
#include <cstdint>

namespace rr64::highlight_render {
// Reserved projection identities, carried by the actual display list rather
// than a live global which could change before the renderer consumes the task.
constexpr std::uint32_t marker_prefix = 0x484c0000u;
// These identify known transitions, not a certificate of continuous motion.
// In particular, unchanged controls do not authorize a static-world AUTO bypass.
struct NormalCamera {
    std::uint32_t mode = 0, owner = ~0u, flags = 0, generation = 0, bike = 0, rider = 0;
    bool valid = false;
    bool operator==(const NormalCamera &) const = default;
};
struct NormalPhase {
    std::uint32_t round = 0, layout = 0, views = 0;
    std::array<NormalCamera, 4> cameras{};
    bool operator==(const NormalPhase &) const = default;
};
class Boundary {
    const void *mapping_ = nullptr;
    std::uint32_t epoch_ = 0, key_ = 0, serial_ = 0;
    bool armed_ = false;
    bool normal_ = false;
    NormalPhase phase_{};
    void advance() noexcept { if (++serial_ > 0xffffu) serial_ = 1; }
public:
    std::uint32_t observe(const void *mapping, std::uint32_t epoch,
                          std::uint32_t replay_key, const NormalPhase *normal = nullptr) noexcept {
        if (mapping != mapping_ || epoch < epoch_) {
            mapping_ = mapping;
            key_ = 0;
            armed_ = false;
            normal_ = false;
            phase_ = {};
            advance();
        }
        epoch_ = epoch;
        if (replay_key != key_ || normal_ != (normal != nullptr) ||
            (normal && *normal != phase_)) {
            key_ = replay_key;
            normal_ = normal != nullptr;
            phase_ = normal ? *normal : NormalPhase{};
            armed_ = true;
            advance();
        }
        return armed_ ? marker_prefix | serial_ : 0u;
    }
};
// Called under the recorder lock; copies its already-authored native reset
// generations. The graphics task owns the resulting phase and marker.
void begin_normal(unsigned char *memory, unsigned round,
                  const std::array<std::uint32_t, 14> &generations);
}

extern "C" {
#endif
// Graphics-worker-only scope around one native 6A638 draw. Zero is ordinary
// racing/results; a nonzero key identifies the replay clip and camera angle.
void rr64_highlight_render_begin(unsigned char *memory, unsigned replay_key);
void rr64_highlight_render_projection(unsigned char *memory);
void rr64_highlight_render_end(unsigned char *memory);
#ifdef __cplusplus
}
#endif
