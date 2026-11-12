#!/usr/bin/env python3
"""Shared, dependency-free helpers for the Road Rash 64 recompilation bootstrap."""

from __future__ import annotations

from dataclasses import dataclass
from hashlib import md5, sha1, sha256
from pathlib import Path
from typing import Iterable
import struct
import zipfile
import zlib

SUPPORTED_SHA1 = "87727a298f583ec8325f5655088ff21e37b335b2"
SUPPORTED_SHA256 = "74e49e863484b5d17dcbe3891b99f2cafe3cf7511aa1dc5427022f699301db73"
SUPPORTED_MD5 = "28c2373f6d831eec81f6146a809e701b"
SUPPORTED_SIZE = 0x02000000
SUPPORTED_TITLE = "ROAD RASH 64"

ROM_LOAD_START = 0x00001000
VRAM_LOAD_START = 0x80000400
ENTRY_END = 0x80000440
MAIN_TEXT_START = 0x80008630
MAIN_TEXT_END_A = 0x8007DC88
RSP_PREFIX_START = MAIN_TEXT_END_A
RSP_PROGRAM_START = 0x8007DC90
RSP_BOOT_START = 0x8007E8F0
RSP_GFX_START = 0x8007E9C0
RSP_END = 0x8007FD50
MAIN_TEXT_RESUME = RSP_END
MAIN_TEXT_END = 0x8009CB40
MAIN_DATA_START = MAIN_TEXT_END
INITIALIZED_END = 0x800A98C0
BSS_END = 0x800E8D00
CONTENT_START = 0x000AA4C0

CPU_RANGES: tuple[tuple[str, int, int], ...] = (
    ("entry", VRAM_LOAD_START, ENTRY_END),
    ("main_text_0", MAIN_TEXT_START, MAIN_TEXT_END_A),
    ("main_text_1", MAIN_TEXT_RESUME, MAIN_TEXT_END),
)

RSP_SEGMENTS: tuple[tuple[str, int, int, str], ...] = (
    ("rsp_prefix_metadata", RSP_PREFIX_START, RSP_PROGRAM_START, "metadata"),
    (
        "task_ucode_candidate",
        RSP_PROGRAM_START,
        RSP_BOOT_START,
        "rsp_text; likely audio/task microcode, identification pending",
    ),
    ("rspboot_text", RSP_BOOT_START, RSP_GFX_START, "standard RSP boot text"),
    (
        "f3dex_text_and_overlays",
        RSP_GFX_START,
        RSP_END,
        "F3DEX.NoN FIFO text plus resident RSP overlay payload",
    ),
)
RSP_REGION = (RSP_PREFIX_START, RSP_END)

VERIFIED_CALLBACK_TABLE_START = 0x800A7F00
VERIFIED_CALLBACK_TABLE_COUNT = 33

CIC_BY_IPL3_CRC32 = {
    0x90BB6CB5: "CIC-NUS-6102/7101",
    0x0B050EE0: "CIC-NUS-6103/7103",
    0x98BC2C86: "CIC-NUS-6105/7105",
    0xACC8580A: "CIC-NUS-6106/7106",
}


class RomError(RuntimeError):
    pass


@dataclass(frozen=True)
class LoadedRom:
    source_path: Path
    member_name: str | None
    source_byte_order: str
    data: bytes

    @property
    def display_name(self) -> str:
        if self.member_name:
            return f"{self.source_path.name}:{self.member_name}"
        return self.source_path.name


def _normalize_byte_order(data: bytes) -> tuple[str, bytes]:
    if len(data) < 4:
        raise RomError("File is too small to be an N64 ROM")

    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return "z64 (big-endian)", data
    if magic == b"\x37\x80\x40\x12":
        if len(data) % 2:
            raise RomError("Byte-swapped V64 image has an odd length")
        out = bytearray(data)
        for i in range(0, len(out), 2):
            out[i], out[i + 1] = out[i + 1], out[i]
        return "v64 (byte-swapped)", bytes(out)
    if magic == b"\x40\x12\x37\x80":
        if len(data) % 4:
            raise RomError("Little-endian N64 image length is not divisible by four")
        out = bytearray(len(data))
        for i in range(0, len(data), 4):
            out[i : i + 4] = data[i : i + 4][::-1]
        return "n64 (little-endian)", bytes(out)

    raise RomError(f"Unrecognized N64 ROM magic: {magic.hex().upper()}")


