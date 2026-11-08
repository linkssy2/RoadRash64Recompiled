#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

static int checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)
struct SDL_Gamepad { int identity; };
SDL_Gamepad controller_a{1}, controller_b{2};

namespace recompinput {
enum class InputDevice { Keyboard, Controller, COUNT };
enum class GameInput { Attack, Menu, COUNT };
struct InputField { int input_id = 0; };
constexpr int num_bindings_per_input = 2;
bool get_game_input_is_menu(GameInput input) { return input == GameInput::Menu; }
struct Player { SDL_Gamepad* controller = nullptr; bool keyboard_enabled = false; };
std::array<Player, 4> slots;
std::vector<SDL_Gamepad*> connected;
std::vector<SDL_Gamepad*> connected_controllers() { return connected; }
namespace players {
InputDevice get_player_input_device(int player) {
    if (player < 0 || player >= 4) return InputDevice::COUNT;
    if (slots[player].controller) return InputDevice::Controller;
    return slots[player].keyboard_enabled ? InputDevice::Keyboard : InputDevice::COUNT;
}
const Player& get_player(int player) { CHECK(player >= 0 && player < 4); return slots[player]; }
}
namespace profiles {
std::array<InputDevice, 4> devices{InputDevice::Controller, InputDevice::Keyboard, InputDevice::Controller, InputDevice::Keyboard};
std::array<int, 4> assigned{0, 0, 0, 0};
std::array<std::array<InputField, 2>, 4> bindings;
int selected_count = 0, mutation_count = 0, reads = 0;
int get_input_profile_count() { return 4; }
InputDevice get_input_profile_device(int index) { CHECK(index >= 0 && index < 4); ++reads; return devices[index]; }
int get_input_profile_for_player(int player, InputDevice) { CHECK(player >= 0 && player < 4); return assigned[player]; }
int get_sp_keyboard_profile_index() { return 1; }
int get_sp_controller_profile_index() { return 0; }
bool select_input_profile_for_player(int player, int index, InputDevice device) {
    if (player < 0 || player >= 4 || index < 0 || index >= 4 || devices[index] != device || players::get_player_input_device(player) != device) return false;
    assigned[player] = index; ++selected_count; return true;
}
InputField get_input_binding(int profile, GameInput, int index) { CHECK(profile >= 0 && profile < 4); CHECK(index >= 0 && index < 2); ++reads; return bindings[profile][index]; }
void clear_input_binding(int profile, GameInput) { CHECK(profile >= 0 && profile < 4); bindings[profile] = {}; ++mutation_count; }
void reset_input_binding(int profile, InputDevice device, GameInput) { CHECK(profile >= 0 && profile < 4); CHECK(devices[profile] == device); bindings[profile][0].input_id = 99; ++mutation_count; }
}
namespace binding {
bool active = false;
int stops = 0, starts = 0, player = -1;
InputDevice source = InputDevice::COUNT;
void stop_scanning() { active = false; ++stops; }
bool is_binding() { return active; }
void start_scanning(int slot, GameInput, int, InputDevice device) { CHECK(device != InputDevice::COUNT); active = true; ++starts; player = slot; source = device; }
}
}

