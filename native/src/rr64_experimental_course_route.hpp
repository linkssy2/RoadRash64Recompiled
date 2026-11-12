#pragma once

#ifdef __cplusplus
#include <cstdint>

namespace rr64::experimental_course {
struct SpawnPose {
    float x = 0, z = 0, height = 0, heading = 0;
};
// Immutable data owned by the loaded course. Version-one packs retain their
// compensated native grid; version two supplies explicit checked grid poses.
struct RouteData {
    const std::uint8_t* records_be = nullptr;
    std::uint32_t byte_count = 0;
    std::uint32_t record_count = 0;
    std::uint32_t wrap_segment = 0;
    std::uint32_t finish_segment = 0;
    std::uint32_t initial_adjustment = 0;
    std::uint32_t prior_laps_required = 0;
    float lap_period = 0;
    float lap_threshold = 0;
    float finish_threshold = 0;
    float finish_parameter = 0;
    float start[3]{};
    float finish[3]{};
    const float* record_heights = nullptr;
    std::uint32_t height_count = 0;
    // One byte per quadratic curve (0 = airborne/unsupported, 1 = checked
    // continuous support). This restricts recovery, never normal lap routing.
    const std::uint8_t* recovery_support = nullptr;
    std::uint32_t recovery_support_count = 0;
    SpawnPose grid[14]{};
    std::uint32_t spawn_count = 0;
    bool native_laps = false;
    // Loaded-course rule, independent of the draft menu choice.
    bool delayed_fall_recovery = false;
    // Native missing-contact fallback plane, below all authored course surfaces.
    float fall_floor = -8;
};
// Throws std::runtime_error if the immutable pack is not supported. The core
// calls this before replacing ROM data; the installer repeats it defensively.
void validate_route(const RouteData& data);
// Restore only descriptor coordinates that still exactly match our last
// write. Native menu changes therefore cannot be overwritten on stock return.
void restore_descriptor(unsigned char* memory) noexcept;
// Call before initializing a new guest session, whose RDRAM may reuse an address.
void reset_descriptor() noexcept;
} // namespace rr64::experimental_course

extern "C" {
#endif
// At func_80066040 entry: if this returns 1, skip the stock route constructor.
// Inactive packs return 0 without touching memory/context. An invalid active
// route throws an initialization error; it never falls back onto stock roads
// while custom terrain remains active. Preserves the complete caller context.
#ifdef __cplusplus
int rr64_experimental_course_build_route(unsigned char* rdram, void* context) noexcept(false);
void rr64_experimental_course_spawn(unsigned char* rdram, void* context) noexcept(false);
int rr64_experimental_course_recovery_point(unsigned char* rdram, void* context) noexcept(false);
void rr64_experimental_course_ground_hint(unsigned char* rdram, void* context) noexcept(false);
int rr64_experimental_course_fall_action(unsigned char* rdram, unsigned actor);
void rr64_experimental_course_missing_floor(unsigned char* rdram, void* context, unsigned kind);
int rr64_experimental_course_progress(unsigned char* rdram, void* context);
int rr64_experimental_course_recovery_progress(unsigned char* rdram, void* context);
#else
int rr64_experimental_course_build_route(unsigned char* rdram, void* context);
void rr64_experimental_course_spawn(unsigned char* rdram, void* context);
int rr64_experimental_course_recovery_point(unsigned char* rdram, void* context);
void rr64_experimental_course_ground_hint(unsigned char* rdram, void* context);
int rr64_experimental_course_fall_action(unsigned char* rdram, unsigned actor);
void rr64_experimental_course_missing_floor(unsigned char* rdram, void* context, unsigned kind);
int rr64_experimental_course_progress(unsigned char* rdram, void* context);
int rr64_experimental_course_recovery_progress(unsigned char* rdram, void* context);
#endif
#ifdef __cplusplus
}
#endif
