#include "rr64_msvc_crt_compat.hpp"
#include "rr64_achievements.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_offline_modifiers.hpp"
#include "rr64_prediction_replay.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "recomp.h"
#include "rr64_achievement_audio.hpp"
#include "rr64_achievement_logic.hpp"
#include "rr64_native.hpp"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "util/file.h"
#include "elements/ui_config_page.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "elements/ui_scroll_container.h"
#include "elements/ui_theme.h"

namespace rr64::achievements {
namespace {

struct AchievementDefinition {
    std::uint32_t retro_id;
    const char *title;
    const char *description;
    std::uint16_t points;
};

// RetroAchievements game 10238, Base Set. The IDs, display order, wording,
// and point values are retained so this local presentation can stay aligned
// with the published Road Rash 64 set without shipping any ROM-derived art.
constexpr std::array<AchievementDefinition, 52> kAchievements{{
    {157994, "Level 2", "Qualify on all level 1 races and buy a level 2 bike", 10},
    {157995, "Level 3", "Qualify on all level 2 races and buy a level 3 bike", 10},
    {157996, "Level 4", "Qualify on all level 3 races and buy a level 4 bike", 10},
    {157997, "Level 5", "Qualify on all level 4 races and buy a level 5 bike", 25},
    {157998, "Congratulations!", "Qualify on all level 5 races", 25},
    {157999, "Taunting at the End - lvl 1", "Finish first while taunting on a level 1 track", 5},
    {158000, "Taunting at the End - lvl 2", "Finish first while taunting on a level 2 track", 10},
    {158001, "Taunting at the End - lvl 3", "Finish first while taunting on a level 3 track", 10},
    {158002, "Taunting at the End - lvl 4", "Finish first while taunting on a level 4 track", 10},
    {158003, "Taunting at the End - lvl 5", "Finish first while taunting on a level 5 track", 10},
    {158004, "One Point!", "Run over a pedestrian", 1},
    {158005, "Two Points! (Big Game)", "Run over a pedestrian while taunting", 2},
    {158006, "Five Points! (Big Game)", "Run over a pedestrian while taunting and riding a wheelie",
     5},
    {158007, "Airborne (Big Game)", "Finish a race while airborne", 10},
    {158008, "Deputize Me (Big Game)", "Have two stolen police batons at once", 5},
    {158009, "I'm the Law Now (Big Game)", "Have four stolen police batons at once", 10},
    {158010, "Not Playing Billiards (Big Game)", "Find a pool cue", 5},
    {158011, "Jammed (Big Game)", "Flip four riders in one race by jamming their wheels", 10},
    {158012, "Quite the Rap Sheet (Big Game)",
     "KO two drivers, flip two drivers, and assault an officer three times in one race", 10},
    {158013, "Super Powered KO (Big Game)",
     "Knock out three riders in one race while under the 4x buff", 25},
    {158014, "Other Riders", "Switch between riders", 2},
    {158015, "Master Thief (Big Game)", "Steal three weapons and win the race", 10},
    {158016, "Unstoppable (Big Game)", "Crash three cops in one race", 25},
    {158017, "Bike Repair (Big Game)", "Find a wrench and repair your bike during a race", 5},
    {158018, "I Do What I Want (Big Game)", "Crash a cop while they are trying to arrest you", 5},
    {158019, "They Had It Coming (Big Game)", "Run over three pedestrians and qualify", 10},
    {158020, "Two Times the Power (Big Game)", "Get the 2x power-up", 5},
    {158021, "Four Times the Power (Big Game)", "Get the 4x power-up", 5},
    {158022, "Join a Gang (Big Game)", "Become a gang member while moving to level 2", 5},
    {158023, "Firecracker 400 (Big Game)", "Win a race using the Firecracker 400", 5},
    {158024, "Dumoto 500 (Big Game)", "Win a race using the Dumoto 500", 5},
    {158025, "Rattler 600 (Big Game)", "Win a race using the Rattler 600", 5},
    {158026, "Razorback 650 (Big Game)", "Win a race using the Razorback 650", 5},
    {158027, "How's it Feel?? (Big Game)", "Shock a cop", 10},
    {158028, "Have at You (Big Game)", "Attack a pedestrian with a weapon", 5},
    {158029, "Just Gonna Lay There? (Big Game)", "Stay down after a crash", 3},
    {158030, "It's Mine Now (Big Game)", "Steal another rider's weapon", 5},
    {158031, "One Wheel", "Finish first while riding a wheelie", 5},
    {158032, "Busted", "Get caught by the law", 3},
    {158033, "Airborne ShowStopper (Big Game)", "Finish a race airborne and taunting", 10},
    {158034, "Taunt the Other Racers", "Taunt the other racers a second time", 2},
    {158035, "Top Gear", "Purchase the Executioner or Hammerhead", 25},
    {158036, "Level 1 Thrash",
     "Win the specified level 1 Thrash race after knocking out or crashing at least three riders",
     10},
    {158037, "Level 2 Thrash",
     "Win the specified level 2 Thrash race after knocking out at least three riders", 10},
    {158038, "Level 3 Thrash",
     "Win the specified level 3 Thrash race after knocking out at least three riders", 10},
    {158039, "Multiplayer - Level 1 Race 3",
     "Win the one-lap level 1 race 3 challenge against the toughest CPU", 5},
    {158040, "Multiplayer - Level 2 Race 2",
     "Win the one-lap level 2 race 2 challenge against the toughest CPU", 10},
    {158041, "Multiplayer - Level 3 Race 5",
     "Win the three-lap level 3 race 5 challenge against the toughest CPU", 10},
    {158042, "Multiplayer Deathmatch",
     "Win the level 3 race 4 Deathmatch challenge against the toughest CPU", 25},
    {158043, "Multiplayer Tag", "Win the level 2 race 1 Tag challenge against the toughest CPU",
     25},
    {158044, "Multiplayer PED Hunt", "Win the level 3 PED Hunt challenge against the toughest CPU",
     5},
    {158045, "Multiplayer PED Hunt - 10 Points",
     "Win the level 3 PED Hunt challenge with at least 10 points against the toughest CPU", 10},
}};

constexpr std::uint16_t kTotalPoints = 488;
constexpr std::uint32_t achievement_point_total() {
    std::uint32_t total = 0;
    for (const AchievementDefinition &achievement : kAchievements) {
        total += achievement.points;
    }
    return total;
}

static_assert(kAchievements.size() == 52, "Road Rash 64 Base Set entry count changed");
static_assert(achievement_point_total() == kTotalPoints,
              "Road Rash 64 Base Set point total changed");
constexpr auto kToastEnterDuration = std::chrono::milliseconds(900);
constexpr auto kToastHoldDuration = std::chrono::milliseconds(3900);
constexpr auto kToastExitDuration = std::chrono::milliseconds(380);
constexpr auto kToastDuration = kToastEnterDuration + kToastHoldDuration + kToastExitDuration;
constexpr const char *kToastFrameResource = "rr64-achievement-toast-frame";
constexpr const char *kToastFrameAsset = "achievement-toast-frame-v2.png";

struct UiState {
    recompui::ContextId toast_context = recompui::ContextId::null();
    recompui::Element *toast_container = nullptr;
    recompui::Image *toast_frame = nullptr;
    recompui::Image *toast_image = nullptr;
    recompui::Label *toast_title = nullptr;
    recompui::Label *toast_description = nullptr;
    recompui::Label *toast_points = nullptr;
    bool toast_initialized = false;
    std::optional<std::size_t> active_toast{};
    std::chrono::steady_clock::time_point toast_started{};
    std::chrono::steady_clock::time_point toast_expires{};
};

UiState g_ui{};
std::mutex g_mutex;
std::array<bool, kAchievements.size()> g_unlocked{};
std::deque<std::size_t> g_toast_queue;
bool g_initialized = false;
bool g_persist_dirty = false;
bool g_was_in_race = false;
unsigned g_jam_count = 0;
std::array<bool, kAchievements.size()> g_badge_loaded{};
std::atomic_bool g_enabled{true};
std::atomic_bool g_hide_toast_requested{false};
std::filesystem::path g_progress_path;

std::filesystem::path progress_path() {
    return recompui::file::get_app_folder_path() / "achievements.txt";
}

std::string badge_resource_name(std::size_t index) {
    return "rr64-achievement-" + std::to_string(kAchievements[index].retro_id) + ".png";
}

float smoothstep(float progress) {
    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    return clamped * clamped * (3.0f - (2.0f * clamped));
}

float lerp(float start, float end, float progress) {
    return start + ((end - start) * progress);
}

float ease_out_back(float progress) {
    // A restrained overshoot gives the tile an Xbox-style settling motion
    // without making the text or badge feel springy.
    constexpr float overshoot = 1.25f;
    constexpr float coefficient = overshoot + 1.0f;
    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    const float shifted = clamped - 1.0f;
    return 1.0f + (coefficient * shifted * shifted * shifted) + (overshoot * shifted * shifted);
}

void apply_toast_animation(std::chrono::steady_clock::time_point now) {
    if (g_ui.toast_container == nullptr || !g_ui.active_toast.has_value()) {
        return;
    }

    const float elapsed_ms =
        std::chrono::duration<float, std::milli>(now - g_ui.toast_started).count();
    const float total_ms = static_cast<float>(kToastDuration.count());
    const float enter_ms = static_cast<float>(kToastEnterDuration.count());
    const float exit_ms = static_cast<float>(kToastExitDuration.count());

    float opacity = 1.0f;
    float translate_x = 0.0f;
    float translate_y = 0.0f;
    float rotation = 0.0f;
    float tile_scale = 1.0f;
    float badge_scale = 1.0f;

    if (elapsed_ms < enter_ms) {
        const float progress = std::clamp(elapsed_ms / enter_ms, 0.0f, 1.0f);
        opacity = smoothstep(std::clamp(progress / 0.16f, 0.0f, 1.0f));
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        if (progress < 0.58f) {
            const float phase = smoothstep(progress / 0.58f);
            translate_x = lerp(34.0f, 0.0f, phase);
            translate_y = lerp(-285.0f, 18.0f, phase);
            rotation = lerp(-11.0f, 2.0f, phase);
            scale_x = lerp(0.92f, 1.06f, phase);
            scale_y = lerp(0.92f, 0.92f, phase);
        } else if (progress < 0.75f) {
            const float phase = smoothstep((progress - 0.58f) / 0.17f);
            translate_y = lerp(18.0f, -12.0f, phase);
            rotation = lerp(2.0f, -1.2f, phase);
            scale_x = lerp(1.06f, 0.985f, phase);
            scale_y = lerp(0.92f, 1.04f, phase);
        } else if (progress < 0.89f) {
            const float phase = smoothstep((progress - 0.75f) / 0.14f);
            translate_y = lerp(-12.0f, 5.0f, phase);
            rotation = lerp(-1.2f, 0.6f, phase);
            scale_x = lerp(0.985f, 1.025f, phase);
            scale_y = lerp(1.04f, 0.975f, phase);
        } else {
            const float phase = smoothstep((progress - 0.89f) / 0.11f);
            translate_y = lerp(5.0f, 0.0f, phase);
            rotation = lerp(0.6f, 0.0f, phase);
            scale_x = lerp(1.025f, 1.0f, phase);
            scale_y = lerp(0.975f, 1.0f, phase);
        }
        g_ui.toast_container->set_scale_2D(scale_x, scale_y);

        const float badge_progress = std::clamp((elapsed_ms - 150.0f) / 590.0f, 0.0f, 1.0f);
        badge_scale = 0.66f + (0.34f * ease_out_back(badge_progress));
    } else if (elapsed_ms > total_ms - exit_ms) {
        const float progress =
            std::clamp((elapsed_ms - (total_ms - exit_ms)) / exit_ms, 0.0f, 1.0f);
        const float fade = smoothstep(progress);
        const float accelerate = progress * progress * progress;
        opacity = 1.0f - fade;
        translate_x = 155.0f * accelerate;
        translate_y = -20.0f * fade;
        rotation = 5.5f * accelerate;
        tile_scale = 1.0f - (0.045f * fade);
        badge_scale = 1.0f - (0.05f * fade);
    }

    g_ui.toast_container->set_translate_2D(translate_x, translate_y, recompui::Unit::Dp);
    if (elapsed_ms >= enter_ms) {
        g_ui.toast_container->set_scale_2D(tile_scale, tile_scale);
    }
    g_ui.toast_container->set_rotation(rotation);
    g_ui.toast_container->set_opacity(opacity);
    if (g_ui.toast_image != nullptr) {
        g_ui.toast_image->set_scale_2D(badge_scale, badge_scale);
        const float badge_rotation =
            elapsed_ms < enter_ms ? -8.0f * (1.0f - std::clamp(elapsed_ms / enter_ms, 0.0f, 1.0f))
                                  : 0.0f;
        g_ui.toast_image->set_rotation(badge_rotation);
    }
}

void queue_badge_resources() {
    {
        const std::filesystem::path frame_path = recompui::file::get_asset_path(kToastFrameAsset);
        std::ifstream frame_file(frame_path, std::ios::binary | std::ios::ate);
        if (frame_file) {
            const std::streamsize frame_size = frame_file.tellg();
            if (frame_size > 0) {
                std::vector<char> frame_bytes(static_cast<std::size_t>(frame_size));
                frame_file.seekg(0, std::ios::beg);
                if (frame_file.read(frame_bytes.data(), frame_size)) {
                    recompui::queue_image_from_bytes_file(kToastFrameResource, frame_bytes);
                }
            }
        }
    }

    for (std::size_t i = 0; i < kAchievements.size(); ++i) {
        const std::string filename = std::to_string(kAchievements[i].retro_id) + ".png";
        const std::string relative_path = "achievements/" + filename;
        const std::filesystem::path badge_path =
            recompui::file::get_asset_path(relative_path.c_str());
        std::ifstream badge_file(badge_path, std::ios::binary | std::ios::ate);
        if (!badge_file) {
            std::fprintf(stderr, "[RR64-ACH] Missing RetroAchievements badge: " RR64_PATH_FORMAT "\n",
                         badge_path.c_str());
            continue;
        }

        const std::streamsize badge_size = badge_file.tellg();
        if (badge_size <= 0) {
            std::fprintf(stderr, "[RR64-ACH] Empty RetroAchievements badge: " RR64_PATH_FORMAT "\n",
                         badge_path.c_str());
            continue;
        }

        std::vector<char> badge_bytes(static_cast<std::size_t>(badge_size));
        badge_file.seekg(0, std::ios::beg);
        if (!badge_file.read(badge_bytes.data(), badge_size)) {
            std::fprintf(stderr, "[RR64-ACH] Could not read RetroAchievements badge: " RR64_PATH_FORMAT "\n",
                         badge_path.c_str());
            continue;
        }

        recompui::queue_image_from_bytes_file(badge_resource_name(i), badge_bytes);
        g_badge_loaded[i] = true;
    }
}

std::size_t find_by_retro_id(std::uint32_t retro_id) {
    for (std::size_t i = 0; i < kAchievements.size(); ++i) {
        if (kAchievements[i].retro_id == retro_id) {
            return i;
        }
    }
    return kAchievements.size();
}

bool save_progress(const std::array<bool, kAchievements.size()> &unlocked) {
    std::error_code ec;
    std::filesystem::create_directories(g_progress_path.parent_path(), ec);
    std::ofstream output(g_progress_path, std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "[RR64-ACH] Could not save achievement progress.\n");
        return false;
    }
    output << "# Road Rash 64 Recompiled local achievements v1\n";
    for (std::size_t i = 0; i < kAchievements.size(); ++i) {
        if (unlocked[i]) {
            output << kAchievements[i].retro_id << '\n';
        }
    }
    output.close();
    return static_cast<bool>(output);
}

void load_progress() {
    std::ifstream input(progress_path());
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        try {
            const auto id = static_cast<std::uint32_t>(std::stoul(line));
            const std::size_t index = find_by_retro_id(id);
            if (index < kAchievements.size()) {
                g_unlocked[index] = true;
            }
        } catch (...) {
            std::fprintf(stderr, "[RR64-ACH] Ignoring malformed progress entry.\n");
        }
    }
}

