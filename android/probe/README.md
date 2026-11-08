# RR64 RT64 probe

A minimal Android app that answers one question on a real phone: **can RT64
create a Vulkan device and present a frame on this GPU?** It is the gate for
the whole Road Rash 64 Android port — if the probe fails on Ray's Mali
devices, there is no point building the full game yet.

**NO ROM needed. NO game code. NO N64Recomp. NO touch controls. NO Turnip.**

## What it does

1. **Raw-Vulkan pre-check** (surface-free): loads `libvulkan.so` via SDL,
   creates a Vulkan 1.1 instance (retries at 1.0), enumerates the first
   physical device and queries exactly the features RT64 needs:
   `descriptorBindingPartiallyBound` + `descriptorBindingVariableDescriptorCount`
   + `runtimeDescriptorArray`, and `scalarBlockLayout`. Reports the device
   name, API version and each feature, so a failure names the missing piece.
2. **RT64 setup**: creates an SDL window (`SDL_WINDOW_VULKAN`), builds an
   `RT64::Application::Core` with a stub 4 MiB RDRAM and stub VI registers
   (a visible 320x~124 16-bit framebuffer — the presented test frame is a
   black frame), constructs `RT64::Application` with
   `useConfigurationFile=false, detectDataPath=false`, forces Vulkan +
   console presentation mode, and calls `app->setup(0)` — the same call
   RecompFrontend's `RT64Context` makes for the real game. Reports the
   `SetupResult` and, on success, the device description and capabilities.
3. **Frame 1**: `app->updateScreen()` + waits for the present to complete.
4. **Swapchain recreate**: resizes the SDL window to 960x540, presents
   frame 2, reports the swapchain size before/after.

## Pass / fail

- **PASS** = RT64 setup succeeded AND frame 1 presented (frame 2 too).
  The screen shows `RESULT: PASS`.
- **FAIL** = anything else, with the reason on screen: SDL init failure, no
  Vulkan library, `GraphicsDeviceNotFound` (with the raw pre-check lines
  above saying whether the device is missing entirely or just lacks
  descriptor indexing / scalar block layout), or a freeze after "frame 1
  submitted" (present never completed).

## Build

```sh
python3 scripts/build_probe.py          # fetch, patch, host tools, Gradle
python3 scripts/build_probe.py --install  # + install on the connected device
```

Full prerequisites, commands and device-test steps: `docs/ANDROID_PROBE.md`.

## Layout

- `src/main/cpp/probe.cpp` — the whole probe (SDL_main, raw pre-check,
  stub core, RT64 setup, two frames).
- `src/main/java/.../ProbeActivity.java` — SDLActivity + on-screen report
  overlay (large monospace text; Ray debugs via screenshots).
- `CMakeLists.txt` — SDL2 + RT64 static + `main` shared lib.
- `cmake/FindSDL2.cmake` — lets RT64's `find_package(SDL2)` use our SDL.
- `host-tools/CMakeLists.txt` — host-built `file_to_c` only.
- `patches/` — Android boundary patches, verified to apply on top of RR64's
  own patches at the pinned commits (see `patches/README.md`).

## Deliberate deviations from the real game path

- `detectDataPath=false`: `RT64Context` leaves it on; the probe must not go
  looking for a data dir on the phone.
- RR64's own `native_lib_rt64.patch` and plume patch (frame pacing,
  present-queue / swapchain-policy changes) **are** applied to the probe
  tree, via `scripts/setup_dependencies.py`, before the Android patches:
  the probe must test the renderer the game will actually ship, and the
  Android patches in `patches/` were verified on top of exactly that
  stack. RR64's N64ModernRuntime / RecompFrontend patches are *not*
  needed for the probe (no runtime, no frontend in the probe) and are not
  applied to anything the probe links — but they are fetched by
  `setup_dependencies.py` anyway, so the full-game tree is ready.
- No RecompFrontend: the probe constructs `RT64::Application` directly.
  `RT64Context` is a thin wrapper around exactly this construction.

## Known quirks / risks (not compile-checked — no NDK here)

- **Vulkan 1.1 vs 1.2**: the manifest gates on Vulkan 1.1, but plume
  requests a 1.2 instance (`plume_vulkan.cpp:4478`). A strict 1.1-only
  driver may fail instance creation or run lower. The probe reports the
  outcome; the Poco M6 Pro (Vulkan 1.1.177) is the risky device.
- **RT64 hardcodes `ANDROID_PLATFORM android-24` / `ANDROID_ABI arm64-v8a`**
  in its own `CMakeLists.txt` (plain `set()`, shadows the toolchain value
  inside the RT64 directory). Harmless for the probe as far as can be told
  here; if the NDK build misbehaves, that is the first place to look.
- **RT64 builds `texture_hasher` / `texture_packer` executables** for the
  target as part of `all`. They are never run at build time, but if they
  fail to cross-compile, gate them out of the probe build (they are not
  needed for device creation).
- **SDL2main is deliberately not linked** (SDLActivity calls `SDL_main`
  directly). If the link fails with an undefined `SDL_main` reference, add
  `SDL2main` back in `CMakeLists.txt`.
- **If the probe freezes after "frame 1 submitted"**, the present never
  completed — report that as the failure (do not just say "it hung").
