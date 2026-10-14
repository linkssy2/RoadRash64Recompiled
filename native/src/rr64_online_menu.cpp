#include "rr64_popup_input.hpp"
#include "rr64_online_menu.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_native.hpp"
#include "rr64_local_race_options.hpp"
#include "rr64_local_players.hpp"
#include "recompinput/players.h"
#include "recompinput/input_state.h"

#include <array>
#include <bit>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>

#include "recomp.h"
#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_button.h"
#include "elements/ui_document.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_text_input.h"
#include "elements/ui_theme.h"

#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "rr64_connection_address.hpp"

namespace rr64::online_menu {
namespace {

void online_log(const char *format, ...) {
    if (!rr64::diagnostics::network_detail_enabled()) { return; }
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

enum class Page {
    Connect,
    Lobby,
};

struct UiState {
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element *connect_page = nullptr;
    recompui::Element *lobby_page = nullptr;
    recompui::TextInput *player_name = nullptr;
    recompui::TextInput *host_address = nullptr;
    recompui::TextInput *port = nullptr;
    recompui::Label *lobby_status = nullptr;
    recompui::Label *course_error = nullptr;
    std::array<recompui::Label *, netplay::kMaximumPlayers> player_labels{};
    recompui::Button *host_button = nullptr;
    recompui::Button *ready_button = nullptr;
    recompui::Button *continue_button = nullptr;
    recompui::Button *disconnect_button = nullptr;
    Page page = Page::Connect;
    std::optional<Page> pending_page{};
    recompui::Element *pending_focus = nullptr;
    unsigned focus_delay_frames = 0;
    bool initialized = false;
    bool online_entry_queued = false;
    bool session_active = false;
    bool course_blocked = false;
};

UiState g_ui{};
unsigned g_player_limit = netplay::kMaximumPlayers;
std::atomic_bool g_show_requested = false;
std::atomic_bool g_local_start_requested = false;
std::atomic_bool g_reset_local_requested = false;
std::atomic_bool g_force_multiplayer_transition = false;
std::atomic_bool g_allow_original_multiplayer = false;
std::atomic_bool g_reset_session_requested = false;
std::atomic_bool g_guest_character_select_active = false;
std::atomic_uint32_t g_applied_game_setup_revision = 0;
std::atomic_bool g_return_to_lobby_requested = false;
// Both route_mode and the immediately following native mode setter run on the
// game thread. Do not expose the local-start request to the UI until that
// setter has finished retiring the previous main-menu/local gameplay state.
bool g_local_start_pending_mode = false;

bool return_to_lobby(unsigned char *rdram) {
    if(!rdram || !g_return_to_lobby_requested.exchange(false,std::memory_order_acq_rel)) return false;
    engine::write_u32(rdram,engine::globals::pending_mode,0x20u);
    engine::write_u32(rdram,engine::globals::multiplayer_stage,0u);
    engine::write_u32(rdram,0x8009E238u,0u);
    g_force_multiplayer_transition.store(false,std::memory_order_release);
    return true;
}

bool has_course_problem(const netplay::Status& status) {
    if(!status.game_setup.valid || status.phase<netplay::Phase::CharacterSelect ||
       status.phase>=netplay::Phase::Race) return false;
    for(const auto& player:status.players)
        if(player.connected && player.course_compatibility>=race_pack::Compatibility::Missing) return true;
    return false;
}

static_assert(engine::globals::multiplayer_game_setup_words.size() == netplay::kGameSetupWordCount);

void reset_guest_setup_progress() {
    local_race_options::reset_online();
    g_guest_character_select_active.store(false, std::memory_order_release);
    g_applied_game_setup_revision.store(0, std::memory_order_release);
}

std::uint32_t guest_multiplayer_stage(unsigned char *rdram) {
    return static_cast<std::uint32_t>(
        MEM_W(0, engine::guest_address(engine::globals::multiplayer_stage)));
}

netplay::GameSetupState capture_game_setup(unsigned char *rdram) {
    const auto status = netplay::get_status();
    if (status.replicated_riders) {
        // One physical controller per machine; remote slots live in the
        // fourteen-actor pool, not in four-entry menu/camera arrays.
        engine::write_u32(rdram, 0x8009EF5Cu, 1u);
        engine::write_u32(rdram, 0x800A6578u, 1u);
        std::uint32_t count = 0;
        engine::read_u32(rdram, 0x800A6574u, count);
        engine::write_u32(
            rdram, 0x800A6574u,
            std::clamp<unsigned>(std::max<unsigned>(count, status.connected_players), 1u, 14u));
    }
    netplay::GameSetupState setup{};
    setup.valid = true;
    setup.course = race_pack::selected_identity();
    setup.race_options=local_race_options::online_options();
    engine::read_u32(rdram,engine::globals::random_state,setup.random_seed);
    for (std::size_t i = 0; i < engine::globals::multiplayer_game_setup_words.size(); ++i) {
        setup.words[i] = static_cast<std::uint32_t>(
            MEM_W(0, engine::guest_address(engine::globals::multiplayer_game_setup_words[i])));
    }
    std::uint16_t buttons = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    netplay::get_player_input(0, buttons, stick_x, stick_y);
    setup.transition_buttons = buttons;
    return setup;
}

void apply_game_setup(unsigned char *rdram, const netplay::GameSetupState &setup) {
    if (!setup.valid || setup.revision == 0) return;
    // Content is checked before the revision shortcut: a previous ACK must
    // never survive a local availability failure. No native settings/load are
    // applied until the exact identity is accepted.
    const auto compatible=race_pack::compatibility(setup.course);
    if(compatible!=race_pack::Compatibility::Ready) {
        netplay::report_course_compatibility(setup.revision,setup.course,compatible);
        return;
    }
    if(setup.revision<=g_applied_game_setup_revision.load(std::memory_order_acquire)) {
        netplay::acknowledge_course(setup.revision,setup.course);
        return;
    }
    if(!race_pack::apply_identity(setup.course)) {
        netplay::report_course_compatibility(setup.revision,setup.course,race_pack::Compatibility::Different);
        return;
    }
    if(!netplay::acknowledge_course(setup.revision,setup.course))return;
    for (std::size_t i = 0; i < engine::globals::multiplayer_game_setup_words.size(); ++i) {
        MEM_W(0, engine::guest_address(engine::globals::multiplayer_game_setup_words[i])) =
            setup.words[i];
    }
    local_race_options::apply_online_options(setup.race_options);
    g_applied_game_setup_revision.store(setup.revision, std::memory_order_release);
    online_log("[RR64-ONLINE] Applied host game/race settings revision %u.\n", setup.revision);
}

void set_page(Page page) {
    if (recompui::Element *focused = g_ui.context.get_focused_element(); focused != nullptr) {
        // Never leave focus on a page that is about to be hidden. RecompUI's
        // directional tree contains visible elements only, so navigating from
        // a stale hidden focus can terminate its navigation pass.
        focused->blur();
    }
    g_ui.page = page;
    if (g_ui.connect_page != nullptr) {
        g_ui.connect_page->set_display(page == Page::Connect ? recompui::Display::Flex
                                                             : recompui::Display::None);
    }
    if (g_ui.lobby_page != nullptr) {
        g_ui.lobby_page->set_display(page == Page::Lobby ? recompui::Display::Flex
                                                         : recompui::Display::None);
    }

    recompui::Button *focus_button = nullptr;
    switch (page) {
    case Page::Connect:
        focus_button = g_ui.host_button;
        break;
    case Page::Lobby:
        focus_button = g_ui.ready_button;
        break;
    }
    if (focus_button != nullptr) {
        g_ui.context.set_autofocus_element(focus_button);
        // Page visibility is finalized by RmlUi at the end of the frame. Wait
        // one complete frame before focusing the newly visible button.
        g_ui.pending_focus = focus_button;
        g_ui.focus_delay_frames = 1;
    }
}

void request_page(Page page) {
    g_ui.pending_page = page;
}

void apply_pending_page() {
    if (!g_ui.pending_page.has_value()) {
        return;
    }
    const Page page = *g_ui.pending_page;
    g_ui.pending_page.reset();
    set_page(page);
    online_log("[RR64-ONLINE] Changed overlay page to %u.\n", static_cast<unsigned>(page));
}

void restore_page_focus_if_ready() {
    if (g_ui.pending_focus == nullptr || !recompui::is_context_shown(g_ui.context)) {
        return;
    }
    if (g_ui.focus_delay_frames > 0) {
        --g_ui.focus_delay_frames;
        return;
    }
    if (g_ui.pending_focus->focus()) {
        g_ui.pending_focus = nullptr;
    }
}

recompui::Element *create_page(recompui::ContextId context, recompui::Element *parent) {
    auto *page = context.create_element<recompui::Element>(parent);
    page->set_display(recompui::Display::Flex);
    page->set_flex_direction(recompui::FlexDirection::Column);
    page->set_gap(14.0f, recompui::Unit::Dp);
    page->set_as_navigation_container(recompui::NavigationType::Vertical);
    page->set_nav_wrapping(true);
    return page;
}

std::uint16_t selected_port() {
    std::uint32_t parsed = netplay::kDefaultPort;
    if (g_ui.port != nullptr) {
        const std::string text = netplay::trim_address(g_ui.port->get_text());
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            return 0;
        }
    }
    if (parsed == 0 || parsed > 65535) {
        return 0;
    }
    return static_cast<std::uint16_t>(parsed);
}

std::string selected_name() {
    std::string name = g_ui.player_name != nullptr ? g_ui.player_name->get_text() : "Rider";
    if (name.empty()) {
        name = "Rider";
    }
    if (name.size() > 23) {
        name.resize(23);
    }
    return name;
}

void begin_session(netplay::Mode mode) {
    netplay::Config config{};
    config.mode = mode;
    config.maximum_players = static_cast<std::uint8_t>(g_player_limit);
    config.player_name = selected_name();
    config.port = selected_port();
    if (mode == netplay::Mode::Join) {
        config.host_address = g_ui.host_address != nullptr ? g_ui.host_address->get_text() : "";
    }
    reset_guest_setup_progress();
    netplay::configure(config);
    g_ui.online_entry_queued = false;
    request_page(Page::Lobby);
    const netplay::Status status = netplay::get_status();
    online_log("[RR64-ONLINE] %s requested: active=%u connected=%u phase=%u message=%s\n",
               mode == netplay::Mode::Host ? "Host" : "Join", status.active ? 1u : 0u,
               status.connected ? 1u : 0u, static_cast<unsigned>(status.phase),
               status.message.c_str());
}

void queue_original_multiplayer() {
    g_allow_original_multiplayer.store(false, std::memory_order_release);
    g_force_multiplayer_transition.store(true, std::memory_order_release);
}

const char *phase_name(netplay::Phase phase) {
    switch (phase) {
    case netplay::Phase::Connecting:
        return "CONNECTING";
    case netplay::Phase::Lobby:
        return "LOBBY";
    case netplay::Phase::GameSetup:
        return "HOST GAME SETUP";
    case netplay::Phase::CharacterSelect:
        return "CHARACTER SELECT";
    case netplay::Phase::TrackSelect:
        return "TRACK SELECT";
    case netplay::Phase::Race:
        return "RACE";
    case netplay::Phase::Offline:
    default:
        return "OFFLINE";
    }
}

} // namespace

void initialize_ui() {
    if (g_ui.initialized) {
        return;
    }

    // Launcher callbacks execute with the launcher context open. Temporarily
    // close it while constructing this independent in-game context, then put
    // the caller's context back exactly as RecompFrontend expects.
    recompui::ContextId previous_context = recompui::try_close_current_context();
    g_ui.context = recompui::create_context();
    g_ui.context.open();
    g_ui.context.set_captures_input(true);
    g_ui.context.set_captures_mouse(true);

    auto *root = g_ui.context.create_element<recompui::Element>(g_ui.context.get_root_element());
    root->set_display(recompui::Display::Flex);
    root->set_position(recompui::Position::Absolute);
    root->set_top(0);
    root->set_right(0);
    root->set_bottom(0);
    root->set_left(0);
    root->set_align_items(recompui::AlignItems::Center);
    root->set_justify_content(recompui::JustifyContent::Center);
    root->set_background_color(recompui::Color{0, 0, 0, 205});

    auto *panel = g_ui.context.create_element<recompui::Element>(root);
    panel->set_display(recompui::Display::Flex);
    panel->set_flex_direction(recompui::FlexDirection::Column);
    panel->set_width(760.0f, recompui::Unit::Dp);
    panel->set_max_width(92.0f, recompui::Unit::Percent);
    panel->set_padding(30.0f, recompui::Unit::Dp);
    panel->set_gap(18.0f, recompui::Unit::Dp);
    panel->set_background_color(recompui::theme::color::ModalOverlay);
    panel->set_border_width(recompui::theme::border::width, recompui::Unit::Dp);
    panel->set_border_color(recompui::theme::color::Primary);
    panel->set_border_radius(recompui::theme::border::radius_lg, recompui::Unit::Dp);

    g_ui.context.create_element<recompui::Label>(panel, "ROAD RASH 64 ONLINE",
                                                 recompui::LabelStyle::Large);
    g_ui.context.create_element<recompui::Label>(
        panel, "Direct host/client multiplayer - up to fourteen riders",
        recompui::LabelStyle::Small);

    g_ui.connect_page = create_page(g_ui.context, panel);
    g_ui.context.create_element<recompui::Label>(g_ui.connect_page, "ONLINE - HOST / JOIN",
                                                 recompui::LabelStyle::Normal);
    g_ui.context.create_element<recompui::Label>(g_ui.connect_page, "Player name",
                                                 recompui::LabelStyle::Small);
    g_ui.player_name = g_ui.context.create_element<recompui::TextInput>(g_ui.connect_page, true);
    g_ui.player_name->set_text("Rider");
    g_ui.context.create_element<recompui::Label>(
        g_ui.connect_page, "Host IPv4 address or hostname (address:port also works)",
        recompui::LabelStyle::Small);
    g_ui.host_address = g_ui.context.create_element<recompui::TextInput>(g_ui.connect_page, true);
    g_ui.host_address->set_text("");
    g_ui.context.create_element<recompui::Label>(g_ui.connect_page, "UDP port",
                                                 recompui::LabelStyle::Small);
    g_ui.port = g_ui.context.create_element<recompui::TextInput>(g_ui.connect_page, true);
    g_ui.port->set_text(std::to_string(netplay::kDefaultPort));
    g_ui.context.create_element<recompui::Label>(
        g_ui.connect_page,
        "Internet: join the host's public IPv4 address. Same network: use the host's local IPv4 address. Host forwards UDP to this PC and allows this build through Windows Firewall.",
        recompui::LabelStyle::Small);
    auto *player_limit = g_ui.context.create_element<recompui::Button>(
        g_ui.connect_page, "PLAYER LIMIT: 14", recompui::ButtonStyle::Secondary,
        recompui::ButtonSize::Medium);
    player_limit->add_pressed_callback([player_limit]() {
        g_player_limit = g_player_limit == netplay::kMaximumPlayers ? 2u : g_player_limit + 1u;
        player_limit->set_text("PLAYER LIMIT: " + std::to_string(g_player_limit));
    });
    g_ui.host_button = g_ui.context.create_element<recompui::Button>(
        g_ui.connect_page, "HOST", recompui::ButtonStyle::Success, recompui::ButtonSize::Large);
    g_ui.host_button->add_pressed_callback([]() { begin_session(netplay::Mode::Host); });
    auto *join = g_ui.context.create_element<recompui::Button>(
        g_ui.connect_page, "JOIN", recompui::ButtonStyle::Primary, recompui::ButtonSize::Large);
    join->add_pressed_callback([]() { begin_session(netplay::Mode::Join); });
    auto *connect_back = g_ui.context.create_element<recompui::Button>(
        g_ui.connect_page, "BACK", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Medium);
    connect_back->add_pressed_callback([]() {
        rr64::popup_input::closed();
        recompui::hide_context(g_ui.context);
    });

    g_ui.lobby_page = create_page(g_ui.context, panel);
    g_ui.context.create_element<recompui::Label>(g_ui.lobby_page, "ONLINE LOBBY",
                                                 recompui::LabelStyle::Normal);
    g_ui.lobby_status = g_ui.context.create_element<recompui::Label>(
        g_ui.lobby_page, "Connecting...", recompui::LabelStyle::Small);
    g_ui.course_error = g_ui.context.create_element<recompui::Label>(
        g_ui.lobby_page,"",recompui::LabelStyle::Small);
    g_ui.course_error->set_display(recompui::Display::None);
    auto *rider_grid = g_ui.context.create_element<recompui::Element>(g_ui.lobby_page);
    rider_grid->set_display(recompui::Display::Flex);
    rider_grid->set_flex_direction(recompui::FlexDirection::Row);
    rider_grid->set_gap(24.0f, recompui::Unit::Dp);
    auto rider_columns = std::array<recompui::Element *, 2>{};
    for (auto *&column : rider_columns) {
        column = g_ui.context.create_element<recompui::Element>(rider_grid);
        column->set_display(recompui::Display::Flex);
        column->set_flex_direction(recompui::FlexDirection::Column);
        column->set_gap(5.0f, recompui::Unit::Dp);
        column->set_flex_grow(1.0f);
        column->set_flex_basis(50.0f, recompui::Unit::Percent);
    }
    for (std::uint8_t slot = 0; slot < netplay::kMaximumPlayers; ++slot) {
        g_ui.player_labels[slot] = g_ui.context.create_element<recompui::Label>(
            rider_columns[slot / 7], "EMPTY", recompui::LabelStyle::Small);
    }
    g_ui.ready_button = g_ui.context.create_element<recompui::Button>(
        g_ui.lobby_page, "READY", recompui::ButtonStyle::Primary, recompui::ButtonSize::Large);
    g_ui.ready_button->add_pressed_callback([]() {
        const netplay::Status status = netplay::get_status();
        if (status.local_slot < netplay::kMaximumPlayers) {
            netplay::set_ready(!status.players[status.local_slot].ready);
        }
    });
    g_ui.continue_button = g_ui.context.create_element<recompui::Button>(
        g_ui.lobby_page, "BEGIN GAME SETUP", recompui::ButtonStyle::Success,
        recompui::ButtonSize::Large);
    g_ui.continue_button->add_pressed_callback([]() {
        const auto status=netplay::get_status();
        if(status.is_host && has_course_problem(status)) {
            // Keep the session and every peer. This is an explicit return to
            // settings, not an automatic substitution of a stock carrier.
            netplay::host_set_phase(netplay::Phase::Lobby);
            return;
        }
        if (netplay::all_connected_players_ready() &&
            netplay::host_set_phase(netplay::Phase::GameSetup)) {
            reset_guest_setup_progress();
            g_ui.online_entry_queued = true;
            queue_original_multiplayer();
            recompui::hide_context(g_ui.context);
        }
    });
    g_ui.disconnect_button = g_ui.context.create_element<recompui::Button>(
        g_ui.lobby_page, "LEAVE SESSION", recompui::ButtonStyle::Danger,
        recompui::ButtonSize::Medium);
    g_ui.disconnect_button->add_pressed_callback([]() {
        netplay::shutdown();
        reset_guest_setup_progress();
        g_ui.online_entry_queued = false;
        request_page(Page::Connect);
    });

    set_page(Page::Connect);
    g_ui.context.close();
    g_ui.initialized = true;
    online_log("[RR64-ONLINE] In-game multiplayer UI initialized.\n");
    if (previous_context != recompui::ContextId::null()) {
        previous_context.open();
    }
}

void update_ui() {
    const int coop_input=campaign_coop::input_request.exchange(-1);
    if (coop_input>=0) recompinput::players::set_single_player_mode(coop_input==0);
    if (!g_ui.initialized) {
        return;
    }

    // Element setters must run with their owning context open. The launcher
    // update callback normally has the launcher context open, so switch to the
    // online context for this update and restore the launcher before returning.
    recompui::ContextId previous_context = recompui::try_close_current_context();
    g_ui.context.open();

    // Button callbacks are processed after the update callback that owns this
    // function. Applying their page request on the following frame guarantees
    // that no controller event sees a half-hidden navigation tree.
    apply_pending_page();
    const auto complete_local_reset = []() {
        if (g_reset_local_requested.exchange(false, std::memory_order_acq_rel)) {
            local_players::active.store(false, std::memory_order_release);
            recompinput::suspend_all_rumble();
            recompinput::players::set_single_player_mode(true);
        }
    };
    complete_local_reset();

    // The stock multiplayer screens can return directly to the main menu,
    // outside this overlay's button callbacks. Finish that teardown here on
    // the UI thread before allowing the Multiplayer entry to be shown again.
    // This prevents a new stock menu instance from inheriting the previous
    // socket, protocol phase, and one-shot transition flags.
    const auto complete_session_reset = []() {
        netplay::shutdown();
        reset_guest_setup_progress();
        g_force_multiplayer_transition.store(false, std::memory_order_release);
        g_allow_original_multiplayer.store(false, std::memory_order_release);
        g_ui.online_entry_queued = false;
        g_ui.pending_page.reset();
        g_ui.pending_focus = nullptr;
        g_ui.focus_delay_frames = 0;
        online_log("[RR64-ONLINE] Cleared previous session before multiplayer re-entry.\n");
    };
    bool session_reset_completed = false;
    if (g_reset_session_requested.exchange(false, std::memory_order_acq_rel)) {
        complete_session_reset();
        session_reset_completed = true;
    }

    if (g_local_start_requested.exchange(false, std::memory_order_acq_rel)) {
        // The mode setter may publish reset+start after this update's earlier
        // reset checks. Acquiring start observes both preceding reset flags;
        // drain them here so the next UI update cannot tear down this entry.
        complete_local_reset();
        g_reset_session_requested.exchange(false, std::memory_order_acq_rel);
        complete_session_reset();
        netplay::configure({});
        recompinput::suspend_all_rumble();
        // Keyboard-only users can navigate the main menu without assigning a
        // player card. Keep their shared port-one input for this local session;
        // otherwise switching to empty assigned ports loses both Select and
        // Back. The native multiplayer menu already supports one human vs AI.
        // Do not create a persistent keyboard assignment or duplicate a pad.
        recompinput::players::set_single_player_mode(
            recompinput::players::get_number_of_assigned_players() == 0);
        local_players::active.store(true, std::memory_order_release);
        queue_original_multiplayer();
    }
    if (g_show_requested.exchange(false, std::memory_order_acq_rel)) {
        // The game thread can request re-entry between the reset check above
        // and this show check. Consume it a second time at the boundary so the
        // menu can never be displayed from a stale session snapshot.
        if (!session_reset_completed &&
            g_reset_session_requested.exchange(false, std::memory_order_acq_rel)) {
            complete_session_reset();
        }
        online_log("[RR64-ONLINE] Showing in-game multiplayer menu.\n");
        const netplay::Status status = netplay::get_status();
        local_players::active.store(false, std::memory_order_release);
        recompinput::players::set_single_player_mode(true);
        set_page(status.active ? Page::Lobby : Page::Connect);
        if (!recompui::is_context_shown(g_ui.context)) {
            recompui::show_context(g_ui.context, "");
        }
    }

    const netplay::Status status = netplay::get_status();
    // Hidden contexts do not process their queued text updates. Refreshing
    // these labels every race frame would accumulate an unbounded backlog,
    // then replay it when the lobby is shown. Pending requests above may show
    // the context this frame, so read visibility here and paint its latest
    // state before the frontend processes visible contexts. Keep session and
    // game-setup transitions below independent of presentation visibility.
    if(status.connected && status.phase==netplay::Phase::Lobby && g_ui.online_entry_queued) {
        // Host explicitly backed out of an incompatible setup. Return through
        // the native dispatcher on the game thread while keeping UDP alive.
        g_ui.online_entry_queued=false;
        reset_guest_setup_progress();
        g_return_to_lobby_requested.store(true,std::memory_order_release);
        set_page(Page::Lobby);
        if(!recompui::is_context_shown(g_ui.context)) recompui::show_context(g_ui.context,"");
    }
    const bool course_blocked=has_course_problem(status);
    if(course_blocked && !g_ui.course_blocked) {
        set_page(Page::Lobby);
        if(!recompui::is_context_shown(g_ui.context)) recompui::show_context(g_ui.context,"");
    } else if(!course_blocked && g_ui.course_blocked && status.connected &&
              status.phase>=netplay::Phase::GameSetup) {
        recompui::hide_context(g_ui.context);
    }
    g_ui.course_blocked=course_blocked;
    if (recompui::is_context_shown(g_ui.context)) {
        if (g_ui.course_error != nullptr) {
            std::string reason;
            for(const auto& player:status.players) {
                if(player.connected && player.course_compatibility>=race_pack::Compatibility::Missing) {
                    reason="Rider "+std::to_string(player.slot+1)+": "+
                        netplay::course_compatibility_message(player.course_compatibility);
                    break;
                }
            }
            g_ui.course_error->set_text(reason);
            g_ui.course_error->set_display(course_blocked ? recompui::Display::Flex : recompui::Display::None);
        }
        if (g_ui.lobby_status != nullptr) {
            g_ui.lobby_status->set_text(std::string(phase_name(status.phase)) + " - " + status.message);
        }
        for (std::uint8_t slot = 0; slot < netplay::kMaximumPlayers; ++slot) {
            if (g_ui.player_labels[slot] == nullptr) {
                continue;
            }
            const netplay::PlayerInfo &player = status.players[slot];
            if (!player.connected) {
                g_ui.player_labels[slot]->set_text("RIDER " + std::to_string(slot + 1) + "  -  EMPTY");
                continue;
            }
            std::string line = "RIDER " + std::to_string(slot + 1) + "  -  " + player.name;
            line += player.ready ? "  [READY]" : "  [NOT READY]";
            if(status.game_setup.valid) {
                switch(player.course_compatibility) {
                case race_pack::Compatibility::Missing: line += "  [PACK MISSING]"; break;
                case race_pack::Compatibility::Disabled: line += "  [PACK DISABLED]"; break;
                case race_pack::Compatibility::Different: line += "  [PACK DIFFERS]"; break;
                case race_pack::Compatibility::Pending: line += "  [CHECKING PACK]"; break;
                default: break;
                }
            }
            line += "  " + std::to_string(player.ping_ms) + " ms";
            if (slot == status.local_slot) {
                line += "  (YOU)";
            }
            g_ui.player_labels[slot]->set_text(line);
        }
        if (g_ui.ready_button != nullptr) {
            const bool can_ready = status.connected && status.phase == netplay::Phase::Lobby;
            g_ui.ready_button->set_enabled(can_ready);
            const bool ready =
                status.local_slot < netplay::kMaximumPlayers && status.players[status.local_slot].ready;
            g_ui.ready_button->set_text(ready ? "NOT READY" : "READY");
            if (g_ui.page == Page::Lobby && !can_ready && g_ui.pending_focus == g_ui.ready_button &&
                g_ui.disconnect_button != nullptr) {
                // A failed host bind or a client still connecting must retain a
                // usable controller target instead of autofocusing a disabled
                // Ready button.
                g_ui.pending_focus = g_ui.disconnect_button;
                g_ui.context.set_autofocus_element(g_ui.disconnect_button);
            }
        }
        if (g_ui.continue_button != nullptr) {
            g_ui.continue_button->set_display(status.is_host ? recompui::Display::Flex
                                                             : recompui::Display::None);
            g_ui.continue_button->set_text(course_blocked ? "CHANGE RACE SETTINGS" : "BEGIN GAME SETUP");
            g_ui.continue_button->set_enabled(status.is_host && (course_blocked ||
                (status.phase == netplay::Phase::Lobby && netplay::all_connected_players_ready())));
        }
    }

    if (status.connected && !course_blocked && status.phase >= netplay::Phase::GameSetup &&
        !g_ui.online_entry_queued) {
        g_ui.online_entry_queued = true;
        queue_original_multiplayer();
        if (recompui::is_context_shown(g_ui.context)) {
            recompui::hide_context(g_ui.context);
        }
    }
    // Reset once when a session ends. Repeating this while offline also
    // restores the local menu every frame, restarting its route preview.
    if (g_ui.session_active && !status.active) {
        g_ui.online_entry_queued = false;
        reset_guest_setup_progress();
    }
    g_ui.session_active = status.active;

    restore_page_focus_if_ready();

    g_ui.context.close();
    if (previous_context != recompui::ContextId::null()) {
        previous_context.open();
    }
}

bool controls_online_players() {
    const netplay::Status status = netplay::get_status();
    return status.active && status.connected && status.phase >= netplay::Phase::GameSetup;
}

bool host_controls_game_setup() {
    const netplay::Status status = netplay::get_status();
    return status.active && status.connected && status.phase >= netplay::Phase::GameSetup &&
           ((status.phase <= netplay::Phase::CharacterSelect &&
             !g_guest_character_select_active.load(std::memory_order_acquire)) ||
            status.phase == netplay::Phase::TrackSelect);
}

} // namespace rr64::online_menu

