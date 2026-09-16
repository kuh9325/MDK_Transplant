#!/usr/bin/env python3
"""Tests for tools/hash_manifest.py using synthetic temp files only.

Run: python3 tests/test_hash_manifest.py   (or: python3 -m pytest tests/)
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import hash_manifest  # noqa: E402


class HashManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "sub").mkdir()
        (self.root / "a.bin").write_bytes(b"hello mdk" * 100)
        (self.root / "sub" / "b.dat").write_bytes(b"\x00\x01\x02" * 7)

    def tearDown(self):
        self.tmp.cleanup()

    def test_manifest_entries(self):
        m = hash_manifest.build_manifest(self.root)
        self.assertEqual(m["file_count"], 2)
        by_path = {e["path"]: e for e in m["files"]}
        self.assertIn("a.bin", by_path)
        self.assertIn("sub/b.dat", by_path)
        self.assertEqual(by_path["a.bin"]["size"], 900)
        self.assertEqual(
            by_path["a.bin"]["sha256"],
            hashlib.sha256(b"hello mdk" * 100).hexdigest(),
        )

    def test_empty_dir(self):
        empty = self.root / "empty"
        empty.mkdir()
        m = hash_manifest.build_manifest(empty)
        self.assertEqual(m["file_count"], 0)

    def test_symlink_not_followed(self):
        outside = self.root.parent / (self.root.name + "_outside_secret")
        outside.write_bytes(b"should never be hashed")
        try:
            (self.root / "link").symlink_to(outside)
            m = hash_manifest.build_manifest(self.root)
            link = [e for e in m["files"] if e["path"] == "link"]
            self.assertEqual(len(link), 1)
            self.assertIsNone(link[0]["sha256"])
        finally:
            outside.unlink()

    def test_not_a_directory(self):
        with self.assertRaises(NotADirectoryError):
            hash_manifest.build_manifest(self.root / "a.bin")

    def test_cli_json(self):
        r = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "hash_manifest.py"),
             str(self.root), "--format", "json"],
            capture_output=True, text=True,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        m = json.loads(r.stdout)
        self.assertEqual(m["file_count"], 2)

    def test_cli_tsv(self):
        r = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "hash_manifest.py"),
             str(self.root), "--format", "tsv"],
            capture_output=True, text=True,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = r.stdout.strip().splitlines()
        self.assertEqual(lines[0], "path\tsize\tsha256")
        self.assertEqual(len(lines), 3)


if __name__ == "__main__":
    unittest.main()
