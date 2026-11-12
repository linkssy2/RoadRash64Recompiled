// The original whole-body writer is the oracle for AEE0 crash/recovery branches.
// This sibling uses the same eleven-bone data layout as the full-weight test.
// Animation, interpolation, quaternion math, trig and graph walking are original.
#include "rr64_engine_layout.hpp"
#include "recomp.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void func_8005AEE0(unsigned char*, recomp_context*);
extern "C" void func_80018114(unsigned char*, recomp_context*);

namespace {
using namespace rr64::engine;
constexpr unsigned bone_count = 11;
constexpr std::uint32_t graph = 0x80200000u;
constexpr std::uint32_t poses = 0x80300000u;
constexpr std::uint32_t channels = 0x80400000u;
constexpr std::uint32_t keys = 0x80410000u;
constexpr std::uint32_t stack = 0x807f0000u;
constexpr unsigned style = 2u, clip_count = 8u, clip_base = 2u;
constexpr std::uint32_t node = 0x80100000u, rider_entity = 0x80110000u, bike_entity = 0x80120000u;
bool passed = true;
unsigned cases = 0;

void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "[RR64-CRASH-ANIMATION] FAILED: %s\n", message); passed = false; }
}
template<std::size_t N>
void words(unsigned char* m, std::uint32_t address, const std::array<std::uint32_t, N>& values) {
    for (unsigned i = 0; i < N; ++i) write_u32(m, address + i * 4u, values[i]);
}
template<std::size_t N>
void floats(unsigned char* m, std::uint32_t address, const std::array<float, N>& values) {
    for (unsigned i = 0; i < N; ++i) write_float(m, address + i * 4u, values[i]);
}
float value(const unsigned char* m, std::uint32_t address) {
    float result = 0;
    // The shared checked reader's legacy signature is mutable; it only reads.
    check(read_float(const_cast<unsigned char*>(m), address, result), "fixture float address remains valid");
    return result;
}
void byte(unsigned char* m, std::uint32_t address, int v) {
    m[(address - kRdramBegin) ^ 3u] = static_cast<unsigned char>(v);
}
std::uint32_t bone_pose(unsigned bone) { return poses + (bone + 1u) * 0x20u; }
std::uint32_t bone_record(unsigned bone) { return graph + (bone + 1u) * 0x20u; }

void seed_constants(unsigned char* m) {
    // Exact initialized USA ROM words (main-segment ROM offset = VA low + C00).
    words(m, 0x80000D08u, std::array<std::uint32_t, 8>{
        0x3C010203u, 0x3F800000u, 0x3F800000u, 0x3F800000u,
        0x3C010203u, 0x3F800000u, 0x3F800000u, 0x3C010203u});
    words(m, 0x80000DC0u, std::array<std::uint32_t, 4>{
        0xBF800000u, 0x3F800000u, 0x3F800000u, 0x3A83126Fu});
    words(m, 0x80000E70u, std::array<std::uint32_t, 9>{
        0x3F800000u, 0x3ADA740Eu, 0x3ADA740Eu, 0x3F800000u,
        0x3ADA740Eu, 0x3ADA740Eu, 0x3F800000u, 0x3ADA740Eu, 0x3ADA740Eu});
    words(m, 0x80007D30u, std::array<std::uint32_t, 15>{
        0x3FC90FDBu, 0xBFC90FDBu, 0x3F800000u, 0x41600000u,
        0x3F800000u, 0x41600000u, 0x3F800000u, 0xBFC90FDBu,
        0x3FC90FDBu, 0x40490FDBu, 0x40490FDBu, 0x6C616773u,
        0x3F800000u, 0x40490FDBu, 0x3FC90FDBu});
    words(m, 0x80008058u, std::array<std::uint32_t, 20>{
        0xBFC55554u, 0xBC83656Du, 0x3F8110EDu, 0x3804C2A0u,
        0xBF29F6FFu, 0xEEA56814u, 0x3EC5DBDFu, 0x0E314BFEu,
        0x3FD45F30u, 0x6DC9C883u, 0x400921FBu, 0x50000000u,
        0x3E6110B4u, 0x611A6263u, 0u, 0u, 0x3FE00000u, 0u, 0x3FE00000u, 0u});
}

