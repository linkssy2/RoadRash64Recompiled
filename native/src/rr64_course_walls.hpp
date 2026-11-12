#pragma once

#ifdef __cplusplus
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace rr64::course_walls {
using Vec = std::array<float, 3>; // Rider-world x, z, height.
struct Triangle {
    std::uint32_t id = 0;
    std::array<Vec, 3> vertices{};
};
struct Sphere {
    Vec center{};
    float radius = 0;
};
// A finite panel proven from two original collision triangles. Sloping roads
// need a height at the contact, not the maximum Z of the entire triangle.
struct Rail {
    std::uint32_t id = 0;
    std::array<std::uint32_t, 2> triangle_ids{};
    std::array<Vec, 2> base{}, top{};
};
class World;
// The loader builds once, then publishes immutable data for the selected course.
// Throws on malformed, nonfinite, degenerate, duplicate-ID or non-wall geometry.
std::shared_ptr<const World> build_world(std::span<const Triangle> triangles,
                                       std::span<const Rail> rails = {});
// All authored faces, including floor/ceiling normals, for moving obstacles.
std::shared_ptr<const World> build_surface_world(std::span<const Triangle> triangles);
const World *world() noexcept;
const World *surface_world() noexcept;
struct SweepHit {
    bool hit = false;
    float fraction = 1, penetration = 0;
    Vec normal{}, point{};
    std::uint32_t triangle_id = 0;
    unsigned triangle_tests = 0;
};
SweepHit sweep_sphere(const World &, Sphere, Vec displacement) noexcept;
struct Motion {
    Vec displacement{}, velocity{}, normal{}, point{};
    std::uint32_t triangle_id = 0;
    float normal_speed = 0, penetration = 0;
    unsigned contacts = 0, triangle_tests = 0;
    struct RailContact {
        Vec normal{}, point{};
        float top = 0;
        unsigned sphere_index = 0;
        std::uint32_t rail_id = 0;
    } rail;
    bool vaulted_rail = false;
};
struct RailPolicy {
    void *context = nullptr;
    bool (*allows)(void *, const Motion::RailContact &) = nullptr;
};
// Continuous compound-sphere sweep with bounded sliding. No allocations or
// global mutations: the same inputs work in live physics and isolated replay.
Motion resolve(const World &, std::span<const Sphere>, Vec displacement, Vec velocity,
               RailPolicy = {}) noexcept;
} // namespace rr64::course_walls
extern "C" {
#endif
// Paired around native34594 (kind0 bike / kind1 detached rider) and34370
// (kind2 bike / kind3 detached rider), before pose/rotation reconstruction.
// Guest context is preserved. No preceding-frame pose is used for sweeps.
void rr64_course_walls_begin(unsigned char *memory, void *context, unsigned kind);
void rr64_course_walls_end(unsigned char *memory, void *context, unsigned kind);
#ifdef __cplusplus
}
#endif
