# Android — RT64 probe build + device test, and the full-game scaffold

> **NOT compile-checked.** No NDK, no Android Studio, no ROM on the machine
> that wrote this. Everything below is written against the pinned sources
> and verified as far as it can be without a build (patch application,
> API signatures, file structure). The first real build happens on Ray's
> Windows machine. If a step fails there, the failure itself is the next
> piece of information — send the failing command + its output back.

## 0. The one-line design correction

**Recompilation (N64Recomp MIPS → C → native ARM64) happens at BUILD time
on the build machine, NOT on the user's phone; the phone only verifies +
imports the ROM and the pre-compiled game reads assets from it.**

## 1. What the probe is

`android/probe/` is a minimal Android app that creates a Vulkan device
through RT64's own setup path and presents test frames — with **no ROM and
no game code**. It is the Mali gate for the whole port: if RT64 cannot
create a device and present on the Helio G81 / Poco M6 Pro, there is no
point building the full game yet.

What it does, in order (`android/probe/src/main/cpp/probe.cpp`):

1. **Raw-Vulkan pre-check** (no surface): loads `libvulkan.so` via SDL,
   creates a Vulkan 1.1 instance (retries at 1.0), enumerates the first
   physical device and queries the features RT64 needs —
   `descriptorBindingPartiallyBound` + `descriptorBindingVariableDescriptorCount`
   + `runtimeDescriptorArray`, and `scalarBlockLayout` — and reports the
   device name, API version and each feature.
2. **RT64 setup**: SDL window (`SDL_WINDOW_VULKAN`) → stub
   `RT64::Application::Core` (4 MiB zeroed RDRAM, stub VI registers for a
   visible 320x~124 16-bit framebuffer) → `RT64::Application` with
   `useConfigurationFile=false, detectDataPath=false`, Vulkan forced,
   console presentation mode → `app->setup(0)`. This is the same call
   RecompFrontend's `RT64Context` makes for the real game. Reports the
   `SetupResult` and, on success, device description + capabilities.
3. **Frame 1**: `app->updateScreen()` + waits for the present to complete.
4. **Swapchain recreate**: resizes the SDL window to 960x540, presents
   frame 2, reports the swapchain size before/after.

Everything is reported **on screen** (large monospace text — Ray debugs
via screenshots) **and** to logcat (tag `RR64Probe`).

**Pass** = RT64 setup succeeded AND frame 1 presented (frame 2 too).
The screen shows `RESULT: PASS`.

**Fail** = anything else, with the reason on screen: SDL init failure, no
Vulkan library, `GraphicsDeviceNotFound` (the raw pre-check lines above it
say whether the device is missing entirely or just lacks descriptor
indexing / scalar block layout), or a freeze after "frame 1 submitted"
(the present never completed).

No Turnip import in the probe — Turnip is Adreno-only and is not a Mali
solution, so it cannot be the fallback for Ray's Mali devices. It is
listed as a future fallback for Adreno devices only, in the full game.

## 2. Prerequisites (Ray's Windows build machine)

- Windows 10/11, 64-bit. (Linux works too — same commands with
  `python3`/`./gradlew`; the Windows variants are given first.)
- **Git** (any recent version).
- **Python 3.11+** (`python` on Windows).
- **CMake 3.22+** and **Ninja** (in `PATH`; Android Studio bundles both,
  or install separately).
- **JDK 17** (Android Studio's bundled JDK is fine; Gradle 8.12 + AGP
  8.9.2 require JDK 17).
- **Android Studio** (any recent version) with:
  - SDK Platform **Android 35** (`platforms;android-35`)
  - Build-tools **35.0.0** (`build-tools;35.0.0`)
  - **NDK 28.2.13676358** (`ndk;28.2.13676358`)
  - `ANDROID_HOME` set to the SDK dir (Android Studio sets this; or set
    it manually and pass `--sdk <path>`).