std::vector<unsigned char> seed(unsigned previous) {
    std::vector<unsigned char> memory(kRdramSize);
    auto* m = memory.data();
    seed_constants(m);
    write_u32(m, 0x8009DC2Cu, style);
    write_u16(m, graph + 4u, 0x13u);
    write_u16(m, graph + 2u, 4u);
    write_u32(m, graph + 0xCu, poses);
    floats(m, poses, std::array<float, 8>{15, -27, 39, 0, 0, 0, 1, 123});
    for (unsigned clip = 0; clip < clip_count; ++clip) {
        const auto channel_table = 0x800B74B0u + clip * 132u + style * 11220u;
    for (unsigned bone = 0; bone < bone_count; ++bone) {
        const auto record = bone_record(bone), pose = bone_pose(bone);
        write_u16(m, record + 4u, 0x12u);
        write_u16(m, record + 2u, bone + 1u == bone_count ? 0u : 4u);
        write_u32(m, record + 0xCu, pose);
        const float p = static_cast<float>(bone + 1u);
        if (previous == 0u) floats(m, pose, std::array<float, 8>{
            10 + p, -20 - p, 30 + 2 * p, 0, 0, 0, 1, 77 + p});
        else floats(m, pose, std::array<float, 8>{
            -70 - p, 90 + 2 * p, -110 - 3 * p, 0, 1, 0, 0, 77 + p});
        const auto channel = channels + clip * 0x1000u + bone * 0x40u;
        const auto rotation = keys + clip * 0x1000u + bone * 0x80u, translation = rotation + 0x40u;
        write_u32(m, channel_table + bone * 12u, channel);
        write_u32(m, channel_table + bone * 12u + 4u, rotation);
        write_u32(m, channel_table + bone * 12u + 8u, translation);
        byte(m, channel + 0xEu, bone == 3u || bone == 8u ? 0 : 2);
        byte(m, channel + 0xFu, bone == 5u || bone == 8u ? 0 : 2);
        write_float(m, channel + 0x14u, 0.23f + p * 0.01f);
        write_float(m, channel + 0x1Cu, 0.41f + p * 0.01f);
        for (unsigned key = 0; key < 2; ++key) {
            const auto r = rotation + key * 8u, t = translation + key * 8u;
            // Unit byte quaternions on different axes exercise true spherical
            // interpolation and both signs, not just identity/linear blending.
            byte(m, r + (key == 0u ? 3u : bone % 3u), bone % 2u ? -127 : 127);
            write_u16(m, r + 4u, key * 100u);
            write_u16(m, t, static_cast<std::uint16_t>(100 + clip * 71u + bone * 17u + key * 43u));
            write_u16(m, t + 2u, static_cast<std::uint16_t>(-200 - static_cast<int>(bone) * 13 + static_cast<int>(key) * 31));
            write_u16(m, t + 4u, static_cast<std::uint16_t>(300 + clip * 37u + bone * 19u - key * 53u));
            write_u16(m, t + 6u, key * 100u);
        }
    }
    }
    write_u32(m, 0x800A1454u, node);
    write_u32(m, node + 4u, rider_entity);
    write_u32(m, node + 0x28u, graph);
    write_u32(m, rider_entity + 0xCu, style);
    write_u32(m, rider_entity + rider::bike_pointer, bike_entity);
    write_u16(m, bike_entity + bike::drive_control_lockout, 1u);
    write_u32(m, 0x800A4F54u + style * 56u, clip_base);
    write_u32(m, 0x80005D60u, 0x3F000000u);
    write_u32(m, 0x80005D64u, 0x3F800000u);
    write_u32(m, 0x80005D9Cu, 0x3FAA3D71u);
    return memory;
}

struct Sample { unsigned clip; float phase; unsigned flags; };
Sample intended_sample(unsigned state, float phase) {
    if (state == 0u) return {clip_base, phase, 0x900u};
    if (state < 4u) return {clip_base + state, 0.0f, 0x100u};
    return {clip_base + state - 3u, phase * std::bit_cast<float>(0x3FAA3D71u), 0x100u};
}
unsigned completed = 0, entered = 0, returned = 0, direct = 0;
std::uint32_t pending_node = 0;
const std::vector<unsigned char>* expected_poses = nullptr;