namespace recompui {
enum class EventType { Update, Other };
struct Event { EventType type; };
enum class Display { None, Block };
struct GameInputRowsWrapper {
    bool is_menu = false;
    Display display = Display::Block;
    void set_margin_top(float) {}
    void set_margin_bottom(float) {}
    void display_hide() { display = Display::None; }
    void display_show() { display = Display::Block; }
    Display get_display() { return display; }
};
struct Context { recompinput::GameInput input_id = recompinput::GameInput::Attack; };
struct Sections {
    Context attack;
    std::vector<Context*> get_all_contexts() { return {&attack}; }
};
using PlayerBindings = std::map<recompinput::GameInput, std::vector<recompinput::InputField>>;
struct GameInputRow {
    std::vector<recompinput::InputField> shown;
    recompinput::GameInput get_input_id() { return recompinput::GameInput::Attack; }
    void update_bindings(std::vector<recompinput::InputField>& bindings) { shown = bindings; }
};
namespace config {
struct Modal { int renders = 0; void render_menu_actions() { ++renders; } } modal;
Modal* get_config_modal() { return &modal; }
}

class ConfigPageControls {
public:
    int last_update_index = 0, update_index = 0;
    int selected_player = 0, selected_profile_index = -1, selected_profile_player = -1;
    recompinput::InputDevice selected_profile_device = recompinput::InputDevice::COUNT;
    SDL_Gamepad* selected_profile_controller = nullptr;
    size_t max_num_players = 4;
    bool multiplayer_enabled = true, multiplayer_view_mappings = false, single_player_show_keyboard_mappings = false;
    bool awaiting_binding = false, awaiting_binding_for_menu_action_button = false, queue_first_game_input_row_focus = false;
    Sections game_input_sections;
    GameInputRowsWrapper wrapper;
    GameInputRow row;
    std::vector<GameInputRowsWrapper*> rows_wrappers{&wrapper};
    std::vector<GameInputRow*> game_input_rows{&row};
    int header_profile = -1, renders = 0;
    void queue_update() {}
    void render_header() {
        ++renders;
        header_profile = multiplayer_view_mappings ? selected_profile_index : -1;
        if (multiplayer_view_mappings) CHECK(selected_profile_index >= 0 && selected_profile_index < 4);
    }
    void render_body() { if (should_show_mappings()) update_control_mappings(); }
    void render_footer() { if (should_show_mappings()) CHECK(selected_profile_index >= 0); }
    void process_event(const Event&);
    void force_update();
    void render_all();
    bool should_show_mappings();
    void on_select_player_profile(int, int);
    void on_edit_player_profile(int);
    bool set_current_profile_index();
    bool can_edit_current_profile();
    void update_control_mappings();
    recompinput::InputDevice get_player_input_device();
    void on_bind_click(recompinput::GameInput, int);
    void on_clear_or_reset_game_input(recompinput::GameInput, bool);
    void tick() { process_event({EventType::Update}); }
};
#ifdef LEGACY_REFRESH
#include "ui-profile-production-legacy.inc"
#else
#include "ui-profile-production.inc"
#endif
}

using recompinput::InputDevice;
using recompinput::GameInput;
using recompui::ConfigPageControls;
void setup() {
    recompinput::slots = {};
    recompinput::slots[0].controller = &controller_a;
    recompinput::connected = {&controller_a, &controller_b};
    recompinput::profiles::assigned = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) recompinput::profiles::bindings[i] = {{{10 + i}, {20 + i}}};
    recompinput::profiles::selected_count = recompinput::profiles::mutation_count = recompinput::profiles::reads = 0;
    recompinput::binding::active = false;
    recompinput::binding::starts = recompinput::binding::stops = 0;
}

