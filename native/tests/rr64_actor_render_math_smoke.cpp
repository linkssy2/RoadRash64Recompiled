// Production snapshot runtime plus unmodified generated root writers, vector/
// quaternion arithmetic, renderer and matrix packing on synthetic RDRAM.
// Animation and secondary shadow-position helpers remain bounded substitutes;
// this checks coordinates/ranges/resources, not full animation or scanout.
#include "rr64_actor_render_fixture.hpp"
#include "rr64_native.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void func_80011CC0(unsigned char*, recomp_context*);
void func_80012CE0(unsigned char*, recomp_context*);
void guPerspectiveF(unsigned char*, recomp_context*);
void func_800167BC(unsigned char*, recomp_context*);
void func_8005EB50(unsigned char*, recomp_context*);
void func_80019F7C(unsigned char*, recomp_context*);
}
namespace {
using namespace rr64::engine;
using Fixture = rr64::lod::test::Fixture;
using Vec3 = std::array<float, 3>;
using Quaternion = std::array<float, 4>;
using Vector = std::array<float, 4>;
using Matrix = std::array<float, 16>;
constexpr std::uint32_t camera = 0x800D69F8u, sector = 0x800A4FDCu;
constexpr std::uint32_t shadow_graph = 0x805A0000u, shadow_pose = 0x805A0200u;
constexpr std::array<float, 3> source_scales{4.0f, 100.0f, 10.0f};
unsigned char* live = nullptr;
Vec3 late_camera{};
std::array<Vec3, 2> late_anchors{};
bool passed = true;
bool keep_anchors = false;
bool depth_only = false;
bool rider_range_only = false;
bool rider_depth_only = false;
bool crash_only = false;
bool trace_camera = false;
unsigned shadow_calls = 0, racer_id = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "[RR64-REAL-MATH] FAILED: %s\n", message); passed = false; }
}
template<std::size_t N>
void floats(unsigned char* memory, std::uint32_t address, const std::array<float, N>& values) {
    for (unsigned i = 0; i < values.size(); ++i) write_float(memory, address + i * 4u, values[i]);
}
recomp_context context() {
    recomp_context ctx{};
    ctx.r20 = static_cast<std::int32_t>(Fixture::bike_node);
    ctx.r19 = static_cast<std::int32_t>(Fixture::rider_node);
    ctx.r22 = static_cast<std::int32_t>(Fixture::bike_entity);
    ctx.r18 = static_cast<std::int32_t>(Fixture::owner);
    ctx.r17 = static_cast<std::int32_t>(0x800D6880u + racer_id * 12u);
    ctx.r16 = static_cast<std::int32_t>(0x800D6940u + racer_id * 12u);
    ctx.r30 = static_cast<std::int32_t>(camera);
    ctx.r29 = static_cast<std::int32_t>(Fixture::stack);
    return ctx;
}
Matrix rotation(const Quaternion& q) {
    const auto [x, y, z, w] = q;
    return {1 - 2 * (y*y + z*z), 2 * (x*y + z*w), 2 * (x*z - y*w), 0,
        2 * (x*y - z*w), 1 - 2 * (x*x + z*z), 2 * (y*z + x*w), 0,
        2 * (x*z + y*w), 2 * (y*z - x*w), 1 - 2 * (x*x + y*y), 0,
        0, 0, 0, 1};
}
Matrix child_matrix(unsigned actor, unsigned child) {
    auto result = rotation(actor ? Quaternion{0.5f, -0.5f, 0.5f, 0.5f}
                                 : Quaternion{-0.5f, 0.5f, 0.5f, 0.5f});
    result[12] = child * 0.25f;
    result[13] = child * (actor ? 0.5f : -0.5f);
    result[14] = child * 0.125f;
    return result;
}
Vector transform(const Vector& input, const Matrix& matrix) {
    Vector output{};
    for (unsigned column = 0; column < 4; ++column) {
        for (unsigned row = 0; row < 4; ++row) output[column] += input[row] * matrix[row * 4u + column];
    }
    return output;
}
void seed_matrix(unsigned char* memory, std::uint32_t address, const Matrix& values) {
    for (unsigned i = 0; i < values.size(); ++i) {
        const auto fixed = static_cast<std::uint32_t>(static_cast<std::int32_t>(values[i] * 65536.0f));
        write_u16(memory, address + i * 2u, fixed >> 16u);
        write_u16(memory, address + 32u + i * 2u, fixed & 0xffffu);
    }
}
Matrix perspective(unsigned char* memory, unsigned step, unsigned slot, float scale, bool clamp_far, float world_far = 700.0f) {
    // The original projection arithmetic is exercised with a bounded trig
    // substitute. The game clamps each bank's far plane to its packed range.
    constexpr std::uint32_t matrix_address = 0x805D0000u, norm_address = matrix_address + 64u;
    const float fov = 50.0f + (step % 3u) * 5.0f + slot * 2.0f, aspect = 4.0f / 3.0f;
    auto ctx = context();
    ctx.r4 = static_cast<std::int32_t>(matrix_address);
    ctx.r5 = static_cast<std::int32_t>(norm_address);
    std::uint32_t fov_bits = 0, aspect_bits = 0;
    std::memcpy(&fov_bits, &fov, 4u);
    std::memcpy(&aspect_bits, &aspect, 4u);
    ctx.r6 = fov_bits;
    ctx.r7 = aspect_bits;
    write_float(memory, Fixture::stack + 0x10u, scale);
    write_float(memory, Fixture::stack + 0x14u, clamp_far ? std::fmin(world_far * scale, 32767.0f) : world_far * scale);
    write_float(memory, Fixture::stack + 0x18u, 1.0f);
    guPerspectiveF(memory, &ctx);
    Matrix result{};
    for (unsigned i = 0; i < 16; ++i) read_float(memory, matrix_address + i * 4u, result[i]);
    return result;
}
Matrix camera_view(unsigned step, float scale) {
    // Look along world +Y with nonzero eye offsets. Both relative-coordinate
    // banks use this same camera orientation and scale their translation.
    Matrix result{1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    result[12] = -(1.0f + step * 0.125f) * scale;
    result[13] = -0.5f * scale;
    result[14] = 2.0f * scale;
    return result;
}
void seed_camera_banks(unsigned char* memory, unsigned step, float world_far = 700.0f,
    float eye_height = 0.5f, const Vec3& terrain_origin = Vec3{}) {
    std::uint32_t old_slot = 0, old_frame = 0;
    read_u32(memory, globals::actor_render_buffer_slot, old_slot);
    read_u32(memory, 0x800A1830u, old_frame);
    write_float(memory, 0x8009DBC8u, 1.0f);
    write_float(memory, 0x8009DBCCu, world_far);
    write_float(memory, 0x8009DBD0u, 4.0f / 3.0f);
    for (unsigned source = 0; source < 3; ++source) {
        const auto address = 0x800B6B68u + source * 0x34u;
        const auto scale = source_scales[source];
        const Vec3 origin = source == 0u ? terrain_origin : Vec3{};
        floats(memory, address, Vec3{(origin[0] + 1.0f + step * 0.125f) * scale,
            (origin[1] + 2.0f) * scale, (origin[2] + eye_height) * scale});
        floats(memory, address + 12u, Vec3{(origin[0] + 1.0f + step * 0.125f) * scale,
            (origin[1] + 3.0f) * scale, (origin[2] + eye_height) * scale});
        floats(memory, address + 24u, Vec3{0, 0, 1});
        write_u16(memory, address + 0x30u, 1u);
        write_float(memory, 0x8009DBD8u + source * 4u, 1.0f);
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
        write_u32(memory, 0x800A1830u, slot);
        write_float(memory, 0x8009DBC4u, 50.0f + (step % 3u) * 5.0f + slot * 2.0f);
        auto ctx = context();
        ctx.f_odd = &ctx.f0.u32h;
        if (trace_camera) std::fprintf(stderr, "[RR64-CAMERA] begin step=%u slot=%u far=%.0f\n", step, slot, world_far);
        func_800167BC(memory, &ctx);
        if (trace_camera) std::fprintf(stderr, "[RR64-CAMERA] end step=%u slot=%u\n", step, slot);
        std::uint32_t generated_slot = 2u;
        read_u32(memory, globals::actor_render_buffer_slot, generated_slot);
        check(generated_slot == slot, "actual camera producer selects the requested physical slot");
    }
    write_u32(memory, globals::actor_render_buffer_slot, old_slot);
    write_u32(memory, 0x800A1830u, old_frame);
}
void seed_roots(Fixture& fixture, unsigned bike_source, bool rider_stock_source2 = false) {
    auto* memory = fixture.live.data();
    write_u32(memory, 0x800A1454u, Fixture::rider_node);
    write_u32(memory, Fixture::owner + 4u, Fixture::model_state);
    write_u32(memory, Fixture::model_state, racer_id);
    write_u16(memory, 0x800D8570u + racer_id * 280u + 0x24u, 1u);
    for (unsigned actor = 0; actor < 2; ++actor) {
        const auto graph = shadow_graph + actor * 0x100u, pose = shadow_pose + actor * 0x100u;
        write_u32(memory, (actor ? 0x800DAC88u : 0x800DAC50u) + racer_id * 4u, graph);
        write_u32(memory, graph + 0xcu, pose);
        write_float(memory, pose + 0x18u, 1.0f);
        floats(memory, actor ? Fixture::owner + 0x164u : Fixture::bike_entity + 0x244u, Quaternion{0, 0, 0, 1});
        const auto source = actor ? Fixture::rider_source : Fixture::bike_source;
        for (unsigned lod = 0; lod < 3; ++lod) {
            // Rider suffix is authored at 100x. Bikes use their own root source;
            // exercise all three accepted source-bank values independently.
            write_u16(memory, source + lod * 0x100u + 0x12u, actor
                ? (lod == 2u && rider_stock_source2 ? 2u : 1u)
                : (lod == 2u && bike_source == 1u ? 2u : bike_source));
        }
    }
    floats(memory, 0x8009DBACu, source_scales);
    write_float(memory, 0x80000DE0u, 1.0f);
    write_float(memory, 0x80000CA0u, 0.5f);
    write_float(memory, 0x80005F9Cu, 268435000.0f);
    write_float(memory, 0x80005FA8u, 268435000.0f);
    write_float(memory, 0x80005FA0u, 10.0f);
    write_float(memory, 0x80005FACu, 10.0f);
    write_float(memory, 0x80005FA4u, -32000.0f);
    write_float(memory, 0x80005FB0u, -32000.0f);
    // Original ROM doubles at 8010..802F used by guPerspectiveF.
    constexpr std::array<std::uint32_t, 8> projection_constants{
        0x3F91DF46u, 0x9D353918u, 0x40000000u, 0u,
        0x41000000u, 0u, 0x41E00000u, 0u};
    for (unsigned i = 0; i < projection_constants.size(); ++i) write_u32(memory, 0x80008010u + i * 4u, projection_constants[i]);
    for (unsigned i = 0; i < projection_constants.size(); ++i) write_u32(memory, 0x80008030u + i * 4u, projection_constants[i]);
    write_u32(memory, 0x80008000u, 0xbff00000u); write_u32(memory, 0x80008004u, 0u);
    write_u32(memory, 0x80008008u, 0x3ff00000u); write_u32(memory, 0x8000800cu, 0u);
    write_float(memory, 0x80000E08u, 32767.0f);
    write_float(memory, 0x80000E0Cu, 1.0f);
}
void run_sequence(unsigned bike_source, unsigned first_slot, bool rider_stock_source2,
    unsigned crash_state = 0u) {
    Fixture fixture;
    live = fixture.live.data();
    seed_roots(fixture, bike_source, rider_stock_source2);
    for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
        for (unsigned view = 0; view < 4; ++view) rr64_lod_observe_allocation(live, node, view, 0x180u);
    }
    // Reuse one mapping across sector/LOD transitions to expose stale captures.
    constexpr std::array<unsigned, 13> lods{0u, 1u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 1u, 1u, 0u};
    constexpr std::array<float, 13> distances{10, 40, 80, 100, 150, 160, 164, 200, 400, 1000, 160, 60, 12};
    for (unsigned step = 0; step < lods.size(); ++step) {
        const unsigned lod = lods[step], slot = (first_slot + step) & 1u;
        const Vec3 full_camera{1000.0f + step * 256.0f, 2000.0f - step * 256.0f, 20.0f + step};
        const Vec3 origin{768.0f + step * 256.0f, 1792.0f - step * 256.0f, 0.0f};
        // A detached rider is airborne and well separated from its fallen
        // bike. Their original root writers must never borrow the peer's
        // position or rotation. The final step also exercises a fresh remount.
        const auto current_crash = step + 1u == lods.size() ? 0u : crash_state;
        if (crash_state) {
            write_u16(live, Fixture::bike_entity + bike::drive_control_lockout, current_crash ? 1u : 0u);
            write_u16(live, Fixture::bike_entity + bike::rider_attached, (current_crash & 1u) ? 0u : 1u);
            write_u16(live, Fixture::owner + rider::bike_attached, (current_crash & 1u) ? 0u : 1u);
            write_u16(live, Fixture::owner + rider::ejected, (current_crash & 2u) ? 1u : 0u);
        }
        const std::array<Vec3, 2> relative = current_crash
            ? std::array<Vec3, 2>{Vec3{0, distances[step], -0.25f}, Vec3{5, distances[step] + 12, 8}}
            : std::array<Vec3, 2>{Vec3{2, distances[step], 0.5f}, Vec3{3, distances[step] + 1, 1.5f}};
        const std::array<Quaternion, 2> quaternions{
            step & 1u ? Quaternion{-0.5f, 0.5f, 0.5f, 0.5f} : Quaternion{0.5f, 0.5f, 0.5f, 0.5f},
            step & 1u ? Quaternion{0.5f, -0.5f, 0.5f, 0.5f} : Quaternion{-0.5f, -0.5f, 0.5f, 0.5f}};
        floats(live, camera, full_camera);
        floats(live, sector, origin);
        for (unsigned actor = 0; actor < 2; ++actor) {
            const auto node = actor ? Fixture::rider_node : Fixture::bike_node;
            const auto graph = actor ? Fixture::rider_graph : Fixture::bike_graph;
            Vec3 position{};
            float distance_squared = 0;
            for (unsigned axis = 0; axis < 3; ++axis) {
                position[axis] = full_camera[axis] + relative[actor][axis];
                distance_squared += relative[actor][axis] * relative[actor][axis];
            }
            floats(live, actor ? Fixture::owner + 0x5dcu : Fixture::bike_entity + 0x53cu, position);
            late_anchors[actor] = position;
            floats(live, actor ? Fixture::owner + 0x164u : Fixture::bike_entity + 0x244u, quaternions[actor]);
            write_float(live, node + 8u, distance_squared);
            write_u16(live, node + actor_scene::selected_lod, lod);
            write_u32(live, node + actor_scene::current_model, graph + lod * 0x100u);
        }
        write_u32(live, globals::actor_render_buffer_slot, slot ^ 1u);
        rr64_lod_begin_preparation(live);
        auto observe_context = context();
        const auto original_observe_context = observe_context;
        rr64_lod_observe_pair(live, &observe_context);
        check(std::memcmp(&observe_context, &original_observe_context, sizeof(observe_context)) == 0,
            "observing mounted or crashed actor roots preserves every caller register");
        late_camera = full_camera;
        if (step != 0u) {
            // Original 7B650 subtract helper, after the pair observation.
            auto rebase_context = context();
            rebase_context.r4 = static_cast<std::int32_t>(camera);
            rebase_context.r5 = static_cast<std::int32_t>(sector);
            rebase_context.r6 = static_cast<std::int32_t>(camera);
            func_80012CE0(live, &rebase_context);
            for (unsigned axis = 0; axis < 3; ++axis) late_camera[axis] -= origin[axis];
        }
        if (!keep_anchors) late_anchors = {Vec3{7, 8, 9}, Vec3{-7, -8, -9}};
        floats(live, Fixture::bike_entity + 0x53cu, late_anchors[0]);
        floats(live, Fixture::owner + 0x5dcu, late_anchors[1]);
        seed_camera_banks(live, step);
        const auto before_prepare = fixture.live;
        shadow_calls = 0;
        auto prepare_context = context();
        const auto original_prepare_context = prepare_context;
        rr64_lod_prepare_shadow(live, &prepare_context, step & 1u);
        check(std::memcmp(&prepare_context, &original_prepare_context, sizeof(prepare_context)) == 0,
            "private mounted or crashed root preparation preserves every caller register");
        check(fixture.live == before_prepare, "private preparation preserves every real guest byte");
        check(shadow_calls == 2u, "both original root writers complete their secondary shadow path");
        write_u32(live, globals::actor_render_buffer_slot, slot);
        rr64_lod_begin_draw(live);
        for (unsigned actor = 0; actor < 2; ++actor) {
            const auto node = actor ? Fixture::rider_node : Fixture::bike_node;
            const auto pose = actor ? Fixture::rider_pose : Fixture::bike_pose;
            const auto buffer = (actor ? Fixture::rider_buffer : Fixture::bike_buffer) + slot * 0x180u;
            const auto source = actor ? 1u : bike_source;
            const float scale = actor ? 100.0f : source_scales[source];
            // The common depth convention applies before and after the old
            // source-1 root limit, including stock tiers still using source 1.
            const bool far_root = source == 1u;
            const unsigned render_source = far_root ? 2u : source;
            check(rr64_lod_select(live, node, lod) == 0u, "original root writers publish a detailed paired pose");
            for (unsigned axis = 0; axis < 3; ++axis) {
                float value = 0;
                read_float(live, pose + axis * 4u, value);
                check(value == relative[actor][axis] * scale, "captured full-camera root survives later camera/anchor changes");
            }
            rr64_lod_end_actor();
            write_u32(live, 0x800AC650u, 0x805C0000u);
            auto render_context = context();
            render_context.r4 = static_cast<std::int32_t>(node);
            render_context.r5 = 0u;
            func_80011CC0(live, &render_context);
            for (unsigned bank = 0; bank < 2; ++bank) {
                const auto origin_address = (bank ? 0x800B6DE8u : 0x800B6568u) + render_source * 128u + slot * 64u;
                check(std::memcmp(live + (buffer - kRdramBegin) + bank * 64u,
                    live + (origin_address - kRdramBegin), 64u) == 0,
                    "renderer uses the detailed source's exact camera bank and current slot");
            }
            Matrix expected = rotation(quaternions[actor]);
            if (source == 0u) { for (unsigned column = 0; column < 4; ++column) expected[8u + column] *= 0.5f; }
            for (unsigned axis = 0; axis < 3; ++axis) expected[12u + axis] = relative[actor][axis] * scale;
            const Matrix unscaled_root = expected;
            if (far_root) {
                for (unsigned row = 0; row < 4; ++row) {
                    for (unsigned axis = 0; axis < 3; ++axis) expected[row * 4u + axis] *= 0.1f;
                }
            }
            Matrix4x4Snapshot actual;
            check(decode_n64_matrix(live, buffer + 128u, actual), "renderer writes the detailed root matrix");
            for (unsigned element = 0; element < 16; ++element) {
                if (std::fabs(actual.values[element] - expected[element]) > 1.0f / 65536.0f) {
                    std::fprintf(stderr, "[RR64-REAL-MATH] packed id=%u source=%u step=%u lod=%u slot=%u actor=%u element=%u actual=%.6f expected=%.6f crash=%u\n",
                        racer_id, source, step, lod, slot, actor, element, actual.values[element], expected[element], current_crash);
                    passed = false;
                }
            }
            // Check each original child writer independently of the root scale.
            // A synthetic three-link chain then exercises the complete affine
            // matrices with nonzero vertices; this is not an animation oracle.
            constexpr std::array<Vector, 3> vertices{Vector{-40, 60, 20, 1},
                Vector{10, 140, -5, 1}, Vector{-120, -20, 50, 1}};
            std::array<Matrix, 3> children{};
            for (unsigned child = 1; child < 4; ++child) {
                Matrix4x4Snapshot child_actual;
                check(decode_n64_matrix(live, buffer + (child + 2u) * 64u, child_actual), "renderer writes all detailed child matrices");
                children[child - 1u] = child_actual.values;
                const auto child_expected = child_matrix(actor, child);
                for (unsigned element = 0; element < 16; ++element) {
                    check(std::fabs(child_actual.values[element] - child_expected[element]) <= 1.0f / 65536.0f,
                        "bank normalization preserves each child's rotation and local translation");
                }
            }
            for (const auto& vertex : vertices) {
                auto actual_vertex = vertex, expected_vertex = vertex;
                for (unsigned child = 3; child > 0; --child) {
                    actual_vertex = transform(actual_vertex, children[child - 1u]);
                    expected_vertex = transform(expected_vertex, child_matrix(actor, child));
                }
                const auto unscaled_vertex = transform(expected_vertex, unscaled_root);
                actual_vertex = transform(actual_vertex, actual.values);
                expected_vertex = transform(expected_vertex, expected);
                for (unsigned axis = 0; axis < 4; ++axis) {
                    check(std::fabs(actual_vertex[axis] - expected_vertex[axis]) <= 0.005f,
                        "detailed child/vertex composition retains geometry through root bank conversion");
                }
                if (far_root) {
                    Matrix4x4Snapshot projection, view;
                    decode_n64_matrix(live, buffer, projection);
                    decode_n64_matrix(live, buffer + 64u, view);
                    const auto actual_clip = transform(transform(actual_vertex, view.values), projection.values);
                    // A virtual uncompressed 100x bank with the same world
                    // clipping planes provides an independent geometric oracle.
                    // It remains float; packing it would overflow the old roots.
                    const auto reference_clip = transform(transform(unscaled_vertex, camera_view(step, 100.0f)),
                        perspective(live, step, slot, 100.0f, false));
                    check(actual_clip[3] > 0.0f && reference_clip[3] > 0.0f, "far detailed geometry remains in front of the camera");
                    for (unsigned axis = 0; axis < 3; ++axis) {
                        check(std::fabs(actual_clip[axis] / actual_clip[3] - reference_clip[axis] / reference_clip[3]) < 0.0001f,
                            "far detailed vertex projection matches the uncompressed world geometry");
                        if (distances[step] < 700.0f) {
                            check(std::fabs(actual_clip[axis]) <= actual_clip[3], "visible detailed vertices survive the matching bank's clip volume");
                        }
                    }
                    if (distances[step] > 700.0f) check(actual_clip[2] > actual_clip[3],
                        "actors beyond the original world far plane remain clipped");
                }
            }
            std::uint32_t display_end = 0;
            read_u32(live, 0x800AC650u, display_end);
            bool saw_vertices = false, saw_normalization = false, saw_display_list = false;
            for (auto command = 0x805C0000u; command + 8u <= display_end; command += 8u) {
                std::uint32_t opcode = 0, argument = 0;
                read_u32(live, command, opcode);
                read_u32(live, command + 4u, argument);
                const auto resource = actor ? Fixture::rider_source : Fixture::bike_source;
                saw_vertices |= opcode == 0xDB06001Cu && argument == resource + 0x840u;
                saw_normalization |= opcode == 0xDB060018u && argument == 0x800B73E8u + render_source * 4u + slot * 2u;
                saw_display_list |= opcode == 0xDE000000u && argument == resource + 0x400u;
            }
            check(saw_vertices && saw_display_list, "near and far draws use the detailed vertex payload and display list");
            check(saw_normalization, "renderer emits the exact selected bank and slot normalization pointer");
            check(std::memcmp(live + (pose - kRdramBegin), before_prepare.data() + (pose - kRdramBegin), 0x80u) == 0,
                "actual renderer exit restores every borrowed detailed pose word");
        }
        rr64_lod_end_draw(live);
    }
}
void test_crash_roots() {
    // Admission, root arithmetic and packed geometry are real; the synthetic
    // completed children do not claim original crash-animation parity.
    for (unsigned state : {1u, 2u, 3u}) {
        for (unsigned slot : {0u, 1u}) {
            for (bool rider_stock_source2 : {false, true}) run_sequence(1u, slot, rider_stock_source2, state);
        }
    }
}
void test_original_range_fallback(unsigned far_actor) {
    Fixture fixture;
    live = fixture.live.data();
    seed_roots(fixture, 1u);
    late_camera = {1000, 2000, 20};
    late_anchors = {Vec3{1002, 2040, 20.5f}, Vec3{1003, 2041, 21.5f}};
    late_anchors[far_actor][1] = 4000.0f;
    floats(live, camera, late_camera);
    floats(live, Fixture::bike_entity + 0x53cu, late_anchors[0]);
    floats(live, Fixture::owner + 0x5dcu, late_anchors[1]);
    seed_camera_banks(live, 0u);
    for (unsigned actor = 0; actor < 2; ++actor) {
        const auto node = actor ? Fixture::rider_node : Fixture::bike_node;
        write_float(live, node + 8u, actor == far_actor ? 2000.0f * 2000.0f : 40.0f * 40.0f);
        for (unsigned view = 0; view < 4; ++view) rr64_lod_observe_allocation(live, node, view, 0x180u);
    }
    rr64_lod_begin_preparation(live);
    auto ctx = context();
    rr64_lod_observe_pair(live, &ctx);
    const auto before = fixture.live;
    rr64_lod_prepare_shadow(live, &ctx, 0);
    check(fixture.live == before, "original root range refusal never mutates live memory");
    rr64_lod_begin_draw(live);
    for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
        check(rr64_lod_select(live, node, 2u) == 2u,
            "either original root range refusal keeps both actors on stock LOD");
    }
    rr64_lod_end_draw(live);
}
void test_stock_depth_on_slopes(unsigned slot, float world_far,
    bool rider_source1 = false, unsigned rider_stock_lod = 2u, unsigned direct_order = 0u) {
    // R13 changed a stock-source2 actor's depth below the root limit. R15
    // retained the same depth mismatch for a rider whose stock source is1:
    // its bank1 far plane clips at327.67 world units, while terrain/bike use
    // the original world far plane. Crossing164m then changes its depth again.
    const std::array<float, 7> distances = rider_source1
        ? std::array<float, 7>{60, 80, 100, 150, 160, 164, 180}
        : std::array<float, 7>{40, 80, 100, 150, 0, 0, 0};
    for (unsigned distance_index = 0; distance_index < (rider_source1 ? 7u : 4u); ++distance_index) {
        const float distance = distances[distance_index];
        Fixture fixture;
        live = fixture.live.data();
        seed_roots(fixture, 1u, !rider_source1);
        write_u16(live, Fixture::rider_node + actor_scene::selected_lod, rider_stock_lod);
        write_u32(live, Fixture::rider_node + actor_scene::current_model,
            Fixture::rider_graph + rider_stock_lod * 0x100u);
        late_camera = {1000, 2000, 20};
        late_anchors = {Vec3{1001, 2000 + distance, 20}, Vec3{1001, 2000 + distance, 20}};
        floats(live, camera, late_camera);
        floats(live, Fixture::bike_entity + 0x53cu, late_anchors[0]);
        floats(live, Fixture::owner + 0x5dcu, late_anchors[1]);
        seed_camera_banks(live, 0u, world_far, 2.0f, late_camera);
        for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
            write_float(live, node + 8u, 1.0f + distance * distance);
            for (unsigned view = 0; view < 4; ++view) rr64_lod_observe_allocation(live, node, view, 0x180u);
        }
        rr64_lod_begin_preparation(live);
        auto ctx = context();
        rr64_lod_observe_pair(live, &ctx);
        const auto before = fixture.live;
        rr64_lod_prepare_shadow(live, &ctx, direct_order);
        check(fixture.live == before, "stock-depth preparation does not move live actors or change visibility");
        write_u32(live, globals::actor_render_buffer_slot, slot);
        rr64_lod_begin_draw(live);
        Matrix4x4Snapshot stock_projection, stock_view, terrain_projection, terrain_view;
        decode_n64_matrix(live, 0x800B6668u + slot * 64u, stock_projection);
        decode_n64_matrix(live, 0x800B6EE8u + slot * 64u, stock_view);
        decode_n64_matrix(live, 0x800B6568u + slot * 64u, terrain_projection);
        decode_n64_matrix(live, 0x800B6DE8u + slot * 64u, terrain_view);
        for (unsigned actor = 0; actor < 2; ++actor) {
            const auto node = actor ? Fixture::rider_node : Fixture::bike_node;
            const auto buffer = (actor ? Fixture::rider_buffer : Fixture::bike_buffer) + slot * 0x180u;
            check(rr64_lod_select(live, node, actor ? rider_stock_lod : 2u) == 0u,
                "middle and far actors receive full detail across all original rider tiers");
            rr64_lod_end_actor();
            write_u32(live, 0x800AC650u, 0x805C0000u);
            auto draw = context(); draw.r4 = static_cast<std::int32_t>(node); draw.r5 = 0u;
            func_80011CC0(live, &draw);
            Matrix4x4Snapshot actual_projection, actual_view, actual_root;
            decode_n64_matrix(live, buffer, actual_projection);
            decode_n64_matrix(live, buffer + 64u, actual_view);
            decode_n64_matrix(live, buffer + 128u, actual_root);
            check(actual_projection.values == stock_projection.values && actual_view.values == stock_view.values,
                "detailed rider and bike share terrain-compatible source2 depth on both sides of the old root limit");
            for (float slope : {-0.003f, 0.0f, 0.03f}) {
                for (float height : {0.02f, 0.25f, 1.0f}) {
                    // Ground passes through (1,distance,0). Intersect the same
                    // camera ray with that sloped plane, so its pixel genuinely
                    // overlaps this actor vertex in the depth test.
                    const float eye_to_plane = 2.0f + slope * (distance - 2.0f);
                    const float t = eye_to_plane / (eye_to_plane - height);
                    const float ground_y = 2.0f + t * (distance - 2.0f);
                    const float ground_z = slope * (ground_y - distance);
                    const Vector terrain_point{(late_camera[0] + 1.0f) * 4.0f,
                        (late_camera[1] + ground_y) * 4.0f, (late_camera[2] + ground_z) * 4.0f, 1};
                    const auto terrain_clip = transform(transform(terrain_point, terrain_view.values), terrain_projection.values);
                    check(terrain_clip[3] > 0.0f && terrain_clip[2] / terrain_clip[3] > -1.0f &&
                        terrain_clip[2] / terrain_clip[3] < 1.0f,
                        "comparison ground point lies inside the original terrain clip range");
                    // Compare the intended world point through the actual stock
                    // camera bank. The bike suffix uses source-dependent units;
                    // this rider case checks the same world-point depth policy,
                    // not a claim about the vanilla rider suffix's fixed scale.
                    const auto stock_clip = transform(transform(Vector{10, distance * 10.0f, height * 10.0f, 1}, stock_view.values), stock_projection.values);
                    const auto actual_clip = transform(transform(transform(Vector{0, 0, height * 100.0f, 1},
                        actual_root.values), actual_view.values), actual_projection.values);
                    for (unsigned axis = 0; axis < 3; ++axis) {
                        const float actual_ndc = actual_clip[axis] / actual_clip[3];
                        const float stock_ndc = stock_clip[axis] / stock_clip[3];
                        if (std::fabs(actual_ndc - stock_ndc) > 0.00003f) {
                            std::fprintf(stderr, "%s FAILED id=%u actor=%u lod=%u order=%u distance=%.0f far=%.0f slope=%.3f height=%.2f axis=%u actual=%.7f stock=%.7f\n",
                                rider_source1 ? "[RR64-RIDER-DEPTH]" : "[RR64-STOCK-DEPTH]",
                                racer_id, actor, actor ? rider_stock_lod : 2u, direct_order, distance,
                                world_far, slope, height, axis, actual_ndc, stock_ndc);
                            passed = false;
                        }
                    }
                    check(std::fabs(stock_clip[1] / stock_clip[3] - terrain_clip[1] / terrain_clip[3]) < 0.00003f,
                        "ground counterexample overlaps the same projected pixel as the actor");
                    check(stock_clip[2] / stock_clip[3] < terrain_clip[2] / terrain_clip[3],
                        "stock actor vertex above the slope passes the terrain depth test");
                    check(actual_clip[2] / actual_clip[3] < terrain_clip[2] / terrain_clip[3] + 0.000002f,
                        "promoted actor vertex above the slope is not buried by a different far-plane depth mapping");
                }
            }
        }
        rr64_lod_end_draw(live);
    }
}
void test_rider_depth_cases() {
    for (unsigned stock_lod : {0u, 1u, 2u}) {
        for (unsigned slot : {0u, 1u}) {
            for (unsigned direct_order : {0u, 1u}) {
                test_stock_depth_on_slopes(slot, 900.0f, true, stock_lod, direct_order);
            }
        }
    }
}
void test_far_bank_refusal(unsigned bad_resource, unsigned slot, float distance = 201.0f) {
    Fixture fixture;
    live = fixture.live.data();
    seed_roots(fixture, 1u);
    late_camera = {1000, 2000, 20};
    late_anchors = {Vec3{1002, 2000 + distance, 20.5f}, Vec3{1003, 2000 + distance, 21.5f}};
    floats(live, camera, late_camera);
    floats(live, Fixture::bike_entity + 0x53cu, late_anchors[0]);
    floats(live, Fixture::owner + 0x5dcu, late_anchors[1]);
    seed_camera_banks(live, 0u);
    // Keep the alternate source's index identical so the pointer mutation case
    // exercises exact publication identity, independently of the index case.
    write_u16(live, Fixture::bike_source + 0x300u + 0x12u, 2u);
    for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
        write_float(live, node + 8u, distance * distance);
        for (unsigned view = 0; view < 4; ++view) rr64_lod_observe_allocation(live, node, view, 0x180u);
    }
    rr64_lod_begin_preparation(live);
    auto ctx = context();
    rr64_lod_observe_pair(live, &ctx);
    const auto before = fixture.live;
    rr64_lod_prepare_shadow(live, &ctx, 0u);
    check(fixture.live == before, "near and far preparation preserves all live camera, distance and pose words");
    write_u32(live, globals::actor_render_buffer_slot, slot);
    // The active camera banks are produced after cached preparation. Reject an
    // unavailable bank at consumption, including the other physical slot.
    if (bad_resource == 0u) write_u16(live, 0x800B6C00u, 0u);
    else if (bad_resource == 1u) write_u16(live, 0x800B73F0u + slot * 2u, 0u);
    else if (bad_resource < 4u) {
        Matrix zero{};
        seed_matrix(live, (bad_resource == 2u ? 0x800B6668u : 0x800B6EE8u) + slot * 64u, zero);
    }
    else if (bad_resource == 4u) write_float(live, 0x8009DBB4u, 11.0f);
    else if (bad_resource == 5u) write_float(live, Fixture::rider_node + 8u, 202.0f * 202.0f);
    else if (bad_resource == 6u) write_u32(live, Fixture::bike_graph + 0x200u + 0x14u, Fixture::bike_source + 0x300u);
    else write_u16(live, Fixture::bike_source + 0x200u + 0x12u, 1u);
    rr64_lod_begin_draw(live);
    for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
        check(rr64_lod_select(live, node, 2u) == 2u,
            "invalid bank or changed source/distance certificate keeps the pair on stock LOD");
        rr64_lod_end_actor();
    }
    rr64_lod_end_draw(live);
}
void test_invalid_far_plan() {
    for (unsigned invalid = 0; invalid < 6; ++invalid) {
        Fixture fixture;
        auto* memory = fixture.live.data();
        seed_roots(fixture, 1u);
        write_float(memory, Fixture::bike_node + 8u, 200.0f * 200.0f);
        rr64::lod::RootRenderPlan plan{};
        check(rr64::lod::capture_root_render_plan(memory, Fixture::bike_node, 0u, plan) && plan.normalized,
            "finite original 100-to-10 source units admit a far plan");
        if (invalid == 0u) write_float(memory, 0x8009DBB0u, 101.0f);
        else if (invalid == 1u) write_float(memory, 0x8009DBB4u, 11.0f);
        else if (invalid == 2u) write_u32(memory, Fixture::bike_node + 8u, 0x7fc00000u);
        else if (invalid == 3u) write_u32(memory, Fixture::bike_node + 8u, 0x7f800000u);
        else if (invalid == 4u) write_float(memory, Fixture::bike_node + 8u, -1.0f);
        else write_u32(memory, 0x8009DBA8u, 1u);
        const auto before = fixture.live;
        check(!rr64::lod::capture_root_render_plan(memory, Fixture::bike_node, 0u, plan),
            "wrong source units, nonfinite/negative distance or wrong distance viewport refuse far conversion");
        check(fixture.live == before, "far plan refusal never repairs or modifies guest inputs");
    }
    for (const auto pair : {std::array<unsigned, 2>{0u, 2u}, std::array<unsigned, 2>{2u, 1u},
        std::array<unsigned, 2>{1u, 0u}}) {
        Fixture fixture;
        auto* memory = fixture.live.data();
        seed_roots(fixture, pair[0]);
        write_float(memory, Fixture::bike_node + 8u, 40.0f * 40.0f);
        write_u16(memory, Fixture::bike_source + 0x200u + 0x12u, pair[1]);
        rr64::lod::RootRenderPlan plan{};
        check(!rr64::lod::capture_root_render_plan(memory, Fixture::bike_node, 0u, plan),
            "unsupported absolute/relative source basis changes refuse promotion");
    }
}
void test_hidden_far_peer() {
    Fixture fixture;
    live = fixture.live.data();
    seed_roots(fixture, 1u);
    late_camera = {1000, 2000, 20};
    late_anchors = {Vec3{1002, 2200, 20.5f}, Vec3{1003, 2201, 21.5f}};
    floats(live, camera, late_camera);
    floats(live, Fixture::bike_entity + 0x53cu, late_anchors[0]);
    floats(live, Fixture::owner + 0x5dcu, late_anchors[1]);
    seed_camera_banks(live, 0u);
    write_u16(live, Fixture::rider_graph + 0x200u + 0xau, 1u);
    for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
        write_float(live, node + 8u, 201.0f * 201.0f);
        for (unsigned view = 0; view < 4; ++view) rr64_lod_observe_allocation(live, node, view, 0x180u);
    }
    rr64_lod_begin_preparation(live);
    auto ctx = context();
    rr64_lod_observe_pair(live, &ctx);
    const auto before = fixture.live;
    shadow_calls = 0;
    rr64_lod_prepare_shadow(live, &ctx, 0u);
    check(shadow_calls == 2u, "visible far bike receives the hidden rider's freshly prepared private peer");
    check(fixture.live == before, "hidden-peer preparation preserves every live graph visibility flag and pose");
    rr64_lod_begin_draw(live);
    for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
        check(rr64_lod_select(live, node, 2u) == 0u, "hidden-peer preparation certifies the complete far pair");
        rr64_lod_end_actor();
    }
    write_u32(live, 0x800AC650u, 0x805C0000u);
    auto draw = context(); draw.r4 = static_cast<std::int32_t>(Fixture::bike_node); draw.r5 = 0u;
    func_80011CC0(live, &draw);
    std::uint32_t display_end = 0;
    read_u32(live, 0x800AC650u, display_end);
    check(display_end > 0x805C0000u, "visible far bike still emits its draw");
    write_u32(live, 0x800AC650u, 0x805C0000u);
    const auto before_hidden_draw = fixture.live;
    draw = context(); draw.r4 = static_cast<std::int32_t>(Fixture::rider_node); draw.r5 = 0u;
    func_80011CC0(live, &draw);
    read_u32(live, 0x800AC650u, display_end);
    check(display_end == 0x805C0000u, "arbitrarily hidden rider without original range provenance stays hidden");
    std::uint16_t flags = 0;
    read_u16(live, Fixture::rider_graph + 0x200u + 0xau, flags);
    check(flags == 1u, "live rider visibility remains stock after preparation and consumption");
    check(std::memcmp(live + (Fixture::rider_buffer - kRdramBegin),
        before_hidden_draw.data() + (Fixture::rider_buffer - kRdramBegin), 0x180u) == 0,
        "hidden rider's transform allocation is untouched by the original renderer");
    rr64_lod_end_draw(live);
}
enum class RiderRangeCase {
    VisibleRangePeer, ArbitraryHidden, Inactive, HiddenBike, MissingBank,
    StalePair, Detached, AlternateCamera, OutsideView, DegenerateView,
    NonfiniteView, ChangedDistance, NearHidden
};
void test_rider_range_visibility(RiderRangeCase scenario, unsigned slot,
    unsigned direct_order, float distance) {
    Fixture fixture;
    live = fixture.live.data();
    seed_roots(fixture, 1u);
    late_camera = {1000, 2000, 20};
    late_anchors = {Vec3{1002, 2000 + distance, 20.5f}, Vec3{1003, 2000 + distance, 21.5f}};
    floats(live, camera, late_camera);
    floats(live, sector, late_camera);
    floats(live, Fixture::bike_entity + 0x53cu, late_anchors[0]);
    floats(live, Fixture::owner + 0x5dcu, late_anchors[1]);
    floats(live, Fixture::owner + 0x8cu, late_anchors[1]);
    // Exact 19F7C/14178 ROM constants: sector-relative XY uses four units,
    // and the triangle's barycentric-coordinate sum is bounded by one.
    write_float(live, 0x80000EB8u, 4.0f);
    write_float(live, 0x80000D50u, 1.0f);
    floats(live, 0x800BACA0u, Vec3{0, 0, 0});
    floats(live, 0x800BACB0u, Vec3{-2000, 4000, 0});
    floats(live, 0x800BACC0u, Vec3{2000, 4000, 0});
    if (scenario == RiderRangeCase::OutsideView) {
        write_float(live, Fixture::owner + 0x8cu, late_camera[0] + 2000.0f);
    }
    if (scenario == RiderRangeCase::DegenerateView) {
        floats(live, 0x800BACC0u, Vec3{-2000, 4000, 0});
    }
    if (scenario == RiderRangeCase::NonfiniteView) {
        write_u32(live, 0x800BACB0u, 0x7fc00000u);
    }
    else {
        auto view_context = context();
        view_context.f_odd = &view_context.f0.u32h;
        view_context.r4 = static_cast<std::int32_t>(Fixture::owner + 0x8cu);
        func_80019F7C(live, &view_context);
        const bool should_be_inside = scenario != RiderRangeCase::OutsideView &&
            scenario != RiderRangeCase::DegenerateView;
        check((view_context.r2 != 0u) == should_be_inside,
            "actual stock rider point/triangle helper confirms the visibility fixture");
    }
    seed_camera_banks(live, 0u);
    for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
        write_float(live, node + 8u, distance * distance + 9.0f);
        for (unsigned view = 0; view < 4; ++view) rr64_lod_observe_allocation(live, node, view, 0x180u);
    }
    rr64_lod_begin_preparation(live);
    auto observed_context = context();
    rr64_lod_observe_pair(live, &observed_context);
    const auto before_preparation = fixture.live;
    shadow_calls = 0;
    rr64_lod_prepare_shadow(live, &observed_context, direct_order);
    check(fixture.live == before_preparation,
        "rider range preparation preserves all real camera, visibility and pose state");
    check(shadow_calls == 2u, "rider range fixture privately prepares both original root writers");

    // The original live pass follows cached preparation. Its EC28 rejection
    // must author the range provenance; writing a hidden flag is insufficient.
    if (scenario == RiderRangeCase::ArbitraryHidden || scenario == RiderRangeCase::NearHidden) {
        write_u16(live, Fixture::rider_graph + 0x200u + 0xau, 1u);
    }
    if (scenario == RiderRangeCase::Inactive) {
        write_u16(live, 0x800D8570u + racer_id * 280u + 0x24u, 0u);
    }
    const auto before_stock_roots = fixture.live;
    auto stock_context = context();
    stock_context.f_odd = &stock_context.f0.u32h;
    func_8005EB50(live, &stock_context);
    std::uint16_t flags = 0;
    read_u16(live, Fixture::rider_graph + 0x200u + 0xau, flags);
    check((flags & 1u) != 0u, "actual original rider pass leaves its stock graph hidden");
    check(shadow_calls == 2u, "live range-hide path does not enter secondary shadow/pose writers");
    check(std::memcmp(live + (Fixture::rider_pose - kRdramBegin),
        before_stock_roots.data() + (Fixture::rider_pose - kRdramBegin), 0x300u) == 0,
        "original range hide leaves every live rider pose untouched");
    if (scenario == RiderRangeCase::HiddenBike) {
        write_u16(live, Fixture::bike_graph + 0x200u + 0xau, 1u);
    }
    if (scenario == RiderRangeCase::MissingBank) write_u16(live, 0x800B73F0u + slot * 2u, 0u);
    if (scenario == RiderRangeCase::Detached) write_u32(live, Fixture::bike_entity + bike::rider_pointer, 0u);
    if (scenario == RiderRangeCase::ChangedDistance) {
        write_float(live, Fixture::rider_node + 8u, distance * distance + 25.0f);
    }
    if (scenario == RiderRangeCase::StalePair) rr64_lod_invalidate(live);
    write_u32(live, globals::actor_render_buffer_slot, slot);
    rr64_lod_begin_draw(live);
    write_u32(live, 0x800AC650u, 0x805C0000u);
    const auto before_draw = fixture.live;
    auto draw_context = context();
    draw_context.r4 = static_cast<std::int32_t>(Fixture::rider_node);
    draw_context.r5 = scenario == RiderRangeCase::AlternateCamera ? 1u : 0u;
    func_80011CC0(live, &draw_context);
    std::uint32_t display_end = 0;
    read_u32(live, 0x800AC650u, display_end);
    const bool should_render = scenario == RiderRangeCase::VisibleRangePeer;
    const bool did_render = display_end > 0x805C0000u;
    if (did_render != should_render) {
        std::fprintf(stderr, "[RR64-RIDER-RANGE] FAILED case=%u id=%u slot=%u order=%u distance=%.0f: rendered=%u expected=%u\n",
            static_cast<unsigned>(scenario), racer_id, slot, direct_order, distance, did_render, should_render);
        passed = false;
    }
    if (did_render && should_render) {
        bool detailed_display_list = false, stock_display_list = false;
        for (auto command = 0x805C0000u; command + 8u <= display_end; command += 8u) {
            std::uint32_t opcode = 0, argument = 0;
            read_u32(live, command, opcode); read_u32(live, command + 4u, argument);
            detailed_display_list |= opcode == 0xDE000000u && argument == Fixture::rider_source + 0x400u;
            stock_display_list |= opcode == 0xDE000000u && argument == Fixture::rider_source + 0x500u;
        }
        check(detailed_display_list && !stock_display_list,
            "range-hidden rider resumes through its highest-detail display list");
        const auto buffer = Fixture::rider_buffer + slot * 0x180u;
        check(std::memcmp(live + (buffer - kRdramBegin), live + (0x800B6668u + slot * 64u - kRdramBegin), 64u) == 0 &&
            std::memcmp(live + (buffer - kRdramBegin) + 64u, live + (0x800B6EE8u + slot * 64u - kRdramBegin), 64u) == 0,
            "rescued rider uses the current slot's source2 camera and depth bank");
        Matrix4x4Snapshot root;
        decode_n64_matrix(live, buffer + 128u, root);
        check(std::fabs(root.values[12] - 30.0f) < 0.0001f &&
            std::fabs(root.values[13] - distance * 10.0f) < 0.0001f &&
            std::fabs(root.values[14] - 15.0f) < 0.0001f,
            "rescued rider retains its own captured position in safe source2 units");
    }
    read_u16(live, Fixture::rider_graph + 0x200u + 0xau, flags);
    check((flags & 1u) != 0u, "render-local range rescue never changes the live stock hidden flag");
    for (auto pose : {Fixture::bike_pose, Fixture::rider_pose}) {
        check(std::memcmp(live + (pose - kRdramBegin), before_draw.data() + (pose - kRdramBegin), 0x300u) == 0,
            "range visibility query and selector restore all paired live poses");
    }
    if (!should_render) {
        check(std::memcmp(live + (Fixture::rider_buffer + slot * 0x180u - kRdramBegin),
            before_draw.data() + (Fixture::rider_buffer + slot * 0x180u - kRdramBegin), 0x180u) == 0,
            "refused hidden rider leaves its renderer destination untouched");
    }
    rr64_lod_end_draw(live);
}
void test_rider_range_cases() {
    for (unsigned slot : {0u, 1u}) {
        for (unsigned direct_order : {0u, 1u}) {
            for (float distance : {180.0f, 240.0f, 400.0f}) {
                test_rider_range_visibility(RiderRangeCase::VisibleRangePeer, slot, direct_order, distance);
            }
            for (auto scenario : {RiderRangeCase::ArbitraryHidden, RiderRangeCase::Inactive,
                RiderRangeCase::HiddenBike, RiderRangeCase::MissingBank, RiderRangeCase::StalePair,
                RiderRangeCase::Detached, RiderRangeCase::AlternateCamera, RiderRangeCase::OutsideView,
                RiderRangeCase::DegenerateView, RiderRangeCase::NonfiniteView, RiderRangeCase::ChangedDistance}) {
                test_rider_range_visibility(scenario, slot, direct_order, 240.0f);
            }
            test_rider_range_visibility(RiderRangeCase::NearHidden, slot, direct_order, 100.0f);
        }
    }
}
void test_alternate_camera() {
    Fixture fixture;
    live = fixture.live.data();
    seed_roots(fixture, 1u);
    late_camera = {1000, 2000, 20};
    late_anchors = {Vec3{1002, 2200, 20.5f}, Vec3{1003, 2201, 21.5f}};
    floats(live, camera, late_camera);
    floats(live, Fixture::bike_entity + 0x53cu, late_anchors[0]);
    floats(live, Fixture::owner + 0x5dcu, late_anchors[1]);
    seed_camera_banks(live, 0u);
    seed_matrix(live, 0x800B19D0u, rotation(Quaternion{0.5f, 0.5f, 0.5f, 0.5f}));
    seed_matrix(live, 0x800B1990u, camera_view(2u, 10.0f));
    for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
        write_float(live, node + 8u, 201.0f * 201.0f);
        for (unsigned view = 0; view < 4; ++view) rr64_lod_observe_allocation(live, node, view, 0x180u);
    }
    rr64_lod_begin_preparation(live);
    auto ctx = context();
    rr64_lod_observe_pair(live, &ctx);
    rr64_lod_prepare_shadow(live, &ctx, 0u);
    rr64_lod_begin_draw(live);
    for (unsigned actor = 0; actor < 2; ++actor) {
        const auto node = actor ? Fixture::rider_node : Fixture::bike_node;
        const auto buffer = actor ? Fixture::rider_buffer : Fixture::bike_buffer;
        check(rr64_lod_select(live, node, 2u) == 0u, "alternate-camera test has a valid normal-camera detailed snapshot");
        rr64_lod_end_actor();
        write_u32(live, 0x800AC650u, 0x805C0000u);
        auto render_context = context();
        render_context.r4 = static_cast<std::int32_t>(node);
        render_context.r5 = 1u;
        func_80011CC0(live, &render_context);
        check(std::memcmp(live + (buffer - kRdramBegin), live + (0x800B19D0u - kRdramBegin), 64u) == 0 &&
            std::memcmp(live + (buffer - kRdramBegin) + 64u, live + (0x800B1990u - kRdramBegin), 64u) == 0,
            "alternate-camera renderer retains its original camera matrices");
        std::uint32_t display_end = 0;
        read_u32(live, 0x800AC650u, display_end);
        bool stock_display_list = false, detailed_display_list = false;
        const auto source = actor ? Fixture::rider_source : Fixture::bike_source;
        for (auto command = 0x805C0000u; command + 8u <= display_end; command += 8u) {
            std::uint32_t opcode = 0, argument = 0;
            read_u32(live, command, opcode); read_u32(live, command + 4u, argument);
            stock_display_list |= opcode == 0xDE000000u && argument == source + 0x500u;
            detailed_display_list |= opcode == 0xDE000000u && argument == source + 0x400u;
        }
        check(stock_display_list && !detailed_display_list, "alternate-camera renderer never promotes a normal-camera snapshot");
    }
    rr64_lod_end_draw(live);
}
void secondary_shadow(unsigned char* memory, recomp_context* ctx) {
    check(memory != live, "root helpers run only on private RDRAM");
    for (unsigned axis = 0; axis < 3; ++axis) {
        float value = 0;
        read_float(memory, camera + axis * 4u, value);
        check(value == late_camera[axis], "suffix restores later camera before original root secondary helpers");
        for (unsigned actor = 0; actor < 2; ++actor) {
            const auto address = actor ? Fixture::owner + 0x5dcu : Fixture::bike_entity + 0x53cu;
            read_float(memory, address + axis * 4u, value);
            check(value == late_anchors[actor][axis], "suffix restores both later anchor inputs before other helpers");
        }
    }
    floats(memory, static_cast<std::uint32_t>(ctx->r6), late_camera);
    floats(memory, static_cast<std::uint32_t>(ctx->r5), Quaternion{0, 0, 0, 1});
    ++shadow_calls;
}
} // namespace
extern "C" void func_8005E880(unsigned char* memory, recomp_context* ctx) { secondary_shadow(memory, ctx); }
extern "C" void func_8005E904(unsigned char* memory, recomp_context* ctx) { secondary_shadow(memory, ctx); }
extern "C" void func_8005AEE0(unsigned char* memory, recomp_context*) {
    for (unsigned actor = 0; actor < 2; ++actor) {
        const auto pose = actor ? Fixture::rider_pose : Fixture::bike_pose;
        for (unsigned child = 1; child < 4; ++child) {
            const auto expected = child_matrix(actor, child);
            floats(memory, pose + child * 32u, Vec3{expected[12], expected[13], expected[14]});
            floats(memory, pose + child * 32u + 12u,
                actor ? Quaternion{0.5f, -0.5f, 0.5f, 0.5f} : Quaternion{-0.5f, 0.5f, 0.5f, 0.5f});
        }
    }
    rr64_lod_shadow_stage(memory, Fixture::rider_node, 4u);
}
extern "C" void func_8005B63C(unsigned char*, recomp_context*) {}
extern "C" void func_8005B948(unsigned char* memory, recomp_context*) { rr64_lod_shadow_stage(memory, Fixture::bike_node, 128u); }
extern "C" void func_8005BEEC(unsigned char*, recomp_context*) {}
extern "C" void __sinf_recomp(unsigned char*, recomp_context* ctx) { ctx->f0.fl = std::sin(ctx->f12.fl); }
extern "C" void __cosf_recomp(unsigned char*, recomp_context* ctx) { ctx->f0.fl = std::cos(ctx->f12.fl); }
extern "C" unsigned int rr64_traffic_render_visibility(unsigned char*, unsigned int, unsigned int original) { return original; }
extern "C" void func_8000F9E8(unsigned char*, recomp_context*) {
    std::fprintf(stderr, "Unexpected accessory renderer in bounded math fixture\n"); std::abort();
}
int main(int argc, char** argv) {
    for (int argument = 1; argument < argc; ++argument) {
        if (std::strcmp(argv[argument], "--keep-anchors") == 0) keep_anchors = true;
        else if (std::strcmp(argv[argument], "--depth-only") == 0) depth_only = true;
        else if (std::strcmp(argv[argument], "--rider-range-only") == 0) rider_range_only = true;
        else if (std::strcmp(argv[argument], "--rider-depth-only") == 0) rider_depth_only = true;
        else if (std::strcmp(argv[argument], "--crash-only") == 0) crash_only = true;
        else if (std::strcmp(argv[argument], "--trace-camera") == 0) trace_camera = true;
        else { std::fprintf(stderr, "Unknown argument: %s\n", argv[argument]); return EXIT_FAILURE; }
    }
    check(rr64_render_only_max_lod_enabled() != 0, "run this fixture with Max LOD enabled");
    const char* mode = depth_only ? "stock-depth-only" : rider_range_only ? "rider-range-only"
        : rider_depth_only ? "rider-depth-only" : crash_only ? "crash-only" : "full";
    std::fprintf(stderr, "[RR64-REAL-MATH] START %s\n", mode);
    for (unsigned id : {0u, 4u}) {
        racer_id = id;
        if (rider_range_only) { test_rider_range_cases(); continue; }
        if (rider_depth_only) { test_rider_depth_cases(); continue; }
        if (crash_only) { test_crash_roots(); continue; }
        if (depth_only) {
            for (unsigned slot : {0u, 1u}) {
                for (float world_far : {500.0f, 700.0f}) test_stock_depth_on_slopes(slot, world_far);
            }
            continue;
        }
        for (unsigned source : {0u, 1u, 2u}) {
            for (unsigned slot : {0u, 1u}) {
                for (bool rider_stock_source2 : {false, true}) run_sequence(source, slot, rider_stock_source2);
            }
        }
        test_original_range_fallback(0u);
        test_original_range_fallback(1u);
        for (unsigned slot : {0u, 1u}) {
            for (float distance : {100.0f, 201.0f}) {
                for (unsigned resource = 0; resource < 8; ++resource) test_far_bank_refusal(resource, slot, distance);
            }
        }
        test_invalid_far_plan();
        test_hidden_far_peer();
        test_rider_range_cases();
        test_rider_depth_cases();
        test_alternate_camera();
        test_crash_roots();
        for (unsigned slot : {0u, 1u}) {
            for (float world_far : {500.0f, 700.0f}) test_stock_depth_on_slopes(slot, world_far);
        }
    }
    std::printf("[RR64-REAL-MATH] %s (%s): actual camera producer, roots, renderer and packing; stock depth/ground occlusion, bank/slot resources and geometry; animation/shadow/trig fixture substituted.\n",
        passed ? "PASS" : "FAIL", mode);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
