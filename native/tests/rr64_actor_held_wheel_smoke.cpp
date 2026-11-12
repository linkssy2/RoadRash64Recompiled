// Exact original wheel writer at zero dt is the oracle for a held visual pose.
// The original speed gate is entered only on the separate oracle mapping;
// the implementation must leave phase, contact, effects and caller state alone.
#include "rr64_actor_render_fixture.hpp"
#include "rr64_actor_held_pose.hpp"
#include "recomp.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" void func_8005B948(unsigned char*, recomp_context*);
extern "C" void func_8001B020(unsigned char*, recomp_context*);

namespace {
using namespace rr64::engine;
using Fixture = rr64::lod::test::Fixture;
constexpr std::uint32_t front_source = 0x80580000u, rear_source = 0x80580100u, static_source = 0x80580200u;
constexpr std::uint32_t second_static_source = 0x80580300u;
constexpr std::uint32_t static_record = Fixture::bike_graph + 0x20u, static_pose = Fixture::bike_pose + 0x20u;
constexpr std::uint32_t second_static_record = Fixture::bike_graph + 0x80u, second_static_pose = Fixture::bike_pose + 0x80u;
constexpr std::array<std::uint32_t, 2> wheel_poses{Fixture::bike_pose + 0x40u, Fixture::bike_pose + 0x60u};
constexpr std::array<std::uint32_t, 2> wheel_records{Fixture::bike_graph + 0x40u, Fixture::bike_graph + 0x60u};
constexpr std::array<std::uint32_t, 4> held_poses{wheel_poses[0], wheel_poses[1], static_pose, second_static_pose};
bool passed = true;
unsigned original_completions = 0;

void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "[RR64-HELD-WHEEL] FAILED: %s\n", message); passed = false; }
}
template<std::size_t N>
void floats(unsigned char* memory, std::uint32_t address, const std::array<float, N>& values) {
    for (std::uint32_t i = 0; i < N; ++i) write_float(memory, address + i * 4u, values[i]);
}
template<std::size_t N>
void words(unsigned char* memory, std::uint32_t address, const std::array<std::uint32_t, N>& values) {
    for (std::uint32_t i = 0; i < N; ++i) write_u32(memory, address + i * 4u, values[i]);
}
void seed(Fixture& fixture, float speed, const std::array<float, 2>& phases,
    const std::array<float, 2>& suspension, bool four_children = false) {
    auto* memory = fixture.live.data();
    write_u32(memory, 0x800A1450u, Fixture::bike_node);
    write_u16(memory, 0x800A65BCu, 1u);
    write_u16(memory, 0x800D8570u + 4u * 0x118u + 0x24u, 1u);
    write_u16(memory, Fixture::bike_node + actor_scene::selected_lod, 0u);
    write_u32(memory, Fixture::bike_node + actor_scene::current_model, Fixture::bike_graph);
    // Actual 1AB20 bike topology: +8 renders static body then two wheels;
    // +2 skips the flag-zero static record and visits only flag-one wheels.
    // Real capture: six matrices = root13's three + three child transforms.
    write_u16(memory, Fixture::bike_graph + 2u, 8u);
    write_u16(memory, wheel_records[0] + 2u, 4u);
    write_u16(memory, wheel_records[1] + 2u, 0u);
    write_u16(memory, wheel_records[1] + 8u, 0u);
    write_u32(memory, static_record + 0x14u, static_source);
    write_u32(memory, wheel_records[0] + 0x14u, front_source);
    write_u32(memory, wheel_records[1] + 0x14u, rear_source);
    for (const auto source : {front_source, rear_source, static_source}) write_u32(memory, source, 0x12u);
    write_u16(memory, front_source + 0x16u, 1u);
    write_u16(memory, rear_source + 0x16u, 1u);
    write_u16(memory, static_source + 0x16u, 0u);
    floats(memory, front_source + 0x38u, std::array<float, 3>{12.25f, -2.5f, 41.0f});
    floats(memory, rear_source + 0x38u, std::array<float, 3>{-16.75f, 3.25f, 37.5f});
    floats(memory, static_source + 0x38u, std::array<float, 3>{65.12f, -3.088f, 1.563f});
    floats(memory, static_source + 0x28u, std::array<float, 4>{0.0f, 0.0f, 0.6f, 0.8f});
    if (four_children) {
        // Actual captured family 80275040 renders wheel/static/wheel/static.
        // Keep the independent +2 wheel chain unchanged; +8 is the full graph.
        write_u16(memory, Fixture::bike_graph + 8u, 8u);
        write_u16(memory, wheel_records[0] + 8u, static_cast<std::uint16_t>(-4));
        write_u16(memory, static_record + 8u, 8u);
        write_u16(memory, wheel_records[1] + 8u, 4u);
        write_u16(memory, second_static_record + 4u, 0x12u);
        write_u16(memory, second_static_record + 8u, 0u);
        write_u32(memory, second_static_record + 0xcu, second_static_pose);
        write_u32(memory, second_static_record + 0x14u, second_static_source);
        write_u32(memory, second_static_source, 0x12u);
        write_u16(memory, second_static_source + 0x16u, 0u);
        floats(memory, second_static_source + 0x38u, std::array<float, 3>{141.313f, -8.595f, 14.4533f});
        floats(memory, second_static_source + 0x28u, std::array<float, 4>{0.6f, 0.0f, 0.0f, 0.8f});
    }
    write_float(memory, Fixture::bike_entity + 0x184u, speed);
    write_float(memory, Fixture::bike_entity + 0x3f0u, phases[0]);
    write_float(memory, Fixture::bike_entity + 0x328u, phases[1]);
    write_float(memory, Fixture::bike_entity + 0x3ecu, 7.75f);
    write_float(memory, Fixture::bike_entity + 0x324u, -3.5f);
    write_float(memory, Fixture::bike_entity + 0x560u, suspension[0]);
    write_float(memory, Fixture::bike_entity + 0x55cu, suspension[1]);
    write_float(memory, 0x8009CBA8u, 0.0f);
    words(memory, 0x80005DA0u, std::array<std::uint32_t, 11>{
        0x42C80000u, 0x3F400000u, 0xC1200000u, 0x40800000u,
        0x41200000u, 0x3F3851ECu, 0xC0A00000u, 0x3F3851ECu,
        0x40000000u, 0x40000000u, 0x3EB4B4AFu});
    write_float(memory, 0x80000DB0u, 1.0f);
    write_float(memory, 0x80000DD0u, 0.5f);
    // Original ROM constants used by the exact generated sin/cos helpers.
    words(memory, 0x80007F88u, std::array<std::uint32_t, 24>{
        0xBFC55554u, 0xBC83656Du, 0x3F8110EDu, 0x3804C2A0u,
        0xBF29F6FFu, 0xEEA56814u, 0x3EC5DBDFu, 0x0E314BFEu,
        0x3FD45F30u, 0x6DC9C883u, 0x400921FBu, 0x50000000u,
        0x3E6110B4u, 0x611A6263u, 0u, 0u,
        0x3FE00000u, 0u, 0x3FE00000u, 0u, 0u, 0u, 0x7F810000u, 0u});
    words(memory, 0x80008058u, std::array<std::uint32_t, 20>{
        0xBFC55554u, 0xBC83656Du, 0x3F8110EDu, 0x3804C2A0u,
        0xBF29F6FFu, 0xEEA56814u, 0x3EC5DBDFu, 0x0E314BFEu,
        0x3FD45F30u, 0x6DC9C883u, 0x400921FBu, 0x50000000u,
        0x3E6110B4u, 0x611A6263u, 0u, 0u, 0x3FE00000u, 0u, 0x3FE00000u, 0u});
    for (unsigned part = 0; part < (four_children ? 4u : 3u); ++part) {
        floats(memory, held_poses[part], std::array<float, 8>{91, 92, 93, 0, 0, 0, 1, 77});
    }
}
void bind_context(recomp_context& context) {
    context.r29 = static_cast<std::int32_t>(Fixture::stack);
    context.f_odd = context.mips3_float_mode ? &context.f1.u32l : &context.f0.u32h;
    context.r18 = 0x1234u;
    context.f22.d = 3.25;
}
void check_write_scope(const std::vector<unsigned char>& before,
    const std::vector<unsigned char>& after, bool allow_wheels, unsigned part_count = 3u) {
    for (std::uint32_t offset = 0; offset < kRdramSize; ++offset) {
        if (before[offset] == after[offset]) continue;
        const auto address = kRdramBegin + offset;
        bool allowed = address >= Fixture::stack - 0x300u && address < Fixture::stack;
        for (unsigned part = 0; part < part_count; ++part) {
            const auto pose = held_poses[part];
            allowed |= allow_wheels && address >= pose && address < pose + 28u;
        }
        if (!allowed) {
            std::fprintf(stderr, "[RR64-HELD-WHEEL] unexpected write at%08x\n", address);
            passed = false;
            return;
        }
    }
}
extern unsigned test_view;
void test_exact_hold(float speed, const std::array<float, 2>& phases,
    const std::array<float, 2>& suspension, bool four_children = false, float airborne = 0.0f, unsigned crash_state = 0u, bool detached = true) {
    Fixture fixture;
    seed(fixture, speed, phases, suspension, four_children);
    if(airborne>0){
        write_u32(fixture.live.data(),Fixture::bike_entity+0x100u,8u);
        write_float(fixture.live.data(),Fixture::bike_entity+0x4d4u,airborne);
    }
    if(crash_state){
        write_u32(fixture.live.data(),Fixture::bike_entity+0x100u,crash_state);
        write_u16(fixture.live.data(),Fixture::bike_entity+bike::rider_attached,detached?0u:1u);
        write_u16(fixture.live.data(),Fixture::rider_entity+rider::ejected,detached?1u:0u);
    }
    const unsigned part_count = four_children ? 4u : 3u;
    write_u32(fixture.live.data(), Fixture::race_player_count, 4u);
    write_u32(fixture.live.data(), 0x8009DB88u, 4u);
    write_u32(fixture.live.data(), globals::active_viewport, test_view);
    auto before = fixture.live;
    auto skipped = before;
    recomp_context skipped_context{};
    bind_context(skipped_context);
    original_completions = 0;
    func_8005B948(skipped.data(), &skipped_context);
    check(original_completions == 0u, "unmodified original low-speed gate skips wheel-pose completion");
    for (unsigned part = 0; part < part_count; ++part) {
        const auto pose = held_poses[part];
        check(std::memcmp(skipped.data() + (pose - kRdramBegin), before.data() + (pose - kRdramBegin), 32u) == 0,
            "original low-speed pass leaves stale model child poses untouched");
    }

    auto oracle = before;
    // Speed affects admission only on this contact-free path. dt remains zero,
    // so the original writer evaluates the existing phase without advancing it.
    write_float(oracle.data(), Fixture::bike_entity + 0x184u, 10.0f);
    // Only the independent oracle bypasses the airborne gate. Its zero-dt
    // writer evaluates current held phase; production never clears the timer.
    write_float(oracle.data(),Fixture::bike_entity+0x4d4u,0.0f);
    recomp_context oracle_context{};
    bind_context(oracle_context);
    // The independently extracted constructor initializes the static child
    // exactly as the real asset loader does; B948 never visits it via +2.
    for (unsigned part = 2; part < part_count; ++part) {
        write_u32(oracle.data(), 0x8009DC48u, 0u);
        oracle_context.r4 = static_cast<std::int32_t>(part == 2u ? static_source : second_static_source);
        oracle_context.r5 = 0u;
        oracle_context.r6 = static_cast<std::int32_t>(held_poses[part]);
        func_8001B020(oracle.data(), &oracle_context);
    }
    const auto initialized_static = oracle;
    original_completions = 0;
    func_8005B948(oracle.data(), &oracle_context);
    check(original_completions == 1u, "actual original zero-dt writer completes both wheel poses");
    for (unsigned part = 2; part < part_count; ++part) {
        check(std::memcmp(oracle.data() + held_poses[part] - kRdramBegin,
            initialized_static.data() + held_poses[part] - kRdramBegin, 32u) == 0,
            "actual B948 leaves every independently constructed static child untouched");
    }
    for (unsigned wheel = 0; wheel < 2; ++wheel) {
        float phase = 0;
        read_float(oracle.data(), Fixture::bike_entity + (wheel ? 0x328u : 0x3f0u), phase);
        check(phase == phases[wheel], "zero-dt original oracle preserves the existing wheel phase");
    }
    recomp_context held_context{};
    bind_context(held_context);
    const auto caller_before = held_context;
    check(rr64::lod::prepare_held_bike_pose(fixture.live.data(), Fixture::bike_node, &held_context),
        "guarded low-speed helper prepares both held wheels and every static model child");
    check(std::memcmp(&held_context, &caller_before, sizeof(held_context)) == 0,
        "held wheel preparation leaves its caller's registers and FPU alias unchanged");
    for (unsigned part = 0; part < part_count; ++part) {
        const auto pose = held_poses[part];
        for (unsigned word = 0; word < 7; ++word) {
            std::uint32_t expected = 0, actual = 0;
            read_u32(oracle.data(), pose + word * 4u, expected);
            read_u32(fixture.live.data(), pose + word * 4u, actual);
            if (actual != expected) {
                std::fprintf(stderr, "[RR64-HELD-WHEEL] pose mismatch speed=%.2f pose=%08x word=%u actual=%08x expected=%08x\n",
                    speed, pose, word, actual, expected);
                passed = false;
            }
        }
    }
    check_write_scope(before, fixture.live, true, part_count);
}
enum class Refusal { Pause, Disabled, Inactive, SpeedLimit, NanSpeed,
    Special, Effect, Hidden, Detached, NonDetailed, ExtraChild, MissingChild,
    BadStaticSource, StaticAnimated, WheelStatic, ChainIncludesStatic, UnterminatedChain,
    ThirdAnimated, SecondStaticPoseAlias, SecondStaticBadQuaternion,
    NanStaticQuaternion, ZeroStaticQuaternion, StaticPoseAlias, StaticSourceAlias,
    NanPhase, NanSuspension, OverflowSuspension, BadConstant, PoseAlias, PoseSourceAlias,
    BadSource, WrongView, WrongScene, NullContext, AirDetached, AirNanTimer, AirContact, BadStack };
