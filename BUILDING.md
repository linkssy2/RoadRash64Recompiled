For the validated experimental Linux recipe and limitations, see [Linux experimental notes](docs/LINUX_EXPERIMENTAL.md).

# Building yourself

The game executable is built ahead of time. Players of the binary release only select a ROM; these instructions are for compiling from source, on Windows or Linux.

## Prerequisites

### Windows

- 64-bit Windows and **Visual Studio 2022**, with Desktop development with C++, the x64 Windows SDK, and the **C++ Clang tools for Windows** component (ClangCL toolset).
- Git, CMake 3.20 or newer, and Python 3.11 or newer available on PATH.
- Internet access to obtain pinned open-source dependencies, SDL2 and Opus.

### Linux

- A 64-bit distro with a C++20-capable compiler (GCC or Clang), `make`, `pkg-config`, and `ninja-build`. On Fedora: `sudo dnf install gcc-c++ clang make ninja-build pkg-config`.
- Git, CMake 3.20 or newer, and Python 3.11 or newer available on PATH.
- SDL2 development package (`sudo dnf install SDL2-devel` on Fedora, `libsdl2-dev` on Debian/Ubuntu).
- Internet access to obtain pinned open-source dependencies (SDL2, Opus, and — Linux only — a minimal FFmpeg build compiled from source for custom soundtrack decoding; see `native/lib/ffmpeg` and `native/CMakeLists.txt`).
- To package a portable AppImage afterward: ImageMagick (`sudo dnf install ImageMagick`) and `curl`. No other setup needed — the AppImage tooling downloads itself.

Both platforms: your own supported Road Rash 64 USA v1.0 ROM. No ROM download is performed by these scripts.

## Obtain the source and dependencies

```bash
git clone https://github.com/linkssy2/RoadRash64Recompiled.git
cd RoadRash64Recompiled
python scripts/setup_dependencies.py
```

(Windows: use `python`, not `python3`, in a Visual Studio Developer PowerShell with the x64 environment initialized; the rest of the commands below are otherwise identical across platforms except where noted.)

`dependencies.lock.json` records the exact upstream commits, including nested dependencies. The script applies the checked-in `dependency-patches` and `dependency-overrides`; cloning only current upstream branches does not reproduce this build. Existing conflicting folders/files cause an error rather than being erased. Use a fresh source folder for a clean build.

If dependency patches change, preserve local edits and use a fresh source folder. Do not reset a modified dependency merely to force setup through.

## Stage your ROM and build the translators

### Windows

```powershell
python tools/stage_rom.py "C:\YourDumps\Road Rash 64 (USA).z64"
cmake -S tools/N64Recomp -B build/toolchain -G "Visual Studio 17 2022" -A x64 -T ClangCL
cmake --build build/toolchain --config Release --target N64RecompCLI RSPRecomp --parallel 2
```

### Linux

```bash
python3 tools/stage_rom.py "/path/to/Road Rash 64 (USA).z64"
cmake -S tools/N64Recomp -B build/toolchain -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/toolchain --target N64RecompCLI RSPRecomp --parallel "$(nproc)"
```

The staging tool normalizes supported byte order and checks the ROM hash. It writes the working ROM to ignored `build/roadrash64.us.z64`. It does not modify the original input. The built tools land at `build/toolchain/N64Recomp` and `build/toolchain/RSPRecomp` on both platforms (`.exe` suffix on Windows).

## Generate CPU and audio code locally

### Windows

```powershell
$cpu = Get-ChildItem build/toolchain -Recurse -Filter N64Recomp.exe | Select-Object -First 1
$rsp = Get-ChildItem build/toolchain -Recurse -Filter RSPRecomp.exe | Select-Object -First 1
if (!$cpu -or !$rsp) { throw "Translator build did not produce both tools" }
& $cpu.FullName config/roadrash64.us.toml
if ($LASTEXITCODE -ne 0) { throw "CPU recompilation failed" }
& $rsp.FullName config/roadrash64.us.audio_rsp.toml
if ($LASTEXITCODE -ne 0) { throw "Audio recompilation failed" }
```