extern "C" void rr64_online_game_setup_restart() {
    // Preserve host preferences (including course music) across rounds. Only
    // the private selector's ownership latch belongs to the completed menu.
    rr64::online_menu::g_guest_character_select_active.store(false,std::memory_order_release);
}

extern "C" void rr64_online_menu_mode_changed(unsigned int requested_mode) {
    if (requested_mode != 0x20u)
        return;
    // This notification runs at both native mode setters, including Back from
    // multiplayer. The main menu's selection dispatcher never sees that Back
    // transition. Clear gameplay ownership now; defer UI/input work to its
    // owning thread so Big Game cannot inherit the previous custom bike tier.
    rr64_local_options_reset_race();
    if (rr64::local_players::active.exchange(false, std::memory_order_acq_rel))
        rr64::online_menu::g_reset_local_requested.store(true, std::memory_order_release);
    if (rr64::online_menu::g_local_start_pending_mode) {
        rr64::online_menu::g_local_start_pending_mode = false;
        rr64::online_menu::g_reset_session_requested.store(true, std::memory_order_release);
        rr64::online_menu::g_local_start_requested.store(true, std::memory_order_release);
    }
}

extern "C" unsigned int rr64_online_menu_route_mode(unsigned int requested_mode) {
    constexpr unsigned int kMainMenuMode = 0x20;
    constexpr unsigned int kLocalMultiplayerMode = 0x23;
    if (requested_mode != kLocalMultiplayerMode) {
        return requested_mode;
    }
    if (rr64::online_menu::g_allow_original_multiplayer.exchange(false,
                                                                 std::memory_order_acq_rel)) {
        return requested_mode;
    }
    // 23C38 calls the native mode setter immediately after this adapter.
    // Its synthetic main-menu transition must retire old gameplay ownership
    // before update_ui can accept the new local-start request.
    rr64::online_menu::g_local_start_pending_mode = true;
    return kMainMenuMode;
}

