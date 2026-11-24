#!/usr/bin/env bash
# Road Rash 64 Recompiled: one-command macOS build.
#
#   ./build_mac.sh "/path/to/Road Rash 64 (USA).z64"     first run (stages the ROM)
#   ./build_mac.sh                                       later runs
#
# Produces native/build-mac/RoadRash64Recompiled.app for Apple Silicon Macs
# (arm64, macOS 13 Ventura or newer). It builds on any Mac, Intel included:
# an Intel Mac cross-compiles the Apple Silicon app.
#
# What it does, in order:
#   1. Checks the Mac: Xcode with its Metal compiler, CMake, Ninja, Python 3,
#      Git. Missing CMake/Ninja are installed with Homebrew when it is present.
#   2. Fetches every pinned dependency (scripts/setup_dependencies.py):
#      SDL3 3.4.16, FFmpeg, FreeType 2.13.3, RT64, N64ModernRuntime,
#      RecompFrontend, ... at the commits in dependencies.lock.json, with this
#      release's patches applied.
#   3. Stages your ROM, builds the two translators (N64Recomp, RSPRecomp) for
#      this Mac, and generates the game code (only when missing or stale).
#   4. Builds the app for arm64 (Apple M1 baseline, NEON; RT64 uses Metal 3
#      features on the GPUs that have them): a complete, signed .app with
#      SDL3 as libSDL3.0.dylib in Contents/Frameworks (the Mac form of
#      SDL3.dll beside the EXE), the decode-only FFmpeg dylibs beside it,
#      and the assets in Contents/Resources.
#
# Options:
#   --regenerate                    re-run N64Recomp/RSPRecomp even if code exists
#   --clean                         delete the macOS build folder first
#   --sign IDENTITY                 codesign identity (default "-", ad hoc), e.g.
#                                   "Developer ID Application: Your Name (TEAMID)"
#   --jobs N                        parallel build jobs (default: all cores)
#   --no-deps                       skip fetching (offline rebuilds)
#   --zip                           also write RoadRash64Recompiled-macOS.zip
#   -h, --help                      this text
#
# Requirements: Xcode 16.3 or newer (so macOS 15 Sequoia or newer on the build
# Mac), selected with xcode-select, with its Metal compiler (on Xcode 26 run
# `xcodebuild -downloadComponent MetalToolchain` once); internet access for
# the first run; your own supported Road Rash 64 USA v1.0 ROM. No ROM is
# downloaded.
#
# Intel Macs are not a target for now: RT64 needs Metal argument buffers,
# which the Metal compiler of older AMD GPU drivers cannot compile.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

APP_NAME="RoadRash64Recompiled"
ARCH="arm64"
MIN_MACOS="13.0"   # Ventura: every Apple Silicon GPU is Metal 3 from here on
OUT_ROOT="$ROOT/native/build-mac"
STAGED_ROM="$ROOT/build/roadrash64.us.z64"
GENERATED_MARKER="$ROOT/build/RecompiledFuncs/funcs.h"

REGENERATE=0
CLEAN=0
SIGN_IDENTITY="-"
JOBS=""
RUN_DEPS=1
MAKE_ZIP=0
ROM_PATH=""

# ---------------------------------------------------------------- output
if [ -t 1 ]; then
    BOLD=$'\033[1m'; RED=$'\033[31m'; YELLOW=$'\033[33m'; GREEN=$'\033[32m'; RESET=$'\033[0m'
else
    BOLD=""; RED=""; YELLOW=""; GREEN=""; RESET=""
fi
step() { printf '\n%s==> %s%s\n' "$BOLD" "$*" "$RESET"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '%swarning:%s %s\n' "$YELLOW" "$RESET" "$*" >&2; }
die()  { printf '\n%serror:%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }
usage() { sed -n '2,/^set -euo pipefail/p' "$0" | sed -e '/^set -euo/d' -e 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------- arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --regenerate)  REGENERATE=1; shift ;;
        --clean)       CLEAN=1; shift ;;
        --sign)        [ $# -ge 2 ] || die "--sign needs an identity"; SIGN_IDENTITY="$2"; shift 2 ;;
        --sign=*)      SIGN_IDENTITY="${1#*=}"; shift ;;
        --jobs)        [ $# -ge 2 ] || die "--jobs needs a number"; JOBS="$2"; shift 2 ;;
        --jobs=*)      JOBS="${1#*=}"; shift ;;
        --no-deps)     RUN_DEPS=0; shift ;;
        --zip)         MAKE_ZIP=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        -*)            die "unknown option: $1 (see --help)" ;;
        *)             [ -z "$ROM_PATH" ] || die "only one ROM path, please (got '$ROM_PATH' and '$1')"
                       ROM_PATH="$1"; shift ;;
    esac