bool pose_equal(const unsigned char* a, const unsigned char* b, unsigned bone) {
    const auto pose = bone_pose(bone);
    for (unsigned i = 0; i < 3; ++i) {
        const float x = value(a, pose + i * 4u), y = value(b, pose + i * 4u);
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x - y) > 0.00002f) return false;
    }
    float same = 0, opposite = 0;
    for (unsigned i = 3; i < 7; ++i) {
        const float x = value(a, pose + i * 4u), y = value(b, pose + i * 4u);
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        same += std::abs(x - y); opposite += std::abs(x + y);
    }
    return std::min(same, opposite) < 0.00004f;
}
void run_direct(std::vector<unsigned char>& memory, const Sample& sample, bool fr1) {
    recomp_context ctx{};
    ctx.mips3_float_mode = fr1;
    ctx.f_odd = fr1 ? &ctx.f1.u32l : &ctx.f0.u32h;
    ctx.r29 = static_cast<std::int32_t>(stack);
    ctx.r4 = static_cast<std::int32_t>(graph);
    ctx.r5 = sample.clip; ctx.r6 = sample.flags;
    ctx.r7 = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(sample.phase));
    write_float(memory.data(), stack + 0x10u, 0.0f);
    func_80018114(memory.data(), &ctx);
    check(static_cast<std::uint32_t>(ctx.r29) == stack, "direct animation returns its stack");
}
void run_original(std::vector<unsigned char>& memory, bool fr1) {
    completed = entered = returned = direct = 0; pending_node = 0;
    recomp_context ctx{};
    ctx.mips3_float_mode = fr1;
    ctx.f_odd = fr1 ? &ctx.f1.u32l : &ctx.f0.u32h;
    ctx.r29 = static_cast<std::int32_t>(stack);
    ctx.r16 = 16; ctx.r17 = 17; ctx.r18 = 18; ctx.r19 = 19;
    ctx.r20 = 20; ctx.r21 = 21; ctx.r22 = 22; ctx.r23 = 23; ctx.r30 = 30;
    func_8005AEE0(memory.data(), &ctx);
    check(static_cast<std::uint32_t>(ctx.r29) == stack && ctx.r16 == 16 && ctx.r17 == 17 &&
        ctx.r18 == 18 && ctx.r19 == 19 && ctx.r20 == 20 && ctx.r21 == 21 &&
        ctx.r22 == 22 && ctx.r23 == 23 && ctx.r30 == 30,
        "original crash animation restores its stack and callee-saved registers");
    check(ctx.f_odd == (fr1 ? &ctx.f1.u32l : &ctx.f0.u32h), "original crash math retains its FPU owner");
}
void check_scope(const std::vector<unsigned char>& before, const std::vector<unsigned char>& after,
    unsigned state) {
    for (std::uint32_t offset = 0; offset < kRdramSize; ++offset) {
        if (before[offset] == after[offset]) continue;
        const auto address = kRdramBegin + offset;
        bool allowed = address >= stack - 0x400u && address < stack;
        // B57C's original short recovery branch resets this private timer.
        allowed |= state > 0u && state < 4u && address >= bike_entity + 0x4D0u && address < bike_entity + 0x4D4u;
        for (unsigned bone = 0; bone < bone_count; ++bone) {
            allowed |= address >= bone_pose(bone) && address < bone_pose(bone) + 28u;
            for (unsigned clip = 0; clip < clip_count; ++clip) {
                const auto channel = channels + clip * 0x1000u + bone * 0x40u;
                allowed |= address >= channel + 0x10u && address < channel + 0x20u;
            }
        }
        if (!allowed) {
            std::fprintf(stderr, "[RR64-CRASH-ANIMATION] unexpected original write at %08x\n", address);
            passed = false; return;
        }
    }
}
void exercise(unsigned state, float phase, bool fr1) {
    auto a = seed(0u), b = seed(1u), oracle = seed(1u);
    for (auto* memory : {&a, &b}) {
        write_u32(memory->data(), rider_entity + 0x5D0u, state);
        write_float(memory->data(), bike_entity + 0x4CCu, phase);
        write_float(memory->data(), bike_entity + 0x4D0u, phase);
        write_u16(memory->data(), bike_entity + bike::rider_attached, 0u);
        write_u16(memory->data(), rider_entity + rider::ejected, 1u);
    }
    const auto before_a = a, before_b = b;
    const auto sample = intended_sample(state, phase);
    run_direct(oracle, sample, fr1);
    expected_poses = &oracle;
    for (auto* memory : {&a, &b}) {
        run_original(*memory, fr1);
        check(completed == 1u && pending_node == 0u, "one completed crash animation call certifies its return");
        check((state == 1u || state == 3u) ? (direct == 1u && entered == 0u) :
            (direct == 0u && entered == 1u && returned == 1u),
            "original crash branch reaches its exact direct or entry/return completion markers");
    }
    expected_poses = nullptr;
    for (unsigned bone = 0; bone < bone_count; ++bone) {
        check(pose_equal(a.data(), oracle.data(), bone), "original crash branch matches intended clip, phase and flags");
        check(pose_equal(a.data(), b.data(), bone), "all eleven crash bones replace independent prior poses");
        check(!pose_equal(a.data(), before_a.data(), bone), "each crash bone actually receives a current pose");
    }
    check_scope(before_a, a, state); check_scope(before_b, b, state);
    check(std::memcmp(a.data() + poses - kRdramBegin, before_a.data() + poses - kRdramBegin, 32u) == 0 &&
        std::memcmp(b.data() + poses - kRdramBegin, before_b.data() + poses - kRdramBegin, 32u) == 0,
        "crash animation preserves separately prepared actor root and padding");
    if (state == 0u && phase > 1.0f) {
        auto wrapped = seed(1u);
        run_direct(wrapped, Sample{clip_base, phase - std::floor(phase), 0x100u}, fr1);
        for (unsigned bone = 0; bone < bone_count; ++bone)
            check(pose_equal(a.data(), wrapped.data(), bone), "0x900 crash animation wraps elapsed phase through its clip");
    }
    ++cases;
}
void controls() {
    for (unsigned refusal = 0; refusal < 3; ++refusal) {
        auto memory = seed(0u);
        if (refusal == 0u) write_u16(memory.data(), globals::gameplay_pause_state, 1u);
        if (refusal == 1u) write_u16(memory.data(), graph + 0xAu, 1u);
        if (refusal == 2u) write_u32(memory.data(), 0x800A1454u, 0u);
        const auto before = memory;
        run_original(memory, false);
        check(completed == 0u && entered == 0u && direct == 0u,
            "paused, hidden or absent actor cannot emit completed animation evidence");
        check(std::memcmp(memory.data() + poses - kRdramBegin, before.data() + poses - kRdramBegin,
            (bone_count + 1u) * 32u) == 0, "skipped original crash animation leaves all root/child poses untouched");
    }
    // This documents why runtime graph coverage remains essential: the guest
    // function can return normally even when its child chain is truncated.
    auto truncated = seed(0u), before = truncated;
    write_u16(truncated.data(), bone_record(6u) + 2u, 0u);
    run_original(truncated, false);
    check(completed == 1u, "a truncated original chain can still reach a normal completion marker");
    for (unsigned bone = 7u; bone < bone_count; ++bone)
        check(pose_equal(truncated.data(), before.data(), bone), "truncated-chain control proves untouched bones must not be certified by return alone");
}
} // namespace
extern "C" int rr64_lod_shadow_rider(unsigned char*, unsigned int candidate) { return candidate == node; }
extern "C" void rr64_lod_shadow_stage(unsigned char* memory, unsigned int candidate, unsigned int stage) {
    check(candidate == node, "original completion belongs to the current rider node");
    bool did_complete = false;
    if (stage == 8u) { pending_node = candidate; ++entered; }
    else if (stage == 16u) {
        ++returned;
        did_complete = pending_node == candidate; pending_node = 0u;
    }
    else if (stage == 4u) { ++direct; did_complete = true; }
    else { check(false, "unexpected stage in original crash animation path"); }
    if (did_complete) {
        ++completed;
        if (expected_poses) for (unsigned bone = 0; bone < bone_count; ++bone)
            check(pose_equal(memory, expected_poses->data(), bone), "completion marker follows every intended original child write");
    }
}
extern "C" void rr64_lod_shadow_full_weight(unsigned char*, void*, int) {
    std::fputs("Unexpected weighted-animation path in crash fixture\n", stderr); std::abort();
}
extern "C" void func_80018BD8(unsigned char*, recomp_context*) {
    std::fputs("Unexpected weighted animation in crash fixture\n", stderr); std::abort();
}
extern "C" void func_80019130(unsigned char*, recomp_context*) {
    std::fputs("Unexpected unrelated animation in crash fixture\n", stderr); std::abort();
}
int main() {
    // Crash paths must not enter the finish hold callback (stub below).
    for (const bool fr1 : {false, true}) for (const unsigned state : {0u, 1u, 3u, 4u})
        for (const float phase : {0.375f, 1.375f}) exercise(state, phase, fr1);
    controls();
    std::printf("[RR64-CRASH-ANIMATION] %s: %u original AEE0 cases, eleven bones, exact clip/phase/flags and completion markers; pause/hidden/truncated controls\n",
        passed ? "PASS" : "FAIL", cases);
    return passed ? 0 : 1;
}
extern "C" void rr64_lod_shadow_finish_hold(unsigned char*, void*) { std::abort(); }
