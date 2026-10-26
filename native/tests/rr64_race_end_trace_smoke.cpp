#include "rr64_race_end_trace.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_replay.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
unsigned checks = 0;
thread_local bool capturing = false;
std::atomic<unsigned> producer_allocations{0};
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::printf("Race end trace check failed: %s\n", message);
        std::exit(1);
    }
}
int descriptor(std::FILE* file) {
#ifdef _WIN32
    return _fileno(file);
#else
    return fileno(file);
#endif
}
int duplicate(int value) {
#ifdef _WIN32
    return _dup(value);
#else
    return dup(value);
#endif
}
int replace(int source, int target) {
#ifdef _WIN32
    return _dup2(source, target);
#else
    return dup2(source, target);
#endif
}
void close_descriptor(int value) {
#ifdef _WIN32
    _close(value);
#else
    close(value);
#endif
}
void environment(const char* name, const char* value) {
#ifdef _WIN32
    require(_putenv_s(name, value) == 0, "test environment");
#else
    require(setenv(name, value, 1) == 0, "test environment");
#endif
}
class Output {
    std::FILE* file_ = nullptr;
    int saved_ = -1;
public:
    Output() {
        std::fflush(stderr);
        file_ = std::tmpfile();
        require(file_ != nullptr, "temporary output file");
        saved_ = duplicate(descriptor(stderr));
        require(saved_ >= 0 && replace(descriptor(file_), descriptor(stderr)) >= 0, "capture stderr");
    }
    ~Output() {
        std::fflush(stderr);
        replace(saved_, descriptor(stderr));
        close_descriptor(saved_);
        std::fclose(file_);
    }
    std::string read() {
        std::fflush(stderr);
        // stderr and file_ have separate stdio position caches even though
        // their descriptors share one file. Refresh after the other stream wrote.
        require(std::fseek(file_, 0, SEEK_END) == 0, "refresh output position");
        const long size = std::ftell(file_);
        require(size >= 0 && std::fseek(file_, 0, SEEK_SET) == 0, "output seek");
        std::string text(static_cast<std::size_t>(size), '\0');
        require(std::fread(text.data(), 1, text.size(), file_) == text.size(), "output read");
        require(std::fseek(file_, 0, SEEK_END) == 0, "output append position");
        return text;
    }
};
unsigned occurrences(const std::string& text, const std::string& match) {
    unsigned count = 0;
    for (std::size_t at = 0; (at = text.find(match, at)) != std::string::npos; at += match.size())
        ++count;
    return count;
}
void capture(unsigned char* memory, unsigned target = 0x1E) {
    capturing = true;
    rr64_trace_race_end(memory, target, 0x8004025C);
    capturing = false;
}
void observe(unsigned char* memory) {
    capturing = true;
    rr64_trace_lap_frame(memory);
    capturing = false;
}
void recover(unsigned char* memory, unsigned actor) {
    capturing = true;
    rr64_trace_race_recovery(memory, actor);
    capturing = false;
}
void fill(unsigned char* memory) {
    using namespace rr64::engine;
    write_u32(memory, globals::main_mode, 0x1C);
    write_u32(memory, globals::pending_mode, 0x1C);
    write_float(memory, globals::total_ticks, 123.5f);
    write_float(memory, 0x800D7670, 45.25f);
    write_u32(memory, local_race::humans, 2);
    write_u32(memory, local_race::racers, 14);
    write_u32(memory, 0x800A656C, 14);
    write_u32(memory, 0x800D8524, 2);
    write_u32(memory, 0x800A6544, 0x80200000);
    write_u32(memory, 0x800A6540, 21);
    write_u32(memory, 0x800D763C, 16);
    write_u32(memory, 0x800D7644, 2);
    write_float(memory, 0x800D762C, 3300.0f);
    write_float(memory, 0x800D7630, 3200.0f);
    write_float(memory, 0x800D7634, 9700.0f);
    write_u32(memory, 0x800D7628, 16);
    write_float(memory, 0x800D7638, 0.75f);
    for (unsigned offset : {8u, 12u, 24u, 28u, 40u, 44u}) {
        write_float(memory, 0x80200000 + offset, float(offset));
        write_float(memory, 0x80200000 + 16 * 16 + offset, float(offset));
    }
    write_u32(memory, 0x800D7648, 1);
    write_u32(memory, 0x800D764C, 12);
    write_u32(memory, 0x800D7658, 1);
    write_u32(memory, 0x800D765C, 1);
    write_float(memory, 0x800D7678, 20.5f);
    for (unsigned slot = 0; slot < 14; ++slot) {
        const unsigned actor = 0x800D8570 + slot * 0x118;
        const unsigned state = 0x800D7810 + slot * 0x64;
        const unsigned bike = 0x80100000 + slot * 0x868;
        write_u32(memory, actor + 8, slot < 2 ? slot : 0xFFFFFFFFu);
        write_u32(memory, actor + 0x20, slot == 2 ? 7 : 0);
        write_u16(memory, actor + 0x24, 1);
        write_u16(memory, actor + 0x26, slot >= 2);
        write_u32(memory, actor + 0xE8, state);
        write_u32(memory, actor + 0xE0, bike);
        write_u32(memory, state, slot * 2);
        write_u32(memory, state + 4, 1);
        write_float(memory, state + 8, 0.25f);
        write_float(memory, state + 0xC, 123.5f + float(slot));
        write_float(memory, state + 0x20, 500.0f + float(slot));
        write_u32(memory, state + 0x40, 13 - slot);
        write_u32(memory, state + 0x44, 12 - (slot % 13));
        write_u16(memory, state + 0x48, 1);
        write_u16(memory, state + 0x4C, slot == 3);
        write_u16(memory, state + 0x4E, slot == 4);
        write_u16(memory, state + 0x50, slot == 0);
        write_u16(memory, state + 0x52, 5);
        write_float(memory, bike + 0x4F8, 75.0f);
        write_float(memory, bike + 0x4FC, 100.0f);
        write_float(memory, bike + 0x4CC, 2.0f);
        write_u16(memory, bike + 0x7F6, 1);
        write_float(memory, bike + 0x16C, -42.0f);
        write_float(memory, bike + 0x170, 64.0f);
    }
    write_u32(memory, 0x800D7810 + 11 * 0x64 + 8, 0x7FC12345); // Keep NaN payload.
    write_u32(memory, 0x800D8570 + 12 * 0x118 + 0xE8, 0x800D7801); // Unaligned.
    write_u32(memory, 0x800D8570 + 13 * 0x118 + 0xE0, 0xFFFFFFFC); // Wrapped pointer.
}
} // namespace

