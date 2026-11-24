# macOS build (Apple Silicon)

`build_mac.sh` in the repository root builds `RoadRash64Recompiled.app` for
Apple Silicon Macs (M1 and newer, macOS 13 Ventura or newer). It fetches what
it needs, builds the game and signs the bundle. The build itself can run on
any Mac that has the required Xcode, Intel Macs included: an Intel Mac
cross-compiles the Apple Silicon app.

```bash
./build_mac.sh "/path/to/Road Rash 64 (USA).z64"
```

The finished app is `native/build-mac/RoadRash64Recompiled.app`.

## What you need on the Mac doing the build

- **Xcode 16.3 or newer** (needs macOS 15 Sequoia), opened once so it finishes
  installing. Older Xcode versions ship a C++ library without the
  `std::jthread` support the game uses; the configure step stops with a clear
  message if the Xcode is too old.
- The **Metal toolchain**. Newer Xcode versions download it separately; if the
  script reports it missing, run
  `xcodebuild -downloadComponent MetalToolchain` and try again. The Command
  Line Tools alone are not enough: the Metal shader compiler only comes with
  Xcode.
- Git and Python 3.9 or newer (both come with Xcode's command line tools).
- CMake 3.20+ and Ninja. If they are missing and Homebrew is installed, the
  script installs them with `brew install cmake ninja`.
- Your own Road Rash 64 USA v1.0 ROM. Nothing is downloaded for it.

## Options

| Option | Meaning |
| --- | --- |
| `--regenerate` | Regenerate the recompiled CPU and audio code even if it looks current |
| `--clean` | Delete the build folder first |
| `--sign "Developer ID Application: ..."` | Sign with your identity instead of ad-hoc |
| `--jobs N` | Parallel compile jobs |
| `--no-deps` | Skip `scripts/setup_dependencies.py` (offline rebuilds) |
| `--zip` | Also write a zip of the app next to it |

## What the script does

1. Checks the Mac: Xcode, Metal compiler, git, Python, CMake and Ninja.
2. Runs `scripts/setup_dependencies.py`, which clones every pinned dependency
   (including **SDL3 3.4.16** and, on macOS only, **FreeType 2.13.3**) and
   applies the dependency patches.
3. Stages the ROM and runs N64Recomp and RSPRecomp. This is skipped on later
   runs unless the ROM, the recompiler configuration, or `--regenerate` says
   otherwise.
4. Configures and builds `native/build-mac/arm64`. CMake's bundling step
   copies SDL3 and FFmpeg into the app and signs it (nested libraries first,
   then the app with the hardened runtime and
   `cmake/macos/entitlements.plist`).
5. Copies the app to `native/build-mac/` and verifies it: arm64 everywhere,
   every binary runs on macOS 13, every library loads from inside the bundle,
   and the signature is valid.

## CPU and GPU

- **CPU:** arm64 with `-mcpu=apple-m1` (NEON). x86 SSE intrinsics in the
  runtime map to NEON through sse2neon. `-ffp-contract=off` keeps the game
  simulation free of fused multiply-adds, so results match the Windows and
  Linux builds (see [SIMD.md](SIMD.md)).
- **GPU:** RT64's Metal renderer with Tier 2 argument buffers and bindless
  textures. Every Apple Silicon Mac can run macOS 13 Ventura, and from there
  on all of their GPUs are in the Metal 3 family, so the floor of macOS 13
  gives every player the same path: resource IDs and GPU buffer addresses
  written straight into the argument buffers. On macOS 15 and newer, plume
  also keeps all resources in a Metal residency set.

## SDL3 on macOS

SDL3 builds as `libSDL3.0.dylib` and sits in
`RoadRash64Recompiled.app/Contents/Frameworks`, which is the Mac form of
`SDL3.dll` beside the EXE. The executable loads it through
`@executable_path/../Frameworks`, so a newer SDL3 3.x arm64 dylib can be
dropped in without rebuilding; re-sign the app afterwards
(`codesign --force --deep -s - RoadRash64Recompiled.app` for an ad-hoc build).
Controllers are read through IOKit; SDL's libusb path is off so a Homebrew
libusb on the build Mac cannot end up in the app. `-DRR64_SDL3_SHARED=OFF`
links SDL3 into the executable instead. See [SDL3.md](SDL3.md).

## Other Mac-specific build pieces

- **Build-host tools.** RT64 converts its shaders with two small helper
  programs while building. `native/cmake/macos/HostTools.cmake` builds them for
  the Mac doing the build, so an Intel Mac can build the Apple Silicon app.
- **libc++ portability.** Two pieces of game code did not compile with
  Apple's libc++: `std::atomic<std::shared_ptr>` (music and online terrain;
  now `rr64::AtomicSharedPtr` in `native/src/rr64_atomic_shared_ptr.hpp`, which
  is still the standard type on Windows and Linux) and JSON parsed from
  `unsigned char` buffers (race packs; libc++ 19 dropped the generic
  `std::char_traits`, so the bytes are now handed to the parser as `char`).
- **FreeType** is built from source and linked statically for RmlUi, so the app
  does not depend on a Homebrew FreeType.
- **FFmpeg** (custom soundtrack decoding) builds as small dylibs in
  `Contents/Frameworks`, cross-compiled for arm64 on an Intel build Mac.
- **DXC** comes from `native/cmake/macos/DxcMac.cmake`; the RT64 shaders become
  Metal libraries for macOS 13.
- Files the game writes (config, saves, the custom music folder) go to
  `~/Library/Application Support/RoadRash64Recompiled`; the runtime log is
  `~/Library/Logs/RoadRash64Recompiled-runtime.log`.

## First launch

An ad-hoc signed app made on one Mac runs on that Mac. Copied to another Mac,
Gatekeeper blocks it: right-click the app and choose **Open**, or run
`xattr -dr com.apple.quarantine RoadRash64Recompiled.app`. Distributing it
widely needs a Developer ID signature (`--sign`) and notarisation.

## Limits

- Intel Macs are not a target for now. RT64 binds every texture, sampler and
  buffer through Metal argument buffers, and the Metal compiler of older AMD
  drivers (tested: GCN FirePro) crashes on any shader that uses one, even
  though the GPU reports Tier 2 support. Supporting such GPUs would need a
  second, direct-binding path through RT64's Metal renderer.
- If a GPU driver cannot compile RT64's pipelines, the game names the failing
  shaders in `~/Library/Logs/RoadRash64Recompiled-gpu.log`, shows an alert and
  quits instead of hanging at a black window. Set
  `PLUME_METAL_CONTINUE_ON_PIPELINE_FAILURE=1` to keep running and log every
  failing pipeline.
- The optional MK64 course importer (`tools/mk64-importer`) is not bundled by
  `build_mac.sh` yet. The game looks for it at
  `RoadRash64Recompiled.app/Contents/Resources/tools/mk64-importer/rr64-mk64-importer`.
