#pragma once
#include <SDL3/SDL.h>
#include <string>

// RR64-SDL3: SDL3 3.4 HIDAPI Nintendo Classic driver, N64 subtype 12.
// The driver labels the face buttons (RemapButton, AlwaysUsesLabels) and now
// reports the D-pad as hat 0 and Capture as button 11 (SDL 2.30.3 used
// buttons 11-14 and 15). Raw layout:
//   b0 A, b1 B, b2 C-Left (Switch X), b3 C-Up (Switch Y), b4 C-Right (Minus),
//   a5 C-Down (Switch ZR), a4 Z (Switch ZL), b9 L, b10 R, b6 Start, b5 Home,
//   b11 Capture, b7/b8 the two stick-click bits.
// Physical ZR: Linux hid-nintendo.c and SDL3's own N64 map both put it on the
// left stick-click bit (b7); the SDL2-era map here used b8. Map b7 as the
// right trigger and keep b8 reachable as right-stick click, so ZR stays
// bindable whichever bit a given controller revision uses.
// The stock SDL3 N64 map sends C-Up to Back and C-Down to X; normalize the C
// directions to the right stick, like our D-mode maps, without changing the
// raw driver, saved profiles or other pads.
// Sources: libsdl-org/SDL release-3.4.16 SDL_hidapi_switch.c (HandleFullControllerState,
// RemapButton), SDL_gamepad.c (k_eSwitchDeviceInfoControllerType_N64);
// torvalds/linux drivers/hid/hid-nintendo.c N64 table.
namespace rr64::input {
inline constexpr const char* n64_hidapi_mapping =
    "Nintendo N64 Controller,a:b0,b:b1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,"
    "guide:b5,leftshoulder:b9,lefttrigger:a4,leftx:a0,lefty:a1,"
    "rightshoulder:b10,righttrigger:b7,rightstick:b8,start:b6,misc1:b11,"
    "-rightx:b2,+rightx:b4,-righty:b3,+righty:a5,";

inline bool is_n64_hidapi_guid(SDL_GUID guid) {
    Uint16 vendor = 0, product = 0;
    SDL_GetJoystickGUIDInfo(guid, &vendor, &product, nullptr, nullptr);
    // Raw evdev/DInput GUIDs use other layouts even with this same VID/PID.
    return vendor == 0x057e && product == 0x2019 && guid.data[14] == 'h' && guid.data[15] == 12;
}

inline int register_n64_hidapi_mapping(SDL_GUID guid) {
    if (!is_n64_hidapi_guid(guid)) return 0;
    char text[33]{};
    SDL_GUIDToString(guid, text, sizeof(text));
    const std::string mapping = std::string(text) + "," + n64_hidapi_mapping;
    // SDL_AddGamepadMapping: 1 added, 0 updated, -1 error (unchanged from SDL2).
    return SDL_AddGamepadMapping(mapping.c_str()) < 0 ? -1 : 1;
}
}