extern "C" void rr64_online_menu_apply_pending_guest_input(unsigned char *rdram) {
    if(rr64::online_menu::return_to_lobby(rdram)) return;
    if (rdram && rr64::popup_input::consume_menu_action()) {
        MEM_W(0, 0xFFFFFFFF8009E238ULL) = 0;
    }
    rr64_coop_main_menu(rdram);
    // Fifth stock-style main-menu row opens Host/Join directly. Consume the
    // accepted action before the original four-entry dispatcher can see it.
    if (rdram && MEM_W(0, 0xFFFFFFFF8009E128ULL) == 4 && (MEM_W(0, 0xFFFFFFFF8009E238ULL) & 5)) {
        MEM_W(0, 0xFFFFFFFF8009E238ULL) = 0;
        rr64::online_menu::g_reset_session_requested.store(true, std::memory_order_release);
        rr64::online_menu::g_show_requested.store(true, std::memory_order_release);
    }
    if (!rr64::online_menu::g_force_multiplayer_transition.exchange(false,
                                                                    std::memory_order_acq_rel)) {
        return;
    }
    // The stock main-menu selection value 1 is MULTIPLAYER. Bit 0 is the
    // accepted menu action. Feeding these through the original handler keeps
    // all local setup and transitions on the game's normal path.
    MEM_W(0, 0xFFFFFFFF8009E128ULL) = 1;
    MEM_W(0, 0xFFFFFFFF8009E238ULL) |= 1;
    rr64::online_menu::g_allow_original_multiplayer.store(true, std::memory_order_release);
}