- **Host C/C++ compiler** for the host tools (`file_to_c`, N64Recomp):
  MinGW-w64 GCC/G++ on Windows (`gcc.exe`/`g++.exe` in `PATH`), system
  GCC/G++ on Linux.
- A phone with **USB debugging** on, for `--install` / `adb`.
- **NO ROM needed for the probe.** Do not stage, copy or commit any ROM
  for this step.

## 3. Build the probe — exact steps

Run from the repo root, on the `feat/rr64-android-probe` branch, after the
fork exists (see §7 for the push commands).

```bat
:: 1. Clone the fork (first time only) and check out the branch
git clone https://github.com/rayishere0/RoadRash64Recompiled.git
cd RoadRash64Recompiled
git checkout feat/rr64-android-probe

:: 2. Build the probe APK (fetch deps, apply patches, host tools, Gradle)
python scripts/build_probe.py

:: 3. Install on the connected phone (or pass --install in step 2)
python scripts/build_probe.py --install
```

What step 2 does, in order (all on the **build machine**):

1. `python scripts/setup_dependencies.py` — fetches every pinned
   dependency from `dependencies.lock.json` (RT64 `6f1c2d99`, plume
   `d890ac89`, N64ModernRuntime, RecompFrontend, tools, …) and applies
   RR64's own `dependency-patches/` + `dependency-overrides/`.
2. Applies the Android boundary patches (`android/probe/patches/`) on top:
   `0019` + `0023` into `native/lib/rt64`, `0020` into
   `native/lib/rt64/src/contrib/plume`. Idempotent — re-running the script
   skips patches that are already applied.
3. Fetches SDL at the AeroGauge-verified pin (`5d24957...` = SDL 2.32.10)
   into `build-probe/deps/SDL`.
4. Builds host `file_to_c` (x86_64) into `build-probe/host/bin`.
5. Runs Gradle `:probe:assembleDebug` (NDK cross-compile, arm64-v8a).
6. Verifies the APK (`lib/arm64-v8a/libmain.so`, `libSDL2.so`,
   `libc++_shared.so` present; no `.z64/.v64/.n64/.mpk/.pak` inside) and
   copies it to `dist/rr64-rt64-probe-debug.apk`.

On Linux, replace `python` with `python3` and `gradlew.bat` with
`./gradlew` (the script does this itself); the `scripts/build_probe.py`
invocation is the same. Re-running the script is safe (fetches and
patches are idempotent; pass `--skip-deps` to reuse already-fetched
trees).

If the build fails: send back the failing command and the last ~50 lines
of its output. The most likely first-failure points (not compile-checked
here) are listed in `android/probe/README.md` under "Known quirks / risks".

## 4. Device test — exact steps

### Devices

- **Primary: Helio G81** (Mali-G52 MC2, Vulkan 1.3.278) — the probe must
  pass here for the port to proceed.
- **If available: Poco M6 Pro** (Helio G99, Mali-G57 MC2, Vulkan 1.1.177)
  — the risky one (see the 1.1-vs-1.2 question below). A pass here is a
  bonus; a fail here with a pass on the G81 still lets the port proceed
  for 1.2+ devices, but must be recorded.

### Steps

1. Install `dist/rr64-rt64-probe-debug.apk` on the phone (via
   `--install`, or `adb install -r dist/rr64-rt64-probe-debug.apk`).
2. Open the **RR64 RT64 Probe** app. It opens in landscape and fills the
   screen with report text on a black background. Wait for it to finish:
   it runs the pre-check, RT64 setup, frame 1, resize, frame 2, then
   shows `RESULT: PASS` (or `RESULT: FAIL ...`) and waits. The whole run
   takes seconds.
3. **Take a screenshot of the probe screen.**
4. Press Back (or close the app) to exit.

### What to send back

- **If PASS:** the screenshot is enough. (The on-screen text already
  contains the device name, Vulkan version, features and swapchain sizes.)