### Linux

Run from the repository root, so the relative paths inside the `.toml` configs resolve correctly:

```bash
./build/toolchain/N64Recomp config/roadrash64.us.toml
./build/toolchain/RSPRecomp config/roadrash64.us.audio_rsp.toml
```

Generated code belongs under ignored `build/RecompiledFuncs`. It is excluded from the public source archive and must not be committed. The checked-in symbol map and hooks are the release inputs; do not replace them with an older generated symbol map. If you pull source changes that touch `config/roadrash64.us.toml`, the generated `funcs_*.c` bodies can go stale against updated native function signatures (a compile error citing a mismatched argument count is the usual symptom) — re-run both commands above, then rebuild.

## Compile the native application

### Windows

```powershell
cmake -S native -B native/build -G "Visual Studio 17 2022" -A x64 -T ClangCL
cmake --build native/build --config Release --target RoadRash64Recompiled RoadRash64DirectStart --parallel 2
```

The executable, frontend assets and required graphics/SDL DLLs are placed under `native/build/bin`. Run `RoadRash64Recompiled.exe` there and select your supported ROM. Keep the DLLs and assets with the EXE. `RoadRash64DirectStart.exe` uses the same configured ROM and settings.

### Linux

```bash
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12
cmake --build native/build --target RoadRash64Recompiled --parallel "$(nproc)"
```

The executable and frontend assets are placed under `native/build/bin`; run `RoadRash64Recompiled` there and select your supported ROM. Custom-soundtrack decoding (FLAC/MP3/MP4/M4A/AAC/WMA) links bundled shared libraries from a minimal FFmpeg built from the pinned source in `native/lib/ffmpeg` as part of this step — no system `ffmpeg`/`ffmpeg-devel` package is required, the AppImage bundles the required FFmpeg libraries.

Build time and memory use depend on the machine; on Linux, the first build additionally compiles that vendored FFmpeg subset, adding a couple of minutes. Do not copy a game's installed AppData/config folder (`~/.config/RoadRash64Recompiled` on Linux) into a public package; it can contain the locally stored ROM and saves.

## Packaging a portable Linux release (AppImage)

After a successful Linux build, build the native importer as described below.
Course-enabled packages require its verified bundle manifest. Then create an
AppImage for compatible Linux hosts with working Vulkan drivers:

```bash
python3 scripts/build_appimage.py --mk64-importer-bundle build/mk64-importer-bundle/bundle.json
```

This produces `native/build/RoadRash64Recompiled-x86_64.AppImage`. It downloads and caches its own packaging tools (`linuxdeploy`, `appimagetool`) under `native/build/appimage-tools/` on first run — no `sudo` needed. Test the result before distributing it:

```bash
./native/build/RoadRash64Recompiled-x86_64.AppImage
```

The script deliberately does its own post-processing pass after `linuxdeploy` runs, rather than trusting `linuxdeploy`'s bundled library copies directly — see the comment at the top of `scripts/build_appimage.py` for why (an older bundled `patchelf` corrupts libraries built with newer toolchains' compressed relative relocations, in a way that only surfaces as a segfault at launch, not at build time). If a future `linuxdeploy` release fixes that upstream, the workaround can likely be simplified.

## Validation and limitations

The release preparation used the existing Windows toolchain and regression suite. The four dependency patches were applied to clean archives of their pinned upstream revisions and compared against the working sources (ignoring line-ending differences). The release owner additionally reports a successful source build by another user on a different computer; that tester's build log and archive checksum were not supplied. A successful compilation or offline input/rendering test does not prove the absence of runtime bugs; multiplayer and the latest menu changes still require broader live testing. The Linux port and its AppImage packaging are comparatively newer and have seen less real-world testing than the Windows build.

