#!/usr/bin/env python3
"""Raise the __TEXT segment's maximum protection to rwx in a Mach-O executable.

N64ModernRuntime applies code mods by patching recompiled functions in place:
it mprotect()s the page read-write, writes a jump, and restores read-execute.
On macOS that mprotect fails unless the segment's *maximum* protection already
allows writing, and ld64 always emits __TEXT with maxprot r-x. Only the
maxprot field changes here; initprot stays r-x, so the code is mapped exactly
as before until a mod asks for a patch. The bundle is signed afterwards with
entitlements that permit the write (cmake/macos/entitlements.plist).

Standard library only, so no extra Python packages are needed on the build Mac.
usage: set_text_maxprot.py <Mach-O executable>
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF
LC_SEGMENT_64 = 0x19
VM_PROT_ALL = 0x7


def patch_thin(data, base):
    magic, _cpu, _sub, _type, ncmds, _size, _flags, _res = struct.unpack_from('<8I', data, base)
    if magic != MH_MAGIC_64:
        raise SystemExit(f'not a 64-bit little-endian Mach-O image at offset {base:#x}')
    offset = base + 32
    patched = False
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<2I', data, offset)
        if cmd == LC_SEGMENT_64:
            segname = bytes(data[offset + 8:offset + 24]).rstrip(b'\0').decode('ascii', 'replace')
            if segname == '__TEXT':
                maxprot_at = offset + 56
                maxprot, initprot = struct.unpack_from('<2i', data, maxprot_at)
                if maxprot != VM_PROT_ALL:
                    struct.pack_into('<i', data, maxprot_at, VM_PROT_ALL)
                print(f'__TEXT maxprot {maxprot:#x} -> {VM_PROT_ALL:#x} (initprot {initprot:#x} unchanged)')
                patched = True
        offset += cmdsize
    if not patched:
        raise SystemExit('no __TEXT segment found')


def main():
    path = sys.argv[1]
    data = bytearray(open(path, 'rb').read())
    magic_be = struct.unpack_from('>I', data, 0)[0]
    if magic_be in (FAT_MAGIC, FAT_MAGIC_64):
        count = struct.unpack_from('>I', data, 4)[0]
        entry = 20 if magic_be == FAT_MAGIC else 32
        for i in range(count):
            at = 8 + i * entry
            if magic_be == FAT_MAGIC:
                arch_offset = struct.unpack_from('>I', data, at + 8)[0]
            else:
                arch_offset = struct.unpack_from('>Q', data, at + 8)[0]
            patch_thin(data, arch_offset)
    else:
        patch_thin(data, 0)
    open(path, 'wb').write(data)


if __name__ == '__main__':
    main()
