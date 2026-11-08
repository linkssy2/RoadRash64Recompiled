#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include "recomp.h"
#include "recompinput/input_state.h"
#include "recompinput/players.h"
#include "recompinput/profiles.h"

#include "rr64_native.hpp"

namespace {
enum class ControllerFamily {
    Xbox,
    PlayStation,
    Nintendo,
    Generic,
};

ControllerFamily active_controller_family() {
    switch (recompinput::get_active_controller_type()) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return ControllerFamily::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return ControllerFamily::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return ControllerFamily::Nintendo;
    default:
        return ControllerFamily::Generic;
    }
}

std::string face_button_label(SDL_GamepadButton button, ControllerFamily family) {
    if (family == ControllerFamily::PlayStation) {
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "X";
        case SDL_GAMEPAD_BUTTON_EAST: return "O";
        case SDL_GAMEPAD_BUTTON_WEST: return "SQ";
        case SDL_GAMEPAD_BUTTON_NORTH: return "TRI";
        default: break;
        }
    }
    else if (family == ControllerFamily::Nintendo) {
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "B";
        case SDL_GAMEPAD_BUTTON_EAST: return "A";
        case SDL_GAMEPAD_BUTTON_WEST: return "Y";
        case SDL_GAMEPAD_BUTTON_NORTH: return "X";
        default: break;
        }
    }
    else {
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "A";
        case SDL_GAMEPAD_BUTTON_EAST: return "B";
        case SDL_GAMEPAD_BUTTON_WEST: return "X";
        case SDL_GAMEPAD_BUTTON_NORTH: return "Y";
        default: break;
        }
    }
    return {};
}

std::string controller_button_label(SDL_GamepadButton button) {
    const ControllerFamily family = active_controller_family();
    if (std::string face = face_button_label(button, family); !face.empty()) {
        return face;
    }

    switch (button) {
    case SDL_GAMEPAD_BUTTON_BACK:
        if (family == ControllerFamily::PlayStation) return "SHARE";
        if (family == ControllerFamily::Nintendo) return "-";
        return "VIEW";
    case SDL_GAMEPAD_BUTTON_GUIDE:
        return "HOME";
    case SDL_GAMEPAD_BUTTON_START:
        if (family == ControllerFamily::PlayStation) return "OPTIONS";
        if (family == ControllerFamily::Nintendo) return "+";
        return "MENU";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return family == ControllerFamily::PlayStation ? "L3" : "LS";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return family == ControllerFamily::PlayStation ? "R3" : "RS";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        if (family == ControllerFamily::PlayStation) return "L1";
        if (family == ControllerFamily::Nintendo) return "L";
        return "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        if (family == ControllerFamily::PlayStation) return "R1";
        if (family == ControllerFamily::Nintendo) return "R";
        return "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-UP";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-DOWN";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-LEFT";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-RIGHT";
    case SDL_GAMEPAD_BUTTON_MISC1: return "MISC";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "P1";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "P2";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "P3";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "P4";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "PAD";
    default: return "PAD";
    }
}

std::string controller_axis_label(int input_id) {
    const bool positive = input_id > 0;
    const auto axis = static_cast<SDL_GamepadAxis>(std::abs(input_id) - 1);
    const ControllerFamily family = active_controller_family();
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return positive ? "LS-R" : "LS-L";
    case SDL_GAMEPAD_AXIS_LEFTY: return positive ? "LS-D" : "LS-U";
    case SDL_GAMEPAD_AXIS_RIGHTX: return positive ? "RS-R" : "RS-L";
    case SDL_GAMEPAD_AXIS_RIGHTY: return positive ? "RS-D" : "RS-U";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        if (family == ControllerFamily::PlayStation) return "L2";
        if (family == ControllerFamily::Nintendo) return "ZL";
        return "LT";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        if (family == ControllerFamily::PlayStation) return "R2";
        if (family == ControllerFamily::Nintendo) return "ZR";
        return "RT";
    default: return "AXIS";
    }
}

std::string keyboard_label(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_RETURN: return "ENT";
    case SDL_SCANCODE_ESCAPE: return "ESC";
    case SDL_SCANCODE_SPACE: return "SPC";
    case SDL_SCANCODE_BACKSPACE: return "BSP";
    case SDL_SCANCODE_TAB: return "TAB";
    case SDL_SCANCODE_LSHIFT: return "LSH";
    case SDL_SCANCODE_RSHIFT: return "RSH";
    case SDL_SCANCODE_LCTRL: return "LCT";
    case SDL_SCANCODE_RCTRL: return "RCT";
    case SDL_SCANCODE_LALT: return "LAL";
    case SDL_SCANCODE_RALT: return "RAL";
    case SDL_SCANCODE_UP: return "UP";
    case SDL_SCANCODE_DOWN: return "DOWN";
    case SDL_SCANCODE_LEFT: return "LEFT";
    case SDL_SCANCODE_RIGHT: return "RIGHT";
    default: break;
    }

    std::string label = SDL_GetScancodeName(scancode);
    if (label.empty()) {
        return "KEY";
    }
    std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    label.erase(std::remove_if(label.begin(), label.end(), [](unsigned char c) {
        return !(std::isalnum(c) || c == '-' || c == '+');
    }), label.end());
    return label.empty() ? "KEY" : label;
}