// Detect accidental allocation on the producer, without restricting stdio or
// the test fixture's own allocations on the consumer thread.
void* operator new(std::size_t size) {
    if (capturing) producer_allocations.fetch_add(1);
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main(int argc, char** argv) {
    using namespace rr64;
    const bool disabled = argc == 2 && std::strcmp(argv[1], "--disabled") == 0;
    const bool diagnostics = argc == 2 && std::strcmp(argv[1], "--diagnostics") == 0;
    const bool population = argc == 2 && std::strcmp(argv[1], "--population") == 0;
    environment("RR64_POPULATION_TRACE", population ? "1" : "0");
    environment("RR64_RUNTIME_TRACE", !disabled && !diagnostics ? "1" : "0");
    environment("RR64_DIAGNOSTICS", diagnostics ? "1" : "0");
    environment("RR64_COURSE_PHYSICS_TRACE", "0");
    std::vector<unsigned char> memory(engine::kRdramSize, 0);
    fill(memory.data());
    const auto original = memory;
    Output output;
    race_end_trace::drain();
    if (population) {
        engine::write_u32(memory.data(), 0x800D8570 + 0x20, 7);
        engine::write_float(memory.data(), 0x800D8560, 36.f);
        const auto before = memory;
        capturing = true;
        rr64_trace_population(memory.data(), 1);
        capturing = false;
        require(output.read().empty(), "population producer performs no I/O");
        race_end_trace::drain();
        auto text = output.read();
        require(text.find("phase=1 cars=0 density=36") != std::string::npos, "population settings captured");
        require(text.find("actor_mask=0005") != std::string::npos, "only police physics selected");
        {
            prediction::ReplayScope replay;
            rr64_trace_population(memory.data(), 2);
        }
        rr64_trace_population(nullptr, 2);
        race_end_trace::drain();
        require(output.read() == text, "prediction and null observations excluded");
        require(memory == before, "population observation preserves guest state");
        unsigned police_bike = 0;
        engine::read_u32(memory.data(), 0x800D8570 + 0xE0, police_bike);
        capturing = true;
        rr64_trace_police_crash(memory.data(), police_bike, 0x8003F950);
        capturing = false;
        require(output.read() == text && memory == before, "crash producer preserves state and performs no I/O");
        race_end_trace::drain();
        text = output.read();
        require(text.find("site=8003F950") != std::string::npos, "exact crash call site captured");
        rr64_trace_police_crash(memory.data(), 0xFFFFFFFC, 0);
        rr64_trace_police_crash(nullptr, police_bike, 0);
        race_end_trace::drain();
        require(output.read() == text, "invalid crash identities ignored");
        engine::write_u32(memory.data(), 0x800A6528, 1);
        engine::write_u32(memory.data(), 0x800D76E0, 0x80300000);
        engine::write_float(memory.data(), 0x803000C0, 30.f);
        engine::write_float(memory.data(), 0x80300328, 0.f);
        engine::write_u16(memory.data(), 0x80300334, 1);
        capturing = true;
        rr64_trace_traffic_target(memory.data(), 0x80300000, 0x41F00000);
        rr64_trace_traffic_target(memory.data(), 0x80300000, 0x41F00000);
        capturing = false;
        require(output.read() == text, "traffic decision producer performs no I/O");
        race_end_trace::drain();
        text = output.read();
        require(occurrences(text, "phase=4 cars=") == 1, "unchanged stopped target does not flood trace");
        require(text.find("before_road_bits=41F00000") != std::string::npos, "road braking distinguished from prior following brake");
        engine::write_float(memory.data(), 0x80300328, 30.f);
        rr64_trace_traffic_target(memory.data(), 0x80300000, 0x41F00000);
        race_end_trace::drain();
        text = output.read();
        require(occurrences(text, "phase=4 cars=") == 2, "resumed traffic captured");
        {
            prediction::ReplayScope replay;
            rr64_trace_police_crash(memory.data(), police_bike, 0);
            rr64_trace_traffic_target(memory.data(), 0x80300000, 0);
        }
        rr64_trace_traffic_target(nullptr, 0x80300000, 0);
        rr64_trace_traffic_target(memory.data(), 0xFFFFFFFC, 0);
        race_end_trace::drain();
        require(output.read() == text, "invalid and prediction decisions excluded");
        engine::write_float(memory.data(), 0x80300328, 0.f);
        const auto traffic_before = memory;
        for (unsigned i = 4; i < 1200; ++i) {
            rr64_trace_population(memory.data(), 2);
            race_end_trace::drain();
        }
        text = output.read();
        require(occurrences(text, "[RR64-POPULATION]") == 1200, "population capture bounded");
        require(text.find("speed=30 target=0 active=1") != std::string::npos, "traffic braking captured separately from speed");
        rr64_trace_population(memory.data(), 2);
        race_end_trace::drain();
        require(output.read() == text && memory == traffic_before, "cap stays silent and guest state unchanged");
        require(producer_allocations.load() == 0, "population producer allocates nothing");
        std::printf("RR64 population trace: %u checks passed.\n", checks);
        return 0;
    }
    capture(memory.data());
    require(output.read().empty(), "producer performs no output");
    race_end_trace::drain();
    std::string text = output.read();
    if (disabled) {
        rr64_trace_police_crash(memory.data(), 0x80240000, 0);
        rr64_trace_traffic_target(memory.data(), 0x80300000, 0);
        recover(memory.data(), 0x800D8570);
        observe(memory.data());
        race_end_trace::drain();
        require(text.empty() && memory == original, "disabled diagnostics remain dormant");
        require(output.read().empty(), "disabled lap/recovery hooks remain dormant");
        require(producer_allocations.load() == 0, "disabled producer allocates nothing");
        std::printf("RR64 race end trace: %u disabled checks passed.\n", checks);
        return 0;
    }
    require(occurrences(text, "[RR64-RACE-END]") == 1, "one captured transition");
    require(occurrences(text, "[RR64-RACE-END-ACTOR]") == 14, "all fourteen actor slots");
    require(text.find("target=30 target_hex=1E raw_ra=8004025C") != std::string::npos, "decimal and hex modes distinct");
    require(text.find("total_ticks_bits=42F70000 frame_timing_sum=123.5 race_elapsed=45.25 type=2 humans=2 racers=14 total=14 captured=14") != std::string::npos, "distinct frame timing and accumulated native race time");
    require(text.find("t_bits=7FC12345") != std::string::npos, "NaN bits preserved");
    require(text.find("state=800D7801 state_valid=0") != std::string::npos, "unaligned state skipped");
    require(text.find("bike=FFFFFFFC bike_valid=0") != std::string::npos, "invalid bike skipped");
    require(text.find("eligible=1 busted=1 wrecked=0 finished=0 flag52=5") != std::string::npos, "terminal flags");
    require(text.find("table_rank=10 race_place=9 eligible=1 busted=1") != std::string::npos,
            "table order and eligible race place remain distinct");
    require(text.find("health=75 health_bits=42960000 max_health=100") != std::string::npos, "bike health");
    require(memory == original, "capture and drain never modify guest memory");
    race_end_trace::drain();
    require(output.read() == text, "draining twice does not repeat rows");

    capture(nullptr);
    capture(memory.data(), 0x1C);
    {
        prediction::ReplayScope replay;
        capture(memory.data());
    }
    engine::write_u32(memory.data(), engine::globals::main_mode, 0x1E);
    capture(memory.data());
    engine::write_u32(memory.data(), engine::globals::main_mode, 0x1C);
    engine::write_u32(memory.data(), engine::globals::pending_mode, 0x1E);
    capture(memory.data());
    engine::write_u32(memory.data(), engine::globals::pending_mode, 0x1C);
    race_end_trace::drain();
    require(output.read() == text, "null, live target, replay, results source and duplicate pending excluded");

    for (unsigned target : {0x0Bu, 0x14u, 0x19u, 0x1Eu, 0x1Fu, 0x30u, 0x31u, 0x32u, 0x33u})
        capture(memory.data(), target);
    for (unsigned i = 0; i < 5; ++i) {
        const unsigned before = occurrences(output.read(), "[RR64-RACE-END]");
        race_end_trace::drain();
        require(occurrences(output.read(), "[RR64-RACE-END]") - before <= 2, "UI drain bounded to two snapshots");
    }
    text = output.read();
    require(occurrences(text, "[RR64-RACE-END]") == 10, "all documented terminal modes captured");
    for (unsigned i = 0; i < 19; ++i) capture(memory.data());
    for (unsigned i = 0; i < 8; ++i) race_end_trace::drain();
    text = output.read();
    require(occurrences(text, "[RR64-RACE-END]") == 26, "ring stores exactly sixteen pending snapshots");
    require(occurrences(text, "[RR64-RACE-END-LOSS] dropped=3 capacity=16") == 1, "overflow reported once");
    race_end_trace::drain();
    require(output.read() == text, "empty drain repeats neither snapshots nor loss count");
    require(memory == original, "overflow and replay leave memory unchanged");
    engine::write_u32(memory.data(), 0x800A656C, 0xFFFFFFFFu);
    capture(memory.data());
    race_end_trace::drain();
    text = output.read();
    require(text.find("total=4294967295 captured=14") != std::string::npos, "corrupt count bounded");
    require(occurrences(text, "[RR64-RACE-END-ACTOR]") == 27 * 14, "no actor read beyond fourteen");

    memory = original;
    engine::write_u32(memory.data(), 0x800D7810 + 4, 0);
    engine::write_u16(memory.data(), 0x800D7810 + 0x50, 0);
    auto before_observe = memory;
    observe(memory.data());
    require(output.read() == text, "lap producer performs no output");
    race_end_trace::drain();
    text = output.read();
    require(occurrences(text, "[RR64-LAP-EVENT]") == 1, "one circuit start snapshot");
    require(occurrences(text, "[RR64-LAP-ACTOR]") == 2, "only offline humans receive event rows");
    require(text.find("prior_laps_required=2 lap_threshold=3300") != std::string::npos, "lap settings captured");
    require(text.find("period=3200 period_bits=45480000 finish_threshold=9700") != std::string::npos, "period and finish threshold captured");
    require(memory == before_observe, "lap observer is read-only");
    for (unsigned frame = 0; frame < 10; ++frame) observe(memory.data());
    engine::write_u32(memory.data(), 0x800D7810 + 2 * 0x64, 6); // AI only.
    engine::write_float(memory.data(), 0x800D7810 + 8, 0.5f); // No marker crossing.
    observe(memory.data());
    race_end_trace::drain();
    require(output.read() == text, "stationary, ordinary motion and AI changes produce no periodic flood");
    engine::write_float(memory.data(), 0x800D7810 + 8, 0.8f);
    observe(memory.data());
    race_end_trace::drain();
    text = output.read();
    require(occurrences(text, "[RR64-LAP-EVENT]") == 2, "short initial physical marker crossing captured");
    require(text.find("slot=0 reasons=64 previous_segment=0 previous_lap=0") != std::string::npos,
            "physical marker crossing distinct from a completed-lap change");
    engine::write_u32(memory.data(), 0x800D7810, 2);
    engine::write_u32(memory.data(), 0x800D7810 + 4, 1);
    engine::write_u32(memory.data(), 0x800D7874, 4);
    engine::write_u32(memory.data(), 0x800D7874 + 4, 2);
    recover(memory.data(), 0x800D8570);
    recover(memory.data(), 0x800D8570);
    require(output.read() == text, "recovery hook only records a marker");
    before_observe = memory;
    observe(memory.data());
    race_end_trace::drain();
    text = output.read();
    require(occurrences(text, "[RR64-LAP-EVENT]") == 3, "multiple human changes coalesce into one frame snapshot");
    require(text.find("slot=0 reasons=22 previous_segment=0 previous_lap=0") != std::string::npos,
            "recovery plus segment and lap changes retain previous state");
    require(memory == before_observe, "recovery observation does not modify guest state");
    {
        prediction::ReplayScope replay;
        recover(memory.data(), 0x800D8570);
        observe(memory.data());
    }
    recover(memory.data(), 0);
    recover(memory.data(), 0x800D8571);
    recover(memory.data(), 0x800D8570 + 14 * 0x118);
    recover(memory.data(), 0x800D8570 + 13 * 0x118); // Offline AI.
    observe(memory.data());
    race_end_trace::drain();
    require(output.read() == text, "replay and invalid/nonhuman recovery markers are excluded");
    engine::write_u16(memory.data(), 0x800D7810 + 0x50, 1);
    observe(memory.data());
    race_end_trace::drain();
    text = output.read();
    require(occurrences(text, "[RR64-LAP-EVENT]") == 4, "individual human finish event captured");
    require(text.find("slot=0 reasons=8 previous_segment=2 previous_lap=1") != std::string::npos,
            "finish event separated from lap change");
    race_end_trace::set_authority_humans(1u << 13);
    recover(memory.data(), 0x800D8570 + 13 * 0x118);
    observe(memory.data());
    race_end_trace::drain();
    text = output.read();
    require(occurrences(text, "[RR64-LAP-EVENT]") == 5, "online ownership mask selects remote human 13");
    require(text.find("slot=13 reasons=48") != std::string::npos, "remote actor identity and recovery recorded despite AI/controller flags");
    race_end_trace::set_authority_humans(0);
    engine::write_u32(memory.data(), engine::globals::main_mode, 0);
    observe(memory.data());
    engine::write_u32(memory.data(), engine::globals::main_mode, 0x1C);
    observe(memory.data());
    race_end_trace::drain();
    text = output.read();
    require(text.find("epoch=2 observed_frame=1 reasons=1") != std::string::npos, "menu transition resets next race baseline");
    engine::write_u32(memory.data(), 0x800D7810, 16);
    engine::write_float(memory.data(), 0x800D7810 + 8, 0.5f);
    observe(memory.data());
    race_end_trace::drain();
    text = output.read();
    constexpr unsigned replacement_state = 0x80210000;
    std::memcpy(memory.data() + (replacement_state & 0x1FFFFFFF),
                memory.data() + (0x800D7810 & 0x1FFFFFFF), 0x64);
    engine::write_float(memory.data(), replacement_state + 8, 0.8f);
    engine::write_u32(memory.data(), 0x800D8570 + 0xE8, replacement_state);
    observe(memory.data());
    race_end_trace::drain();
    const auto identity_change = output.read().substr(text.size());
    require(identity_change.find("slot=0 reasons=32 previous_segment=16") != std::string::npos,
            "replaced state identity never implies a physical marker crossing");
    require(producer_allocations.load() == 0, "producer allocates nothing");
    std::printf("RR64 race end trace: %u checks passed (%s).\n", checks, diagnostics ? "diagnostics" : "runtime trace");
}