extern "C" void rr64_online_game_setup_before_update(unsigned char *rdram) {
    if (rdram == nullptr) {
        return;
    }
    rr64_coop_selection_begin(rdram);
    const rr64::netplay::Status status = rr64::netplay::get_status();
    if (!status.active || !status.connected || status.phase < rr64::netplay::Phase::GameSetup) {
        return;
    }
    if (!status.is_host) {
        rr64::online_menu::apply_game_setup(rdram, status.game_setup);
    } else if(status.game_setup.valid && status.phase<rr64::netplay::Phase::Race) {
        rr64::netplay::report_course_compatibility(status.game_setup.revision,status.game_setup.course,
            rr64::race_pack::compatibility(status.game_setup.course));
    }
    if (rr64::online_menu::guest_multiplayer_stage(rdram) >= 2) {
        rr64::online_menu::g_guest_character_select_active.store(true, std::memory_order_release);
        rr64_online_private_selection_begin(rdram);
    }
}

extern "C" void rr64_online_game_setup_after_update(unsigned char *rdram) {
    if (rdram == nullptr) {
        return;
    }
    rr64_coop_selection_end(rdram);
    rr64_online_private_selection_end(rdram);
    // The original main-menu handler is not running while selecting a rider.
    // Consume the return at this menu's epilogue too, after native input writes.
    if(rr64::online_menu::return_to_lobby(rdram)) return;
    const std::uint32_t stage = rr64::online_menu::guest_multiplayer_stage(rdram);
    if (stage < 2) {
        return;
    }

    rr64::online_menu::g_guest_character_select_active.store(true, std::memory_order_release);
    const rr64::netplay::Status status = rr64::netplay::get_status();
    if (status.is_host && status.phase == rr64::netplay::Phase::GameSetup) {
        rr64::netplay::GameSetupState setup = rr64::online_menu::capture_game_setup(rdram);
        if (rr64::netplay::host_commit_game_setup(setup)) {
            rr64::online_menu::online_log(
                "[RR64-ONLINE] Host locked setup; individual rider selection is active.\n");
        }
    } else if (!status.is_host) {
        rr64::online_menu::apply_game_setup(rdram, status.game_setup);
    }
}

