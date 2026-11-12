#pragma once

#ifdef __cplusplus
namespace rr64::world {
inline constexpr float far_distance = 30000.0f;
}
extern "C" {
#endif
// Only the local arguments of the original camera producer are extended.
// Simulation, stock streaming range and camera origins are not changed.
void rr64_world_camera_far(unsigned char *rdram, void *context);
void rr64_world_camera_normalization(unsigned char *rdram, void *context);
int rr64_world_camera_ready(unsigned char *rdram, unsigned source, unsigned slot);
// Fresh completed original camera producer, with either stock or extended far
// plane. This observation is independent of the Extended World Objects option;
// world geometry must continue to use the option-gated readiness above.
int rr64_world_camera_native_ready(unsigned char *rdram, unsigned source, unsigned slot);
// Imported open gaps are not camera obstructions. Called only at the native
// no-floor branch; real terrain clearance and normal chase smoothing remain.
int rr64_world_camera_open_gap(unsigned char *rdram, void *context);
#ifdef __cplusplus
}
#endif