def load_rom(path_like: str | Path) -> LoadedRom:
    path = Path(path_like).expanduser().resolve()
    if not path.is_file():
        raise RomError(f"ROM input does not exist: {path}")

    member_name: str | None = None
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path, "r") as archive:
            candidates = [
                info
                for info in archive.infolist()
                if not info.is_dir()
                and Path(info.filename).suffix.lower() in {".z64", ".v64", ".n64", ".rom", ".bin"}
            ]
            if not candidates:
                raise RomError("ZIP contains no recognizable N64 ROM image")
            candidates.sort(key=lambda item: item.file_size, reverse=True)
            best = candidates[0]
            if len(candidates) > 1 and candidates[1].file_size == best.file_size:
                names = ", ".join(item.filename for item in candidates[:8])
                raise RomError(f"ZIP contains multiple same-sized ROM candidates: {names}")
            member_name = best.filename
            raw = archive.read(best)
    else:
        raw = path.read_bytes()

    byte_order, normalized = _normalize_byte_order(raw)
    return LoadedRom(path, member_name, byte_order, normalized)


def hashes(data: bytes) -> dict[str, str | int]:
    return {
        "size": len(data),
        "crc32": f"{zlib.crc32(data) & 0xFFFFFFFF:08x}",
        "md5": md5(data).hexdigest(),
        "sha1": sha1(data).hexdigest(),
        "sha256": sha256(data).hexdigest(),
    }


def is_supported(data: bytes) -> bool:
    return len(data) == SUPPORTED_SIZE and sha1(data).hexdigest() == SUPPORTED_SHA1


def read_u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise RomError(f"Read outside ROM at 0x{offset:X}")
    return struct.unpack_from(">I", data, offset)[0]


def signed16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def parse_header(data: bytes) -> dict[str, int | str]:
    if len(data) < 0x40:
        raise RomError("ROM is too small to contain an N64 header")
    title = data[0x20:0x34].decode("ascii", errors="replace").rstrip(" \x00")
    return {
        "pi_config": read_u32(data, 0x00),
        "clock_rate": read_u32(data, 0x04),
        "entrypoint": read_u32(data, 0x08),
        "release": read_u32(data, 0x0C),
        "crc1": read_u32(data, 0x10),
        "crc2": read_u32(data, 0x14),
        "title": title,
        "format": chr(data[0x3B]) if data[0x3B] else "",
        "cart_id": data[0x3C:0x3E].decode("ascii", errors="replace"),
        "country": chr(data[0x3E]) if data[0x3E] else "",
        "version": data[0x3F],
    }


def parse_ipl3(data: bytes) -> dict[str, str]:
    if len(data) < 0x1000:
        raise RomError("ROM is too small to contain IPL3")
    ipl3 = data[0x40:0x1000]
    crc = zlib.crc32(ipl3) & 0xFFFFFFFF
    return {
        "crc32": f"{crc:08x}",
        "md5": md5(ipl3).hexdigest(),
        "sha1": sha1(ipl3).hexdigest(),
        "cic": CIC_BY_IPL3_CRC32.get(crc, "unknown"),
    }


def vram_to_rom(vram: int) -> int:
    return vram - VRAM_LOAD_START + ROM_LOAD_START


def rom_to_vram(rom: int) -> int:
    return rom - ROM_LOAD_START + VRAM_LOAD_START


def in_cpu_range(address: int) -> bool:
    return any(start <= address < end for _, start, end in CPU_RANGES)


def section_for_vram(address: int) -> tuple[str, int, int] | None:
    for section in CPU_RANGES:
        if section[1] <= address < section[2]:
            return section
    return None