done

# ---------------------------------------------------------------- 1. the Mac
step "Checking this Mac"
[ "$(uname -s)" = "Darwin" ] || die "build_mac.sh runs on macOS. On Windows and Linux follow BUILDING.md."

HOST_ARCH="$(uname -m)"
MACOS_VERSION="$(sw_vers -productVersion)"
info "macOS $MACOS_VERSION on $HOST_ARCH ($(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown CPU))"
if [ "$HOST_ARCH" != "arm64" ]; then
    info "This Mac builds the Apple Silicon app by cross-compiling; the app runs on Apple Silicon Macs only."
fi

if [ -z "$JOBS" ]; then
    JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi
case "$JOBS" in ''|*[!0-9]*) die "--jobs must be a number (got '$JOBS')" ;; esac

DEVELOPER_DIR_PATH="$(xcode-select -p 2>/dev/null || true)"
case "$DEVELOPER_DIR_PATH" in
    *CommandLineTools*)
        die "xcode-select points at the Command Line Tools ($DEVELOPER_DIR_PATH).
The Metal shader compiler only comes with the full Xcode. Install Xcode, then run:
    sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" ;;
esac
if [ -z "$DEVELOPER_DIR_PATH" ] || [ ! -d "$DEVELOPER_DIR_PATH" ]; then
    die "No Xcode found. Install Xcode (App Store or developer.apple.com), then run:
    sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
    sudo xcodebuild -license accept"
fi
info "$(xcodebuild -version 2>/dev/null | tr '\n' ' ')($DEVELOPER_DIR_PATH)"
if ! xcrun -sdk macosx --find metal >/dev/null 2>&1 || ! xcrun -sdk macosx metal -v >/dev/null 2>&1; then
    die "Xcode's Metal compiler is not available. On Xcode 26 or newer it is a separate download:
    xcodebuild -downloadComponent MetalToolchain
Then check it with:  xcrun -sdk macosx metal -v"
fi
xcrun -sdk macosx --find metallib >/dev/null 2>&1 || die "xcrun cannot find metallib (part of Xcode's Metal toolchain)."

command -v git >/dev/null 2>&1 || die "git is missing (it comes with Xcode; check xcode-select)."

PYTHON="$(command -v python3 || true)"
[ -n "$PYTHON" ] || die "python3 is missing (it comes with Xcode; check xcode-select)."
"$PYTHON" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' \
    || die "Python 3.9 or newer is needed ($PYTHON is $("$PYTHON" -V 2>&1))."

version_ge() {  # version_ge 3.23.1 3.23 -> true
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -t. -k1,1n -k2,2n -k3,3n | head -n1)" = "$2" ]
}
need_brew=""
if ! command -v cmake >/dev/null 2>&1; then
    need_brew="cmake"
elif ! version_ge "$(cmake --version | sed -n '1s/[^0-9]*\([0-9][0-9.]*\).*/\1/p')" "3.20"; then
    need_brew="cmake"
fi
command -v ninja >/dev/null 2>&1 || need_brew="$need_brew ninja"
if [ -n "$need_brew" ]; then
    if command -v brew >/dev/null 2>&1; then
        step "Installing build tools with Homebrew:$need_brew"
        # shellcheck disable=SC2086
        brew install $need_brew
    else
        die "Needed:$need_brew (CMake 3.20 or newer, and Ninja).
Install Homebrew from https://brew.sh and run:  brew install cmake ninja
or install CMake from https://cmake.org/download and Ninja from https://ninja-build.org."
    fi
fi
CMAKE="$(command -v cmake)"
NINJA="$(command -v ninja)"
info "$("$CMAKE" --version | head -n1), ninja $("$NINJA" --version), $("$PYTHON" -V 2>&1)"
info "Building: $ARCH (macOS $MIN_MACOS+), signing identity: $SIGN_IDENTITY, $JOBS jobs"

