#include "rr64_course_walls.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace rr64::course_walls {
namespace {
using D = std::array<double, 3>;
D cast(Vec v) { return {v[0], v[1], v[2]}; }
Vec narrow(D v) { return {float(v[0]), float(v[1]), float(v[2])}; }
D add(D a, D b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
D sub(D a, D b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D mul(D a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
double dot(D a, D b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D cross(D a, D b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double length(D a) { return std::sqrt(dot(a, a)); }
bool finite(D a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }
struct Bounds {
    D lo{}, hi{};
};
Bounds empty_bounds() {
    constexpr double n = std::numeric_limits<double>::infinity();
    return {{n, n, n}, {-n, -n, -n}};
}
void extend(Bounds &b, D p) {
    for (unsigned k = 0; k < 3; ++k) {
        b.lo[k] = std::min(b.lo[k], p[k]);
        b.hi[k] = std::max(b.hi[k], p[k]);
    }
}
bool overlaps(const Bounds &a, const Bounds &b) {
    for (unsigned k = 0; k < 3; ++k)
        if (a.lo[k] > b.hi[k] || a.hi[k] < b.lo[k])
            return false;
    return true;
}
struct Face {
    std::uint32_t id;
    std::array<D, 3> p;
    D normal;
    Bounds bounds;
    int rail = -1;
};
struct Node {
    Bounds bounds;
    unsigned begin = 0, count = 0, left = 0, right = 0;
};
struct Hit {
    double t = 2, depth = 0;
    D normal{};
    std::uint32_t id = 0;
    int rail = -1;
};

// Closest point in every triangle Voronoi region, including finite edges and
// vertices. An infinite plane alone would block gaps and open wall ends.
D closest(D p, const Face &f) {
    const D a = f.p[0], b = f.p[1], c = f.p[2], ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return a;
    const D bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return add(a, mul(ab, d1 / (d1 - d3)));
    const D cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return add(a, mul(ac, d2 / (d2 - d6)));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    const double inverse = 1 / (va + vb + vc);
    return add(a, add(mul(ab, vb * inverse), mul(ac, vc * inverse)));
}
bool inside(D p, const Face &f) {
    for (unsigned i = 0; i < 3; ++i)
        if (dot(cross(sub(f.p[(i + 1) % 3], f.p[i]), sub(p, f.p[i])), f.normal) < -1e-8)
            return false;
    return true;
}
void consider(Hit &best, double t, D normal, std::uint32_t id, double depth = 0,
              int rail = -1) {
    if (t < 0 || t > 1 || !finite(normal))
        return;
    if (t < best.t - 1e-10 ||
        (std::abs(t - best.t) <= 1e-10 &&
         (depth > best.depth + 1e-10 || (depth == best.depth && id < best.id))))
        best = {t, depth, normal, id, rail};
}
double quadratic_entry(D p, D v, double radius) {
    const double a = dot(v, v), b = dot(p, v), c = dot(p, p) - radius * radius;
    if (a <= 1e-20 || b >= 0)
        return 2;
    const double discriminant = b * b - a * c;
    if (discriminant < 0)
        return 2;
    return (-b - std::sqrt(std::max(0.0, discriminant))) / a;
}
Hit sweep_face(D start, D move, double radius, const Face &f) {
    Hit hit;
    const D nearest = closest(start, f), away = sub(start, nearest);
    const double d = length(away);
    if (d < radius - 1e-8) {
        D normal = d > 1e-10 ? mul(away, 1 / d) : mul(f.normal, dot(move, f.normal) > 0 ? -1 : 1);
        consider(hit, 0, normal, f.id, radius - d);
    }
    const double plane = dot(sub(start, f.p[0]), f.normal), speed = dot(move, f.normal);
    if (std::abs(speed) > 1e-14)
        for (double side : {-1.0, 1.0}) {
            const D normal = mul(f.normal, side);
            if (dot(move, normal) >= 0)
                continue;
            const double t = (side * radius - plane) / speed;
            if (t >= 0 && t <= 1 && inside(sub(add(start, mul(move, t)), mul(normal, radius)), f))
                consider(hit, t, normal, f.id);
        }
    for (unsigned i = 0; i < 3; ++i) {
        const D a = f.p[i], edge = sub(f.p[(i + 1) % 3], a), p = sub(start, a);
        const double ee = dot(edge, edge);
        const D pp = sub(p, mul(edge, dot(p, edge) / ee)),
                vv = sub(move, mul(edge, dot(move, edge) / ee));
        const double t = quadratic_entry(pp, vv, radius);
        if (t >= 0 && t <= 1) {
            const double u = dot(add(p, mul(move, t)), edge) / ee;
            if (u >= 0 && u <= 1)
                consider(hit, t, mul(add(pp, mul(vv, t)), 1 / radius), f.id);
        }
        const double v = quadratic_entry(p, move, radius);
        if (v >= 0 && v <= 1)
            consider(hit, v, mul(add(p, mul(move, v)), 1 / radius), f.id);
    }
    return hit;
}
} // namespace

class World {
  public:
    std::vector<Face> faces;
    std::vector<unsigned> order;
    std::vector<Node> nodes;
    std::vector<Rail> rails;
    unsigned build(unsigned begin, unsigned count) {
        const unsigned id = unsigned(nodes.size());
        nodes.push_back({});
        Bounds bounds = empty_bounds();
        for (unsigned i = begin; i < begin + count; ++i) {
            extend(bounds, faces[order[i]].bounds.lo);
            extend(bounds, faces[order[i]].bounds.hi);
        }
        nodes[id].bounds = bounds;
        if (count <= 8) {
            nodes[id].begin = begin;
            nodes[id].count = count;
            return id;
        }
        unsigned axis = 0;
        for (unsigned i = 1; i < 3; ++i)
            if (bounds.hi[i] - bounds.lo[i] > bounds.hi[axis] - bounds.lo[axis])
                axis = i;
        std::sort(order.begin() + begin, order.begin() + begin + count,
                  [&](unsigned a, unsigned b) {
                      const auto &aa = faces[a].bounds;
                      const auto &bb = faces[b].bounds;
                      const double ca = aa.lo[axis] + aa.hi[axis], cb = bb.lo[axis] + bb.hi[axis];
                      return ca == cb ? faces[a].id < faces[b].id : ca < cb;
                  });
        const unsigned left = build(begin, count / 2),
                       right = build(begin + count / 2, count - count / 2);
        nodes[id].left = left;
        nodes[id].right = right;
        return id;
    }
    Hit sweep(D start, D move, double radius, unsigned &tests,
              std::span<const std::pair<int, unsigned>> skipped = {},
              unsigned sphere_index = 0) const noexcept {
        Hit best;
        if (nodes.empty())
            return best;
        Bounds bounds = empty_bounds();
        extend(bounds, start);
        extend(bounds, add(start, move));
        for (unsigned i = 0; i < 3; ++i) {
            bounds.lo[i] -= radius;
            bounds.hi[i] += radius;
        }
        std::array<unsigned, 64> stack{};
        unsigned size = 1;
        while (size) {
            const auto &n = nodes[stack[--size]];
            if (!overlaps(bounds, n.bounds))
                continue;
            if (n.count)
                for (unsigned i = n.begin; i < n.begin + n.count; ++i) {
                    const auto &face = faces[order[i]];
                    if (face.rail >= 0 &&
                        std::find(skipped.begin(), skipped.end(),
                                  std::pair{face.rail, sphere_index}) != skipped.end())
                        continue;
                    ++tests;
                    const auto h = sweep_face(start, move, radius, face);
                    consider(best, h.t, h.normal, h.id, h.depth, face.rail);
                }
            else {
                stack[size++] = n.right;
                stack[size++] = n.left;
            }
        }
        return best;
    }
};

static std::shared_ptr<World> build(std::span<const Triangle> triangles, bool walls_only) {
    // Source audit's largest course currently has fewer than 3,200 collision
    // faces. The format ceiling bounds memory and balanced-BVH traversal depth.
    if (triangles.size() > 65536)
        throw std::runtime_error("Imported walls exceed the format limit");
    auto w = std::make_shared<World>();
    w->faces.reserve(triangles.size());
    std::vector<unsigned> ids;
    ids.reserve(triangles.size());
    for (const auto &t : triangles) {
        Face face{};
        face.id = t.id;
        face.bounds = empty_bounds();
        ids.push_back(t.id);
        for (unsigned i = 0; i < 3; ++i) {
            face.p[i] = cast(t.vertices[i]);
            if (!finite(face.p[i]))
                throw std::runtime_error("Imported wall has a nonfinite vertex");
            for (unsigned k = 0; k < 3; ++k)
                if (std::abs(face.p[i][k]) > (k == 2 ? 4350.0 : 8700.0))
                    throw std::runtime_error("Imported wall exceeds native world bounds");
            extend(face.bounds, face.p[i]);
        }
        const D normal = cross(sub(face.p[1], face.p[0]), sub(face.p[2], face.p[0]));
        const double size = length(normal);
        if (size < 1e-10)
            throw std::runtime_error("Imported wall is degenerate");
        face.normal = mul(normal, 1 / size);
        // MK64 collision.c chooses the dominant axis; Y-dominant source faces
        // remain native floors/ceilings rather than becoming artificial walls.
        if (walls_only && std::abs(face.normal[2]) >
                              std::max(std::abs(face.normal[0]), std::abs(face.normal[1])) + 0.0001)
            throw std::runtime_error("Imported wall is a floor or ceiling");
        w->faces.push_back(face);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        throw std::runtime_error("Imported wall has a duplicate identity");
    w->order.resize(w->faces.size());
    std::iota(w->order.begin(), w->order.end(), 0u);
    if (!w->faces.empty())
        w->build(0, unsigned(w->faces.size()));
    return w;
}

std::shared_ptr<const World> build_world(std::span<const Triangle> triangles,
                                       std::span<const Rail> rails) {
    auto w = build(triangles, true);
    if (rails.size() > triangles.size() / 2)
        throw std::runtime_error("Imported rail count exceeds wall pairs");
    // The metadata is not permission to add collision or lower a tall wall.
    // Require the exact two existing faces and their four authored corners.
    std::vector<std::pair<unsigned, unsigned>> face_ids;
    for (unsigned i = 0; i < w->faces.size(); ++i)
        face_ids.emplace_back(w->faces[i].id, i);
    std::sort(face_ids.begin(), face_ids.end());
    std::vector<unsigned> rail_ids;
    for (const auto &r : rails) {
        std::array<Face *, 2> faces{};
        for (unsigned i = 0; i < 2; ++i) {
            auto at = std::lower_bound(face_ids.begin(), face_ids.end(),
                                      std::pair{r.triangle_ids[i], 0u});
            if (at == face_ids.end() || at->first != r.triangle_ids[i])
                throw std::runtime_error("Imported rail references missing wall");
            faces[i] = &w->faces[at->second];
            if (faces[i]->rail >= 0)
                throw std::runtime_error("Imported rail reuses wall");
        }
        if (faces[0] == faces[1])
            throw std::runtime_error("Imported rail requires two distinct walls");
        const std::array<D, 4> corners{cast(r.base[0]), cast(r.base[1]),
                                      cast(r.top[0]), cast(r.top[1])};
        std::array<unsigned, 4> use{};
        for (const auto *f : faces)
            for (const auto &v : f->p) {
                auto at = std::find(corners.begin(), corners.end(), v);
                if (at == corners.end())
                    throw std::runtime_error("Imported rail changes authored corners");
                ++use[unsigned(at - corners.begin())];
            }
        if (std::count(use.begin(), use.end(), 1u) != 2 ||
            std::count(use.begin(), use.end(), 2u) != 2 ||
            !((use[0] == 2 && use[3] == 2) || (use[1] == 2 && use[2] == 2)))
            throw std::runtime_error("Imported rail is not a diagonal wall pair");
        for (unsigned i = 0; i < 2; ++i) {
            const auto side = sub(corners[i + 2], corners[i]);
            if (!finite(side) || side[2] <= 0 ||
                std::hypot(side[0], side[1]) > std::min(.25, side[2] * .125 + 1e-5))
                throw std::runtime_error("Imported rail has invalid vertical endpoints");
        }
        const D base = sub(corners[1], corners[0]), top = sub(corners[3], corners[2]);
        if (std::hypot(top[0], top[1]) < .001 ||
            base[0] * top[0] + base[1] * top[1] <= 0 ||
            std::abs(dot(faces[0]->normal, faces[1]->normal)) < .98)
            throw std::runtime_error("Imported rail endpoints do not align");
        for (auto *f : faces)
            f->rail = int(w->rails.size());
        w->rails.push_back(r);
        rail_ids.push_back(r.id);
    }
    std::sort(rail_ids.begin(), rail_ids.end());
    if (std::adjacent_find(rail_ids.begin(), rail_ids.end()) != rail_ids.end())
        throw std::runtime_error("Imported rail has duplicate identity");
    return w;
}
std::shared_ptr<const World> build_surface_world(std::span<const Triangle> triangles) {
    return build(triangles, false);
}
SweepHit sweep_sphere(const World &world, Sphere sphere, Vec displacement) noexcept {
    SweepHit result;
    if (!finite(cast(sphere.center)) || !finite(cast(displacement)) ||
        !std::isfinite(sphere.radius) || sphere.radius <= 0 || sphere.radius > 10)
        return result;
    const auto hit =
        world.sweep(cast(sphere.center), cast(displacement), sphere.radius, result.triangle_tests);
    if (hit.t > 1)
        return result;
    result.hit = true;
    result.fraction = float(hit.t);
    result.penetration = float(hit.depth);
    result.normal = narrow(hit.normal);
    result.point = narrow(sub(add(cast(sphere.center), mul(cast(displacement), hit.t)),
                              mul(hit.normal, sphere.radius - hit.depth)));
    result.triangle_id = hit.id;
    return result;
}

Motion resolve(const World &world, std::span<const Sphere> spheres, Vec displacement,
               Vec velocity, RailPolicy policy) noexcept {
    Motion out;
    out.displacement = displacement;
    out.velocity = velocity;
    if (spheres.empty() || spheres.size() > 8 || !finite(cast(displacement)) ||
        !finite(cast(velocity)))
        return out;
    for (const auto &sphere : spheres)
        if (!finite(cast(sphere.center)) || !std::isfinite(sphere.radius) || sphere.radius <= 0 ||
            sphere.radius > 10)
            return out;
    D offset{}, remaining = cast(displacement), v = cast(velocity);
    std::array<std::pair<int, unsigned>, 24> skipped{};
    unsigned skipped_count = 0;
    constexpr double skin = .001;
    for (unsigned iteration = 0; iteration < 8;) {
        Hit best;
        D contact_point{};
        unsigned selected_sphere = 0;
        for (unsigned i = 0; i < spheres.size(); ++i) {
            const auto &sphere = spheres[i];
            const D start = add(cast(sphere.center), offset);
            const auto h = world.sweep(start, remaining, sphere.radius, out.triangle_tests,
                                       {skipped.data(), skipped_count}, i);
            const auto previous = best;
            consider(best, h.t, h.normal, h.id, h.depth, h.rail);
            if (best.t != previous.t || best.id != previous.id || best.depth != previous.depth) {
                selected_sphere = i;
                contact_point =
                    sub(add(start, mul(remaining, h.t)), mul(h.normal, sphere.radius - h.depth));
            }
        }
        if (best.t > 1) {
            offset = add(offset, remaining);
            remaining = {};
            break;
        }
        if (best.rail >= 0 && policy.allows && skipped_count < skipped.size()) {
            const auto &rail = world.rails[best.rail];
            const D edge = sub(cast(rail.top[1]), cast(rail.top[0]));
            const D relative = sub(contact_point, cast(rail.top[0]));
            const double t = std::clamp((relative[0] * edge[0] + relative[1] * edge[1]) /
                                           (edge[0] * edge[0] + edge[1] * edge[1]), 0.0, 1.0);
            // The native line-barrier response uses a horizontal outward normal;
            // rounding a triangle's top edge must not turn it into a floor jump.
            const double size = std::hypot(best.normal[0], best.normal[1]);
            if (size > 1e-6) {
                Motion::RailContact contact;
                contact.normal = {float(best.normal[0] / size), float(best.normal[1] / size), 0};
                contact.point = narrow(contact_point);
                contact.top = float(rail.top[0][2] + t * edge[2]);
                contact.sphere_index = selected_sphere;
                contact.rail_id = rail.id;
                if (policy.allows(policy.context, contact)) {
                    if (!out.vaulted_rail)
                        out.rail = contact;
                    out.vaulted_rail = true;
                    skipped[skipped_count++] = {best.rail, selected_sphere};
                    continue;
                }
            }
        }
        ++iteration;
        ++out.contacts;
        offset = add(offset, mul(remaining, best.t));
        offset = add(offset, mul(best.normal, best.depth + skin));
        remaining = mul(remaining, 1 - best.t);
        const double inward = dot(remaining, best.normal);
        if (inward < 0)
            remaining = sub(remaining, mul(best.normal, inward));
        const double speed = dot(v, best.normal);
        if (out.contacts == 1 || -speed > out.normal_speed) {
            out.normal_speed = float(std::max(0.0, -speed));
            out.normal = narrow(best.normal);
            out.point = narrow(contact_point);
            out.penetration = float(best.depth);
            out.triangle_id = best.id;
        }
        if (speed < 0)
            v = sub(v, mul(best.normal, speed));
        if (length(remaining) < 1e-10)
            break;
    }
    // An exhausted corner budget stops at the last safe position, never by
    // applying the untested remainder through another wall.
    out.displacement = narrow(offset);
    out.velocity = narrow(v);
    return out;
}
} // namespace rr64::course_walls
#include "rr64_course_hazards.hpp"
#include "rr64_course_guardrail.hpp"
#include "rr64_course_impact.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_rules.hpp"
#include "rr64_race_end_trace.hpp"
#include "rr64_mk64_items.hpp"

extern "C" void func_8004E754(unsigned char *, recomp_context *);

namespace rr64::course_walls {
namespace {
using namespace engine;
constexpr unsigned actor_base = 0x800D8570u, actor_stride = 0x118u;
struct Pending {
    unsigned char *memory = nullptr;
    const World *geometry = nullptr;
    unsigned body = 0, object = 0, actor = 0, rider = 0, route = 0, recovery = 0, count = 0;
    std::uint64_t replay_epoch = 0;
    std::array<Sphere, 3> spheres{};
    Vec position{};
    float delta = 0, motion_bound = 0;
    bool valid = false, replay = false, local_client = false;
};
// Paired within native integrators, never retained as a previous-frame sweep.
// Recovery/teleports outside integration therefore cannot collide along their path.
thread_local std::array<Pending, 4> pending{};
unsigned word(unsigned char *m, unsigned address) {
    unsigned value = 0;
    read_u32(m, address, value);
    return value;
}
unsigned half(unsigned char *m, unsigned address) {
    std::uint16_t value = 0;
    read_u16(m, address, value);
    return value;
}
bool vector_at(unsigned char *m, unsigned address, Vec &value) {
    for (unsigned k = 0; k < 3; ++k)
        if (!read_float(m, address + k * 4, value[k]) || !std::isfinite(value[k]))
            return false;
    return true;
}
void put_vector(unsigned char *m, unsigned address, Vec value) {
    for (unsigned k = 0; k < 3; ++k)
        write_float(m, address + k * 4, value[k]);
}
bool scene(unsigned char *m) {
    return m &&
           is_live_race_transition(word(m, globals::main_mode), word(m, globals::pending_mode)) &&
           !half(m, globals::gameplay_pause_state);
}
bool owned(unsigned char *m, Pending &p, unsigned kind) {
    const bool detached = (kind & 1) != 0;
    for (unsigned i = 0; i < kMaximumRacers; ++i) {
        const unsigned actor = actor_base + i * actor_stride;
        if (half(m, actor + 0x24) && word(m, actor + (detached ? 0xE4 : 0xE0)) == p.object) {
            p.actor = i;
            p.rider = word(m, actor + 0xE4);
            p.route = word(m, actor + 0xE8);
            const unsigned owner_bike = word(m, actor + 0xE0);
            if (!valid_guest_range(p.route, 0x64) || !valid_guest_range(p.rider, rider::stride) ||
                !valid_guest_range(owner_bike, bike::stride) ||
                word(m, owner_bike + bike::rider_pointer) != p.rider ||
                word(m, p.rider + rider::bike_pointer) != owner_bike)
                return false;
            if (detached && half(m, p.rider + rider::bike_attached))
                return false;
            const auto status = prediction::physics_rules();
            if (status.active) {
                if (!status.connected || !status.authoritative ||
                    (!prediction::active() && status.phase != netplay::Phase::Race))
                    return false;
                p.local_client = !status.is_host;
                if (p.local_client &&
                    (status.local_slot >= kMaximumRacers ||
                     i != online_flow::mapped_slot(status.local_slot, status.local_slot,
                                                   status.replicated_riders)))
                    return false;
            }
            p.recovery = word(m, p.route + 0x40);
            return true;
        }
    }
    return false;
}
void begin(unsigned char *m, recomp_context &context, unsigned kind) {
    auto &p = pending[kind];
    p = {};
    p.geometry = world();
    if (!p.geometry || !scene(m))
        return;
    p.object = unsigned((kind & 1) ? context.r21 : context.r17);
    p.body = p.object + ((kind & 1) ? 0x28u : 0x108u);
    if (!valid_guest_range(p.object, (kind & 1) ? rider::stride : bike::stride) ||
        unsigned((kind & 1) ? context.r20 : context.r18) != p.body || !owned(m, p, kind))
        return;
    if (!read_float(m, globals::physics_delta, p.delta) || !std::isfinite(p.delta) ||
        p.delta <= 0 || p.delta > .25f)
        return;
    p.count = word(m, p.body + 0x2C);
    if (!p.count || p.count > p.spheres.size() || !vector_at(m, p.body + 0x64, p.position))
        return;
    float mass = 0;
    Vec velocity{}, force{}, impulse{}, acceleration{};
    if (!read_float(m, p.body, mass) || !std::isfinite(mass) || mass <= 0 ||
        !vector_at(m, p.body + 0x70, velocity) || !vector_at(m, p.body + 0xF4, force) ||
        !vector_at(m, p.body + 0x100, impulse) || !vector_at(m, p.body + 0x80, acceleration))
        return;
    // Native34594/34370 cannot travel farther than this bound from their own
    // velocity, force and accumulated impulses. Reject a foreign relocation,
    // not a high-speed continuous movement (the sweep has no speed cutoff).
    p.motion_bound =
        float(2 * (length(cast(velocity)) * p.delta +
                   (length(cast(force)) / mass + length(cast(acceleration))) * p.delta * p.delta +
                   length(cast(impulse)) / mass * p.delta) +
              .1);
    if (!std::isfinite(p.motion_bound))
        return;
    for (const auto range : {std::pair{0x30u, p.count * 3}, std::pair{0x8Cu, 3u},
                             std::pair{0xC0u, 9u}, std::pair{0x118u, 9u}})
        for (unsigned i = 0; i < range.second; ++i) {
            float value = 0;
            if (!read_float(m, p.body + range.first + 4 * i, value) || !std::isfinite(value))
                return;
        }
    for (unsigned i = 0; i < p.count; ++i)
        if (!read_float(m, p.body + 0x54 + 4 * i, p.spheres[i].radius) ||
            !std::isfinite(p.spheres[i].radius) || p.spheres[i].radius <= 0 ||
            p.spheres[i].radius > 10)
            return;
    if (!valid_guest_range(unsigned(context.r29) - 0x38u, 0x38u))
        return;
    auto refresh = context;
    refresh.f_odd = &refresh.f0.u32h;
    refresh.r4 = guest_address(p.body);
    func_8004E754(m, &refresh);
    for (unsigned i = 0; i < p.count; ++i)
        if (!vector_at(m, p.body + 0x190 + 12 * i, p.spheres[i].center))
            return;
    p.memory = m;
    p.replay = prediction::active();
    p.replay_epoch = prediction::replay_epoch;
    p.valid = true;
}
void preserve_mounted_velocity(unsigned char *m, const Pending &p, Vec before, Vec after,
                               unsigned kind) {
    if (!p.local_client || !half(m, p.rider + rider::bike_attached))
        return;
    // Geometric prediction must not invent the host-owned native detach event.
    // Preserve the mounted relative velocities used by36B78's mismatch test.
    for (unsigned offset : {0x98u, 0xC0u}) {
        if (kind >= 2 && offset == 0x98)
            continue;
        Vec velocity{};
        if (vector_at(m, p.rider + offset, velocity)) {
            for (unsigned k = 0; k < 3; ++k)
                velocity[k] += after[k] - before[k];
            put_vector(m, p.rider + offset, velocity);
        }
    }
}
course_guardrail::Contact rail_contact(const Motion::RailContact &rail) {
    return {rail.normal, rail.top, rail.sphere_index, true};
}
struct RailContext {
    unsigned char *memory;
    unsigned actor;
};
bool can_vault(void *opaque, const Motion::RailContact &rail) {
    const auto &context = *static_cast<const RailContext *>(opaque);
    return course_guardrail::eligibility(context.memory, context.actor, rail_contact(rail)) !=
           course_guardrail::Eligibility::Blocked;
}
void end(unsigned char *m, recomp_context &context, unsigned kind) {
    const Pending p = pending[kind];
    pending[kind].valid = false;
    if (!p.valid || p.memory != m || p.geometry != world() || !scene(m) ||
        p.replay != prediction::active() || p.replay_epoch != prediction::replay_epoch ||
        word(m, p.route + 0x40) != p.recovery ||
        word(m, actor_base + p.actor * actor_stride + ((kind & 1) ? 0xE4 : 0xE0)) != p.object)
        return;
    const unsigned position_offset = kind < 2 ? 0x64 : 0x8C,
                   velocity_offset = kind < 2 ? 0x70 : 0x98;
    Vec position{}, velocity{};
    if (!vector_at(m, p.body + position_offset, position) ||
        !vector_at(m, p.body + velocity_offset, velocity))
        return;
    const auto displacement = narrow(sub(cast(position), cast(p.position)));
    if (length(cast(displacement)) > p.motion_bound)
        return;
    RailContext rail_context{m, p.actor};
    RailPolicy rails{&rail_context, (kind & 1) ? nullptr : can_vault};
    auto motion = resolve(*p.geometry, {p.spheres.data(), p.count}, displacement, velocity, rails);
    course_hazards::DynamicMotion dynamic;
    const auto resolve_dynamic = [&] {
        if (p.local_client || p.replay)
            return;
#ifdef RR64_EXPERIMENTAL_COURSE
        if (rr64_mk64_items_ghost(m, p.object))
            return;
#endif
        dynamic = course_hazards::resolve({p.spheres.data(), p.count}, motion.displacement,
                                          motion.velocity, p.delta, kind >= 2);
        if (dynamic.contacts) {
            // Recheck static geometry after dynamic depenetration: an obstacle
            // cannot push a rider through a nearby authored barrier.
            const auto checked = resolve(*p.geometry, {p.spheres.data(), p.count},
                                         dynamic.displacement, dynamic.velocity, rails);
            motion.displacement = checked.displacement;
            motion.velocity = checked.velocity;
            if (checked.vaulted_rail && !motion.vaulted_rail) {
                motion.vaulted_rail = true;
                motion.rail = checked.rail;
            }
        }
    };
    resolve_dynamic();
    bool vault_applied = false;
    if (kind == 0 && !p.local_client && !p.replay && motion.vaulted_rail) {
        // Eligibility and the native response inspect the same uncorrected
        // current root. If the response rejects its inputs, keep solid contact.
        const auto vault = course_guardrail::apply(m, context, p.actor, rail_contact(motion.rail));
        vault_applied = vault.consumed;
        if (!vault_applied) {
            rails = {};
            motion = resolve(*p.geometry, {p.spheres.data(), p.count}, displacement, velocity);
            resolve_dynamic();
        }
    }
    const bool diagnostic_dynamic = dynamic.contacts &&
        (!motion.contacts || dynamic.normal_speed > motion.normal_speed);
    race_end_trace::observe_course_wall(m, p.actor, kind, motion.contacts, dynamic.contacts,
        motion.triangle_id, dynamic.id, displacement.data(), motion.displacement.data(),
        diagnostic_dynamic ? dynamic.normal.data() : motion.normal.data(),
        diagnostic_dynamic ? dynamic.normal_speed : motion.normal_speed);
    race_end_trace::CourseWallContext observed;
    observed.body = p.body; observed.route = p.route; observed.sphere_count = p.count;
    observed.static_contacts = motion.contacts; observed.dynamic_contacts = dynamic.contacts;
    observed.triangle = motion.triangle_id; observed.hazard = dynamic.id;
    observed.triangle_tests = motion.triangle_tests;
    observed.previous = p.position; observed.requested = position;
    observed.resolved = narrow(add(cast(p.position), cast(motion.displacement)));
    observed.point = diagnostic_dynamic ? dynamic.point : motion.point;
    observed.normal = diagnostic_dynamic ? dynamic.normal : motion.normal;
    observed.delta = p.delta;
    for (unsigned i = 0; i < p.count; ++i) {
        for (unsigned axis = 0; axis < 3; ++axis)
            observed.spheres[i][axis] = p.spheres[i].center[axis];
        observed.spheres[i][3] = p.spheres[i].radius;
    }
    race_end_trace::observe_course_wall_context(m, p.actor, kind, observed);
    if (!motion.contacts && !dynamic.contacts && !motion.vaulted_rail)
        return;
    put_vector(m, p.body + position_offset,
               narrow(add(cast(p.position), cast(motion.displacement))));
    Vec final_velocity = motion.velocity;
    if (vault_applied)
        vector_at(m, p.body + velocity_offset, final_velocity);
    if (kind < 2 && !p.local_client && !p.replay && (motion.contacts || dynamic.contacts)) {
        course_impact::Contact contact;
        const bool moving_contact =
            dynamic.contacts && (!motion.contacts || dynamic.normal_speed > motion.normal_speed);
        contact.normal = moving_contact ? dynamic.normal : motion.normal;
        contact.point = moving_contact ? dynamic.point : motion.point;
        contact.penetration = moving_contact ? dynamic.penetration : motion.penetration;
        if (moving_contact)
            contact.surface_velocity = dynamic.surface_velocity;
        const auto applied = course_impact::apply(
            m, context, p.actor,
            (kind & 1) ? course_impact::Body::Rider : course_impact::Body::Bike, contact);
        if (applied.applied && vector_at(m, p.body + velocity_offset, final_velocity)) {
            // Clip only any residual inward part after the native impulse.
            const auto residual =
                resolve(*p.geometry, {p.spheres.data(), p.count}, displacement, final_velocity, rails);
            final_velocity = residual.velocity;
            if (dynamic.contacts)
                final_velocity =
                    course_hazards::resolve({p.spheres.data(), p.count}, motion.displacement,
                                            final_velocity, p.delta, kind >= 2)
                        .velocity;
        }
    }
    if (kind >= 2) {
        //34370 predicts the next pose; this is not a second physical impact.
        //36B78 copies this velocity into the mounted rider's baseline before
        //the next update. Clipping it here erases the deceleration which the
        //native crash test must observe when34594 actually reaches the wall.
        //Keep the predicted root constrained, but let the current-step hook
        //apply the impulse and the native rider update decide crash/damage.
        //Clients also retain this baseline: adjusting the mounted rider to a
        //speculative stop would instead cause an early, client-owned detach.
        final_velocity = velocity;
    }
    if (!(kind & 1))
        preserve_mounted_velocity(m, p, velocity, final_velocity, kind);
    put_vector(m, p.body + velocity_offset, final_velocity);
    write_float(m, p.body + (kind < 2 ? 0x7C : 0xA4), float(length(cast(final_velocity))));
    write_u16(m, p.body + 0x188, 0); // Native sphere cache must follow corrected roots.
    // Only a real host/local impact can knock away a mole or break a snowman.
    // Predicted movement and AI lookahead remain read-only collision queries.
    if (kind < 2 && dynamic.contacts && !p.local_client && !p.replay)
        course_hazards::notify_hit(dynamic.id, velocity);
}
} // namespace
} // namespace rr64::course_walls

extern "C" void rr64_course_walls_begin(unsigned char *m, void *context, unsigned kind) {
    if (context && kind < 4)
        rr64::course_walls::begin(m, *static_cast<recomp_context *>(context), kind);
}
extern "C" void rr64_course_walls_end(unsigned char *m, void *context, unsigned kind) {
    if (context && kind < 4)
        rr64::course_walls::end(m, *static_cast<recomp_context *>(context), kind);
}