For an initial ROM-free check after configuring:

```powershell
# Windows
cmake --build native/build --config Release --target RR64PopupInputSmoke RR64ActionBindingsSmoke RR64AchievementAudioSmoke --parallel 2
& native/build/bin/RR64PopupInputSmoke.exe
& native/build/bin/RR64ActionBindingsSmoke.exe
& native/build/bin/RR64AchievementAudioSmoke.exe
```

```bash
# Linux
cmake --build native/build --target RR64PopupInputSmoke RR64ActionBindingsSmoke RR64AchievementAudioSmoke --parallel "$(nproc)"
./native/build/bin/RR64PopupInputSmoke
./native/build/bin/RR64ActionBindingsSmoke
./native/build/bin/RR64AchievementAudioSmoke
```

Some other developer tests require locally generated fixtures or the local ROM and are not part of the player release; a few `EXCLUDE_FROM_ALL` test targets are also still Windows-only (they reference Windows-specific paths) and have not been ported. Routine diagnostic reports can be explicitly enabled in a developer session with `$env:RR64_DIAGNOSTICS='1'` (PowerShell) or `RR64_DIAGNOSTICS=1` (Linux, as an environment variable prefix); leave this unset for normal play. Never publish ROM-containing fixtures or personal logs.

## Versioning and existing mods

The application version is **1.4.5**. The mod loader retains an explicit compatibility floor of 1.0.6 from earlier internal development builds; this allows existing working mods to remain installed without pretending the application version is 1.0.6. Mods requesting newer unsupported interfaces still fail their version check.


## Package the optional MK64 importer

The course-support build option `RR64_EXPERIMENTAL_COURSE` retains its historical
name for script compatibility and now defaults ON. It embeds conversion code,
not course content. Configure it OFF for a build without optional course support.
Do not distribute a locally generated `race-packs/mk64` folder or either ROM.

For the verified Windows packaging path, use Python 3.12 in an isolated build
environment and install the pinned packages in
`scripts/mk64-importer-requirements.txt`. Build the two native helpers explicitly:

```powershell
cmake --build native/build --config Release --target rr64-mk64-contact rr64-mk64-motion --parallel 2
python -m pip install -r scripts/mk64-importer-requirements.txt
python scripts/build_mk64_importer.py --output build/mk64-importer-bundle --contact-helper native/build/bin/rr64-mk64-contact.exe --motion-helper native/build/bin/rr64-mk64-motion.exe
```

Use a new output directory for each bundle. Copy the entire resulting
`dist/rr64-mk64-importer` directory to `tools/mk64-importer` beside the game EXE,
including its `_internal` folder, native helpers and licenses. Players need none
of these build tools installed. The Mods tab offers a file picker and validates
the selected ROMs before converting all 16 tracks locally.

For Linux, use an isolated Python 3.12 environment with the same pinned packages:

```bash
cmake --build native/build --target rr64-mk64-contact rr64-mk64-motion --parallel 2
python3 -m pip install -r scripts/mk64-importer-requirements.txt
python3 scripts/build_mk64_importer.py --output build/mk64-importer-bundle --contact-helper native/build/bin/rr64-mk64-contact --motion-helper native/build/bin/rr64-mk64-motion --python-license /path/to/your/CPython/LICENSE
```

The license argument must identify the exact Python runtime's upstream notice;
it is optional when that installation already includes `LICENSE` or `LICENSE.txt`.
The AppImage packager verifies the Linux bundle manifest and installs the whole
tool inside `usr/bin/tools/mk64-importer` after dependency processing, preserving
its files and executable permissions. User-generated tracks stay outside the
read-only image. A new converter build requires full ROM-to-pack comparison
against the other supported platform; compilation alone does not prove that
online participants will generate matching course files.
See [importer architecture](docs/mk64-portable-importer.md) for stage ownership and
[source navigation](docs/RELEASE_CANDIDATE_EDITING_GUIDE.md) before editing.