if [ "$CLEAN" = 1 ]; then
    step "Removing $OUT_ROOT"
    rm -rf "$OUT_ROOT"
fi
mkdir -p "$OUT_ROOT"

# ---------------------------------------------------------------- 2. dependencies
if [ "$RUN_DEPS" = 1 ]; then
    step "Fetching pinned dependencies (SDL3, FFmpeg, FreeType, RT64, ...)"
    "$PYTHON" scripts/setup_dependencies.py
else
    step "Skipping dependency setup (--no-deps)"
fi
for required in native/lib/SDL3/CMakeLists.txt native/lib/freetype/CMakeLists.txt native/lib/ffmpeg/configure \
                native/lib/rt64/CMakeLists.txt tools/N64Recomp/CMakeLists.txt; do
    [ -e "$required" ] || die "$required is missing. Run without --no-deps (needs internet once)."
done

# ---------------------------------------------------------------- 3. ROM and generated code
step "ROM"
rom_changed=0
if [ -n "$ROM_PATH" ]; then
    [ -f "$ROM_PATH" ] || die "ROM not found: $ROM_PATH"
    before="$( [ -f "$STAGED_ROM" ] && shasum -a 256 "$STAGED_ROM" | cut -d' ' -f1 || echo none)"
    "$PYTHON" tools/stage_rom.py "$ROM_PATH"
    after="$(shasum -a 256 "$STAGED_ROM" | cut -d' ' -f1)"
    [ "$before" = "$after" ] || rom_changed=1
elif [ -f "$STAGED_ROM" ]; then
    info "Using the ROM staged earlier ($STAGED_ROM)."
else
    die "No ROM staged yet. Pass your Road Rash 64 USA v1.0 ROM once:
    ./build_mac.sh \"/path/to/Road Rash 64 (USA).z64\""
fi

needs_generation=0
if [ "$REGENERATE" = 1 ] || [ "$rom_changed" = 1 ] || [ ! -f "$GENERATED_MARKER" ]; then
    needs_generation=1
else
    for config in config/roadrash64.us.toml config/roadrash64.us.audio_rsp.toml; do
        if [ "$config" -nt "$GENERATED_MARKER" ]; then
            info "$config changed since the code was generated."
            needs_generation=1
        fi
    done
fi

if [ "$needs_generation" = 1 ]; then
    step "Building the translators (N64Recomp, RSPRecomp) for this Mac"
    "$CMAKE" -S tools/N64Recomp -B build/toolchain -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_BUILD_TYPE=Release
    "$CMAKE" --build build/toolchain --target N64RecompCLI RSPRecomp --parallel "$JOBS"
    [ -x build/toolchain/N64Recomp ] && [ -x build/toolchain/RSPRecomp ] \
        || die "The translator build did not produce build/toolchain/N64Recomp and RSPRecomp."

    step "Generating the game code"
    ./build/toolchain/N64Recomp config/roadrash64.us.toml
    ./build/toolchain/RSPRecomp config/roadrash64.us.audio_rsp.toml
    [ -f "$GENERATED_MARKER" ] || die "Code generation did not produce $GENERATED_MARKER."
else
    step "Game code already generated (use --regenerate to redo it)"
fi

# ---------------------------------------------------------------- 4. the app
BUILD_DIR="$OUT_ROOT/$ARCH"
step "Building the Apple Silicon app (arm64, macOS $MIN_MACOS+)"
"$CMAKE" -S native -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
    -DRR64_BUILD_TESTS=OFF \
    -DRR64_SDL3_SHARED=ON \
    -DRR64_CODESIGN_IDENTITY="$SIGN_IDENTITY" \
    -DPython3_EXECUTABLE="$PYTHON"
"$CMAKE" --build "$BUILD_DIR" --target "$APP_NAME" --parallel "$JOBS"
BUILT_APP="$BUILD_DIR/bin/$APP_NAME.app"
[ -x "$BUILT_APP/Contents/MacOS/$APP_NAME" ] || die "The build did not produce $BUILT_APP."
[ -f "$BUILT_APP/Contents/Frameworks/libSDL3.0.dylib" ] || die "libSDL3.0.dylib is missing from the app's Frameworks folder."

