// Retained actor-only regression fixtures deliberately exercise the accepted
// actor path with the independently controlled world option disabled.
#include "highlight_hooks_disabled.hpp"
extern "C" {
// Generated actor functions retain current hook calls. These isolated math
// tests exercise normal drawing, without replay, weapons or online smoothing.
int rr64_highlights_presenting() { return 0; }
unsigned rr64_highlights_hidden(unsigned char *, unsigned, unsigned hidden) { return hidden; }
void rr64_highlights_actor_end() {}
unsigned rr64_highlights_lod(unsigned char *, unsigned, unsigned tier) { return tier; }
int rr64_highlights_actor(unsigned char *, unsigned, unsigned) { return 1; }
void rr64_online_presentation_matrix(unsigned char *, unsigned, unsigned, unsigned, unsigned) {}
void rr64_weapon_begin(unsigned char *, unsigned, unsigned) {}
void rr64_weapon_end() {}
int rr64_world_distance_enabled() { return 0; }
void rr64_world_invalidate(unsigned char*) {}
void rr64_world_observe_allocation(unsigned char*, unsigned, unsigned, unsigned) {}
void rr64_world_observe_roots(unsigned char*, unsigned) {}
void rr64_world_begin_draw(unsigned char*) {}
void rr64_world_end_draw(unsigned char*) {}
unsigned rr64_world_actor_hidden(unsigned char*, unsigned, unsigned hidden, const void*) { return hidden; }
unsigned rr64_world_select(unsigned char*, unsigned, unsigned stock) { return stock; }
unsigned rr64_world_root_source(unsigned char*, unsigned, unsigned, unsigned source) { return source; }
void rr64_world_scale_root_matrix(unsigned char*, unsigned, unsigned, unsigned) {}
void rr64_world_end_actor() {}
void rr64_world_camera_far(unsigned char*, void*) {}
void rr64_world_camera_normalization(unsigned char*, void*) {}
void rr64_world_terrain_begin(unsigned char*) {}
void rr64_world_terrain_observe(unsigned char*, unsigned) {}
void rr64_world_terrain_draw(unsigned char*) {}
void rr64_world_objects_begin(unsigned char*) {}
void rr64_world_objects_observe(unsigned char*, unsigned) {}
void rr64_world_objects_sample(unsigned char*, unsigned, unsigned) {}
void rr64_world_objects_draw(unsigned char*) {}
}
