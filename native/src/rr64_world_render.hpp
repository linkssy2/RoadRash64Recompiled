#pragma once

// R18 world presentation is independent from the accepted rider/bike option.
// Hooks never create entities or change their simulation/streaming lifecycle.
#ifdef __cplusplus
extern "C" {
#endif
int rr64_world_distance_enabled();
// Monotonic totals: 0 pedestrian, 1 traffic, 2 object promotions; 3 fallbacks;
// 4 resource/animation refusals (a subset of fallbacks).
unsigned long long rr64_world_counter(unsigned index);
void rr64_world_invalidate(unsigned char *rdram);
void rr64_world_observe_allocation(unsigned char *rdram, unsigned node, unsigned view,
                                   unsigned bytes);
void rr64_world_observe_roots(unsigned char *rdram, unsigned type);
void rr64_world_begin_draw(unsigned char *rdram);
void rr64_world_end_draw(unsigned char *rdram);
unsigned rr64_world_actor_hidden(unsigned char *rdram, unsigned node, unsigned hidden,
                                 const void *context);
unsigned rr64_world_select(unsigned char *rdram, unsigned node, unsigned stock_lod);
unsigned rr64_world_root_source(unsigned char *rdram, unsigned node, unsigned record,
                                unsigned source);
void rr64_world_scale_root_matrix(unsigned char *rdram, unsigned node, unsigned record,
                                  unsigned matrix);
void rr64_world_end_actor();
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "rr64_actor_render_snapshot.hpp"
namespace rr64::world {
// Camera support is independent of extended-world admission. Keep the
// per-view camera layout, but use supported_scene for distant geometry:
// split-screen returns to stock terrain/scenery to bound rendering cost.
inline bool static_scene(unsigned char *memory) noexcept {
    std::uint32_t view = 0, views = 0;
    return lod::supported_scene(memory) && engine::read_u32(memory, 0x8009DB88u, views) &&
           engine::read_u32(memory, engine::globals::active_viewport, view) && view < views;
}
inline bool supported_scene(unsigned char *memory) noexcept {
    std::uint32_t views = 0;
    return lod::supported_scene(memory) && engine::read_u32(memory, 0x8009DB88u, views) &&
           views == 1u;
}
}
#endif