void test_refusal(Refusal refusal) {
    Fixture fixture;
    const bool four_children = refusal == Refusal::ThirdAnimated ||
        refusal == Refusal::SecondStaticPoseAlias || refusal == Refusal::SecondStaticBadQuaternion;
    seed(fixture, 2.0f, {0.7f, -1.1f}, {0.025f, -0.015f}, four_children);
    auto* memory = fixture.live.data();
    recomp_context context{};
    bind_context(context);
    switch (refusal) {
    case Refusal::Pause: write_u16(memory, globals::gameplay_pause_state, 1u); break;
    case Refusal::Disabled: write_u16(memory, 0x800A65BCu, 0u); break;
    case Refusal::Inactive: write_u16(memory, 0x800D8570u + 4u * 0x118u + 0x24u, 0u); break;
    case Refusal::SpeedLimit: write_float(memory, Fixture::bike_entity + 0x184u, 10.0f); break;
    case Refusal::NanSpeed: write_u32(memory, Fixture::bike_entity + 0x184u, 0x7fc00000u); break;
    case Refusal::Special: write_u16(memory, Fixture::bike_entity + 0x818u, 1u); break;
    case Refusal::Effect: write_float(memory, Fixture::bike_entity + 0x4d4u, 1.0f);
        write_u16(memory,Fixture::rider_entity+rider::ejected,1u);break; // inconsistent attached + ejected
    case Refusal::AirDetached:
        write_u32(memory,Fixture::bike_entity+0x100u,8u);write_float(memory,Fixture::bike_entity+0x4d4u,1.0f);
        write_u16(memory,Fixture::bike_entity+bike::rider_attached,0u);break;
    case Refusal::AirNanTimer:
        write_u32(memory,Fixture::bike_entity+0x100u,8u);write_u32(memory,Fixture::bike_entity+0x4d4u,0x7fc00000u);break;
    case Refusal::AirContact:
        write_u32(memory,Fixture::bike_entity+0x100u,8u);write_float(memory,Fixture::bike_entity+0x4d4u,1.0f);
        write_u16(memory,Fixture::bike_entity+0x818u,1u);break;
    case Refusal::Hidden: write_u16(memory, Fixture::bike_graph + 0xau, 1u); break;
    case Refusal::Detached: write_u32(memory, Fixture::bike_entity + bike::rider_pointer, 0u); break;
    case Refusal::NonDetailed: write_u16(memory, Fixture::bike_node + actor_scene::selected_lod, 1u); break;
    case Refusal::ExtraChild:
        write_u16(memory, wheel_records[1] + 8u, 4u);
        write_u16(memory, Fixture::bike_graph + 0x84u, 0x12u);
        break;
    case Refusal::MissingChild: write_u16(memory, wheel_records[0] + 2u, 0u); break;
    case Refusal::BadStaticSource: write_u32(memory, static_source, 0x11u); break;
    case Refusal::StaticAnimated: write_u16(memory, static_source + 0x16u, 1u); break;
    case Refusal::WheelStatic: write_u16(memory, front_source + 0x16u, 0u); break;
    case Refusal::ChainIncludesStatic: write_u16(memory, Fixture::bike_graph + 2u, 4u); break;
    case Refusal::UnterminatedChain: write_u16(memory, wheel_records[1] + 2u, 4u); break;
    case Refusal::ThirdAnimated: write_u16(memory, second_static_source + 0x16u, 1u); break;
    case Refusal::SecondStaticPoseAlias: write_u32(memory, second_static_record + 0xcu, static_pose); break;
    case Refusal::SecondStaticBadQuaternion: write_u32(memory, second_static_source + 0x28u, 0x7fc00000u); break;
    case Refusal::NanStaticQuaternion: write_u32(memory, static_source + 0x34u, 0x7fc00000u); break;
    case Refusal::ZeroStaticQuaternion: floats(memory, static_source + 0x28u, std::array<float, 4>{0, 0, 0, 0}); break;
    case Refusal::StaticPoseAlias: write_u32(memory, static_record + 0xcu, wheel_poses[0]); break;
    case Refusal::StaticSourceAlias: write_u32(memory, static_record + 0xcu, front_source + 0x20u); break;
    case Refusal::NanPhase: write_u32(memory, Fixture::bike_entity + 0x328u, 0x7fc00000u); break;
    case Refusal::NanSuspension: write_u32(memory, Fixture::bike_entity + 0x55cu, 0x7fc00000u); break;
    case Refusal::OverflowSuspension: write_float(memory, Fixture::bike_entity + 0x560u, std::numeric_limits<float>::max()); break;
    case Refusal::BadConstant: write_float(memory, 0x80005DC8u, 0.5f); break;
    case Refusal::PoseAlias: write_u32(memory, wheel_records[1] + 0xcu, wheel_poses[0]); break;
    case Refusal::PoseSourceAlias: write_u32(memory, wheel_records[1] + 0xcu, front_source + 0x20u); break;
    case Refusal::BadSource: write_u32(memory, wheel_records[1] + 0x14u, 0u); break;
    case Refusal::WrongView: write_u32(memory, globals::active_viewport, 1u); break;
    case Refusal::WrongScene: write_u32(memory, globals::main_mode, 0u); break;
    case Refusal::NullContext: break;
    case Refusal::BadStack: context.r29 = 0u; break;
    }
    const auto before = fixture.live;
    const auto caller_before = context;
    const bool accepted = rr64::lod::prepare_held_bike_pose(memory, Fixture::bike_node,
        refusal == Refusal::NullContext ? nullptr : &context);
    if (accepted) { std::fprintf(stderr, "[RR64-HELD-WHEEL] incorrectly accepted refusal=%u\n", unsigned(refusal)); passed = false; }
    check_write_scope(before, fixture.live, false);
    check(std::memcmp(&context, &caller_before, sizeof(context)) == 0, "refused held pose preserves caller context");
}
[[noreturn]] void unexpected_path(const char* name) {
    std::fprintf(stderr, "[RR64-HELD-WHEEL] unexpected effect/vector path%s\n", name);
    std::abort();
}
} // namespace
extern "C" void rr64_lod_shadow_stage(unsigned char*, unsigned node, unsigned stage) {
    check(node == Fixture::bike_node && stage == 128u, "original oracle reports its own completed wheel stage");
    ++original_completions;
}
#define UNREACHED_WHEEL_HELPER(name) extern "C" void name(unsigned char*, recomp_context*) { unexpected_path(#name); }
UNREACHED_WHEEL_HELPER(func_80012A58)
UNREACHED_WHEEL_HELPER(func_80012AF8)
UNREACHED_WHEEL_HELPER(func_80012B9C)
UNREACHED_WHEEL_HELPER(func_80012CE0)
UNREACHED_WHEEL_HELPER(func_80015534)
UNREACHED_WHEEL_HELPER(func_8005B7DC)
namespace { unsigned test_view = 0; }
int run_cases() {
    for(float speed:{0.0f,2.0f,23.72f})for(bool four:{false,true})
        test_exact_hold(speed,{0.7f,-1.1f},{0.02f,-0.04f},four,0.817f,2u,false);
    for(unsigned state:{2u,8u})for(float speed:{2.0f,23.72f,45.08f})
        for(float timer:{0.016f,0.817f,2.183f})for(bool four:{false,true})
            test_exact_hold(speed,{0.7f,-1.1f},{0.02f,-0.04f},four,timer,state);
    for(float speed:{2.0f,10.0f,23.72f,45.08f})for(float timer:{0.016f,0.817f,2.183f})
        for(bool four:{false,true})test_exact_hold(speed,{0.7f,-1.1f},{0.02f,-0.04f},four,timer);
    for (float speed : {-3.0f, -0.001f, 0.0f, 2.0f, 9.99f}) {
        test_exact_hold(speed, {0.7f, -1.1f}, {-0.2f, 0.3f});
        test_exact_hold(speed, {3.2f, 5.75f}, {0.0125f, 0.065f});
        test_exact_hold(speed, {-4.4f, 0.1f}, {0.3f, -0.2f});
        test_exact_hold(speed, {-1.2f, 2.1f}, {0.02f, -0.04f}, true);
    }
    for (unsigned value = 0; value <= unsigned(Refusal::BadStack); ++value) test_refusal(static_cast<Refusal>(value));
    std::printf("[RR64-HELD-WHEEL] %s: actual B948 zero-dt wheel parity + 1B020 static-child parity, real three/four-child topology, signed low-speed and guarded pose-only writes.\n",
        passed ? "PASS" : "FAIL");
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main() { for (test_view = 0; test_view < 4; ++test_view) { if (run_cases()) return EXIT_FAILURE; } return EXIT_SUCCESS; }
