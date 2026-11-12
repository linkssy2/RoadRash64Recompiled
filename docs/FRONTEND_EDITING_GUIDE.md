# Frontend changes: contributor map

Paths below start at `native/lib/RecompFrontend`. Export tracked edits through
`dependency-patches/native_lib_RecompFrontend.patch`; preserve the pinned revision
and the exact-fetch setup fix. This guide concerns frontend integration only.

| Feature | Source | Important constraint |
| --- | --- | --- |
| Eject/spoke-jam bindings | `recompinput/include/recompinput/input_types.h`, `src/input_mapping.cpp`, `src/input_types.cpp`, `src/profiles.cpp` | Keep action enum names/order and selected-profile lookup together. Game-side code owns gameplay gating. |
| Rumble and focus | `recompinput/src/input_state.cpp`, `src/input_events.cpp` | Preserve command throttling, finite durations, immediate stops, controller removal checks and lock ordering. These prevent long-session slowdown and stuck vibration. |
| Controller-specific prompts | Same input files; game-side `native/src/rr64_input_prompts.cpp` | Active device tracking affects prompt display, not player ownership. |
| Popup dismissal | `recompui/src/base/ui_state.cpp`; game-side `rr64_popup_input.hpp` | Arm suppression before releasing input capture. Noninteractive notifications must not block gameplay. |
| Post-input UI updates | `recompui/include/recompui/recompui.h`, `src/base/ui_state.cpp` | Callback runs after the input batch so an opening press cannot also activate the new screen. |
| Launcher artwork | `recompui/src/base/ui_launcher.*`; game-side launcher configuration | Background replacement clears both SVG/image pointers. Keep image lifetime tied to its wrapper. |
| Graphics options | `recompui/src/config/ui_config_tab_graphics.cpp`, `src/renderer/rt64_render_context.cpp` | Keep enum conversion consistent with runtime/RT64. Fullscreen and VSync changes use the presentation lock. |
| Stylesheet loading | `recompui/src/core/ui_context.cpp` | Binary reads and size/read errors protect startup on Windows. |
| Mod installer | `recompui/src/composites/ui_mod_installer.cpp` | Archive rejection explains supported formats; it is not the texture conversion implementation. |
| UI graphics initialization | `recompui/src/renderer/ui_renderer.cpp` | Keep value-initialized descriptors and exception reporting. Progress prints are unnecessary. |
| Local player assignment and names | `recompinput/src/players.cpp`, `recompui/src/config/ui_config_page_controls.cpp`; game-side `native/src/rr64_local_players.hpp` and `main.cpp` | Keep controller slot assignment separate from display names. Detect disconnected controllers; do not infer online ownership from a local profile. |

The cleanup deliberately leaves runtime algorithms intact. Short comments should
explain timing, ownership or compatibility requirements. Further changes need
relevant input/UI tests and user verification; a successful build does not prove
controller feel, popup behavior or graphics transitions are visually correct.