- **If FAIL:** the screenshot **plus** logcat, captured while the probe
  runs (or right after, before launching anything else):
  ```sh
  adb logcat -s RR64Probe > probe-fail.log
  ```
  (Run that command first, then open the probe; Ctrl+C after it fails.)
  If the probe **freezes** (no `RESULT:` line, screen stuck on
  "frame 1 submitted"), say so explicitly — a frozen present is itself
  the failure, and the logcat up to the freeze is what matters.
- Also say which phone it was (G81 or Poco) — the two devices answer
  different questions.

### Open question the test answers: Vulkan 1.1 vs 1.2

The manifest gates on Vulkan **1.1** (`0x00401000`), but plume requests a
**1.2** instance (`plume_vulkan.cpp:4478`, `appInfo.apiVersion =
VK_API_VERSION_1_2`). On the G81 (Vulkan 1.3) this should be fine. On a
strict 1.1-only driver (Poco, Vulkan 1.1.177) instance creation may fail
or run at a lower version — AeroGauge never tested a 1.1-only device. The
probe reports the raw instance-creation result, so the test settles it:
if the Poco fails at instance creation while the G81 passes, the fix is a
plume patch lowering the request (or a higher manifest gate), not a
redesign.

## 5. Full-game scaffold — what exists and what happens after the probe

### What exists now (structure + build script only)

- **Gradle/NDK/CMake scaffold** in `android/` (Gradle root: AGP 8.9.2,
  Gradle 8.12, JDK 17, NDK 28.2.13676358, minSdk 28, targetSdk/compileSdk
  35, arm64-v8a only) + `cmake/Android.cmake` (SDL/FreeType/AdrenoTools
  from source, host `file_to_c`, host DXC, 16 KiB page size) +
  `cmake/android/FindSDL2.cmake` / `FindFreetype.cmake`.
