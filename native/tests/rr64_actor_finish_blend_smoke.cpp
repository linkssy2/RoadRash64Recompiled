// Reuse the asymmetric eleven-bone data and direct original-animation oracle.
// No production animation, interpolation, quaternion or graph code is copied.
#include "highlight_hooks_disabled.hpp"
#define main rr64_original_full_weight_test_main
#include "rr64_actor_full_weight_smoke.cpp"
#undef main
#include "rr64_actor_render_fixture.hpp"
#include "rr64_actor_render_diagnostics.hpp"
#include "rr64_native.hpp"
#include <cstdlib>

// This fixture compares original offline animation; network pose injection is
// independently tested by online fixtures and remains inactive here.
extern "C" void rr64_online_attack_pose_begin(unsigned char *) {}
extern "C" void rr64_online_attack_pose_end(unsigned char *) {}

namespace {
using Fixture = rr64::lod::test::Fixture;
constexpr std::uint32_t bike_graph = 0x80210000u, bike_poses = 0x80320000u;
constexpr std::uint32_t rider_sources = 0x80550000u, finish_state = 0x80590000u;
unsigned char* live_mapping = nullptr;

std::vector<unsigned char> finish_seed() {
    Fixture fixture;
    auto memory = fixture.live;
    auto* m = memory.data();
    // Move the bike model out of the independent animation fixture's graph.
    std::memcpy(m + bike_graph - kRdramBegin, m + Fixture::bike_graph - kRdramBegin, 0x300u);
    std::memcpy(m + bike_poses - kRdramBegin, m + Fixture::bike_pose - kRdramBegin, 0x300u);
    for (unsigned lod = 0; lod < 3u; ++lod) {
        write_u32(m, Fixture::bike_node + actor_scene::lod_models + lod * 4u, bike_graph + lod * 0x100u);
        for (unsigned part = 0; part < (lod == 2u ? 2u : 4u); ++part)
            write_u32(m, bike_graph + lod * 0x100u + part * 0x20u + 0xcu,
                bike_poses + lod * 0x100u + part * 0x20u);
    }
    write_u32(m, Fixture::bike_node + actor_scene::current_model, bike_graph + 0x200u);
    const auto animation_memory = seed(0u);
    const auto copy = [&](std::uint32_t address, std::size_t bytes) {
        std::memcpy(m + address - kRdramBegin, animation_memory.data() + address - kRdramBegin, bytes);
    };
    copy(graph, (bone_count + 1u) * 32u);
    copy(poses, (bone_count + 1u) * 32u);
    copy(channels, bone_count * 0x40u);
    copy(keys, bone_count * 0x80u);
    copy(channel_table, bone_count * 12u);
    seed_constants(m);
    write_u32(m, 0x8009DC2Cu, style);
    write_u32(m, Fixture::rider_node + actor_scene::lod_models, graph);
    write_u16(m, graph + 8u, 4u);
    write_u32(m, graph + 0x14u, rider_sources);
    for (unsigned bone = 0; bone < bone_count; ++bone) {
        write_u16(m, bone_record(bone) + 8u, bone + 1u == bone_count ? 0u : 4u);
        write_u32(m, bone_record(bone) + 0x14u, rider_sources + (bone + 1u) * 0x40u);
    }
    write_u32(m, 0x800A1454u, Fixture::rider_node);
    write_u32(m, Fixture::rider_node + 0x40u, 4u);
    write_u32(m, Fixture::rider_entity + 0xcu, style);
    write_u32(m, 0x800D8570u + 4u * 0x118u + 0xe8u, finish_state);
    write_float(m, finish_state + 0x5cu, 1.0f);
    write_u32(m, finish_state + 0x60u, 0xffffffffu);
    write_u32(m, Fixture::bike_entity + 0x100u, 2u);
    write_u32(m, 0x800A4F34u + style * 56u, animation);
    write_u32(m, 0x80005D60u, 0x3f000000u);
    write_u32(m, 0x80005D64u, 0x3f800000u);
    words(m, 0x80005D70u, std::array<std::uint32_t, 4>{0x41200000u,0x3fc00000u,0x3dcccccdu,0x3d4ccccdu});
    return memory;
}

void allocate(unsigned char* m) {
    rr64_lod_observe_allocation(m, Fixture::bike_node, 0u, 6u * 64u);
    rr64_lod_observe_allocation(m, Fixture::rider_node, 0u, 14u * 64u);
}
void prepare(std::vector<unsigned char>& memory, float speed, bool fr1, unsigned slot, bool direct) {
    auto* m = memory.data();
    write_float(m, Fixture::bike_entity + 0x184u, speed);
    write_u32(m, globals::actor_render_buffer_slot, slot);
    recomp_context observed{};
    observed.mips3_float_mode = fr1;
    observed.f_odd = fr1 ? &observed.f1.u32l : &observed.f0.u32h;
    observed.r29 = static_cast<std::int32_t>(stack);
    observed.r20 = Fixture::bike_node; observed.r19 = Fixture::rider_node;
    observed.r22 = Fixture::bike_entity; observed.r18 = Fixture::rider_entity;
    observed.r17 = 0x800D6880u + 4u * 12u; observed.r16 = 0x800D6940u + 4u * 12u;
    observed.r30 = 0x800D69F8u;
    const auto context_before = observed;
    const auto memory_before = memory;
    rr64_lod_begin_preparation(m);
    rr64_lod_observe_pair(m, &observed);
    rr64_lod_prepare_shadow(m, &observed, direct);
    check(memory == memory_before, "preparation must preserve every live guest byte");
    check(std::memcmp(&observed, &context_before, sizeof(observed)) == 0, "preparation preserves caller context and FPU owner");
}
void check_selected(std::vector<unsigned char>& memory, const std::vector<unsigned char>& oracle,
    unsigned stock_lod, bool expected) {
    const auto before = memory;
    rr64_lod_begin_draw(memory.data());
    check(rr64_lod_select(memory.data(), Fixture::bike_node, 2u) == (expected ? 0u : 2u),
        "finish transition retains detailed bike only with a complete current pair");
    rr64_lod_end_actor();
    check(rr64_lod_select(memory.data(), Fixture::rider_node, stock_lod) == (expected ? 0u : stock_lod),
        "finish transition rider uses the certified original blend");
    if (expected) for (unsigned bone = 0; bone < bone_count; ++bone)
        check(pose_equal(memory.data(), oracle.data(), bone), "each current blended bone equals original18BD8 applied to last detailed pose");
    rr64_lod_end_draw(memory.data());
    check(memory == before, "both actor bindings restore all live bytes");
}

void sequence(bool fr1, unsigned slot, bool direct) {
    auto memory = finish_seed();
    live_mapping = memory.data();
    allocate(memory.data());
    auto oracle = memory;
    run(oracle, true, 1.0f, 1.0f, 0x100u, fr1);
    prepare(memory, 1.0f, fr1, slot, direct);
    check_selected(memory, oracle, 2u, true);
    auto stale = seed(1u);
    for(float speed:{24.0f,10.37f,10.01f,10.0f}){
        // Deliberately stale live detail: the original finish branch performs
        // no child writes here, so the expected pose stays exactly unchanged.
        for(unsigned bone=0;bone<bone_count;++bone)
            std::memcpy(memory.data()+bone_pose(bone)-kRdramBegin,
                stale.data()+bone_pose(bone)-kRdramBegin,28u);
        write_u32(memory.data(),globals::main_mode,0x19u);
        write_u32(memory.data(),globals::pending_mode,0x19u);
        prepare(memory,speed,fr1,slot,direct);
        check_selected(memory,oracle,2u,true);
    }
    for (unsigned iteration = 0; iteration < 3u; ++iteration) {
        const float speed = std::array{8.0f, 4.0f, 1.6f}[iteration];
        constexpr unsigned unrelated_node = 0x80140000u;
        if (iteration == 0u) {
            std::memcpy(memory.data() + unrelated_node - kRdramBegin,
                memory.data() + Fixture::bike_node - kRdramBegin, 0x148u);
            rr64_lod_observe_allocation(memory.data(), unrelated_node, 0u, 6u * 64u);
        }
        if (iteration == 1u) rr64_lod_release_node(memory.data(), unrelated_node);
        // Distinct valid stale detail data proves the seed is actually used.
        for (unsigned bone = 0; bone < bone_count; ++bone)
            std::memcpy(memory.data() + bone_pose(bone) - kRdramBegin,
                stale.data() + bone_pose(bone) - kRdramBegin, 28u);
        const unsigned stock_lod = iteration == 1u ? 1u : 2u;
        write_u16(memory.data(), Fixture::rider_node + actor_scene::selected_lod, stock_lod);
        write_u32(memory.data(), Fixture::rider_node + actor_scene::current_model,
            Fixture::rider_graph + stock_lod * 0x100u);
        run(oracle, true, 1.0f-speed*0.05f, 1.0f-speed*0.1f, 0x100u, fr1);
        const auto old_count = rr64::lod::read_activity().counts[static_cast<std::size_t>(rr64::lod::ActivityCounter::FinishBlendPrepared)];
        prepare(memory, speed, fr1, slot ^ (iteration & 1u), direct);
        check(rr64::lod::read_activity().counts[static_cast<std::size_t>(rr64::lod::ActivityCounter::FinishBlendPrepared)] == old_count + 1u,
            "only completed original partial calls increment finish preparation evidence");
        check_selected(memory, oracle, stock_lod, true);
    }
    // A held pause must preserve the last detailed children exactly, even
    // when the stock detailed storage is stale and the camera buffer flips.
    write_u16(memory.data(),globals::gameplay_pause_state,1u);
    write_u16(memory.data(),globals::pause_menu_state,1u);
    for(unsigned paused=0;paused<4;paused++){
        prepare(memory,4.0f,fr1,paused&1u,direct);
        check_selected(memory,oracle,2u,true);
    }
    write_u16(memory.data(),globals::gameplay_pause_state,0u);
    write_u16(memory.data(),globals::pause_menu_state,0u);
    for(unsigned mode:{0x18u,0x19u}){
        write_u32(memory.data(),globals::main_mode,mode);
        write_u32(memory.data(),globals::pending_mode,mode);
        run(oracle,true,0.8f,0.6f,0x100u,fr1);
        prepare(memory,4.0f,fr1,slot,direct);
        check_selected(memory,oracle,2u,true);
    }
    ++cases;
}

enum class Refusal { MissingHistory, SkippedEpoch, Allocation, Resource, Ownership,
    Scene, Contact, Transition, WrongPhaseFactors, BrokenChain, Style,
    ReorderedChain, Count };
void refusal(Refusal refusal) {
    auto memory = finish_seed(); live_mapping = memory.data(); allocate(memory.data());
    auto oracle = memory; run(oracle, true, 1.0f, 1.0f, 0x100u, false);
    if (refusal != Refusal::MissingHistory) {
        prepare(memory, 1.0f, false, 0u, false); check_selected(memory, oracle, 2u, true);
    }
    switch (refusal) {
    case Refusal::MissingHistory: break;
    case Refusal::SkippedEpoch: rr64_lod_begin_preparation(memory.data()); break;
    case Refusal::Allocation: allocate(memory.data()); break;
    case Refusal::Resource: write_u32(memory.data(), Fixture::rider_source + 0x400u, 0x12345678u); break;
    case Refusal::Ownership: write_u16(memory.data(), Fixture::rider_entity + rider::ejected, 1u); break;
    case Refusal::Scene: write_u32(memory.data(), globals::main_mode, 0x20u); write_u32(memory.data(), globals::pending_mode, 0x20u); break;

    case Refusal::Contact: write_u16(memory.data(), Fixture::bike_entity + 0x818u, 1u); break;
    case Refusal::Transition: write_float(memory.data(), Fixture::bike_entity + 0x4d4u, 0.5f); break;
    case Refusal::WrongPhaseFactors: write_u32(memory.data(), 0x80005d78u, 0x3dcccccEu); break;
    case Refusal::BrokenChain: write_u16(memory.data(), bone_record(6u)+2u, 0u); break;
    case Refusal::Style:
        write_u32(memory.data(), Fixture::rider_entity + 0xcu, style + 1u);
        write_u32(memory.data(), 0x800A4F34u + (style + 1u) * 56u, animation);
        break;
    case Refusal::ReorderedChain:
        write_u16(memory.data(), graph+2u, 8u);
        write_u16(memory.data(), bone_record(1u)+2u, static_cast<std::uint16_t>(-4));
        write_u16(memory.data(), bone_record(0u)+2u, 8u);
        break;
    case Refusal::Count: break;
    }
    const bool recover=refusal==Refusal::MissingHistory||refusal==Refusal::SkippedEpoch||refusal==Refusal::Allocation;
    if(recover)run(oracle,false,1.0f,0.6f,0x100u,false);
    prepare(memory, 4.0f, false, 1u, false);
    check_selected(memory, oracle, 2u, recover);
    if(recover){
        run(oracle,true,0.8f,0.6f,0x100u,false);
        prepare(memory,4.0f,false,0u,false);
        check_selected(memory,oracle,2u,true);
    }
}
} // namespace