void queue_unlock(std::size_t index) {
    if (!g_enabled.load(std::memory_order_acquire) || rr64::offline_modifiers::enabled_any() ||
        index >= kAchievements.size()) {
        return;
    }

    std::lock_guard lock(g_mutex);
    if (g_unlocked[index]) {
        return;
    }
    g_unlocked[index] = true;
    g_toast_queue.push_back(index);
    g_persist_dirty = true;
    rr64::achievement_audio::request_guitar_sting();
    if (rr64::diagnostics::routine_enabled()) {
        std::fprintf(stderr, "[RR64-ACH] Unlocked %u: %s (%u points).\n", kAchievements[index].retro_id,
                     kAchievements[index].title, kAchievements[index].points);
    }
}

std::pair<unsigned, unsigned>
progress_totals(const std::array<bool, kAchievements.size()> &unlocked) {
    unsigned unlocked_count = 0;
    unsigned unlocked_points = 0;
    for (std::size_t i = 0; i < kAchievements.size(); ++i) {
        if (unlocked[i]) {
            ++unlocked_count;
            unlocked_points += kAchievements[i].points;
        }
    }
    return {unlocked_count, unlocked_points};
}

void create_achievements_tab(recompui::ContextId context, recompui::Element *parent) {
    // Config pages are destroyed when the Escape menu closes. Build every
    // label from a fresh progress snapshot and never retain pointers to this
    // page; otherwise a later unlock could dereference a destroyed RmlUi node.
    std::array<bool, kAchievements.size()> unlocked{};
    {
        std::lock_guard lock(g_mutex);
        unlocked = g_unlocked;
    }
    const auto [unlocked_count, unlocked_points] = progress_totals(unlocked);

    auto *page = context.create_element<recompui::ConfigPage>(parent);
    auto *left = page->get_body()->get_left();
    auto *right = page->get_body()->get_right();
    right->set_display(recompui::Display::None);
    left->set_display(recompui::Display::Flex);
    left->set_flex_direction(recompui::FlexDirection::Column);
    left->set_width(100.0f, recompui::Unit::Percent);
    left->set_gap(10.0f, recompui::Unit::Dp);

    context.create_element<recompui::Label>(left, "ROAD RASH 64 ACHIEVEMENTS",
                                            recompui::LabelStyle::Large);
    if (!g_enabled.load(std::memory_order_acquire)) {
        auto *disabled = context.create_element<recompui::Label>(
            left,
            "Achievement tracking, popups, and unlock sounds are currently disabled in Gameplay settings.",
            recompui::LabelStyle::Normal);
        disabled->set_color(recompui::Color{238, 137, 35, 255});
    }
    context.create_element<recompui::Label>(left,
                                            std::to_string(unlocked_count) + " / " +
                                                std::to_string(kAchievements.size()) +
                                                " unlocked    " + std::to_string(unlocked_points) +
                                                " / " + std::to_string(kTotalPoints) + " points",
                                            recompui::LabelStyle::Normal);
    auto *attribution = context.create_element<recompui::Label>(
        left,
        "Achievement names and badge artwork from the published RetroAchievements Road Rash 64 Base Set. Progress is stored locally by this recompilation.",
        recompui::LabelStyle::Small);
    attribution->set_color(recompui::theme::color::TextDim);
    attribution->set_margin_bottom(6.0f, recompui::Unit::Dp);

    auto *scroll = context.create_element<recompui::ScrollContainer>(
        left, recompui::ScrollDirection::Vertical);
    scroll->set_display(recompui::Display::Flex);
    scroll->set_flex_direction(recompui::FlexDirection::Column);
    scroll->set_gap(8.0f, recompui::Unit::Dp);
    scroll->set_padding_right(12.0f, recompui::Unit::Dp);

    // Bound initial layout and synchronous badge uploads to one page.
    struct Row {
        recompui::Element *card;
        recompui::Image *badge;
        recompui::Label *status;
        recompui::Label *description;
    };
    auto rows = std::make_shared<std::array<Row, 8>>();
    auto page_index = std::make_shared<std::size_t>(0);
    for (std::size_t i = 0; i < 8; ++i) {
        auto *card = context.create_element<recompui::Element>(scroll);
        card->set_display(recompui::Display::Flex);
        card->set_flex_direction(recompui::FlexDirection::Row);
        card->set_align_items(recompui::AlignItems::Center);
        card->set_flex_shrink(0.0f);
        card->set_gap(13.0f, recompui::Unit::Dp);
        card->set_padding(10.0f, recompui::Unit::Dp);
        card->set_background_color(recompui::theme::color::BGOverlay);
        card->set_border_left_width(4.0f, recompui::Unit::Dp);
        card->set_border_left_color(unlocked[i] ? recompui::Color{107, 187, 58, 255}
                                                : recompui::Color{96, 96, 96, 255});
        card->set_border_radius(recompui::theme::border::radius_sm, recompui::Unit::Dp);

        recompui::Image *badge = nullptr;
        if (g_badge_loaded[i]) {
            badge = context.create_element<recompui::Image>(card, badge_resource_name(i));
            badge->set_display(g_badge_loaded[i] ? recompui::Display::Block
                                                 : recompui::Display::None);
            badge->set_width(76.0f, recompui::Unit::Dp);
            badge->set_height(76.0f, recompui::Unit::Dp);
            badge->set_flex_shrink(0.0f);
            badge->set_opacity(unlocked[i] ? 1.0f : 0.38f);
            badge->set_border_radius(recompui::theme::border::radius_sm, recompui::Unit::Dp);
        }

        auto *copy = context.create_element<recompui::Element>(card);
        copy->set_display(recompui::Display::Flex);
        copy->set_flex_direction(recompui::FlexDirection::Column);
        copy->set_flex_grow(1.0f);
        copy->set_min_width(0.0f, recompui::Unit::Dp);
        copy->set_gap(3.0f, recompui::Unit::Dp);

        const std::string state = unlocked[i] ? "UNLOCKED  " : "LOCKED  ";
        auto *status = context.create_element<recompui::Label>(
            copy,
            state + std::string(kAchievements[i].title) + "  -  " +
                std::to_string(kAchievements[i].points) + "G",
            recompui::LabelStyle::Normal);
        status->set_color(unlocked[i] ? recompui::Color{126, 211, 66, 255}
                                      : recompui::Color{164, 164, 164, 255});
        auto *description = context.create_element<recompui::Label>(
            copy, kAchievements[i].description, recompui::LabelStyle::Small);
        description->set_color(recompui::theme::color::TextDim);
        (*rows)[i] = {card, badge, status, description};
    }
    auto *navigation = context.create_element<recompui::Element>(left);
    navigation->set_display(recompui::Display::Flex);
    navigation->set_gap(16.0f, recompui::Unit::Dp);
    navigation->set_align_items(recompui::AlignItems::Center);
    auto *previous = context.create_element<recompui::Button>(
        navigation, "Previous", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    auto *page_label =
        context.create_element<recompui::Label>(navigation, "1 / 7", recompui::LabelStyle::Normal);
    auto *next = context.create_element<recompui::Button>(
        navigation, "Next", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    // These pointers belong exclusively to the page; its buttons own the callbacks.
    // Switching tabs destroys the callbacks and rows together, with no globals.
    auto refresh = [rows, page_index, page_label, unlocked]() {
        page_label->set_text(std::to_string(*page_index + 1) + " / 7");
        for (std::size_t slot = 0; slot < rows->size(); ++slot) {
            auto &row = (*rows)[slot];
            const auto i = *page_index * rows->size() + slot;
            row.card->set_display(i < kAchievements.size() ? recompui::Display::Flex
                                                           : recompui::Display::None);
            if (i >= kAchievements.size()) {
                continue;
            }
            row.card->set_border_left_color(unlocked[i] ? recompui::Color{107, 187, 58, 255}
                                                        : recompui::Color{96, 96, 96, 255});
            if (row.badge) {
                row.badge->set_display(g_badge_loaded[i] ? recompui::Display::Block
                                                         : recompui::Display::None);
                row.badge->set_src(badge_resource_name(i));
                row.badge->set_opacity(unlocked[i] ? 1.0f : 0.38f);
            }
            row.status->set_text(std::string(unlocked[i] ? "UNLOCKED  " : "LOCKED  ") +
                                 kAchievements[i].title + "  -  " +
                                 std::to_string(kAchievements[i].points) + "G");
            row.status->set_color(unlocked[i] ? recompui::Color{126, 211, 66, 255}
                                              : recompui::Color{164, 164, 164, 255});
            row.description->set_text(kAchievements[i].description);
        }
    };
    previous->add_pressed_callback([page_index, refresh]() {
        *page_index = (*page_index + 6) % 7;
        refresh();
    });
    next->add_pressed_callback([page_index, refresh]() {
        *page_index = (*page_index + 1) % 7;
        refresh();
    });
}

} // namespace

void initialize() {
    std::lock_guard lock(g_mutex);
    if (g_initialized) {
        return;
    }
    load_progress();
    g_progress_path = progress_path(); // Resolve frontend paths on its owning thread.
    g_initialized = true;
}

void flush_progress() {
    std::array<bool, kAchievements.size()> unlocked{};
    {
        std::lock_guard lock(g_mutex);
        if (!g_initialized || !g_persist_dirty) return;
        unlocked = g_unlocked;
        g_persist_dirty = false;
    }
    // No game/UI lock is held during filesystem work. An unlock arriving
    // during this write sets dirty again, preserving the newer snapshot.
    if (!save_progress(unlocked)) {
        std::lock_guard lock(g_mutex);
        g_persist_dirty = true; // Retry on the next worker tick, not every UI frame.
    }
}

void set_enabled(bool enabled_value) {
    g_enabled.store(enabled_value, std::memory_order_release);
    if (enabled_value) {
        return;
    }

    {
        std::lock_guard lock(g_mutex);
        g_toast_queue.clear();
        g_jam_count = 0;
    }
    g_hide_toast_requested.store(true, std::memory_order_release);
    rr64::achievement_audio::cancel_guitar_sting();
}

bool enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void register_config_tab() {
    recompui::config::create_tab("Achievements", "rr64_achievements",
                                 [](recompui::ContextId context, recompui::Element *parent) {
                                     create_achievements_tab(context, parent);
                                 });
}

void initialize_toast_ui() {
    if (g_ui.toast_initialized) {
        return;
    }

    queue_badge_resources();

    recompui::ContextId previous_context = recompui::try_close_current_context();
    g_ui.toast_context = recompui::create_context();
    g_ui.toast_context.open();
    g_ui.toast_context.set_captures_input(false);
    g_ui.toast_context.set_captures_mouse(false);

    auto *root =
        g_ui.toast_context.create_element<recompui::Element>(g_ui.toast_context.get_root_element());
    root->set_display(recompui::Display::Flex);
    root->set_position(recompui::Position::Absolute);
    root->set_top(0);
    root->set_right(0);
    root->set_bottom(0);
    root->set_left(0);
    root->set_align_items(recompui::AlignItems::FlexStart);
    root->set_justify_content(recompui::JustifyContent::FlexEnd);
    root->set_padding_top(38.0f, recompui::Unit::Dp);
    root->set_padding_right(34.0f, recompui::Unit::Dp);

    auto *toast = g_ui.toast_context.create_element<recompui::Element>(root);
    g_ui.toast_container = toast;
    toast->set_display(recompui::Display::Flex);
    toast->set_flex_direction(recompui::FlexDirection::Column);
    toast->set_position(recompui::Position::Relative);
    toast->set_width(660.0f, recompui::Unit::Dp);
    toast->set_height(220.0f, recompui::Unit::Dp);
    toast->set_max_width(92.0f, recompui::Unit::Percent);
    toast->set_background_color(recompui::Color{0, 0, 0, 0});
    toast->set_overflow(recompui::Overflow::Visible);
    toast->set_opacity(0.0f);
    toast->set_translate_2D(34.0f, -285.0f, recompui::Unit::Dp);
    toast->set_scale_2D(0.92f, 0.92f);
    toast->set_rotation(-11.0f);

    g_ui.toast_frame =
        g_ui.toast_context.create_element<recompui::Image>(toast, kToastFrameResource);
    g_ui.toast_frame->set_position(recompui::Position::Absolute);
    g_ui.toast_frame->set_inset(0.0f, recompui::Unit::Dp);
    g_ui.toast_frame->set_width(100.0f, recompui::Unit::Percent);
    g_ui.toast_frame->set_height(100.0f, recompui::Unit::Percent);

    auto *toast_content = g_ui.toast_context.create_element<recompui::Element>(toast);
    toast_content->set_display(recompui::Display::Flex);
    toast_content->set_position(recompui::Position::Relative);
    toast_content->set_flex_direction(recompui::FlexDirection::Row);
    toast_content->set_align_items(recompui::AlignItems::Center);
    toast_content->set_width(100.0f, recompui::Unit::Percent);
    toast_content->set_height(100.0f, recompui::Unit::Percent);
    toast_content->set_padding_left(55.0f, recompui::Unit::Dp);
    toast_content->set_padding_right(62.0f, recompui::Unit::Dp);
    toast_content->set_padding_top(50.0f, recompui::Unit::Dp);
    toast_content->set_padding_bottom(35.0f, recompui::Unit::Dp);
    toast_content->set_gap(17.0f, recompui::Unit::Dp);

    g_ui.toast_image =
        g_ui.toast_context.create_element<recompui::Image>(toast_content, badge_resource_name(0));
    g_ui.toast_image->set_width(100.0f, recompui::Unit::Dp);
    g_ui.toast_image->set_height(100.0f, recompui::Unit::Dp);
    g_ui.toast_image->set_flex_shrink(0.0f);
    g_ui.toast_image->set_background_color(recompui::Color{8, 7, 6, 255});
    g_ui.toast_image->set_border_width(4.0f, recompui::Unit::Dp);
    g_ui.toast_image->set_border_left_color(recompui::Color{212, 207, 190, 255});
    g_ui.toast_image->set_border_top_color(recompui::Color{117, 105, 87, 255});
    g_ui.toast_image->set_border_right_color(recompui::Color{56, 47, 39, 255});
    g_ui.toast_image->set_border_bottom_color(recompui::Color{148, 39, 21, 255});
    g_ui.toast_image->set_border_radius(1.0f, recompui::Unit::Dp);

    auto *toast_copy = g_ui.toast_context.create_element<recompui::Element>(toast_content);
    toast_copy->set_display(recompui::Display::Flex);
    toast_copy->set_flex_direction(recompui::FlexDirection::Column);
    toast_copy->set_flex_grow(1.0f);
    toast_copy->set_min_width(0.0f, recompui::Unit::Dp);
    toast_copy->set_align_items(recompui::AlignItems::FlexStart);
    toast_copy->set_gap(3.0f, recompui::Unit::Dp);

    auto *kicker = g_ui.toast_context.create_element<recompui::Label>(
        toast_copy, "ROAD RASH 64  //  ACHIEVEMENT WRECKED", recompui::LabelStyle::Small);
    kicker->set_color(recompui::Color{255, 173, 28, 255});
    kicker->set_font_weight(800);
    kicker->set_letter_spacing(1.1f, recompui::Unit::Dp);
    g_ui.toast_title = g_ui.toast_context.create_element<recompui::Label>(
        toast_copy, "", recompui::LabelStyle::Large);
    g_ui.toast_title->set_color(recompui::Color{246, 243, 228, 255});
    g_ui.toast_title->set_font_weight(800);
    g_ui.toast_description = g_ui.toast_context.create_element<recompui::Label>(
        toast_copy, "", recompui::LabelStyle::Small);
    g_ui.toast_description->set_color(recompui::Color{204, 195, 174, 255});
    g_ui.toast_points = g_ui.toast_context.create_element<recompui::Label>(
        toast_copy, "", recompui::LabelStyle::Normal);
    g_ui.toast_points->set_color(recompui::Color{255, 214, 80, 255});
    g_ui.toast_points->set_font_weight(800);
    g_ui.toast_points->set_background_color(recompui::Color{103, 25, 16, 238});
    g_ui.toast_points->set_border_width(1.0f, recompui::Unit::Dp);
    g_ui.toast_points->set_border_color(recompui::Color{223, 103, 29, 255});
    g_ui.toast_points->set_padding_left(8.0f, recompui::Unit::Dp);
    g_ui.toast_points->set_padding_right(8.0f, recompui::Unit::Dp);
    g_ui.toast_points->set_padding_top(2.0f, recompui::Unit::Dp);
    g_ui.toast_points->set_padding_bottom(2.0f, recompui::Unit::Dp);
    g_ui.toast_points->set_rotation(-1.2f);

    g_ui.toast_context.close();
    g_ui.toast_initialized = true;
    if (previous_context != recompui::ContextId::null()) {
        previous_context.open();
    }
}

void update_ui() {
    if (g_hide_toast_requested.exchange(false, std::memory_order_acq_rel) &&
        g_ui.toast_initialized) {
        recompui::ContextId previous_context = recompui::try_close_current_context();
        g_ui.toast_context.open();
        if (recompui::is_context_shown(g_ui.toast_context)) {
            recompui::hide_context(g_ui.toast_context);
        }
        g_ui.toast_context.close();
        if (previous_context != recompui::ContextId::null()) {
            previous_context.open();
        }
        g_ui.active_toast.reset();
    }
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }

    std::optional<std::size_t> next_toast{};
    {
        std::lock_guard lock(g_mutex);
        if (!g_ui.active_toast.has_value() && !g_toast_queue.empty()) {
            next_toast = g_toast_queue.front();
            g_toast_queue.pop_front();
        }
    }

    if (!g_ui.toast_initialized) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool toast_expired = g_ui.active_toast.has_value() && now >= g_ui.toast_expires;
    if (toast_expired) {
        recompui::ContextId previous_context = recompui::try_close_current_context();
        g_ui.toast_context.open();
        if (recompui::is_context_shown(g_ui.toast_context)) {
            recompui::hide_context(g_ui.toast_context);
        }
        g_ui.toast_context.close();
        if (previous_context != recompui::ContextId::null()) {
            previous_context.open();
        }
        g_ui.active_toast.reset();
        std::lock_guard lock(g_mutex);
        if (!g_toast_queue.empty()) {
            next_toast = g_toast_queue.front();
            g_toast_queue.pop_front();
        }
    }

    if (g_ui.active_toast.has_value() && !toast_expired) {
        recompui::ContextId previous_context = recompui::try_close_current_context();
        g_ui.toast_context.open();
        apply_toast_animation(now);
        g_ui.toast_context.close();
        if (previous_context != recompui::ContextId::null()) {
            previous_context.open();
        }
    }

    if (!next_toast.has_value()) {
        return;
    }

    const std::size_t index = *next_toast;
    recompui::ContextId previous_context = recompui::try_close_current_context();
    g_ui.toast_context.open();
    if (g_ui.toast_image != nullptr && g_badge_loaded[index]) {
        g_ui.toast_image->set_src(badge_resource_name(index));
    }
    g_ui.toast_title->set_text(kAchievements[index].title);
    g_ui.toast_description->set_text(kAchievements[index].description);
    g_ui.toast_points->set_text(std::to_string(kAchievements[index].points) + " PTS");
    g_ui.active_toast = index;
    g_ui.toast_started = now;
    g_ui.toast_expires = now + kToastDuration;
    apply_toast_animation(now);
    if (!recompui::is_context_shown(g_ui.toast_context)) {
        recompui::show_context(g_ui.toast_context, "");
    }
    g_ui.toast_context.close();
    if (previous_context != recompui::ContextId::null()) {
        previous_context.open();
    }
}

