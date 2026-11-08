#include "rr64_controller_mappings.hpp"
#include "recompinput/input_binding.h"
#include "recompinput/profiles.h"
#include "recompinput/rr64_n64_switch_mapping.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Stub only profile storage/assignment; exercise the real binding implementation.
static recompinput::InputField menu_binding{};
static int expected_profile = 7;
namespace recompinput::profiles {
int get_input_profile_for_player(int player, InputDevice) { return player == 2 ? expected_profile : 0; }
int get_input_profile_count() { return expected_profile + 1; }
InputDevice get_input_profile_device(int) { return InputDevice::Controller; }
InputField& get_input_binding(int profile, GameInput, size_t) {
    if (profile != expected_profile) std::abort();
    return menu_binding;
}
void set_input_binding(int, GameInput, size_t, InputField) {}
}
namespace recompinput { std::vector<SDL_Gamepad*> connected_controllers() { return {}; } }
namespace recompinput::players {
bool is_single_player_mode() { return false; }
size_t get_number_of_assigned_players() { return 0; }
const Player& get_player(int, bool) { static Player player; return player; }
}

static unsigned checks;
static void check(bool good, const char* what) {
    ++checks;
    if (!good) { std::fprintf(stderr, "FAIL: %s (%s)\n", what, SDL_GetError()); std::exit(1); }
}

// RR64-SDL3: virtual joysticks are described by a struct and identified by
// instance ID (0 = failure); SDL2 returned a device index (-1 = failure).
static SDL_JoystickID attach_virtual(int axes, int buttons, int hats) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    desc.naxes = Uint16(axes);
    desc.nbuttons = Uint16(buttons);
    desc.nhats = Uint16(hats);
    return SDL_AttachVirtualJoystick(&desc);
}

static std::string guid_text(SDL_JoystickID id) {
    char guid[33]{};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    return guid;
}

// Mirrors rr64_register_controller_mappings(): SDL3 reports "macOS" while the
// database rows say "Mac OS X".
static bool for_this_platform(const char* mapping) {
    const std::string platform = std::string("platform:") + SDL_GetPlatform() + ",";
    if (std::strstr(mapping, platform.c_str())) return true;
#if defined(__APPLE__)
    return std::strstr(mapping, "platform:Mac OS X,") != nullptr;
#else
    return false;
#endif
}

