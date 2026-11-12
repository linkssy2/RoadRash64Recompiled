#pragma once

// Private course-import prototype. This header is intentionally usable by the
// generated C hooks as well as the C++ loader. Normal builds omit every hook.
#include "rr64_experimental_course_route.hpp"
#ifdef __cplusplus
namespace rr64::experimental_course {
bool active() noexcept;
bool installed() noexcept;
unsigned terrain_rom_offset() noexcept;
unsigned terrain_texture_count() noexcept;
// Source model units to rider-world units; older private packs used 0.05.
float source_to_world_scale() noexcept;
bool cell_allowed(unsigned cell) noexcept;
void load_selection() noexcept;
void load_stock() noexcept;
const RouteData *route_data() noexcept;
// Called after normal ROM validation, before the native entrypoint/cache reads.
// An explicitly requested but invalid pack fails initialization atomically.
void initialize();
}
extern "C" {
unsigned rr64_experimental_course_rom_base(void);
#endif
int rr64_experimental_course_floor_cell_allowed(unsigned cell);
void rr64_experimental_course_floor_indices(unsigned char *rdram, void *context);
void rr64_experimental_course_floor_subindices(unsigned char *rdram, void *context);
#ifdef __cplusplus
void rr64_experimental_course_prepare_race(unsigned char *rdram) noexcept(false);
void rr64_experimental_course_mode(unsigned char *rdram, void *context) noexcept(false);
#else
unsigned rr64_experimental_course_rom_base(void);
void rr64_experimental_course_prepare_race(unsigned char *rdram);
void rr64_experimental_course_mode(unsigned char *rdram, void *context);
#endif
#ifdef __cplusplus
}
#endif
