#!/usr/bin/env python3
"""reColossus: the program's imports that the PS5's libc module (sce_module/libc.prx) doesn't export. A call to one of
them jumps to an unmapped address on the console (realpath and getcwd were two). Imports are matched by NID, the
PS4/PS5 hash of the name. Names the console resolves elsewhere (data symbols, getenv/setenv: seen working) are listed
as known.

    tools/check_imports.py <eboot.bin or the linked ELF's import names file> <libc.prx> <names.txt>
names.txt: the program's undefined symbols (llvm-nm -u), one per line."""
import base64, hashlib, re, struct, sys

SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
KNOWN = {"_CurrentRuneLocale", "__mb_cur_max", "__mb_sb_limit", "__stderrp", "__stdinp", "__stdoutp", "getenv",
         "setenv", "_Unwind_Backtrace"}


def nid(name: str) -> str:
    h = hashlib.sha1(name.encode() + SUFFIX).digest()
    return base64.b64encode(struct.pack(">Q", struct.unpack("<Q", h[:8])[0])).decode().rstrip("=").replace("/", "-")


def main() -> None:
    eboot, libc, names = sys.argv[1:4]
    program = open(eboot, "rb").read()
    module = open(libc, "rb").read()
    imports = {m.group(1).decode(): m.group(2).decode()
               for m in re.finditer(rb"([A-Za-z0-9+-]{11})#([A-Z])#([A-Z])\x00", program)}
    wanted = [l.split()[-1] for l in open(names) if l.strip()]
    by_nid = {nid(n): n for n in wanted}
    libc_lib = imports.get(nid("malloc"))
    missing = sorted(by_nid.get(k, k) for k, lib in imports.items()
                     if lib == libc_lib and k.encode() not in module and by_nid.get(k, k) not in KNOWN)
    if missing:
        print("imports: not in the PS5's libc module (they would crash when called): " + " ".join(missing))
        sys.exit(1)
    print(f"imports: every libc import is in the PS5's libc module ({sum(1 for v in imports.values() if v == libc_lib)})")


if __name__ == "__main__":
    main()
