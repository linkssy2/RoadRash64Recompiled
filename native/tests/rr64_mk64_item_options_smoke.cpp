#include "rr64_local_race_options.hpp"
#include "rr64_local_players.hpp"
#include "rr64_race_pack_menu.hpp"
#include "rr64_race_pack.hpp"
#include "rr64_thrash_options.hpp"
#include "rr64_netplay.hpp"
#include "rr64_offline_modifiers.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_engine_layout.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <source_location>
#include <string>
#include <vector>
extern "C" int rr64_highlights_presenting() { return 0; }

namespace {
unsigned checks = 0;
rr64::netplay::Status session{};
std::optional<std::size_t> selected;
std::array<rr64::race_pack::CourseMenuEntry, 17> courses;
void check(bool value, std::source_location at = std::source_location::current()) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "MK64 item options failed at line %u\n", at.line());
        std::exit(1);
    }
}
std::string contents(const std::filesystem::path &path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
} // namespace
namespace rr64::netplay {
Status get_status() { return session; }
PhysicsRules get_physics_rules() {
    PhysicsRules rules{};
    rules.active = session.active;
    rules.connected = session.connected;
    rules.is_host = session.is_host;
    return rules;
}
} // namespace rr64::netplay
namespace rr64::race_pack {
std::span<const CourseMenuEntry> menu_courses() noexcept { return courses; }
std::optional<std::size_t> selected_course() noexcept { return selected; }
bool select_course(std::size_t index) { selected = index; return index < courses.size(); }
void select_stock() noexcept { selected.reset(); }
} // namespace rr64::race_pack
namespace recomp {
void *alloc(unsigned char *memory, size_t size) {
    static size_t next = 0x700000;
    const size_t offset = next;
    next += (size + 15) & ~size_t(15);
    check(next < 0x800000);
    return memory + offset;
}
void free(unsigned char *, void *) {}
} // namespace recomp
namespace rr64::offline_modifiers { bool enabled(Flag) { return false; } }
extern "C" int rr64_offline_bikes_active(unsigned char *, unsigned) { return 0; }
extern "C" unsigned rr64_offline_bikes_entry(unsigned char *, unsigned, unsigned original) { return original; }
extern "C" int rr64_custom_cop_active() { return 0; }
extern "C" int rr64_custom_cop_finished(unsigned char *) { return 0; }
extern "C" void func_800740A0(unsigned char *, recomp_context *);
extern "C" void rr64_custom_cop_reset() {}
extern "C" void rr64_custom_cop_begin(unsigned char *, int) {}
extern "C" void rr64_course_progress_log(const char *, ...) {}
extern "C" void switch_error(const char *, uint32_t, uint32_t) { std::abort(); }
extern "C" void item_options_native_solo(unsigned char *, recomp_context *);
extern "C" void item_options_native_multiplayer(unsigned char *, recomp_context *);

