#!/usr/bin/env python3
"""reColossus: the FNV-1a 64 hashes of the four files this build's code is recompiled from, as a C++ header the app
checks the player's disc against (ps5/main_ps5.cpp): the boot ELF and the three XFF modules.

    tools/game_hashes.py <extracted disc folder> <out.inc>
"""
import sys
from pathlib import Path

FILES = [("kBootElf", "SCUS_974.72"), ("kKernelXff", "KERNEL.XFF"), ("kManagerXff", "MANAGER.XFF"),
         ("kGamecoreXff", "GAMECORE.XFF")]


def child(folder: Path, name: str) -> Path:
    for p in folder.iterdir():
        if p.name.lower() == name.lower():
            return p
    sys.exit(f"game_hashes: no {name} in {folder}")


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    folder, out = Path(sys.argv[1]), Path(sys.argv[2])
    lines = ["// written by tools/game_hashes.py from the disc this build was made from (FNV-1a 64)"]
    for symbol, name in FILES:
        h = fnv1a64(child(folder, name).read_bytes())
        lines.append(f"static constexpr uint64_t {symbol} = 0x{h:016x}ull; // {name}")
    out.write_text("\n".join(lines) + "\n")
    print(f"game_hashes: {out}")


if __name__ == "__main__":
    main()
