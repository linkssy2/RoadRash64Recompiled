# android/app/ — full-game module SCAFFOLD (not wired in, not buildable)

This directory holds the **shape** of the full-game Android app module so
the port after the probe gate does not start from zero. It is deliberately
**not** included in `android/settings.gradle` and must not be built yet.

## What exists here

- `build.gradle` — the real app-module config, adapted from AeroGauge
  (AGP 8.9.2, NDK 28.2.13676358, compileSdk/targetSdk 35, minSdk 28,
  arm64-v8a only, version from `RR64_VERSION`, release signing from env,
  `useLegacyPackaging=true` for the AdrenoTools Turnip hooks).
- `src/main/AndroidManifest.xml` — intended manifest (Vulkan 1.1 feature
  gate, `LauncherActivity`, `GameActivity` in a separate `:game` process).

## What is missing (full-port work, AFTER the probe passes)

- `src/main/java/.../LauncherActivity.java` — SAF ROM picker, ROM import +
  hash check, Turnip driver import, Play flow (adapt AeroGauge's).
- `src/main/java/.../GameActivity.java` — SDLActivity + Turnip loader env +
  touch overlay host.
- `src/main/java/.../RomImport.java` — 32 MiB size + byte-order
  normalisation + **SHA-1** gate (see `docs/ANDROID_PROBE.md`, "File-picker
  model" — the runtime re-checks XXH3-64; do NOT copy AeroGauge's SHA-256).
- `src/main/java/.../DriverImport.java`, `TouchControls.java`,
  `SaveTransfer.java` — adapt from AeroGauge (touch controls are a
  redesign for Road Rash: throttle/brake/punch/kick/weapon).
- `src/android/` native JNI bridge + Vulkan loader (adapt
  `aero_android.cpp` / `aero_vulkan_loader.cpp`).
- Native target: `native/cmake/Application.cmake`'s `RoadRash64Recompiled`
  executable must become a shared library named `main` (SDLActivity loads
  `libmain.so`), `SDL_MAIN_HANDLED`/`main()` must become the Android entry,
  `create_window()` needs the Android `WindowHandle`, `NFD_Init` goes away,
  `--skip-launcher`/`--auto-rom` become the normal launch path driven by
  the Java picker. `cmake/Android.cmake` (repo root `cmake/`) is the
  native-deps config for that step; `native/CMakeLists.txt` needs
  `if(ANDROID) include(cmake/Android.cmake)` added then.
- Launcher icon (reuse `native/assets/RoadRashIcon.png`).

## Promotion checklist (when the probe passes)

1. Implement the items above.
2. Add `include ':app'` to `android/settings.gradle`.
3. `python3 scripts/build_android.py` (see `docs/ANDROID_PROBE.md`).
