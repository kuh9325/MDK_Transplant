#!/usr/bin/env python3
"""Tests for tools/binary_inventory.py using synthetic header fixtures only.

Run: python3 tests/test_binary_inventory.py   (or: python3 -m pytest tests/)
"""

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import binary_inventory  # noqa: E402


def make_mz_pe32(machine=0x014C) -> bytes:
    """Minimal MZ -> PE/COFF fixture (not executable; header-shaped only)."""
    buf = bytearray(0x200)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x80)  # e_lfanew
    buf[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", buf, 0x84, machine)
    return bytes(buf)


def make_macho64(cputype=0x0100000C) -> bytes:
    """Minimal little-endian Mach-O 64 fixture."""
    return struct.pack("<II", 0xFEEDFACF, cputype) + b"\0" * 64


def make_macho_fat(cputypes=(18, 7)) -> bytes:
    """Minimal fat Mach-O fixture (big-endian header)."""
    out = struct.pack(">II", 0xCAFEBABE, len(cputypes))
    space = 8 + 20 * len(cputypes)
    for ct in cputypes:
        out += struct.pack(">iiIII", ct, 0, 0, 0, 0)
    return out + b"\0" * (space - len(out) + 64)


def make_elf32(machine=3) -> bytes:
    """Minimal little-endian 32-bit ELF fixture."""
    e = bytearray(64)
    e[0:4] = b"\x7fELF"
    e[4] = 1  # 32-bit
    e[5] = 1  # LE
    struct.pack_into("<H", e, 18, machine)
    return bytes(e)


class IdentifyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, name, data):
        p = self.root / name
        p.write_bytes(data)
        return p

    def test_pe_i386(self):
        r = binary_inventory.identify(self._write("game.exe", make_mz_pe32()))
        self.assertEqual(r["format"], "PE/COFF")
        self.assertEqual(r["arch"], "x86 (i386)")

    def test_pe_arm64(self):
        r = binary_inventory.identify(self._write("x.exe", make_mz_pe32(0xAA64)))
        self.assertEqual(r["arch"], "ARM64")

    def test_macho_arm64(self):
        r = binary_inventory.identify(self._write("app", make_macho64()))
        self.assertEqual(r["format"], "Mach-O")
        self.assertEqual(r["arch"], "ARM64")

    def test_macho_fat(self):
        r = binary_inventory.identify(self._write("uni", make_macho_fat()))
        self.assertEqual(r["format"], "Mach-O universal (fat)")
        self.assertEqual(r["archs"], ["PowerPC", "x86 (i386)"])

    def test_elf_i386(self):
        r = binary_inventory.identify(self._write("prog", make_elf32()))
        self.assertEqual(r["format"], "ELF")
        self.assertEqual(r["arch"], "x86 (i386)")

    def test_plain_mz(self):
        mz = b"MZ" + b"\0" * 62  # e_lfanew = 0 -> no ext signature
        r = binary_inventory.identify(self._write("dos.exe", mz))
        self.assertEqual(r["format"], "MZ (DOS executable)")

    def test_unknown(self):
        r = binary_inventory.identify(self._write("blob", b"\xde\xad\xbe\xef" * 8))
        self.assertEqual(r["format"], "data/unknown")


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_inventory_fields(self):
        p = self.root / "x.exe"
        p.write_bytes(make_mz_pe32())
        recs = binary_inventory.inventory([p])
        self.assertEqual(len(recs), 1)
        r = recs[0]
        for key in ("path", "size", "sha256", "file", "format", "arch"):
            self.assertIn(key, r)
        self.assertEqual(r["format"], "PE/COFF")

    def test_directory_scan(self):
        (self.root / "sub").mkdir()
        (self.root / "a").write_bytes(make_macho64())
        (self.root / "sub" / "b").write_bytes(make_elf32())
        recs = binary_inventory.inventory([self.root])
        self.assertEqual(len(recs), 2)

    def test_missing_path_reported(self):
        recs = binary_inventory.inventory([self.root / "nope"])
        self.assertEqual(recs[0]["error"], "missing")

    def test_cli_json(self):
        p = self.root / "x.exe"
        p.write_bytes(make_mz_pe32())
        r = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "binary_inventory.py"), str(p)],
            capture_output=True, text=True,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("PE/COFF", r.stdout)


if __name__ == "__main__":
    unittest.main()
