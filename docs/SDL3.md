# SDL3 port

SDL is SDL3 3.4.16 (`native/lib/SDL3`, pinned in `dependencies.lock.json`,
byte-identical to `SDL-release-3.4.16.zip`), built from source by
`native/cmake/Sdl.cmake`. On Windows it is a shared `SDL3.dll` beside the EXE,
as the stock builds shipped `SDL2.dll` (built straight into `<build>/bin`;
`-DRR64_SDL3_SHARED=OFF` links it into the EXE instead). On Linux it is a
static library linked into the executable, so the AppImage carries no SDL
library and no SDL development package is needed. SDL2 is gone: the game, RT64
and RecompFrontend call the SDL3 API directly, through the dependency patches
and overrides.

What was ported, by area:

- **Game** (`native/src`): window creation through properties (centred, no
  SDL_SysWMinfo; HWND / NSWindow come from window properties), packed version
  numbers, bool-returning init, and audio. Game audio moves from
  `SDL_QueueAudio` to an `SDL_AudioStream` bound to the default playback
  device: same S16 stereo submission, same queued-frame accounting
  (`SDL_GetAudioStreamQueued`), same prebuffer/pause/resume and resync logic;
  the 1024-frame device period is requested with
  `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES`. Voice chat capture moves to a
  recording stream (48 kHz mono S16), selected by device name as before; a
  lost named microphone is detected by its absence from the device list (SDL3
  keeps it open silently instead of reporting "stopped"). Custom WAV music uses
  `SDL_ConvertAudioSamples` in place of `SDL_AudioCVT`. The Windows audio driver
  is chosen with `SDL_HINT_AUDIO_DRIVER` (SDL3 reads `SDL_AUDIO_DRIVER`, not
  `SDL_AUDIODRIVER`).
- **Controller mappings**: the HIDAPI N64 override is redone for SDL3's raw
  order (D-pad on hat 0, Capture on b11); see the header for the ZR note. SDL3
  also reports the Apple platform as `macOS` while SDL_GameControllerDB rows
  say `Mac OS X`, so the registration filter accepts both there.
- **RecompFrontend / recompinput**: `SDL_Gamepad`, instance IDs everywhere
  (`SDL_EVENT_GAMEPAD_ADDED.which` is an instance ID now, and 0 replaces -1 as
  "no controller"), bool `SDL_RumbleJoystick`, `bool` keyboard state,
  per-window relative mouse mode following the focused window, explicit cursor
  show/hide, nanosecond sensor timestamps converted for GamepadMotion, drop
  events (`drop.data`, owned by SDL), and float wheel/motion values
  (`integer_y` keeps whole wheel notches). Saved controller profiles are keyed
  by vendor/product/version/name-CRC; if SDL3 reports a different version or
  name for a pad saved by an SDL2 build, `adopt_controller_identity()` re-keys
  the single matching saved entry so its layout follows the pad.
- **RmlUi SDL platform**: SDL3 events, cursors and keycodes, plus
  `ActivateKeyboard`/`DeactivateKeyboard` (SDL3 delivers text input only while
  it is started on a window, so text fields start/stop it).
- **RT64**: `imgui_impl_sdl3_custom` (the controller-free custom backend, ported),
  inspector event filter, application window (properties, display IDs, float
  refresh rate, borderless fullscreen via a null fullscreen mode), and plume's
  Vulkan surface/extension calls (SDL3 lists Vulkan instance extensions
  without a window). The first-window lookup uses `SDL_GetWindows()` because
  SDL3 window IDs share a counter with displays.
- **RecompFrontend's Apple support file** looks up SDL's Metal view class,
  which SDL3 registers as `SDL3_cocoametalview`; the lookup tries that name
  first.

Build settings: `SDL_SHARED=ON` on Windows (`SDL_STATIC=ON` on Linux, or on
Windows with `RR64_SDL3_SHARED=OFF`), `SDL_LIBC=ON` (same CRT as the rest of
the build), and the GPU, 2D renderer, camera and tray subsystems are off (RT64
owns rendering). SDL3's own SIMD paths stay run-time dispatched. Nothing
includes `<SDL3/SDL_main.h>`, so `main()` is never renamed and Windows
keeps `/ENTRY:mainCRTStartup`.

Verified on Linux: all ported libraries and game sources compile; the game's
objects link against static SDL3 with no SDL symbol left unresolved and no
`libSDL*` dependency; the controller mapping/binding smoke (164 checks, real
SDL3 virtual joysticks, including the HIDAPI N64 map) and the remap smoke's
five modes (real profiles, scanner and SDL polling, 267 checks) pass, as does
the controller smoke against SDL3 built as a shared library.