int main(int argc, char **argv) {
    check(argc >= 2);
    namespace option = rr64::local_race_options;
    namespace menu = rr64::engine::local_race;
    using namespace rr64::engine;
    const std::filesystem::path directory = argv[1];
    std::filesystem::create_directories(directory);
    option::initialize(directory);
    if (argc == 3) {
        if (std::string(argv[2]) == "environment") {
            std::vector<unsigned char> memory(kRdramSize);
            rr64::local_players::active.store(true);
            rr64_local_options_menu_begin(memory.data());
            unsigned traffic = 99;
            read_u32(memory.data(), 0x8009EAD8, traffic);
            check(traffic == 0);
            std::puts("Local traffic preference survives restart");
            return 0;
        }
        check(option::mk64_items_enabled() == (std::string(argv[2]) == "1"));
        std::printf("MK64 item preference restart: %u checks passed\n", checks);
        return 0;
    }
    check(option::mk64_items_enabled());
    for (unsigned i = 0; i < 16; ++i)
        courses[i] = {"mk64", "Mario Kart 64", option::music_courses[i], "Imported Race", {}, 0, 0, 0, 0};
    courses[16] = {"other", "Other Pack", "unrelated_course", "Other Race", {}, 0, 0, 0, 0};
    std::vector<unsigned char> memory(kRdramSize);
    auto *rdram = memory.data();
    const auto put = [&](unsigned address, unsigned value) { write_u32(rdram, address, value); };
    const auto get = [&](unsigned address) { unsigned value = 0; read_u32(rdram, address, value); return value; };
    for (unsigned level = 0; level < 15; ++level) {
        MEM_H(0, guest_address(0x800A5334 + level * 2)) = 1;
        put(0x800A7420 + level * 4, 6);
    }
    for (unsigned level = 0; level < 8; ++level)
        put(0x8009ECB8 + level * 12, level);
    // Native table records remain intact, including the original traffic row.
    for (unsigned i = 0; i < 8 * 36; i += 4) {
        put(menu::menu_table + i, 0x12340000 + i);
        put(0x8009EB4C + i, 0x56780000 + i);
    }
    put(menu::humans, 2); put(menu::menu_humans, 2);
    const auto enter = [&](bool multiplayer) {
        rr64_thrash_options_mode(multiplayer ? 0 : 33);
        rr64::local_players::active.store(multiplayer);
        put(globals::multiplayer_stage, 0);
        put(0x8009ECA0, 0);
        if (multiplayer) rr64_local_options_menu_begin(rdram);
        else rr64_thrash_options_begin(rdram);
        rr64_race_pack_menu_begin(rdram);
    };
    const auto input = [&](bool multiplayer, unsigned buttons) {
        put(menu::menu_cursor, 6); put(menu::menu_buttons, buttons);
        recomp_context context{};
        context.r29 = guest_address(0x80600000);
        if (multiplayer) item_options_native_multiplayer(rdram, &context);
        else item_options_native_solo(rdram, &context);
        check(get(menu::menu_buttons) == buttons);
    };
    const auto text = [&] {
        constexpr unsigned buffer = 0x80601000;
        for (unsigned i = 0; i < 28; ++i) MEM_B(i, guest_address(buffer)) = 0x5A;
        rr64_race_pack_menu_text(rdram, 6, buffer);
        std::string value;
        for (unsigned i = 0; i < 24 && MEM_BU(i, guest_address(buffer)); ++i)
            value.push_back(static_cast<char>(MEM_BU(i, guest_address(buffer))));
        check(get(buffer + 24) == 0x5A5A5A5A);
        return value;
    };
    for (bool multiplayer : {false, true}) {
        enter(multiplayer);
        for (unsigned index = 0; index < 16; ++index) {
            selected = index;
            const unsigned traffic_address = multiplayer ? 0x8009EAD8 : 0x8009EAC4;
            put(traffic_address, 2);
            input(multiplayer, 0x40);
            check(!option::mk64_items_enabled());
            check(text() == "MK64 Items: Off");
            check(get(traffic_address) == 2);
            input(multiplayer, 0x20);
            check(option::mk64_items_enabled());
            check(text() == "MK64 Items: On");
            check(get(traffic_address) == 2);
            if (multiplayer) {
                // Race types that hide every optional native row still expose it.
                for (unsigned i = 0; i < 6; ++i) MEM_B(i, guest_address(menu::visibility)) = 0;
                check(rr64_local_options_visible(6, 0) == 1);
                put(menu::menu_cursor, 4); put(menu::menu_buttons, 0x10);
                check(rr64_local_options_navigation(rdram) == 6);
            }
            rr64_local_options_reset_race();
            check(option::mk64_items_enabled());
        }
        selected = 16;
        input(multiplayer, 0x40);
        check(option::mk64_items_enabled());
        check(text() == "Traffic: Unavailable");
        if (multiplayer) check(rr64_local_options_visible(6, 0) == 1);
        selected.reset();
        input(multiplayer, 0);
        if (multiplayer) {
            put(0x8009EAD8, 1);
            // Light -> Some -> Heavy -> None; the native flag remains boolean.
            for (unsigned enabled : {1u, 1u, 0u}) {
                input(true, 0x40);
                check(get(0x8009EAD8) == enabled);
            }
            check(rr64_local_options_visible(6, 0) == 1);
        } else {
            put(0x8009EAC4, 1);
            input(false, 0x40);
            check(get(0x8009EAC4) == 2);
        }
        rr64_race_pack_menu_end(rdram);
        check(rr64_local_options_visible(6, 0) == unsigned(multiplayer));
    }
    // Host choice includes bit 27 without changing the existing packed options.
    session.active = session.connected = session.is_host = true;
    option::reset_online();
    constexpr unsigned unrelated = 7 | (2 << 4) | (5 << 6) | 512 | 1024 | option::course_music_mask;
    option::apply_online_options(unrelated);
    enter(true); selected = 0;
    input(true, 0x40);
    check(!option::mk64_items_enabled());
    check(option::online_options() == (unrelated | option::mk64_items_disabled_bit));
    option::flush();
    check(contents(directory / "mk64-item-options.cfg") == "1 0\n");
    const auto local_before = contents(directory / "local-race-options.cfg");
    const auto solo_before = contents(directory / "thrash-race-options.cfg");
    session.is_host = false;
    for (unsigned bits : {unrelated, unrelated | option::mk64_items_disabled_bit}) {
        option::apply_online_options(bits);
        check(option::mk64_items_enabled() == !(bits & option::mk64_items_disabled_bit));
        input(true, 0x40);
        check(option::online_options() == bits);
        option::flush();
        check(contents(directory / "mk64-item-options.cfg") == "1 0\n");
    }
    option::apply_online_options(unrelated | (1u << 28));
    check(option::online_options() == (unrelated | option::mk64_items_disabled_bit));
    session = {};
    check(!option::mk64_items_enabled());
    option::reset_online();
    check(option::online_options() & option::mk64_items_disabled_bit);
    // No live setting mutation or read is exposed to a private replay.
    rr64::prediction::replay_active = true;
    option::toggle_mk64_items(option::music_courses[0]);
    check(!option::mk64_items_enabled());
    rr64::prediction::replay_active = false;
    check(!option::mk64_items_enabled());
    option::flush();
    check(contents(directory / "local-race-options.cfg") == local_before);
    check(contents(directory / "thrash-race-options.cfg") == solo_before);
    check(contents(directory / "mk64-item-options.cfg") == "1 0\n");
    // Execute the original finish evaluator: a busted human decrements the
    // human count, but surviving humans still keep ordinary races running.
    put(0x800D8524, 1); put(menu::humans, 4);
    put(0x800D764C, 7);
    recomp_context finish{};
    for (unsigned remaining = 1; remaining <= 4; ++remaining) {
        put(0x800D7648, remaining);
        func_800740A0(rdram, &finish);
        check(finish.r2 == 0);
    }
    put(0x800D7648, 0);
    func_800740A0(rdram, &finish);
    check(finish.r2 == 1);
    // Original native input must not undo the independent population choices.
    selected.reset(); enter(true);
    put(0x8009EAD8, 1); put(menu::pedestrian_choice, 2);
    put(menu::menu_cursor, 7); put(menu::menu_buttons, 0x40);
    recomp_context input_context{}; input_context.r29 = guest_address(0x80600000);
    item_options_native_multiplayer(rdram, &input_context);
    check(get(0x8009EAD8) == 1);
    const unsigned pedestrians = get(menu::pedestrian_choice);
    put(menu::menu_cursor, 6);
    for (unsigned enabled : {1u, 1u, 1u, 0u}) {
        item_options_native_multiplayer(rdram, &input_context);
        check(get(0x8009EAD8) == enabled && get(menu::pedestrian_choice) == pedestrians);
    }
    option::flush();
    std::printf("MK64 native item options and local race completion: %u checks passed\n", checks);
}