// Root/vector and unrelated visual passes remain explicit fixture adapters.
// The actual AEE0/18BD8/18114 and all their exercised math are linked unchanged.
extern "C" void func_80012CE0(unsigned char* m, recomp_context* c) {
    for (unsigned i=0;i<3u;++i) write_float(m, static_cast<unsigned>(c->r6)+4u*i,
        value(m, static_cast<unsigned>(c->r4)+4u*i)-value(m, static_cast<unsigned>(c->r5)+4u*i));
}
extern "C" void func_80012DBC(unsigned char* m, recomp_context* c) {
    for (unsigned i=0;i<3u;++i) write_float(m, static_cast<unsigned>(c->r5)+4u*i,
        value(m, static_cast<unsigned>(c->r4)+4u*i)*std::bit_cast<float>(static_cast<unsigned>(c->r6)));
}
extern "C" void func_8005E980(unsigned char* m, recomp_context*) {
    check(m != live_mapping, "root adapter receives private memory");
    floats(m, bike_poses, std::array<float,7>{20,-30,40,0,0,0,1});
    rr64_lod_shadow_stage(m, Fixture::bike_node, 1u);
}
extern "C" void func_8005EB50(unsigned char* m, recomp_context*) {
    floats(m, poses, std::array<float,7>{15,-27,39,0,0,0,1});
    rr64_lod_shadow_stage(m, Fixture::rider_node, 2u);
}
extern "C" void func_8005B948(unsigned char* m, recomp_context*) {
    floats(m, bike_poses+0x20u, std::array<float,7>{1,2,3,0,0,0,1});
    rr64_lod_shadow_stage(m, Fixture::bike_node, 128u);
}
extern "C" void func_8005B63C(unsigned char*, recomp_context*) {}
extern "C" void func_8005BEEC(unsigned char*, recomp_context*) {}
extern "C" void func_80019130(unsigned char*, recomp_context*) { std::abort(); }
extern "C" void func_80015834(unsigned char*, recomp_context*) { std::abort(); }
int main() {
    {
        auto memory=finish_seed();live_mapping=memory.data();allocate(memory.data());
        auto oracle=memory;
        prepare(memory,10.37f,false,0u,false);
        check_selected(memory,oracle,2u,false); // No verified history must not invent a pose.
    }
    for (bool fr1 : {false,true}) for (unsigned slot : {0u,1u}) for (bool direct : {false,true})
        sequence(fr1,slot,direct);
    for (unsigned i=0;i<unsigned(Refusal::Count);++i) refusal(static_cast<Refusal>(i));
    std::printf("[RR64-FINISH-BLEND] %s: %u sequential actual AEE0/18BD8 cases,11 bones,stockLOD changes,original partial weights and isolated bindings; lifecycle refusals.\n",
        passed ? "PASS" : "FAIL", cases);
    return passed ? 0 : 1;
}