int main() {
    setup();
    ConfigPageControls stale;
    // The old editor initially snapshots profile 0. Reassigning later must refresh it.
    stale.selected_profile_index = 0;
    stale.multiplayer_view_mappings = true;
    stale.set_current_profile_index();
    recompinput::profiles::assigned[0] = 2;
    stale.force_update(); stale.tick();
    CHECK(stale.selected_profile_index == 2);
    CHECK(stale.header_profile == 2 && stale.row.shown[0].input_id == 12);
#ifdef LEGACY_REFRESH
    std::puts("ERROR: legacy negative control unexpectedly passed");
    return 2;
#endif

    setup();
    ConfigPageControls ui;
    ui.on_edit_player_profile(0); ui.tick();
    CHECK(ui.selected_profile_index == 0 && ui.row.shown[0].input_id == 10);
    ui.on_bind_click(GameInput::Attack, 0);
    CHECK(ui.awaiting_binding && recompinput::binding::active);
    ui.force_update(); ui.tick();
    CHECK(recompinput::binding::active && recompinput::binding::stops == 0);
    recompinput::profiles::assigned[0] = 2;
    ui.force_update();
    CHECK(!ui.awaiting_binding && !recompinput::binding::active && recompinput::binding::stops == 1);
    ui.on_clear_or_reset_game_input(GameInput::Attack, false);
    ui.on_bind_click(GameInput::Attack, 0);
    CHECK(recompinput::profiles::mutation_count == 0 && recompinput::binding::starts == 1);
    ui.tick();
    CHECK(ui.row.shown[0].input_id == 12);
    ui.on_clear_or_reset_game_input(GameInput::Attack, false);
    CHECK(recompinput::profiles::bindings[0][0].input_id == 10 && recompinput::profiles::bindings[2][0].input_id == 0);
    ui.on_clear_or_reset_game_input(GameInput::Attack, true);
    CHECK(ui.row.shown[0].input_id == 99);

    ui.on_bind_click(GameInput::Menu, 0);
    recompinput::slots[0].controller = &controller_b; // Same profile, different physical device.
    ui.tick();
    CHECK(!ui.awaiting_binding && !ui.awaiting_binding_for_menu_action_button);
    CHECK(ui.selected_profile_controller == &controller_b);
    ui.on_bind_click(GameInput::Attack, 0);
    recompinput::slots[0] = {};
    ui.force_update(); ui.tick();
    CHECK(!ui.multiplayer_view_mappings && ui.selected_profile_index == -1 && !recompinput::binding::active);
    const int writes = recompinput::profiles::mutation_count;
    const int starts = recompinput::binding::starts;
    ui.on_bind_click(GameInput::Attack, 0);
    ui.on_clear_or_reset_game_input(GameInput::Attack, true);
    ui.update_control_mappings();
    CHECK(recompinput::binding::starts == starts && recompinput::profiles::mutation_count == writes);

    recompinput::slots[0].keyboard_enabled = true;
    recompinput::profiles::assigned[0] = 3;
    ui.on_edit_player_profile(0); ui.tick();
    CHECK(ui.selected_profile_index == 3 && ui.selected_profile_device == InputDevice::Keyboard);
    ui.on_bind_click(GameInput::Attack, 0);
    CHECK(recompinput::binding::source == InputDevice::Keyboard);
    ui.on_select_player_profile(0, 1); ui.tick();
    CHECK(ui.selected_profile_index == 1 && recompinput::profiles::selected_count == 1);
    CHECK(!recompinput::binding::active);
    ui.on_select_player_profile(0, 2); ui.tick();
    CHECK(ui.selected_profile_index == 1 && recompinput::profiles::selected_count == 1);

    for (int invalid : {-1, 4, 100, 2}) { // Profile 2 is valid, but belongs to a controller.
        recompinput::profiles::assigned[0] = invalid;
        ui.multiplayer_view_mappings = true;
        ui.force_update(); ui.tick();
        CHECK(ui.selected_profile_index == -1 && !ui.multiplayer_view_mappings);
        ui.on_clear_or_reset_game_input(GameInput::Attack, false);
        ui.on_bind_click(GameInput::Attack, 0);
    }
    for (int invalid_player : {-1, 4, 99}) {
        ui.on_edit_player_profile(invalid_player); ui.tick();
        CHECK(ui.selected_profile_index == -1 && !ui.multiplayer_view_mappings);
    }

    setup();
    ConfigPageControls polled;
    polled.on_edit_player_profile(0); polled.tick();
    recompinput::profiles::assigned[0] = 2; // No explicit force_update notification.
    polled.on_clear_or_reset_game_input(GameInput::Attack, false);
    CHECK(recompinput::profiles::mutation_count == 0);
    polled.tick();
    CHECK(polled.row.shown[0].input_id == 12);
    recompinput::slots[1].controller = &controller_b;
    recompinput::profiles::assigned[1] = 2;
    polled.on_bind_click(GameInput::Attack, 0);
    polled.on_edit_player_profile(1); polled.tick();
    CHECK(polled.selected_player == 1 && !recompinput::binding::active);
    polled.on_bind_click(GameInput::Attack, 0);
    CHECK(recompinput::binding::player == 1);

    setup();
    ConfigPageControls removed;
    removed.on_edit_player_profile(0); removed.tick();
    removed.on_bind_click(GameInput::Attack, 0);
    recompinput::connected.clear(); // SDL removed device; slot still contains its old handle.
    removed.tick();
    CHECK(!removed.multiplayer_view_mappings && removed.header_profile == -1);
    CHECK(!recompinput::binding::active && !removed.awaiting_binding);
    removed.on_clear_or_reset_game_input(GameInput::Attack, false);
    removed.on_bind_click(GameInput::Attack, 0);
    CHECK(recompinput::profiles::mutation_count == 0 && recompinput::binding::starts == 1);

    setup();
    ConfigPageControls legacy_sp;
    legacy_sp.multiplayer_enabled = false;
    legacy_sp.force_update(); legacy_sp.tick();
    CHECK(legacy_sp.selected_profile_index == 0);
    legacy_sp.single_player_show_keyboard_mappings = true;
    legacy_sp.update_control_mappings(); legacy_sp.tick();
    CHECK(legacy_sp.selected_profile_index == 1 && legacy_sp.row.shown[0].input_id == 11);
    std::printf("PASS Controls profile refresh: %d checks (extracted production methods)\n", checks);
}
