"""Package a built Linux binary into a portable AppImage.

Run this after a normal native/build (see BUILDING.md / the Linux build
steps) has produced native/build/bin/RoadRash64Recompiled. Produces
native/build/RoadRash64Recompiled-x86_64.AppImage.

Course-enabled builds require --mk64-importer-bundle pointing to the Linux
bundle.json from build_mk64_importer.py. The verified tool is installed beside
the executable; players' imported data remains outside the read-only image.

This does its own dependency bundling pass rather than trusting
linuxdeploy's bundled patchelf for the final library copies: on newer
toolchains (glibc/binutils that emit DT_RELR compressed relative
relocations, e.g. Fedora 40+), linuxdeploy's older internal patchelf
corrupts those relocations when it rewrites RPATH, producing shared
libraries that segfault during their own ELF constructors at load time.
linuxdeploy is still used for what it does well: discovering the full
dependency closure via ldd and applying its maintained exclude-list (so
things like glibc, libstdc++, and Mesa/GL stay off the AppImage and are
loaded from the host, as is standard AppImage practice). After it runs,
every bundled library is replaced with a pristine, unmodified copy from
the host, and a small custom AppRun sets LD_LIBRARY_PATH instead of
relying on per-library RPATH patches - sidestepping the corruption
without giving up linuxdeploy's dependency discovery.

Deliberately does NOT add extra exclusions beyond linuxdeploy's own
exclude-list (earlier revisions of this script excluded libsystemd/
libselinux/libmount/libblkid too, as a blanket "these segfaulted once"
precaution taken before the pristine-copy fix above existed). Once every
library is copied pristine, that crash risk is gone for all of them
equally, and excluding a library instead means silently depending on the
host having it - which broke the AppImage on SteamOS (Steam Deck), which
has no libselinux at all. Bundle everything linuxdeploy finds; don't
assume any of it is safe to leave off the host's word alone.
"""
import argparse
import hashlib
import json
import re
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Override to package a build made in a different directory, e.g. one built
# inside a container against an older glibc for wider compatibility.
NATIVE_BUILD = Path(os.environ["RR64_NATIVE_BUILD_DIR"]) if os.environ.get("RR64_NATIVE_BUILD_DIR") else ROOT / "native" / "build"
BINARY = NATIVE_BUILD / "bin" / "RoadRash64Recompiled"
ASSETS = NATIVE_BUILD / "bin" / "assets"
ICON_SOURCE = ROOT / "native" / "assets" / "RoadRashIcon.png"
APPDIR = NATIVE_BUILD / "AppDir"
TOOLS_DIR = NATIVE_BUILD / "appimage-tools"
OUTPUT = NATIVE_BUILD / "RoadRash64Recompiled-x86_64.AppImage"

LINUXDEPLOY_URL = "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL_URL = "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
RUNTIME_URL = "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"

TOOL_HASHES = {LINUXDEPLOY_URL: "c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d", APPIMAGETOOL_URL: "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"}
TOOL_HASHES[RUNTIME_URL] = "1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf"

SEARCH_DIRS = ["/lib64", "/usr/lib64", "/lib", "/usr/lib", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/usr/local/lib"]

APPRUN = """#!/bin/sh
HERE="$(dirname "$(readlink -f "${0}")")"
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
cd "${HERE}/usr/bin"
exec ./RoadRash64Recompiled "$@"
"""

DESKTOP_FILE = """[Desktop Entry]
Type=Application
Name=Road Rash 64 Recompiled
Comment=Native PC port of Road Rash 64, built from the recompiled source
Exec=RoadRash64Recompiled
Icon=roadrash64recompiled
Categories=Game;
Terminal=false
"""


def download(url: str, dest: Path) -> None:
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["curl", "--fail", "--location", "--retry", "3", "-o", str(dest), url], check=True)
    if hashlib.sha256(dest.read_bytes()).hexdigest() != TOOL_HASHES[url]:
        raise RuntimeError(f"Packaging tool checksum mismatch: {dest}")
    dest.chmod(dest.stat().st_mode | stat.S_IEXEC)


def original_libraries() -> dict[str, Path]:
    # Resolve the original executable's dependency closure before linuxdeploy
    # modifies any files. Never select a different ABI by basename search order.
    listing = subprocess.check_output(["ldd", str(BINARY)], text=True)
    if "not found" in listing:
        raise RuntimeError(listing)
    return {name: Path(path).resolve() for name, path in
            re.findall(r"^\s*(\S+) => (/\S+) \(", listing, re.M)}