void observe_frame(bool in_race) {
    std::lock_guard lock(g_mutex);
    if (in_race && !g_was_in_race) {
        g_jam_count = 0;
    }
    if (!in_race && g_was_in_race) {
        g_jam_count = 0;
    }
    g_was_in_race = in_race;
}

void handle_game_event(std::uint32_t event_id) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    // func_800636B0 is the game's own local notification dispatcher. These
    // event IDs are therefore safer than deriving achievements from rendered
    // text, and do not alter any game state.
    switch (event_id) {
    case 22: // "Busted by ..."
        queue_unlock(38);
        break;
    case 23: // "Splat!" pedestrian event
        queue_unlock(10);
        break;
    case 25: { // "Jammed ..."
        bool unlock = false;
        {
            std::lock_guard lock(g_mutex);
            if (g_was_in_race) {
                unlock = ++g_jam_count >= 4;
            }
        }
        if (unlock) {
            queue_unlock(17);
        }
        break;
    }
    case 39: // "Stole weapon from ..."
        queue_unlock(36);
        break;
    default:
        break;
    }
}

void handle_campaign_level_advanced(std::uint32_t new_level) {
    const std::size_t index = logic::campaign_level_index(new_level);
    if (index != logic::kInvalidAchievement) {
        queue_unlock(index);
    }
}

void handle_campaign_completed() {
    queue_unlock(logic::kCampaignCompletionIndex);
}

} // namespace rr64::achievements

extern "C" void rr64_achievement_observe_frame(unsigned char *) {
    rr64::achievements::observe_frame(rr64_is_race_mode_active() != 0);
}

extern "C" void rr64_achievement_game_event(unsigned char *, unsigned int event_id, unsigned int,
                                            unsigned int) {
    if(rr64::prediction::active())return;
    rr64::achievements::handle_game_event(event_id);
}

extern "C" void rr64_achievement_campaign_level_advanced(unsigned char *, unsigned int new_level) {
    rr64::achievements::handle_campaign_level_advanced(new_level);
}

extern "C" void rr64_achievement_campaign_completed(unsigned char *) {
    rr64::achievements::handle_campaign_completed();
}
