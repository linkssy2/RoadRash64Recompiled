# Road Rash 64 Recompiled — Android

> **NOT compile-checked.** Nothing in this tree has been built for Android
> yet: no NDK, no Android Studio, no ROM on the machine that wrote it.
> Every file here is scaffold + probe code written against the pinned
> sources. The first real build happens on Ray's Windows machine.

## Two pieces, in order

### 1. The RT64 probe — `android/probe/` (DO THIS FIRST)

A minimal app that answers one question on a real phone: **can RT64 create
a Vulkan device and present a frame on this GPU?** No ROM, no game code.

```sh
python3 scripts/build_probe.py --install
```

Full build steps, device-test steps and pass/fail criteria:
**`docs/ANDROID_PROBE.md`**. Probe design notes: `android/probe/README.md`.

Do not build the full game before the probe passes on the Helio G81.
Mali (G52/G57) is unproven territory for this stack — the template
(AeroGauge) was only ever tested on Adreno, and Turnip is explicitly not a
Mali solution.

### 2. The full-game scaffold — `android/app/`, `cmake/Android.cmake`, `scripts/build_android.py`

Structure + build script only. `android/app/` is **not** wired into
`android/settings.gradle` and cannot be built: the native target, the
launcher, the JNI bridge and the touch controls do not exist yet. See
`android/app/README_SCAFFOLD.md`.

```sh
python3 scripts/build_android.py --rom "C:\YourDumps\Road Rash 64 (USA).z64"
# Runs: deps -> host tools -> ROM staging -> codegen, then STOPS with the
# exact native-stage commands for when the target lands.
```

## Design correction (read this first)

**Recompilation happens at BUILD time on the build machine, NOT on the
user's phone.** N64Recomp translates the ROM's MIPS code to C when the APK
is built; the phone only verifies + imports the user's own ROM, and the
pre-compiled game reads assets from it. Same model as the IGI ports: bring
your own game, no copyrighted data in the app. (Verified from AeroGauge's
`scripts/build_android.py`: the build machine needs the ROM to recompile,
the packager forbids `.z64/.v64/.n64` in the APK, and nothing on the device
runs N64Recomp, RSPRecomp or DXC.)

## Layout

| Path | What it is |
|---|---|
| `android/probe/` | RT64 probe app (Gradle module `:probe`) |
| `android/app/` | Full-game app module — scaffold only, unwired |
| `android/host-tools/` | Host-tool CMake (N64RecompCLI, RSPRecomp, file_to_c) |
| `android/build.gradle`, `settings.gradle`, `gradle.properties`, `gradlew*`, `gradle/wrapper/`, `lint.xml` | Gradle root (AGP 8.9.2, Gradle 8.12, JDK 17) |
| `cmake/Android.cmake`, `cmake/android/` | Full-game native-deps config + SDL/FreeType find modules |
| `patches/`, `scripts/dependency_patches.py` | Android patch inventory + patch engine |
| `scripts/build_probe.py` | Probe orchestrator (fetch, patch, host tools, Gradle) |
| `scripts/build_android.py` | Full-game orchestrator (stops after codegen for now) |
| `docs/ANDROID_PROBE.md` | Build + device-test docs |

## Versions (pinned, matching AeroGauge's verified template)

Gradle 8.12, AGP 8.9.2, NDK 28.2.13676358, JDK 17, minSdk 28,
targetSdk/compileSdk 35, arm64-v8a only, 16 KiB page size.
SDL `5d24957...` (= SDL 2.32.10), FreeType `42608f7...`,
AdrenoTools `8fae8ce...`. RT64 `6f1c2d99`, plume `d890ac89`
(RR64's `dependencies.lock.json` pins).

## Licence / attribution

`LICENSE`, `COPYRIGHT`, `CREDITS.md`, `LEGAL.md`,
`THIRD_PARTY_NOTICES.md`, `licenses/` and `docs/PROJECT_DETAILS.md` (which
carries the AI-assistance disclosure) are upstream files, kept intact and
unchanged. Do not strip them when rebranding the launcher.