- **Build script** `scripts/build_android.py` (modelled on AeroGauge's):
  dependency fetch via `dependencies.lock.json` pins, host toolchain
  build (N64RecompCLI/RSPRecomp/file_to_c), ROM staging via
  `tools/stage_rom.py`, codegen, then NDK cross-compile. It currently
  runs everything through codegen and then **stops** with the exact
  native-stage commands, because the native target does not exist yet.
- **Minimal gated changes to shared CMake** (desktop build untouched):
  - `native/cmake/Dependencies.cmake`: `-march=nehalem` gated out on
    Android/ARM64 (both the exe and `RecompiledFuncs`); Linux FFmpeg
    `ExternalProject` gated off on Android (custom-soundtrack decoding
    disabled for v1 — game audio does not need FFmpeg); SDL2 for Android
    comes from `cmake/Android.cmake` instead of `find_package(SDL2)`; DXC
    for Android is the host binary (`RR64_HOST_DXC`).
  - `native/cmake/Application.cmake`: same `-march=nehalem` gating;
    `nfd` not linked on Android (no nativefiledialog on Android — file
    dialogs belong to the Java launcher); FFmpeg dependency replaced by
    plain `libz` on Android.
  - Android configure passes `-DRR64_BUILD_TESTS=OFF` (smoke tests are
    desktop-only, some Windows-only) and `-DRR64_EXPERIMENTAL_COURSE=OFF`
    for v1 (removes the MK64 importer + its Python tool dependency).
- **Patch inventory** `patches/series.json` + `scripts/dependency_patches.py`
  (AeroGauge's patch engine): `0019` (RT64 Android, adapted),
  `0020` (plume SDL window), `0021` (SDL USB receiver, API 33+),
  `0022` (RecompFrontend Android, adapted), `0023` (Adreno endian swap,
  all platforms). All five were verified to apply cleanly on top of
  RR64's own patches at the pinned commits — see
  `android/probe/patches/README.md` and `patches/README.md`.

### What does NOT exist yet (full-port work, after the probe passes)

Launcher UI (Java `LauncherActivity` + SAF ROM picker), ROM import +
hash check, Turnip driver import, touch controls (redesign for Road Rash:
throttle/brake/punch/kick/weapon — not AeroGauge's stick + A/B/Z layout),
save handling (Controller Pak, not EEPROM — see below), JNI bridge +
Vulkan loader (`src/android/`), and the native-target conversion
(`RoadRash64Recompiled` executable → shared `main` library, Android
entry point, Android `WindowHandle`, `--skip-launcher`/`--auto-rom` as the
normal launch path). `android/app/README_SCAFFOLD.md` lists every item.

### Build order for the full game (when the target lands)

```bat
:: ROM is required on the BUILD machine only, never committed.
python scripts/build_android.py --rom "C:\YourDumps\Road Rash 64 (USA).z64"
:: 1. setup_dependencies.py (pinned deps + RR64 patches)
:: 2. Android deps (SDL / FreeType / AdrenoTools) + Android patches
:: 3. Host tools: N64RecompCLI, RSPRecomp, file_to_c
:: 4. tools/stage_rom.py verifies the ROM -> build/roadrash64.us.z64
:: 5. N64Recomp config/roadrash64.us.toml + RSPRecomp
::    config/roadrash64.us.audio_rsp.toml  (build-time recompilation)
:: 6. NDK cross-compile (arm64-v8a) + Gradle package + APK payload checks
```

## 6. File-picker model for the later full port (do not build it now)

Same "bring your own game, no copyrighted data in the app" model as the
IGI ports. The launcher fires an Android Storage Access Framework picker
(`Intent.ACTION_OPEN_DOCUMENT`, no storage permission needed) for the
user's `.z64` ROM, then verifies it, then atomically imports it to
app-private storage (`getFilesDir()`), and the game reads assets from the
imported ROM at runtime (the recompiled code reads from the in-memory ROM
image via RecompFrontend/librecomp, exactly like the desktop path).

**Hashes — which one the full port should use, and why.** There are three
hashes in play and they are not interchangeable:

- **Stage gate (build machine): SHA-1
  `87727a298f583ec8325f5655088ff21e37b335b2`** (`tools/rr64_common.py`,
  `is_supported()` — 32 MiB size + this SHA-1). The launcher import check
  should use this same gate: it is the project's own definition of "the
  supported ROM", it matches `tools/stage_rom.py`, and a user-facing
  "unsupported ROM" error is clearest when the import check and the build
  check agree.
- **Runtime check: XXH3-64 `0x517F53BCD9D13BF2ULL`**
  (`native/src/main.cpp:144`, `kRoadRash64UsXxh3`, registered as the
  game's `rom_hash`). The runtime (`recomp::load_stored_rom` /
  `check_stored_rom`) re-checks this when loading the stored ROM and
  deletes it if it is wrong. The launcher does not need to compute it —
  the runtime already does.
- **SHA-256 / MD5 are recorded, not gated** (`tools/rr64_common.py`):
  do not use them for the import check.

So: **import checks size (32 MiB, `0x02000000`) + SHA-1** (with the same
byte-order normalisation as `stage_rom.py`: `80 37 12 40` = z64
pass-through, `37 80 40 12` = v64 16-bit swap, `40 12 37 80` = n64 32-bit
word swap), and the runtime's XXH3-64 check is the second line of defence.
Do **not** copy AeroGauge's SHA-256: AeroGauge's runtime checked SHA-256,
RR64's checks XXH3-64 — copying the hash without the runtime that checks
it would be security theatre, and the SHA-1 gate is the one the project
itself defines.

Saves: Road Rash 64 uses a **Controller Pak**, not cartridge EEPROM/SRAM
(`SaveType::None` in `main.cpp`; the pak implementation is the rewritten
`librecomp/src/pak.cpp` in the N64ModernRuntime patch). There is no `.sav`
/ `.eep` file to manage — save data lives in the runtime's virtual
Controller Pak under the config dir. The full port must confirm the exact
filename when the runtime is fetched and make sure the Android config dir
persists it.