std::string binding_label(const recompinput::InputField& binding) {
    switch (binding.input_type) {
    case recompinput::InputType::Keyboard:
        return keyboard_label(static_cast<SDL_Scancode>(binding.input_id));
    case recompinput::InputType::ControllerDigital:
        return controller_button_label(static_cast<SDL_GamepadButton>(binding.input_id));
    case recompinput::InputType::ControllerAnalog:
        return controller_axis_label(binding.input_id);
    case recompinput::InputType::Mouse:
        return "MOUSE";
    case recompinput::InputType::None:
    default:
        return {};
    }
}

int active_profile_index(recompinput::InputDevice device) {
    if (recompinput::players::is_single_player_mode()) {
        return device == recompinput::InputDevice::Controller
            ? recompinput::profiles::get_sp_controller_profile_index()
            : recompinput::profiles::get_sp_keyboard_profile_index();
    }
    return recompinput::profiles::get_input_profile_for_player(0, device);
}

std::string prompt_for_input(recompinput::GameInput input, std::string_view fallback) {
    const recompinput::InputDevice device = recompinput::get_active_input_device();
    const int profile_index = active_profile_index(device);
    if (profile_index >= 0) {
        for (std::size_t i = 0; i < recompinput::num_bindings_per_input; ++i) {
            const recompinput::InputField binding =
                recompinput::profiles::get_input_binding(profile_index, input, i);
            if (binding.is_empty()) {
                continue;
            }
            if (device == recompinput::InputDevice::Controller &&
                binding.input_type != recompinput::InputType::ControllerDigital &&
                binding.input_type != recompinput::InputType::ControllerAnalog) {
                continue;
            }
            if (device == recompinput::InputDevice::Keyboard &&
                binding.input_type != recompinput::InputType::Keyboard &&
                binding.input_type != recompinput::InputType::Mouse) {
                continue;
            }
            if (std::string label = binding_label(binding); !label.empty()) {
                // The original Road Rash prompts are colored, single-letter
                // face buttons. Keep that exact presentation for controller
                // face bindings, but frame keyboard and extended controller
                // inputs as compact pixel keycaps.
                if (binding.input_type == recompinput::InputType::ControllerDigital &&
                    binding.input_id >= SDL_GAMEPAD_BUTTON_SOUTH &&
                    binding.input_id <= SDL_GAMEPAD_BUTTON_NORTH) {
                    return label;
                }
                return "[" + label + "]";
            }
        }
    }
    return std::string(fallback);
}
} // namespace

extern "C" unsigned int rr64_write_button_prompt(
    unsigned char* rdram,
    unsigned int logical_button,
    unsigned int destination,
    unsigned int capacity)
{
    if (rdram == nullptr || capacity == 0 || destination < 0x80000000u) {
        return 0;
    }

    recompinput::GameInput input = recompinput::GameInput::A;
    std::string_view fallback = "A";
    if (logical_button == RR64_PROMPT_BUTTON_B) {
        input = recompinput::GameInput::B;
        fallback = "B";
    }

    std::string prompt = prompt_for_input(input, fallback);
    // The stock lower-menu renderer allocates one original glyph cell for the
    // A/B face prompt. When this helper targets that two-byte slot, preserve a
    // meaningful pixel glyph instead of truncating a framed keyboard or
    // extended-controller label to just '['. Xbox/Nintendo face labels are
    // already one character; PlayStation SQ/TRI become S/T in the same style.
    if (capacity == 2 && prompt.size() > 1) {
        if (prompt.front() == '[' && prompt.size() >= 3) {
            prompt = prompt.substr(1, 1);
        }
        else {
            prompt.resize(1);
        }
    }
    if (prompt.size() + 1 > capacity) {
        prompt.resize(capacity - 1);
    }

    // Recompiled RDRAM keeps the N64's big-endian byte lanes swizzled within
    // each host word. Use the same byte accessor as guest code so the text
    // renderer sees the intended character order.
    const gpr guest_destination =
        static_cast<gpr>(static_cast<std::int64_t>(static_cast<std::int32_t>(destination)));
    for (std::size_t i = 0; i < prompt.size(); ++i) {
        MEM_B(static_cast<int>(i), guest_destination) = prompt[i];
    }
    MEM_B(static_cast<int>(prompt.size()), guest_destination) = '\0';
    return static_cast<unsigned int>(prompt.size());
}
