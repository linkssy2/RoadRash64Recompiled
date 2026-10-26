#pragma once

#ifdef __cplusplus
#include <array>
namespace rr64::race_end_trace {
// UI thread, beginning before gameplay: caches diagnostic enable flags, then
// drains at most two captured transitions per call. Never reads live RDRAM.
void drain();
// Latest UI-observed authority mask, used only to select diagnostic rows.
// Zero falls back to offline local-controller/AI flags. No gameplay ownership
// is inferred or modified by this diagnostic publication.
void set_authority_humans(unsigned mask);
// Guest-thread diagnostic observation only. Counts are accumulated until the
// next periodic imported-course sample; no guest writes, allocation or I/O.
void observe_course_wall(unsigned char* memory, unsigned actor, unsigned kind,
    unsigned static_contacts, unsigned dynamic_contacts, unsigned triangle,
    unsigned hazard, const float* requested, const float* resolved,
    const float* normal, float normal_speed);
struct CourseWallContext {
    unsigned body = 0, route = 0, sphere_count = 0;
    unsigned static_contacts = 0, dynamic_contacts = 0, triangle = 0, hazard = 0;
    unsigned triangle_tests = 0;
    std::array<std::array<float, 4>, 3> spheres{}; // Start XYZ and radius.
    std::array<float, 3> previous{}, requested{}, resolved{}, point{}, normal{};
    float delta = 0;
};
// Exact inputs/results of an already completed geometry query, never a new
// collision query. Human contact edges retain preceding/current samples in a
// bounded UI-drained queue; periodic context remains available after its cap.
void observe_course_wall_context(unsigned char* memory, unsigned actor, unsigned kind,
                                const CourseWallContext& context);
}
extern "C" {
#endif

// Sole producer: the live guest game thread, at entry to func_80048544, before
// pending_mode changes. Copies bounded state only; never writes guest memory,
// allocates, or performs file I/O. Disposable prediction replay is excluded.
// return_address is raw guest r31; translated direct calls may leave it stale.
void rr64_trace_race_end(unsigned char* memory, unsigned target_mode,
                         unsigned return_address);
// Same producer, once per outer game-loop observation (including menus, which
// reset the race baseline). Online authority waits may skip a simulation step.
// Human segment/lap/status/marker changes are queued by the existing trace.
// RR64_COURSE_PHYSICS_TRACE=1 additionally samples imported-course human
// physics every 30 live observations, capped at 1200 attempts per process.
void rr64_trace_lap_frame(unsigned char* memory);
// Opt-in population trace: phase 0 observes frames, 1/2 bracket cop placement.
void rr64_trace_population(unsigned char* memory, unsigned phase);
void rr64_trace_police_crash(unsigned char* memory, unsigned bike, unsigned site);
void rr64_trace_traffic_target(unsigned char* memory, unsigned car, unsigned before_road);
// Mark recovery before its progress update. The next frame sample records the
// coherent resulting state; this hook itself emits no rows or snapshots.
void rr64_trace_race_recovery(unsigned char* memory, unsigned actor);
#ifdef __cplusplus
}
#endif
