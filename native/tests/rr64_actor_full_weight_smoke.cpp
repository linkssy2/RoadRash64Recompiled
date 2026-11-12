// The original whole-body writer is the independent oracle for the original
// weighted writer's endpoint. This fixture does not substitute animation,
// interpolation, quaternion arithmetic, trig, graph walking, or unpacking.
#include "rr64_engine_layout.hpp"
#include "recomp.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" void func_80018BD8(unsigned char*, recomp_context*);
extern "C" void func_80018114(unsigned char*, recomp_context*);

namespace {
using namespace rr64::engine;
constexpr unsigned bone_count = 11;
constexpr std::uint32_t graph = 0x80200000u;
constexpr std::uint32_t poses = 0x80300000u;
constexpr std::uint32_t channels = 0x80400000u;
constexpr std::uint32_t keys = 0x80410000u;
constexpr std::uint32_t stack = 0x807f0000u;
constexpr unsigned animation = 3u, style = 2u;
constexpr std::uint32_t channel_table = 0x800B74B0u + animation * 132u + style * 11220u;
bool passed = true;
unsigned cases = 0;

void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "[RR64-FULL-WEIGHT] FAILED: %s\n", message); passed = false; }
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
        const auto channel = channels + bone * 0x40u;
        const auto rotation = keys + bone * 0x80u, translation = rotation + 0x40u;
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
            write_u16(m, t, static_cast<std::uint16_t>(100 + bone * 17u + key * 43u));
            write_u16(m, t + 2u, static_cast<std::uint16_t>(-200 - static_cast<int>(bone) * 13 + static_cast<int>(key) * 31));
            write_u16(m, t + 4u, static_cast<std::uint16_t>(300 + bone * 19u - key * 53u));
            write_u16(m, t + 6u, key * 100u);
        }
    }
    return memory;
}

void run(std::vector<unsigned char>& memory, bool weighted, float weight,
    float phase, std::uint32_t flags, bool fr1) {
    recomp_context ctx{};
    ctx.mips3_float_mode = fr1;
    ctx.f_odd = ctx.mips3_float_mode ? &ctx.f1.u32l : &ctx.f0.u32h;
    ctx.r29 = static_cast<std::int32_t>(stack);
    ctx.r4 = static_cast<std::int32_t>(graph);
    ctx.r5 = animation;
    ctx.r6 = flags;
    ctx.r7 = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(phase));
    write_float(memory.data(), stack + 0x10u, 0.0f);
    write_float(memory.data(), stack + 0x14u, weight);
    if (weighted) func_80018BD8(memory.data(), &ctx);
    else func_80018114(memory.data(), &ctx);
    check(static_cast<std::uint32_t>(ctx.r29) == stack, "original animation restores the caller stack");
    check(ctx.f_odd == (fr1 ? &ctx.f1.u32l : &ctx.f0.u32h), "original math retains the executing context's FPU owner");
}

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
        same += std::abs(x - y);
        opposite += std::abs(x + y);
    }
    return std::min(same, opposite) < 0.00004f;
}

void check_scope(const std::vector<unsigned char>& before, const std::vector<unsigned char>& after) {
    for (std::uint32_t offset = 0; offset < kRdramSize; ++offset) {
        if (before[offset] == after[offset]) continue;
        const auto address = kRdramBegin + offset;
        bool allowed = address >= stack - 0x200u && address < stack + 0x18u;
        for (unsigned bone = 0; bone < bone_count; ++bone) {
            const auto pose = bone_pose(bone), channel = channels + bone * 0x40u;
            allowed |= address >= pose && address < pose + 28u;
            allowed |= address >= channel + 0x10u && address < channel + 0x20u;
        }
        if (!allowed) {
            std::fprintf(stderr, "[RR64-FULL-WEIGHT] unexpected original write at %08x\n", address);
            passed = false;
            return;
        }
    }
}

void endpoint(float phase, bool fr1) {
    auto a = seed(0u), b = seed(1u), oracle = seed(1u);
    const auto before_a = a, before_b = b;
    run(a, true, 1.0f, phase, 0x100u, fr1);
    run(b, true, 1.0f, phase, 0x100u, fr1);
    run(oracle, false, 1.0f, phase, 0x100u, fr1);
    for (unsigned bone = 0; bone < bone_count; ++bone) {
        check(pose_equal(a.data(), b.data(), bone), "full weight replaces all eleven bones independently of previous pose");
        check(pose_equal(a.data(), oracle.data(), bone), "full weight equals original whole-body writer up to quaternion sign");
        check(!pose_equal(a.data(), before_a.data(), bone), "each tested bone actually changes from its distinct previous pose");
    }
    check_scope(before_a, a);
    check_scope(before_b, b);
    check(std::memcmp(a.data() + poses - kRdramBegin, before_a.data() + poses - kRdramBegin, 32u) == 0,
        "weighted child writer deliberately does not certify or replace the root pose");
    ++cases;
}

void controls() {
    auto a = seed(0u), b = seed(1u);
    run(a, true, 0.5f, 0.375f, 0x100u, false);
    run(b, true, 0.5f, 0.375f, 0x100u, false);
    for (unsigned bone = 0; bone < bone_count; ++bone)
        check(!pose_equal(a.data(), b.data(), bone), "partial weight retains previous-pose dependence and cannot certify a fresh base");
    for (const auto mask : {1u, 2u}) {
        auto masked = seed(0u), original = masked;
        run(masked, true, 1.0f, 0.375f, 0x100u | mask, false);
        for (unsigned bone = 0; bone < bone_count; ++bone) {
            const bool skipped = mask == 1u ? bone < 7u : bone >= 7u;
            check(pose_equal(masked.data(), original.data(), bone) == skipped,
                "lower/upper skeleton mask proves completion requires both mask bits clear");
        }
    }
    // A successful return is not enough when the transform chain omits a bone.
    auto truncated = seed(0u), original = truncated;
    write_u16(truncated.data(), bone_record(6u) + 2u, 0u);
    run(truncated, true, 1.0f, 0.375f, 0x100u, false);
    for (unsigned bone = 7u; bone < bone_count; ++bone)
        check(pose_equal(truncated.data(), original.data(), bone),
            "truncated transform chain leaves later poses stale despite full weight");
}
} // namespace

int main() {
    for (const bool fr1 : {false, true}) for (const float phase : {0.0f, 0.375f, 1.0f}) endpoint(phase, fr1);
    controls();
    std::printf("[RR64-FULL-WEIGHT] %s: %u endpoint cases, eleven child bones, partial/masked/truncated controls\n",
        passed ? "PASS" : "FAIL", cases);
    return passed ? 0 : 1;
}
