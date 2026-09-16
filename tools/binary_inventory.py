#!/usr/bin/env python3
"""binary_inventory.py — static identification of explicitly supplied binaries.

For each file (or each regular file under each supplied directory) reports:
  - path, size, SHA-256
  - `file(1)` output
  - container format guess: PE/COFF, NE, LE/LX, Mach-O (thin/fat), ELF, raw MZ
  - architecture where the header makes it determinable

Constraints (Phase 0):
  - Python standard library + the macOS `file` tool only
  - read-only; never modifies or executes the inputs
  - no decompilation, no disassembly, no reconstruction
  - directories are scanned without following symlinks and without
    leaving the supplied root

Usage:
    python3 tools/binary_inventory.py <path> [<path> ...] [--format json|tsv]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

_CHUNK = 1 << 20

# --- architecture lookup tables (partial; extend as needed) ---------------

COFF_MACHINES = {
    0x014C: "x86 (i386)",
    0x0162: "MIPS R3000",
    0x0166: "MIPS R4000",
    0x0184: "Alpha AXP",
    0x01A2: "Hitachi SH3",
    0x01A6: "Hitachi SH4",
    0x01C0: "ARM (little-endian)",
    0x01C4: "ARMv7/Thumb-2",
    0x01F0: "PowerPC",
    0x0200: "IA-64",
    0x8664: "x86-64 (AMD64)",
    0xAA64: "ARM64",
}

MACHO_CPUTYPES = {
    6: "MC680x0",
    7: "x86 (i386)",
    10: "MC98000",
    12: "ARM",
    18: "PowerPC",
    0x01000007: "x86-64",
    0x0100000C: "ARM64",
    0x01000012: "PowerPC 64",
}

ELF_MACHINES = {
    3: "x86 (i386)",
    8: "MIPS",
    20: "PowerPC",
    21: "PowerPC 64",
    40: "ARM",
    62: "x86-64",
    183: "AArch64",
    243: "RISC-V",
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(_CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def file_output(path: Path) -> str:
    try:
        r = subprocess.run(
            ["file", "-b", str(path)],
            capture_output=True, text=True, timeout=30,
        )
        return r.stdout.strip() if r.returncode == 0 else f"file error: {r.stderr.strip()}"
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"file unavailable: {e}"


def identify(path: Path) -> dict:
    """Header-level format identification. Returns dict with
    'format' (short label), 'detail', and optional 'arch'/'archs'."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x200)
    except OSError as e:
        return {"format": "unreadable", "detail": str(e)}

    if len(head) < 4:
        return {"format": "data/empty", "detail": "too small to identify"}

    # --- Mach-O (thin) ---
    magic_le = struct.unpack_from("<I", head)[0]
    magic_be = struct.unpack_from(">I", head)[0]
    if magic_le in (0xFEEDFACE, 0xFEEDFACF) or magic_be in (0xFEEDFACE, 0xFEEDFACF):
        is64 = magic_le == 0xFEEDFACF or magic_be == 0xFEEDFACF
        # cputype is stored in the file's endianness
        if magic_le in (0xFEEDFACE, 0xFEEDFACF):
            cputype = struct.unpack_from("<i", head, 4)[0]
        else:
            cputype = struct.unpack_from(">i", head, 4)[0]
        arch = MACHO_CPUTYPES.get(cputype, f"cputype {cputype}")
        return {
            "format": "Mach-O",
            "detail": "64-bit" if is64 else "32-bit",
            "arch": arch,
        }

    # --- Mach-O fat / universal ---
    if magic_be == 0xCAFEBABE or magic_be == 0xCAFEBABF:
        nfat = struct.unpack_from(">I", head, 4)[0]
        archs = []
        is64 = magic_be == 0xCAFEBABF
        rec = 32 if is64 else 20
        for i in range(nfat):
            off = 8 + i * rec
            if off + 8 > len(head):
                break
            cputype = struct.unpack_from(">i", head, off)[0]
            archs.append(MACHO_CPUTYPES.get(cputype, f"cputype {cputype}"))
        return {
            "format": "Mach-O universal (fat)",
            "detail": f"{nfat} slice(s)",
            "archs": archs,
        }

    # --- ELF ---
    if head[:4] == b"\x7fELF":
        ei_class = head[4]  # 1=32-bit, 2=64-bit
        ei_data = head[5]   # 1=LE, 2=BE
        endian = "<" if ei_data == 1 else ">"
        e_machine = struct.unpack_from(endian + "H", head, 18)[0]
        return {
            "format": "ELF",
            "detail": {1: "32-bit", 2: "64-bit"}.get(ei_class, f"class {ei_class}"),
            "arch": ELF_MACHINES.get(e_machine, f"machine {e_machine}"),
        }

    # --- MZ family: PE, NE, LE/LX, plain DOS ---
    if head[:2] == b"MZ":
        if len(head) >= 0x40:
            e_lfanew = struct.unpack_from("<I", head, 0x3C)[0]
            try:
                with open(path, "rb") as f:
                    f.seek(e_lfanew)
                    sig = f.read(4)
                    if sig == b"PE\0\0":
                        machine = struct.unpack("<H", f.read(2))[0]
                        return {
                            "format": "PE/COFF",
                            "detail": f"e_lfanew=0x{e_lfanew:x}",
                            "arch": COFF_MACHINES.get(machine, f"machine 0x{machine:04x}"),
                        }
                    if sig[:2] == b"NE":
                        return {"format": "NE (16-bit Windows/OS2)",
                                "detail": f"e_lfanew=0x{e_lfanew:x}"}
                    if sig[:2] in (b"LE", b"LX"):
                        return {"format": f"{sig[:2].decode()} (VxD/OS2/Win9x)",
                                "detail": f"e_lfanew=0x{e_lfanew:x}"}
            except OSError:
                pass
        return {"format": "MZ (DOS executable)", "detail": "no PE/NE/LE/LX signature found"}

    return {"format": "data/unknown", "detail": head[:16].hex(" ")}