// Match the authored main-menu rows: black lettering, pale-gold selection
// with a dark backing, and the same left edge and 24-pixel row spacing.
extern "C" void func_800795AC(unsigned char *, recomp_context *);
extern "C" void rr64_online_main_menu_draw(unsigned char *rdram, void *context) {
    if (!rdram || !context) return;
    const char *labels[] = {"ONLINE", "BIG GAME CO-OP"};
    for (unsigned row = 0; row < 2; ++row) {
        recomp_context draw = *static_cast<recomp_context *>(context);
        draw.r29 -= 128;
        for (unsigned i = 0;; ++i) {
            MEM_B(64 + i, draw.r29) = labels[row][i];
            if (!labels[row][i]) break;
        }
        const bool selected = MEM_W(0, 0xFFFFFFFF8009E128ULL) == row + 4;
        float sx = 1, sy = 1;
        rr64::engine::read_float(rdram, 0x800B07D0u, sx);
        rr64::engine::read_float(rdram, 0x800B07D4u, sy);
        draw.r4 = draw.r29 + 64;
        draw.r5 = std::bit_cast<unsigned>(124.f * sx);
        draw.r6 = std::bit_cast<unsigned>((186.f + row * 24.f) * sy);
        draw.r7 = selected ? 220 : 0;
        MEM_W(16, draw.r29) = selected ? 210 : 0;
        MEM_W(20, draw.r29) = selected ? 130 : 0;
        MEM_W(24, draw.r29) = 255;
        MEM_W(28, draw.r29) = 45;
        MEM_W(32, draw.r29) = 36;
        MEM_W(36, draw.r29) = 12;
        MEM_W(40, draw.r29) = selected ? 190 : 0;
        MEM_W(44, draw.r29) = 3;
        MEM_W(48, draw.r29) = 0;
        MEM_W(52, draw.r29) = 1;
        MEM_W(56, draw.r29) = 0x3F800000;
        func_800795AC(rdram, &draw);
    }
}
