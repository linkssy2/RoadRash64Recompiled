#!/usr/bin/env python3
"""Verify, normalize, and stage a user-owned Road Rash 64 ROM for local tools."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from rr64_common import SUPPORTED_SHA1, is_supported, load_rom, RomError


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="Raw ROM or ZIP containing it")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/roadrash64.us.z64"),
        help="Staged big-endian ROM path",
    )
    args = parser.parse_args()

    try:
        loaded = load_rom(args.rom)
    except (OSError, RomError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if not is_supported(loaded.data):
        print(f"error: unsupported ROM; expected SHA-1 {SUPPORTED_SHA1}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(loaded.data)
    print(f"Staged verified big-endian ROM: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