# The finished app sits next to the build folder (what --zip packs).
FINAL_APP="$OUT_ROOT/$APP_NAME.app"
rm -rf "$FINAL_APP"
ditto "$BUILT_APP" "$FINAL_APP"

# ---------------------------------------------------------------- report
step "Checking the result"
EXE="$FINAL_APP/Contents/MacOS/$APP_NAME"
macho_minos() {  # macho_minos ARCH FILE -> minimum macOS recorded in that slice
    otool -arch "$1" -l "$2" | awk '$1 == "cmd" && ($2 == "LC_BUILD_VERSION" || $2 == "LC_VERSION_MIN_MACOSX") {f=1}
        f && ($1 == "minos" || $1 == "version") {print $2; exit}'
}
exe_archs="$(lipo -archs "$EXE")"
[ "$exe_archs" = "$ARCH" ] || die "The executable is '$exe_archs', expected $ARCH."
info "Executable:        arm64, NEON (Apple M1 baseline), runs on macOS $(macho_minos "$ARCH" "$EXE")+"
# Every binary in the app must be arm64 and run on the oldest supported macOS.
for bin in "$EXE" "$FINAL_APP"/Contents/Frameworks/*.dylib; do
    [ -L "$bin" ] && continue
    case " $(lipo -archs "$bin") " in *" $ARCH "*) ;; *) die "$(basename "$bin") has no $ARCH code." ;; esac
    got="$(macho_minos "$ARCH" "$bin")"
    [ -n "$got" ] || die "$(basename "$bin") records no minimum macOS version."
    version_ge "$MIN_MACOS" "$got" || die "$(basename "$bin") needs macOS $got, newer than $MIN_MACOS."
done
otool -L "$EXE" | grep -q '@rpath/libSDL3.0.dylib' || die "The executable does not load SDL3 through @rpath/libSDL3.0.dylib."
info "SDL3:              Contents/Frameworks/libSDL3.0.dylib ($(lipo -archs "$FINAL_APP/Contents/Frameworks/libSDL3.0.dylib"))"
framework_names=""
for lib in "$FINAL_APP"/Contents/Frameworks/*; do framework_names="$framework_names $(basename "$lib")"; done
info "Frameworks:       $framework_names"
# Every library must come from the system or from inside the bundle.
outside="$(otool -L "$EXE" "$FINAL_APP"/Contents/Frameworks/*.dylib | awk 'NF > 1 && $1 !~ /^(\/System\/|\/usr\/lib\/|@rpath\/|@executable_path\/|@loader_path\/)/ && $1 !~ /:$/ {print $1}' | sort -u)"
[ -z "$outside" ] || die "The app still loads libraries from outside the bundle:
$outside"
# Library search paths must stay inside the bundle as well.
rpaths="$(otool -l "$EXE" | awk '$1 == "cmd" && $2 == "LC_RPATH" {r=1; next} r && $1 == "path" {print $2; r=0}' | sort -u)"
for rpath in $rpaths; do
    case "$rpath" in @executable_path/*|@loader_path/*) ;; *) die "The executable searches $rpath for libraries (outside the bundle)." ;; esac
done
info "Library paths:     $(echo "$rpaths" | tr '\n' ' ')"
codesign --verify --strict "$FINAL_APP" && info "Signature:         valid ($SIGN_IDENTITY)"

if [ "$MAKE_ZIP" = 1 ]; then
    ZIP="$OUT_ROOT/$APP_NAME-macOS.zip"
    rm -f "$ZIP"
    ditto -c -k --sequesterRsrc --keepParent "$FINAL_APP" "$ZIP"
    info "Zip:               $ZIP"
fi

printf '\n%sDone:%s %s\n' "$GREEN" "$RESET" "$FINAL_APP"
cat <<EOF
    Run it with:  open "$FINAL_APP"
    On first launch, choose your Road Rash 64 ROM. Saves, settings, custom music and
    race packs live in ~/Library/Application Support/RoadRash64Recompiled (put an
    empty portable.txt beside the app to keep them next to it instead).
    Diagnostics (RR64_DIAGNOSTICS=1) log to ~/Library/Logs/RoadRash64Recompiled-runtime.log.
EOF
