#include "rr64_race_pack_mod.hpp"
#include "rr64_netplay.hpp"
#include "json/json.hpp"

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
enum GameStatus { None, Running, Quit };
std::atomic<GameStatus> game_status{None};
std::atomic_bool exited{false};
unsigned entries = 0, heaps = 0, saves = 0, loads = 0;
unsigned initializations = 0, callbacks = 0;
bool stored_rom_valid = true;
bool fail_after_assets = false;
std::vector<std::string> errors;
std::map<std::string, bool> assets{
    {"item_audio_runtime", false}, {"item_audio_bank", false}, {"item_render_session", false},
    {"item_material_session", false}, {"item_render_asset", false},
    {"course_music_runtime", false}, {"course_music_bank", false},
    {"course_audio_runtime", false}, {"wood_surfaces", false}, {"course_audio_bank", false},
    {"boost_runtime", false}, {"sky", false}, {"box_runtime", false},
    {"box_render_session", false}, {"box_render_asset", false}, {"hazard_runtime", false},
    {"hazard_render_session", false}, {"hazard_render_asset", false},
    {"pack_menu", false}, {"course_material", false}, {"route_descriptor", false}};
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void clear(const char* name) { assets[name] = false; }
}

namespace ultramodern {
struct thread_terminated {};
bool is_game_started() { return game_status.load() != None; }
void init_saving(uint8_t*) { ++saves; }
namespace error_handling {
void message_box(const char* message) { errors.emplace_back(message); }
}
}
namespace rr64::netplay {
PhysicsRules get_physics_rules() { return {}; }
}

// These declarations supply platform services to the unmodified runtime startup
// body below. They never create a window, audio device, save file or guest thread.
namespace recomp {
constexpr uint32_t mod_rdram_start = 0;
enum class SaveType { None };
struct GameEntry {
    uint64_t entrypoint_address = 0;
    void (*on_init_callback)(uint8_t*, recomp_context*) = nullptr;
    void (*entrypoint)(uint8_t*, recomp_context*) = nullptr;
    std::string mod_game_id;
    SaveType save_type = SaveType::None;
};
namespace mods {
struct ModLoadErrorDetails { std::string mod_id; int error; std::string error_param; };
struct Details { std::string display_name; };
std::optional<Details> get_details_for_mod(const std::string&) { return {}; }
std::string error_to_string(int) { return {}; }
}
bool load_stored_rom(std::u8string&) { return stored_rom_valid; }
void init_heap(uint8_t*, uint32_t) { ++heaps; }
}
struct ModContext {
    std::vector<recomp::mods::ModLoadErrorDetails> load_mods(
        const recomp::GameEntry&, const std::string&, uint8_t*, uint32_t, uint32_t&) {
        ++loads;
        return {};
    }
};
std::optional<std::u8string> current_game{u8"fixture"};
std::string current_game_mode_id;
std::map<std::u8string, recomp::GameEntry> game_roms;
std::mutex mod_context_mutex;
auto mod_context = std::make_unique<ModContext>();
recomp::SaveType save_type{};
void init(uint8_t*, recomp_context*, uint64_t) { ++initializations; }

// Each real reset call clears an independently poisoned service in late-failure
// cases. A removed cleanup call therefore leaves detectable stale state.
namespace rr64::mk64_items {
void reset_audio() { clear("item_audio_runtime"); }
void clear_audio_bank() { clear("item_audio_bank"); }
void reset_render_session() { clear("item_render_session"); }
void reset_material_session() { clear("item_material_session"); }
void clear_render_asset() { clear("item_render_asset"); }
}
namespace rr64::course_music {
void reset_runtime() { clear("course_music_runtime"); }
void unload_bank() { clear("course_music_bank"); }
}
namespace rr64::course_audio {
void reset_runtime() { clear("course_audio_runtime"); }
void set_wood_surfaces(std::initializer_list<unsigned>) { clear("wood_surfaces"); }
void unload_bank() { clear("course_audio_bank"); }
}
namespace rr64::course_boost { void reset_runtime() { clear("boost_runtime"); } }
namespace rr64::course_sky { void reset() { clear("sky"); } }
namespace rr64::course_items {
void reset_runtime() { clear("box_runtime"); }
void reset_render_session() { clear("box_render_session"); }
void clear_render_asset() { clear("box_render_asset"); }
}
namespace rr64::course_hazards {
void reset_runtime() { clear("hazard_runtime"); }
void reset_hazard_render_session() { clear("hazard_render_session"); }
void clear_hazard_asset() { clear("hazard_render_asset"); }
}
void rr64_race_pack_menu_reset_session() { clear("pack_menu"); }
void rr64_course_material_reset_scratch() { clear("course_material"); }
namespace rr64::experimental_course {
using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;
struct Catalogue { unsigned courses = 0; };
Catalogue catalogue;
std::atomic_bool installed_flag{false};
std::atomic_int selected{-1}, loaded{-1};
void reset_descriptor() { clear("route_descriptor"); }
void fixture_install_assets() {
    for (auto& [name, active] : assets) active = true;
    catalogue.courses = 16;
    selected = loaded = 2;
    if (fail_after_assets) throw std::runtime_error("injected invalid hazard asset");
    installed_flag = true;
}
}