def iter_words(data: bytes, start_vram: int, end_vram: int) -> Iterable[tuple[int, int]]:
    if start_vram % 4 or end_vram % 4:
        raise RomError("Instruction ranges must be word aligned")
    for pc in range(start_vram, end_vram, 4):
        yield pc, read_u32(data, vram_to_rom(pc))


def decode_jump_target(pc: int, word: int) -> int:
    return ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def decode_branch_target(pc: int, word: int) -> int:
    return (pc + 4 + (signed16(word & 0xFFFF) << 2)) & 0xFFFFFFFF


def is_stack_prologue(word: int) -> bool:
    # addiu sp, sp, negative_immediate
    return (
        (word >> 26) == 0x09
        and ((word >> 21) & 0x1F) == 29
        and ((word >> 16) & 0x1F) == 29
        and bool(word & 0x8000)
    )


def is_jr(word: int) -> bool:
    return (word >> 26) == 0 and (word & 0x3F) == 0x08


def is_jalr(word: int) -> bool:
    return (word >> 26) == 0 and (word & 0x3F) == 0x09


def is_jr_ra(word: int) -> bool:
    return word == 0x03E00008


def is_direct_jump(word: int) -> bool:
    return (word >> 26) == 0x02


def is_direct_call(word: int) -> bool:
    return (word >> 26) == 0x03


def is_conditional_branch(word: int) -> bool:
    op = word >> 26
    if op in {0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17}:
        return True
    # COP0/COP1/COP2 branch forms use rs == 8.
    return op in {0x10, 0x11, 0x12} and ((word >> 21) & 0x1F) == 0x08


def has_delay_slot(word: int) -> bool:
    return (
        is_direct_call(word)
        or is_direct_jump(word)
        or is_conditional_branch(word)
        or is_jr(word)
        or is_jalr(word)
    )


def find_all(data: bytes, needle: bytes) -> list[int]:
    offsets: list[int] = []
    start = 0
    while True:
        found = data.find(needle, start)
        if found < 0:
            return offsets
        offsets.append(found)
        start = found + 1


def read_callback_table(data: bytes) -> list[int]:
    targets: list[int] = []
    for index in range(VERIFIED_CALLBACK_TABLE_COUNT):
        value = read_u32(data, vram_to_rom(VERIFIED_CALLBACK_TABLE_START + index * 4))
        if not in_cpu_range(value):
            raise RomError(
                f"Verified callback table entry {index} points outside CPU text: 0x{value:08X}"
            )
        targets.append(value)
    terminator = read_u32(
        data,
        vram_to_rom(VERIFIED_CALLBACK_TABLE_START + VERIFIED_CALLBACK_TABLE_COUNT * 4),
    )
    if terminator != 0:
        raise RomError("Verified callback table no longer has the expected zero terminator")
    return targets


def parse_boot_layout(data: bytes) -> dict[str, int | None]:
    """Decode the simple Road Rash boot stub's LUI/ORI and LUI/ADDIU pairs."""
    words = [read_u32(data, ROM_LOAD_START + i * 4) for i in range(8)]

    def pair_value(lui_word: int, low_word: int, expected_reg: int) -> int | None:
        if (lui_word >> 26) != 0x0F or ((lui_word >> 16) & 0x1F) != expected_reg:
            return None
        upper = (lui_word & 0xFFFF) << 16
        op = low_word >> 26
        rs = (low_word >> 21) & 0x1F
        rt = (low_word >> 16) & 0x1F
        if rs != expected_reg or rt != expected_reg:
            return None
        if op == 0x0D:  # ori
            return upper | (low_word & 0xFFFF)
        if op == 0x09:  # addiu
            return (upper + signed16(low_word & 0xFFFF)) & 0xFFFFFFFF
        return None

    return {
        "stack_pointer": pair_value(words[0], words[1], 29),
        "initialized_end": pair_value(words[2], words[3], 8),
        "bss_end": pair_value(words[4], words[5], 9),
    }
