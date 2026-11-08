#!/usr/bin/env python3
"""Fetch, patch and build the RR64 RT64 probe APK.

The probe needs NO ROM. It answers one question on a real phone: can RT64
create a Vulkan device and present a frame on this GPU?

Steps:
  1. python3 scripts/setup_dependencies.py — fetches every pinned
     dependency (RT64 6f1c2d99, plume d890ac89, ...) and applies RR64's own
     dependency-patches + overrides. No ROM involved.
  2. Applies the Android boundary patches (android/probe/patches/) on top
     of RR64's patches in native/lib/rt64 (+ plume). Idempotent.
  3. Fetches SDL at the AeroGauge-verified pin into build-probe/deps/SDL.
  4. Builds host file_to_c (x86_64) for shader embedding.
  5. Runs Gradle :probe:assembleDebug (NDK cross-compile, arm64-v8a).
  6. Verifies the APK payload (required .so files present, no game data).

Prerequisites: Python 3.11+, Git, CMake 3.22+, Ninja, JDK 17,
Android SDK (platform 35, build-tools 35.0.0, NDK 28.2.13676358).
NOT compile-checked: no NDK / Android Studio on this machine.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build-probe"
DEPS = WORK / "deps"
NDK_VERSION = "28.2.13676358"
SDL_URL = "https://github.com/libsdl-org/SDL.git"
SDL_COMMIT = "5d249570393f7a37e037abf22cd6012a4cc56a71"
# (rt64, plume) Android patches, applied in this order per tree.
RT64_PATCHES = [
    ROOT / "android/probe/patches/0019-rt64-android-cross-build.patch",
    ROOT / "android/probe/patches/0023-rt64-adreno-endian-swap.patch",
]
PLUME_PATCHES = [
    ROOT / "android/probe/patches/0020-plume-android-sdl-window.patch",
]


def run(*args, cwd=ROOT, env=None):
    subprocess.run([str(a) for a in args], cwd=cwd, check=True, env=env)


def fetch_repo(url, destination, commit):
    if not (destination / ".git").exists():
        run("git", "init", destination)
        run("git", "-C", destination, "remote", "add", "origin", url)
        run("git", "-C", destination, "fetch", "--depth", "1", "origin", commit)
        run("git", "-C", destination, "checkout", "--detach", "FETCH_HEAD")
    actual = subprocess.check_output(
        ["git", "-C", str(destination), "rev-parse", "HEAD"], text=True).strip()
    if actual != commit:
        raise RuntimeError(f"Unexpected revision in {destination}: {actual} (want {commit})")


def apply_patch_idempotent(directory, patch):
    """Apply a patch, skipping it if it is already applied."""
    reverse = subprocess.run(
        ["git", "-C", str(directory), "apply", "--reverse", "--check", str(patch)],
        capture_output=True)
    if reverse.returncode == 0:
        print(f"already applied: {patch.name} in {directory}")
        return
    run("git", "-C", directory, "apply", "--check", patch)
    run("git", "-C", directory, "apply", patch)
    print(f"applied: {patch.name} in {directory}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true",
                        help="Install the finished APK on the connected device")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 4, 12))
    parser.add_argument("--sdk", default=os.environ.get("ANDROID_HOME")
                        or os.environ.get("ANDROID_SDK_ROOT"))
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--skip-deps", action="store_true",
                        help="Reuse already-fetched trees (skip setup_dependencies.py + SDL fetch)")
    args = parser.parse_args()
    if not args.sdk:
        parser.error("Set ANDROID_HOME or pass --sdk")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    sdk = Path(args.sdk).resolve()
    ndk = sdk / "ndk" / NDK_VERSION
    if not (ndk / "build/cmake/android.toolchain.cmake").is_file():
        parser.error(f"Install Android NDK {NDK_VERSION} with sdkmanager")
    WORK.mkdir(exist_ok=True)

    if not args.skip_deps:
        # RR64's own pinned deps + patches + overrides. No ROM involved.
        run(sys.executable, "-B", ROOT / "scripts/setup_dependencies.py")
        fetch_repo(SDL_URL, DEPS / "SDL", SDL_COMMIT)
        run("git", "-C", DEPS / "SDL", "submodule", "update", "--init", "--recursive")

    rt64 = ROOT / "native/lib/rt64"
    plume = rt64 / "src/contrib/plume"
    if not (rt64 / "CMakeLists.txt").is_file():
        parser.error("native/lib/rt64 is missing: run scripts/setup_dependencies.py first")
    if not (plume / "plume_vulkan.cpp").is_file():
        parser.error("native/lib/rt64/src/contrib/plume is missing: run scripts/setup_dependencies.py first")
    for patch in RT64_PATCHES:
        apply_patch_idempotent(rt64, patch)
    for patch in PLUME_PATCHES:
        apply_patch_idempotent(plume, patch)

    # Host file_to_c (x86_64). RT64's Android patch skips building it for
    # the target; the probe CMakeLists imports this host binary instead.
    host = WORK / "host"
    rt64_dir = (ROOT / "native/lib/rt64").as_posix()
    run(args.cmake, "-S", ROOT / "android/probe/host-tools", "-B", host,
        "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        f"-DRR64_RT64_DIR={rt64_dir}")
    run(args.cmake, "--build", host, "--target", "file_to_c", "-j", args.jobs)

    # Gradle build. Env vars feed android/probe/build.gradle -> CMake.
    env = dict(os.environ)
    env["ANDROID_HOME"] = str(sdk)
    env["RR64_RT64_DIR"] = rt64_dir
    env["RR64_SDL_DIR"] = (DEPS / "SDL").as_posix()
    env["RR64_HOST_TOOLS_DIR"] = (host / "bin").as_posix()
    dxc = Path(rt64_dir) / "src/contrib/dxc"
    if os.name == "nt":
        env["RR64_HOST_DXC_BIN"] = (dxc / "bin/x64/dxc.exe").as_posix()
    else:
        env["RR64_HOST_DXC_BIN"] = (dxc / "bin/x64/dxc-linux").as_posix()
    env["RR64_HOST_DXC_LIB"] = (dxc / "lib/x64").as_posix()
    wrapper = ROOT / "android" / ("gradlew.bat" if os.name == "nt" else "gradlew")
    command = [wrapper] if os.name == "nt" else ["sh", wrapper]
    run(*command, "--no-daemon", ":probe:assembleDebug", cwd=ROOT / "android", env=env)

    apk = ROOT / "android/probe/build/outputs/apk/debug/probe-debug.apk"
    if not apk.is_file():
        raise RuntimeError(f"Probe APK not produced: {apk}")
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        for name in names:
            if name.lower().endswith((".z64", ".v64", ".n64", ".mpk", ".pak")):
                raise RuntimeError(f"Forbidden game data in probe APK: {name}")
        for name in ["lib/arm64-v8a/libmain.so", "lib/arm64-v8a/libSDL2.so",
                     "lib/arm64-v8a/libc++_shared.so"]:
            if name not in names:
                raise RuntimeError(f"Missing APK payload: {name}")
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    result = dist / "rr64-rt64-probe-debug.apk"
    shutil.copy2(apk, result)
    print(f"Probe APK: {result}")
    if args.install:
        run(sdk / "platform-tools" / ("adb.exe" if os.name == "nt" else "adb"),
            "install", "-r", result)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