#include "rr64_startup_recovery_fixture.inc"

int main(int argc, char** argv) {
    try {
        check(argc == 2, "usage: RR64StartupRecoverySmoke new-scratch-directory");
        namespace fs = std::filesystem;
        namespace pack = rr64::race_pack_mod;
        namespace course = rr64::experimental_course;
        const fs::path scratch = argv[1];
        check(!fs::exists(scratch), "scratch must not exist");
        fs::create_directories(scratch);
        pack::configure_directory(scratch);
        auto& game = game_roms[u8"fixture"];
        game.on_init_callback = [](uint8_t*, recomp_context*) { ++callbacks; course::initialize(); };
        game.entrypoint = [](uint8_t*, recomp_context*) { ++entries; };
        game.mod_game_id = "fixture";
        uint8_t memory{};
        recomp_context context{};
        auto run = [&] {
            game_status = Running;
            return wait_for_game_started(&memory, &context);
        };
        auto unlock = [&] { game_status = None; pack::abort_session(); };
        auto write = [&](const std::string& text) {
            std::ofstream out(scratch / "catalogue.json", std::ios::binary);
            out << text;
            check(bool(out), "write synthetic catalogue");
        };
        auto failed = [&](const char* reason) {
            const auto old_entries = entries, old_heaps = heaps, old_saves = saves, old_loads = loads;
            const auto old_errors = errors.size();
            check(!run(), reason);
            check(game_status == None && !ultramodern::is_game_started(), "failed startup returns launcher");
            check(entries == old_entries && heaps == old_heaps && saves == old_saves && loads == old_loads,
                  "failed init cannot start guest, mods, heap or saving");
            check(errors.size() == old_errors + 1 && errors.back().find("Settings > Mods > MK64 Race Pack") != std::string::npos,
                  "one actionable failure message");
            check(pack::can_change() && !course::installed_flag && course::selected == -1 && course::loaded == -1 &&
                  course::catalogue.courses == 0, "failed catalogue and startup lease cleared");
            for (const auto& [name, active] : assets) check(!active, name.c_str());
        };
        check(run() && entries == 1 && !course::installed_flag, "missing optional pack starts stock game");
        unlock();
        write("{invalid-json");
        failed("malformed catalogue is recoverable");
        check(pack::set_enabled(false), "can disable failed pack without restarting");
        check(run() && entries == 2 && !course::installed_flag, "disabled bad catalogue is not parsed");
        unlock();
        check(pack::set_enabled(true), "can enable pack after failed attempt");
        nlohmann::json catalogue{{"format","rr64-race-pack-catalogue"},{"version",1},
            {"group_id","mk64"},{"group_name","MK64"},{"mk64_items_version",1},
            {"mk64_items_asset","items.bin"},{"mk64_items_audio_asset","audio.bin"}};
        for (const char* field : {"mk64_items_version","mk64_items_asset","mk64_items_audio_asset"}) {
            auto legacy = catalogue;
            legacy.erase(field);
            write(legacy.dump());
            failed("legacy catalogue requests reimport");
            check(errors.back().find("Reimport your Mario Kart 64 ROM") != std::string::npos, "specific compatibility message retained");
        }
        auto wrong_version = catalogue;
        wrong_version["mk64_items_version"] = 2;
        write(wrong_version.dump());
        failed("unsupported item version is rejected");
        write(catalogue.dump());
        fail_after_assets = true;
        failed("partial installation is cleaned after a late loader failure");
        fs::path import_target;
        check(pack::begin_import(import_target) && import_target == scratch, "failed startup permits reimport");
        check(!pack::can_change(), "active import lease still protects pack");
        pack::end_import();
        fail_after_assets = false;
        const auto old_entries = entries;
        check(run() && entries == old_entries + 1 && course::installed_flag, "corrected catalogue retries successfully");
        check(!pack::can_change(), "successful startup keeps pack locked");
        unlock();
        write("null");
        failed("wrong JSON root type is recoverable");
        write("");
        failed("empty catalogue is recoverable");
        write(std::string(2 * 1024 * 1024 + 1, ' '));
        failed("oversized catalogue is rejected before parsing");
        const auto before_rom_failure = std::array{initializations, callbacks, entries, heaps, saves, loads};
        stored_rom_valid = false;
        check(!run() && game_status == None, "missing or changed stored ROM returns launcher");
        check(before_rom_failure == std::array{initializations, callbacks, entries, heaps, saves, loads},
              "invalid stored ROM cannot initialize or execute the game");
        check(errors.back().find("Error opening stored ROM") != std::string::npos,
              "stored ROM error retains recovery instructions");
        stored_rom_valid = true;
        write(catalogue.dump());
        check(run() && course::installed_flag, "valid ROM and pack can retry after ROM failure");
        unlock();
        exited = true;
        write("{invalid-json");
        check(run() && game_status == None, "closing during rejected startup cannot leave game thread waiting");
        stored_rom_valid = false;
        check(run() && game_status == None, "closing during ROM failure cannot leave game thread waiting");
        std::cout << "Startup failure/retry, catalogue compatibility, disabled/missing packs, partial cleanup and import locks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