def copy_importer(manifest_path: Path) -> None:
    """Keep the tested frozen tool intact and outside linuxdeploy's rewriting."""
    manifest = json.loads(manifest_path.read_text())
    if not manifest.get("passed") or manifest.get("platform") != "linux":
        raise ValueError("Provide a verified native Linux importer bundle.")
    if manifest.get("contains_converted_assets") is not False:
        raise ValueError("The importer bundle must contain no converted game assets.")
    source = Path(manifest["directory"]).resolve()
    files = manifest["files"]
    actual = {p.relative_to(source).as_posix() for p in source.rglob("*") if p.is_file()}
    if actual != set(files):
        raise ValueError("The frozen importer file inventory has changed.")
    for name, expected in files.items():
        path = source / name
        if Path(name).is_absolute() or not path.resolve().is_relative_to(source):
            raise ValueError("Importer path escapes its bundle: " + name)
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError("Importer file differs from its manifest: " + name)
    for name in ("rr64-mk64-importer", "rr64-mk64-contact", "rr64-mk64-motion"):
        executable = source / name
        if executable.read_bytes()[:4] != b"\x7fELF" or not os.access(executable, os.X_OK):
            raise ValueError("Missing executable Linux importer component: " + name)
    target = APPDIR / "usr/bin/tools/mk64-importer"
    shutil.copytree(source, target)
    for name, expected in files.items():
        if hashlib.sha256((target / name).read_bytes()).hexdigest() != expected:
            raise ValueError("Copied importer differs: " + name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mk64-importer-bundle", type=Path,
                        help="bundle.json produced by build_mk64_importer.py on Linux")
    args = parser.parse_args()
    if not BINARY.exists():
        sys.exit(f"Build the project first: {BINARY} does not exist.")
    cache = NATIVE_BUILD / "CMakeCache.txt"
    if cache.is_file() and "RR64_EXPERIMENTAL_COURSE:BOOL=ON" in cache.read_text():
        if args.mk64_importer_bundle is None:
            sys.exit("Course-enabled packages require --mk64-importer-bundle.")

    originals = original_libraries()
    linuxdeploy = TOOLS_DIR / "linuxdeploy"
    appimagetool = TOOLS_DIR / "appimagetool"
    runtime = TOOLS_DIR / "runtime-x86_64"
    download(LINUXDEPLOY_URL, linuxdeploy)
    download(APPIMAGETOOL_URL, appimagetool)
    download(RUNTIME_URL, runtime)

    if APPDIR.resolve().parent != NATIVE_BUILD.resolve():
        raise ValueError("AppDir must resolve inside this build directory.")
    if APPDIR.exists():
        shutil.rmtree(APPDIR)
    (APPDIR / "usr" / "bin").mkdir(parents=True)
    shutil.copy2(BINARY, APPDIR / "usr" / "bin" / BINARY.name)
    # Keep the runtime UI inventory clean. AppImage's icon is installed below;
    # Windows icon resources and historical artwork are not loaded from assets/.
    shutil.copytree(ASSETS, APPDIR / "usr" / "bin" / "assets",
                    ignore=shutil.ignore_patterns("sky", "RoadRashLauncher-v3.png",
                                                  "RoadRashIcon.ico", "RoadRashIcon.png",
                                                  "RoadRashIcon.rc"))

    icon_path = APPDIR / "roadrash64recompiled.png"
    subprocess.run(["convert", str(ICON_SOURCE), "-resize", "512x512", str(icon_path)], check=True)
    desktop_path = APPDIR / "roadrash64recompiled.desktop"
    desktop_path.write_text(DESKTOP_FILE)

    print("Running linuxdeploy for dependency discovery...")
    env = dict(os.environ, NO_STRIP="1", APPIMAGE_EXTRACT_AND_RUN="1")
    cmd = [
        str(linuxdeploy),
        "--appdir", str(APPDIR),
        "--executable", str(APPDIR / "usr" / "bin" / BINARY.name),
        "--desktop-file", str(desktop_path),
        "--icon-file", str(icon_path),
    ]
    subprocess.run(cmd, check=True, env=env)

    print("Replacing patchelf-modified libraries with pristine copies...")
    lib_dir = APPDIR / "usr" / "lib"
    missing = []
    for so_file in sorted(lib_dir.glob("*.so*")):
        pristine = originals.get(so_file.name)
        if pristine is None:
            missing.append(so_file.name)
            continue
        if so_file.is_symlink():
            so_file.unlink()
        shutil.copy2(pristine, so_file)
    if missing:
        sys.exit("Could not find a pristine source for: " + ", ".join(missing))

    # Inventory original library identities for matching license/source packaging.
    inventory = []
    for so_file in sorted(lib_dir.glob("*.so*")):
        original = originals[so_file.name]
        inventory.append(dict(name=so_file.name, source=str(original),
            sha256=hashlib.sha256(so_file.read_bytes()).hexdigest()))
    (NATIVE_BUILD / "bundled-libraries.json").write_text(json.dumps(inventory, indent=2))
    shutil.copytree(ROOT / "licenses", APPDIR / "usr/share/licenses/roadrash64", dirs_exist_ok=True)
    for name in ["LICENSE", "CREDITS.md", "THIRD_PARTY_NOTICES.md", "LEGAL.md"]:
        shutil.copy2(ROOT / name, APPDIR / "usr/share/licenses/roadrash64" / name)

    if args.mk64_importer_bundle is not None:
        copy_importer(args.mk64_importer_bundle)

    # Also restore the pristine executable: avoid whatever patchelf did to it too.
    shutil.copy2(BINARY, APPDIR / "usr" / "bin" / BINARY.name)

    apprun_path = APPDIR / "AppRun"
    apprun_path.unlink(missing_ok=True)
    apprun_path.write_text(APPRUN)
    apprun_path.chmod(apprun_path.stat().st_mode | stat.S_IEXEC)

    print("Packaging AppImage...")
    OUTPUT.unlink(missing_ok=True)
    subprocess.run([str(appimagetool), "--runtime-file", str(runtime), str(APPDIR), str(OUTPUT)], check=True, env=env)

    print(f"\nDone: {OUTPUT} ({OUTPUT.stat().st_size / (1024 * 1024):.1f} MiB)")
    print("Test it before releasing: ./native/build/RoadRash64Recompiled-x86_64.AppImage")


if __name__ == "__main__":
    main()
