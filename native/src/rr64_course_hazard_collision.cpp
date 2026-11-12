#include "rr64_course_hazard_motion.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rr64::course_hazards {
namespace {
Vec add(Vec a, const Vec &b) {
    for (unsigned i = 0; i < 3; ++i)
        a[i] += b[i];
    return a;
}
Vec sub(Vec a, const Vec &b) {
    for (unsigned i = 0; i < 3; ++i)
        a[i] -= b[i];
    return a;
}
Vec mul(Vec a, float n) {
    for (auto &v : a)
        v *= n;
    return a;
}
float dot(Vec a, Vec b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(Vec a) { return std::sqrt(dot(a, a)); }
using Basis = std::array<Vec, 3>;
Basis basis(const std::array<std::uint16_t, 3> &angle) {
    constexpr float radians = 6.2831853071795864769f / 65536.f;
    const float sx = std::sin((angle[0] & 0xfff0u) * radians),
                cx = std::cos((angle[0] & 0xfff0u) * radians);
    const float sy = std::sin((angle[1] & 0xfff0u) * radians),
                cy = std::cos((angle[1] & 0xfff0u) * radians);
    const float sz = std::sin((angle[2] & 0xfff0u) * radians),
                cz = std::cos((angle[2] & 0xfff0u) * radians);
    // T * original mtxf_rotate_zxy_translate * inverse(T), T(x,y,z)=(x,-z,y).
    return {{{cy * cz + sx * sy * sz, sy * cz - sx * cy * sz, cx * sz},
             {-cx * sy, cx * cy, sx},
             {-cy * sz + sx * sy * cz, -sy * sz - sx * cy * cz, cx * cz}}};
}
Vec world(const Basis &b, Vec v) {
    return add(add(mul(b[0], v[0]), mul(b[1], v[1])), mul(b[2], v[2]));
}
Vec local(const Basis &b, Vec v) { return {dot(b[0], v), dot(b[1], v), dot(b[2], v)}; }
struct Hit {
    bool hit = false;
    float time = 1, penetration = 0;
    Vec normal{}, point{};
};
Vec closest(Vec p, Vec half) {
    for (unsigned i = 0; i < 3; ++i)
        p[i] = std::clamp(p[i], -half[i], half[i]);
    return p;
}
Hit contact(Vec p, Vec half, float radius, float time) {
    Hit out;
    out.hit = true;
    out.time = time;
    out.point = closest(p, half);
    Vec delta = sub(p, out.point);
    const float distance = length(delta);
    if (distance > 1e-7f) {
        out.normal = mul(delta, 1 / distance);
        out.penetration = std::max(0.f, radius - distance);
    } else {
        unsigned axis = 0;
        float distance_inside = half[0] - std::abs(p[0]);
        for (unsigned i = 1; i < 3; ++i)
            if (half[i] - std::abs(p[i]) < distance_inside) {
                axis = i;
                distance_inside = half[i] - std::abs(p[i]);
            }
        out.normal[axis] = p[axis] < 0 ? -1.f : 1.f;
        out.point[axis] = out.normal[axis] * half[axis];
        out.penetration = radius + distance_inside;
    }
    return out;
}
Hit sweep_box(Vec start, Vec motion, Vec half, float radius) {
    if (dot(sub(start, closest(start, half)), sub(start, closest(start, half))) <= radius * radius)
        return contact(start, half, radius, 0);
    // Distance to an AABB is piecewise quadratic. Split at its six face planes
    // and solve the exact sphere entry, including rounded edges and corners.
    // An expanded-box slab alone would falsely hit riders passing a corner.
    std::array<double, 8> cuts{};
    unsigned count = 2;
    cuts[0] = 0;
    cuts[1] = 1;
    for (unsigned i = 0; i < 3; ++i)
        if (std::abs(motion[i]) > 1e-9f)
            for (float side : {-half[i], half[i]}) {
                const double t = (double(side) - start[i]) / motion[i];
                if (t > 0 && t < 1)
                    cuts[count++] = t;
            }
    std::sort(cuts.begin(), cuts.begin() + count);
    for (unsigned j = 0; j + 1 < count; ++j) {
        const double left = cuts[j], right = cuts[j + 1], mid = (left + right) * .5;
        double aa = 0, bb = 0, cc = -double(radius) * radius;
        for (unsigned i = 0; i < 3; ++i) {
            const double p = start[i] + motion[i] * mid;
            if (p > -half[i] && p < half[i])
                continue;
            const double offset = double(start[i]) - (p < 0 ? -half[i] : half[i]);
            aa += double(motion[i]) * motion[i];
            bb += 2 * offset * motion[i];
            cc += offset * offset;
        }
        if (aa < 1e-14f)
            continue;
        const double discriminant = bb * bb - 4 * aa * cc;
        if (discriminant < 0)
            continue;
        const double root = (-bb - std::sqrt(discriminant)) / (2 * aa);
        if (root >= left - 1e-6f && root <= right + 1e-6f && root >= 0 && root <= 1)
            return contact(add(start, mul(motion, float(root))), half, radius, float(root));
    }
    return {};
}
Hit sweep_ball(Vec start, Vec motion, float radius) {
    const auto dot64 = [](Vec a, Vec b) {
        return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
    };
    const double aa = dot64(motion, motion), bb = dot64(start, motion),
                 cc = dot64(start, start) - double(radius) * radius;
    if (cc <= 0) {
        const float n = length(start);
        return {true, 0, radius - n, n > 1e-7f ? mul(start, 1 / n) : Vec{0, 0, 1}, {}};
    }
    if (aa < 1e-12f || bb >= 0 || bb * bb - aa * cc < 0)
        return {};
    const double t = cc / (-bb + std::sqrt(bb * bb - aa * cc));
    if (t < 0 || t > 1)
        return {};
    const auto p = add(start, mul(motion, float(t)));
    return {true, float(t), 0, mul(p, 1 / radius), {}};
}
Hit sweep_capsule(Vec start, Vec motion, float radius, float half_segment) {
    // Exact continuous sphere against a vertical capsule. Keep rounded ends:
    // expanding an AABB would also block riders outside its circular sides.
    Vec nearest{0, 0, std::clamp(start[2], -half_segment, half_segment)};
    const Vec separation = sub(start, nearest);
    const float distance = length(separation);
    if (distance <= radius)
        return {true, 0, radius - distance,
                distance > 1e-7f ? mul(separation, 1 / distance) : Vec{1, 0, 0}, {}};
    Hit best;
    for (float end : {-half_segment, half_segment}) {
        auto point = start;
        point[2] -= end;
        const auto hit = sweep_ball(point, motion, radius);
        if (hit.hit && (!best.hit || hit.time < best.time))
            best = hit;
    }
    const auto side = sweep_ball({start[0], start[1], 0}, {motion[0], motion[1], 0}, radius);
    if (side.hit && std::abs(start[2] + motion[2] * side.time) <= half_segment &&
        (!best.hit || side.time < best.time))
        best = side;
    return best;
}
} // namespace

DynamicMotion resolve_state(const Data &data, const netplay::CourseHazardState &state,
                            std::span<const course_walls::Sphere> spheres, Vec displacement,
                            Vec velocity, float delta) noexcept {
    DynamicMotion out;
    out.displacement = displacement;
    out.velocity = velocity;
    const auto finite = [](Vec v) {
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    };
    if (state.count > netplay::kMaximumCourseHazards || state.count != data.definitions.size() ||
        spheres.empty() || spheres.size() > 3 || !finite(displacement) || !finite(velocity) ||
        !std::isfinite(data.source_to_world_scale) || data.source_to_world_scale <= 0 ||
        data.source_to_world_scale > 1 || !std::isfinite(delta) || delta <= 0 || delta > .25f)
        return out;
    for (const auto &sphere : spheres)
        if (!finite(sphere.center) || !std::isfinite(sphere.radius) || sphere.radius <= 0 ||
            sphere.radius > 10)
            return out;
    for (unsigned i = 0; i < state.count; ++i) {
        const auto &p = state.poses[i];
        const auto &d = data.definitions[i];
        if (!netplay::valid_course_hazard_pose(p) || !finite(d.offset) || !finite(d.half_extent) ||
            !std::isfinite(d.scale) || d.scale <= 0 || d.scale > 100 ||
            !std::isfinite(d.collision_radius) || d.collision_radius < 0 ||
            d.collision_radius > 100)
            return out;
        for (float extent : d.half_extent)
            if (extent < 0 || extent > 10000)
                return out;
    }
    Vec offset{}, remaining = displacement;
    float elapsed = 0, time_left = 1;
    // Continuous translation with the source pose's orientation. Advancing the
    // contact interval avoids repeatedly depenetrating the same original pose,
    // and lets different spheres touch different faces of the same obstacle.
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        Hit earliest;
        unsigned selected = 0;
        Vec surface{};
        bool found = false;
        for (unsigned i = 0; i < state.count; ++i) {
            const auto &p = state.poses[i];
            const auto &d = data.definitions[i];
            if (p.active != 1 || !d.solid)
                continue;
            const Basis rotation = basis(p.rotation);
            const Vec movement = mul(p.velocity, delta);
            const Vec center = sub(add(p.position, world(rotation, mul(d.offset, d.scale))),
                                   mul(movement, 1 - elapsed));
            const Vec relative = sub(remaining, mul(movement, time_left));
            const Vec half = mul(d.half_extent, d.scale);
            for (const auto &sphere : spheres) {
                const Vec start = local(rotation, sub(add(sphere.center, offset), center));
                // Rainbow's source object origin is 15 units below its path.
                // 80089CBC uses radius10 horizontally and a separate +/-30
                // height gate; a radius10 sphere at that origin misses riders
                // on the road. Adapt that finite volume to native body spheres
                // with rounded caps, without changing the visible actor pose.
                Hit hit = d.kind == Kind::Chomp && d.collision_radius > 0
                              ? sweep_capsule(start, local(rotation, relative),
                                              sphere.radius + d.collision_radius,
                                              std::max(0.f, 30 * data.source_to_world_scale -
                                                                d.collision_radius))
                          : (d.kind == Kind::Rock || d.collision_radius > 0)
                              ? sweep_ball(start, local(rotation, relative),
                                           // Source object boundingBoxSize already describes
                                           // gameplay units, independent of its model scale.
                                           sphere.radius +
                                               (d.collision_radius > 0
                                                    ? d.collision_radius
                                                    : 10 * data.source_to_world_scale * d.scale))
                              : sweep_box(start, local(rotation, relative), half, sphere.radius);
                if (!hit.hit || (found && hit.time >= earliest.time))
                    continue;
                hit.normal = world(rotation, hit.normal);
                // End-of-step velocity can point away during hard deceleration.
                // Swept displacement decides crossing; velocity decides impulse.
                if (hit.penetration < .0001f && dot(relative, hit.normal) >= -1e-7f)
                    continue;
                hit.point = sub(add(add(sphere.center, offset), mul(remaining, hit.time)),
                                mul(hit.normal, sphere.radius - hit.penetration));
                earliest = hit;
                selected = i;
                surface = p.velocity;
                found = true;
            }
        }
        if (!found) {
            offset = add(offset, remaining);
            remaining = {};
            break;
        }
        ++out.contacts;
        const float speed = std::max(0.f, -dot(sub(out.velocity, surface), earliest.normal));
        if (out.contacts == 1 || speed > out.normal_speed) {
            out.id = selected;
            out.normal = earliest.normal;
            out.point = earliest.point;
            out.surface_velocity = surface;
            out.normal_speed = speed;
            out.penetration = earliest.penetration;
        }
        offset = add(offset, mul(remaining, earliest.time));
        offset = add(offset, mul(earliest.normal, earliest.penetration + .001f));
        elapsed += time_left * earliest.time;
        time_left *= 1 - earliest.time;
        remaining = mul(remaining, 1 - earliest.time);
        const float into = dot(sub(remaining, mul(surface, delta * time_left)), earliest.normal);
        if (into < 0)
            remaining = sub(remaining, mul(earliest.normal, into));
        if (speed > 0)
            out.velocity = add(out.velocity, mul(earliest.normal, speed));
    }
    // Exhaustion leaves the last tested position; never append an untested tail.
    out.displacement = offset;
    return out;
}
} // namespace rr64::course_hazards