def iter_files(paths: list[Path]):
    """Yield regular files only; never follow symlinks, never leave the
    supplied roots, preserve explicit ordering, deduplicate."""
    seen = set()
    for p in paths:
        if not p.exists():
            yield p, "missing"
            continue
        if p.is_symlink():
            yield p, "skipped: symlink (not followed)"
            continue
        if p.is_file():
            rp = p.resolve()
            if rp not in seen:
                seen.add(rp)
                yield p, None
            continue
        if p.is_dir():
            root = p.resolve()
            for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
                dirnames.sort()
                for name in sorted(filenames):
                    f = Path(dirpath) / name
                    resolved = f.resolve()
                    if f.is_symlink() or not str(resolved).startswith(str(root) + os.sep):
                        continue
                    if f.is_file() and resolved not in seen:
                        seen.add(resolved)
                        yield f, None
            continue
        yield p, "skipped: not a regular file"


def inventory(paths: list[Path]) -> list[dict]:
    records = []
    for p, err in iter_files(paths):
        rec = {"path": str(p)}
        if err:
            rec["error"] = err
            records.append(rec)
            continue
        rec["size"] = p.stat().st_size
        rec["sha256"] = sha256_file(p)
        rec["file"] = file_output(p)
        rec.update(identify(p))
        records.append(rec)
    return records


def emit_json(records, out) -> None:
    json.dump({"binaries": records, "count": len(records)}, out, indent=2)
    out.write("\n")


def emit_tsv(records, out) -> None:
    out.write("path\tsize\tsha256\tformat\tarch\tfile\n")
    for r in records:
        arch = r.get("arch") or ",".join(r.get("archs", [])) or ""
        out.write("\t".join([
            r["path"],
            str(r.get("size", "")),
            r.get("sha256", ""),
            r.get("format", r.get("error", "")),
            arch,
            r.get("file", "").replace("\t", " "),
        ]) + "\n")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Static format identification of explicitly supplied files/directories."
    )
    ap.add_argument("paths", nargs="+", help="files or directories to inventory")
    ap.add_argument("--format", choices=["json", "tsv"], default="json")
    args = ap.parse_args(argv)

    records = inventory([Path(p) for p in args.paths])
    if not records:
        print("error: no files matched", file=sys.stderr)
        return 1
    if args.format == "json":
        emit_json(records, sys.stdout)
    else:
        emit_tsv(records, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
