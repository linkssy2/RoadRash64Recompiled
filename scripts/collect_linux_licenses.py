"""Collect exact source packages and notices for an experimental AppImage.

Run on the Ubuntu packaging host after build_appimage.py records its library
inventory. Requires enabled deb-src repositories. This never runs the game.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('RR64_NATIVE_BUILD_DIR', ROOT / 'native/build'))
OUT = BUILD / 'redistribution'

def output(*args):
    return subprocess.check_output(args, text=True).strip()

def main():
    OUT.mkdir(exist_ok=True)
    sources = OUT / 'sources'; sources.mkdir(exist_ok=True)
    notices = OUT / 'licenses'; notices.mkdir(exist_ok=True)
    inventory = json.loads((BUILD / 'bundled-libraries.json').read_text())
    packages = {}
    for entry in inventory:
        path = Path(entry['source'])
        if str(path).startswith(str(BUILD / 'ffmpeg-install')):
            entry['sourcePackage'] = 'pinned FFmpeg (see ffmpeg-source.tar.gz)'
            continue
        owner = None
        # Ubuntu can record an unmerged /lib path even though it resolves to /usr/lib.
        for candidate in [str(path), str(path).removeprefix('/usr')]:
            result = subprocess.run(['dpkg-query', '-S', candidate], capture_output=True, text=True)
            if result.returncode == 0:
                owner = result.stdout.split(': ', 1)[0]
                break
        if not owner:
            raise RuntimeError(f'No source/license ownership for {path}')
        info = output('dpkg-query', '-W', '-f=${source:Package}\t${source:Version}', owner).split('\t')
        if len(info) != 2 or not all(info): raise RuntimeError(owner)
        packages[tuple(info)] = owner
        entry['sourcePackage'], entry['sourceVersion'] = info
        name = owner.split(':')[0]
        copyright_file = Path('/usr/share/doc') / name / 'copyright'
        if not copyright_file.exists(): raise RuntimeError(f'Missing notice: {owner}')
        shutil.copy2(copyright_file, notices / (name + '.copyright'))
    for (name, version), owner in sorted(packages.items()):
        destination = sources / name
        destination.mkdir(exist_ok=True)
        subprocess.run(['apt-get', 'source', '--download-only', name + '=' + version], cwd=destination, check=True)
    ffmpeg = ROOT / 'native/lib/ffmpeg'
    with (sources / 'ffmpeg-source.tar.gz').open('wb') as file:
        subprocess.run(['git', 'archive', '--format=tar.gz', '--prefix=ffmpeg/', 'HEAD'], cwd=ffmpeg, stdout=file, check=True)
    shutil.copy2(ffmpeg / 'COPYING.LGPLv2.1', notices / 'FFmpeg.LGPLv2.1')
    # RR64-SDL3: SDL3 is linked statically into the executable (zlib licence);
    # its exact source and notice still travel with the package.
    for folder, archive in [('SDL3', 'SDL3-source.tar.gz')]:
        tree = ROOT / 'native/lib' / folder
        with (sources / archive).open('wb') as file:
            subprocess.run(['git', 'archive', '--format=tar.gz', '--prefix=' + folder + '/', 'HEAD'], cwd=tree, stdout=file, check=True)
        shutil.copy2(tree / 'LICENSE.txt', notices / (folder + '.LICENSE.txt'))
    for name in ['LICENSE', 'CREDITS.md', 'THIRD_PARTY_NOTICES.md']:
        shutil.copy2(ROOT / name, OUT / name)
    shutil.copy2(ROOT / 'native/CMakeLists.txt', OUT / 'native-CMakeLists.txt')
    (OUT / 'bundled-libraries.json').write_text(json.dumps(inventory, indent=2))
    (OUT / 'README.txt').write_text(
        'These sources and notices correspond to the libraries in the experimental Linux package.\n'
        'FFmpeg is a minimal shared LGPL build; exact configure options are in native-CMakeLists.txt.\n'
        'SDL3 3.4.16 is built from the pinned source by native/cmake/Sdl.cmake and linked statically\n'
        '(SDL_LIBC=ON; no GPU/2D renderer/camera/tray subsystems, tests, examples or install rules).\n'
        'Ubuntu source packages include their original packaging/build instructions.\n'
        'No ROM, generated game source or personal saves are included.\n')
    files = sorted(p for p in OUT.rglob('*') if p.is_file() and p.name != 'SHA256SUMS.txt')
    (OUT / 'SHA256SUMS.txt').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p.relative_to(OUT)) + '\n' for p in files))
    print(f'Collected sources/notices for {len(packages)} Ubuntu packages, SDL and FFmpeg.')

if __name__ == '__main__':
    main()