int main() {
    // RR64-SDL3: SDL_SetMainReady lives in SDL_main.h and is not needed on desktop.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    check(SDL_Init(SDL_INIT_GAMEPAD), "input initialization");
    check(rr64_register_controller_mappings(), "mapping registration");
    const std::string platform = std::string("platform:") + SDL_GetPlatform() + ",";
    for (const char* mapping : rr64_controller_mappings) {
        if (!for_this_platform(mapping)) continue;
        std::string mapping_guid(mapping, 32);
        char* registered = SDL_GetGamepadMappingForGUID(SDL_StringToGUID(mapping_guid.c_str()));
        check(registered != nullptr, "exact device GUID recognized");
        SDL_free(registered);
        if (std::strstr(mapping, ",NSO N64 Controller,")) continue; // Exercised separately below.

        // A synthetic joystick has its own GUID. Keep the upstream raw layout
        // unchanged while replacing only that identity for the test device.
        const SDL_JoystickID index = attach_virtual(6, 15, 1);
        check(index != 0, "virtual raw device attached");
        const std::string fixture = guid_text(index) + (mapping + 32);
        check(SDL_AddGamepadMapping(fixture.c_str()) >= 0, "virtual mapping");
        auto* controller = SDL_OpenGamepad(index);
        check(controller != nullptr, "virtual controller recognized");
        auto* joystick = SDL_GetGamepadJoystick(controller);
        const struct { int raw; SDL_GamepadButton mapped; } buttons[] = {
            {0, SDL_GAMEPAD_BUTTON_SOUTH}, {1, SDL_GAMEPAD_BUTTON_EAST},
            {6, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {7, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
            {10, SDL_GAMEPAD_BUTTON_BACK}, {11, SDL_GAMEPAD_BUTTON_START},
            {12, SDL_GAMEPAD_BUTTON_GUIDE}, {13, SDL_GAMEPAD_BUTTON_LEFT_STICK}
        };
        for (auto button : buttons) {
            SDL_SetJoystickVirtualButton(joystick, button.raw, true); SDL_UpdateJoysticks();
            check(SDL_GetGamepadButton(controller, button.mapped), "button press");
            SDL_SetJoystickVirtualButton(joystick, button.raw, false); SDL_UpdateJoysticks();
            check(!SDL_GetGamepadButton(controller, button.mapped), "button release");
        }
        for (int axis = 0; axis < 4; ++axis) {
            // Windows raw axes 2 is unused; Linux/macOS use contiguous XY pairs.
            int raw = axis >= 2 && platform == "platform:Windows," ? axis + 1 : axis;
            for (Sint16 value : {Sint16(-32767), Sint16(32767), Sint16(0)}) {
                SDL_SetJoystickVirtualAxis(joystick, raw, value); SDL_UpdateJoysticks();
                check(SDL_GetGamepadAxis(controller, SDL_GamepadAxis(axis)) == value,
                    "stick/C direction and neutral");
            }
        }
        for (int trigger = 0; trigger < 2; ++trigger) {
            SDL_SetJoystickVirtualButton(joystick, 8 + trigger, true); SDL_UpdateJoysticks();
            check(SDL_GetGamepadAxis(controller, SDL_GamepadAxis(4 + trigger)) > 30000, "Z trigger press");
            SDL_SetJoystickVirtualButton(joystick, 8 + trigger, false); SDL_UpdateJoysticks();
            check(SDL_GetGamepadAxis(controller, SDL_GamepadAxis(4 + trigger)) == 0, "Z trigger release");
        }
        SDL_SetJoystickVirtualHat(joystick, 0, SDL_HAT_RIGHTUP); SDL_UpdateJoysticks();
        check(SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_UP) &&
              SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_RIGHT), "diagonal D-pad");
        SDL_CloseGamepad(controller);
        check(SDL_DetachVirtualJoystick(index), "disconnect");
    }

    // Independent raw C-button fixtures: Windows USB, Linux/macOS raw HID,
    // and the Linux kernel's N64-specific evdev button ordering.
    struct SwitchLayout { const char* guid; int left, right, up, down; };
    const SwitchLayout layouts[] = {
        {"030000007e05000019200000000000",3,8,7,2},
        {"030000007e05000019200000010000",3,8,2,7},
        {"030000007e05000019200000118100",4,2,10,3},
        {"050000007e05000019200000010000",3,8,2,7},
        {"050000007e05000019200000018000",4,2,10,3},
    };
    for (const char* mapping : rr64_controller_mappings) {
        if (!for_this_platform(mapping) || !std::strstr(mapping, ",NSO N64 Controller,")) continue;
        const SwitchLayout* layout = nullptr;
        for (const auto& candidate : layouts) if (std::strncmp(mapping, candidate.guid, 28) == 0) layout = &candidate;
        check(layout != nullptr, "Switch layout fixture identified");
        const SDL_JoystickID index = attach_virtual(6, 16, 1);
        check(index != 0, "Switch virtual device attached");
        check(SDL_AddGamepadMapping((guid_text(index)+(mapping+32)).c_str()) >= 0, "Switch fixture mapped");
        auto* controller=SDL_OpenGamepad(index);
        check(controller != nullptr, "Switch fixture opened");
        auto* joystick=SDL_GetGamepadJoystick(controller);
        const int buttons[]={layout->left,layout->right,layout->up,layout->down};
        for (int i=0;i<4;++i) {
            const auto axis=i<2?SDL_GAMEPAD_AXIS_RIGHTX:SDL_GAMEPAD_AXIS_RIGHTY;
            SDL_SetJoystickVirtualButton(joystick,buttons[i],true);SDL_UpdateJoysticks();
            const int value=SDL_GetGamepadAxis(controller,axis);
            check(i%2?value>30000:value< -30000,"Switch C press recognized");
            check(!SDL_GetGamepadButton(controller,SDL_GAMEPAD_BUTTON_BACK),"C input does not become menu cancel");
            SDL_SetJoystickVirtualButton(joystick,buttons[i],false);SDL_UpdateJoysticks();
            check(SDL_GetGamepadAxis(controller,axis)==0,"Switch C release");
        }
        // Generic raw HID has a spare right-stick-click report slot b11;
        // Linux's N64-specific kernel layout instead uses b11 for Start.
        if (layout->left == 3) {
            SDL_SetJoystickVirtualButton(joystick,11,true);SDL_UpdateJoysticks();
            check(SDL_GetGamepadButton(controller,SDL_GAMEPAD_BUTTON_LEFT_STICK),"raw stick-click exposed as L3");
            check(SDL_GetGamepadAxis(controller,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)==0,"L3 is independent of ZR");
            check(SDL_GetGamepadAxis(controller,SDL_GAMEPAD_AXIS_RIGHTX)==0 && SDL_GetGamepadAxis(controller,SDL_GAMEPAD_AXIS_RIGHTY)==0,"L3 does not press C directions");
            SDL_SetJoystickVirtualButton(joystick,11,false);SDL_UpdateJoysticks();
            check(!SDL_GetGamepadButton(controller,SDL_GAMEPAD_BUTTON_LEFT_STICK),"L3 release");
        } else {
            SDL_SetJoystickVirtualButton(joystick,11,true);SDL_UpdateJoysticks();
            check(SDL_GetGamepadButton(controller,SDL_GAMEPAD_BUTTON_START) && !SDL_GetGamepadButton(controller,SDL_GAMEPAD_BUTTON_LEFT_STICK),"kernel Start is not mislabeled L3");
        }
        SDL_CloseGamepad(controller);SDL_DetachVirtualJoystick(index);
    }

    // HIDAPI reports are already decoded by SDL; its raw order is different
    // from all OS joystick layouts above, even when VID/PID are identical.
    auto hid_guid=SDL_StringToGUID("030000007e050000192000000000680c");
    check(rr64::input::is_n64_hidapi_guid(hid_guid),"HIDAPI N64 identity");
    check(rr64::input::register_n64_hidapi_mapping(hid_guid)==1,"HIDAPI mapping registered");
    for (int field : {4,8,14,15}) {
        auto other=hid_guid;other.data[field]^=1;
        check(!rr64::input::is_n64_hidapi_guid(other),"other driver/product/subtype untouched");
    }
    // RR64-SDL3 raw order (see rr64_n64_switch_mapping.h): D-pad on hat 0,
    // C-Left b2, C-Up b3, C-Right b4, C-Down axis 5, ZR b7, b8 the other click.
    const SDL_JoystickID hid_index=attach_virtual(6,12,1);
    check(hid_index!=0,"HIDAPI virtual device attached");
    check(SDL_AddGamepadMapping((guid_text(hid_index)+","+rr64::input::n64_hidapi_mapping).c_str())>=0,"HIDAPI fixture mapped");
    auto* hid=SDL_OpenGamepad(hid_index);check(hid!=nullptr,"HIDAPI fixture opened");
    auto* raw=SDL_GetGamepadJoystick(hid);
    SDL_SetJoystickVirtualAxis(raw,4,-32768);SDL_SetJoystickVirtualAxis(raw,5,-32768);SDL_UpdateJoysticks();
    check(SDL_GetGamepadAxis(hid,SDL_GAMEPAD_AXIS_RIGHTY)==0,"C-Down neutral");
    for (int button : {2,3,4}) {
        SDL_SetJoystickVirtualButton(raw,button,true);SDL_UpdateJoysticks();
        int value=SDL_GetGamepadAxis(hid,button==3?SDL_GAMEPAD_AXIS_RIGHTY:SDL_GAMEPAD_AXIS_RIGHTX);
        check(button==4?value>30000:value< -30000,"HIDAPI C direction");
        check(!SDL_GetGamepadButton(hid,SDL_GAMEPAD_BUTTON_BACK),"HIDAPI C is not Back");
        SDL_SetJoystickVirtualButton(raw,button,false);SDL_UpdateJoysticks();
    }
    SDL_SetJoystickVirtualAxis(raw,5,32767);SDL_UpdateJoysticks();
    check(SDL_GetGamepadAxis(hid,SDL_GAMEPAD_AXIS_RIGHTY)>30000,"C-Down from ZR report bit");
    check(SDL_GetGamepadAxis(hid,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)==0,"C-Down does not activate physical ZR");
    SDL_SetJoystickVirtualAxis(raw,5,-32768);SDL_SetJoystickVirtualButton(raw,7,true);SDL_UpdateJoysticks();
    check(SDL_GetGamepadAxis(hid,SDL_GAMEPAD_AXIS_RIGHTY)==0,"C-Down released");
    check(SDL_GetGamepadAxis(hid,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)>30000,"physical ZR retained");
    SDL_SetJoystickVirtualButton(raw,7,false);SDL_SetJoystickVirtualButton(raw,8,true);SDL_UpdateJoysticks();
    check(SDL_GetGamepadAxis(hid,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)==0 && SDL_GetGamepadButton(hid,SDL_GAMEPAD_BUTTON_RIGHT_STICK),
        "second click bit stays bindable as R3");
    SDL_SetJoystickVirtualButton(raw,8,false);
    SDL_SetJoystickVirtualHat(raw,0,SDL_HAT_LEFTDOWN);SDL_UpdateJoysticks();
    check(SDL_GetGamepadButton(hid,SDL_GAMEPAD_BUTTON_DPAD_LEFT) && SDL_GetGamepadButton(hid,SDL_GAMEPAD_BUTTON_DPAD_DOWN),
        "HIDAPI D-pad from hat");
    SDL_SetJoystickVirtualHat(raw,0,SDL_HAT_CENTERED);
    SDL_SetJoystickVirtualButton(raw,11,true);SDL_UpdateJoysticks();
    check(SDL_GetGamepadButton(hid,SDL_GAMEPAD_BUTTON_MISC1),"Capture as misc1");
    SDL_SetJoystickVirtualButton(raw,11,false);SDL_UpdateJoysticks();
    SDL_CloseGamepad(hid);SDL_DetachVirtualJoystick(hid_index);
    using namespace recompinput;
    binding::start_scanning(2, GameInput::A, 0, InputDevice::Controller);
    menu_binding = {InputType::Keyboard, SDL_GAMEPAD_BUTTON_SOUTH};
    check(!binding::is_cancel_button(SDL_GAMEPAD_BUTTON_SOUTH), "keyboard ID cannot cancel button");
    menu_binding = {InputType::ControllerAnalog, SDL_GAMEPAD_BUTTON_SOUTH};
    check(!binding::is_cancel_button(SDL_GAMEPAD_BUTTON_SOUTH), "axis ID cannot cancel button");
    menu_binding = {InputType::ControllerDigital, SDL_GAMEPAD_BUTTON_BACK};
    check(binding::is_cancel_button(SDL_GAMEPAD_BUTTON_BACK), "selected profile cancels");
    check(!binding::is_cancel_button(SDL_GAMEPAD_BUTTON_SOUTH), "other button bindable");
    binding::start_scanning(2, GameInput::TOGGLE_MENU, 0, InputDevice::Controller);
    check(!binding::is_cancel_button(SDL_GAMEPAD_BUTTON_BACK), "menu button can be rebound");
    binding::stop_scanning();
    check(!binding::is_cancel_button(SDL_GAMEPAD_BUTTON_BACK), "inactive scan");
    SDL_Quit();
    std::printf("Controller mapping/binding checks passed: %u\n", checks);
}
