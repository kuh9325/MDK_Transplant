#!/usr/bin/env python3
"""Phase 7 (G1) — deterministic Godot-frontend smoke orchestration.

Runs the in-repo Godot 4 project headless with the dummy renderer
(the audit's proven path on Apple Silicon) and asserts the bridge's
self-check output. Skipped unless all three preconditions exist:

  * a Godot 4.7 binary — $MDK_GODOT_BIN, else `godot` on PATH;
  * the built extension — frontend/godot/bin/*/libmdkbridge.dylib
    (build via frontend/godot/build.sh);
  * local original data — original/installed (ignored, never
    committed) or $MDK_DATA_ROOT.

The smoke itself lives in frontend/godot/src/main.gd (--smoke); this
file only orchestrates the subprocess and cross-checks digests
against mdk-inspect when that binary is available.

Run: python3 -m pytest tests/test_godot_frontend.py
"""

import os
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GODOT_PROJECT = ROOT / "frontend" / "godot"
DYLIB_GLOB = "bin/*/libmdkbridge.dylib"
DEFAULT_DATA = ROOT / "original" / "installed"
INSPECT = ROOT / "build" / "mdk-inspect"
GOLDEN_GEOM = "f1cc72cbe4056174"   # LEVEL3 HMO_1, camera-independent
GOLDEN_ORDER = "9ff16337ea1582ec"  # frame-0 spawn camera


def find_godot():
    env = os.environ.get("MDK_GODOT_BIN")
    if env and Path(env).exists():
        return env
    return shutil.which("godot")


def have_prereqs():
    if not find_godot():
        return False, "no Godot binary (set MDK_GODOT_BIN)"
    if not list(GODOT_PROJECT.glob(DYLIB_GLOB)):
        return False, "extension not built (frontend/godot/build.sh)"
    data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
    if not (Path(data) / "TRAVERSE" / "LEVEL3" / "LEVEL3.DTI").exists():
        return False, f"no original data at {data}"
    return True, ""


@unittest.skipUnless(*have_prereqs())
class GodotFrontendSmoke(unittest.TestCase):
    def run_smoke(self) -> subprocess.CompletedProcess:
        godot = find_godot()
        data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
        cmd = [
            godot,
            "--headless",
            "--rendering-driver", "dummy",
            "--audio-driver", "Dummy",
            "--path", str(GODOT_PROJECT),
            "--", "--smoke", "--data-path", str(data),
        ]
        return subprocess.run(
            cmd, capture_output=True, text=True, timeout=120)

    def test_headless_smoke(self):
        proc = self.run_smoke()
        out = proc.stdout + proc.stderr
        m = re.search(r"smoke: (\d+) failure", out)
        self.assertIsNotNone(m, f"no smoke verdict in output:\n{out}")
        self.assertEqual(
            proc.returncode, 0,
            f"godot exited {proc.returncode}:\n{out}")
        self.assertEqual(m.group(1), "0", f"smoke failures:\n{out}")
        # The digest must match the mdk-inspect fold for the same
        # camera — the C++ bridge prints it as order_digest_hex.
        self.assertIn(GOLDEN_GEOM, out)
        self.assertIn(GOLDEN_ORDER, out)

    def test_inspect_crosscheck(self):
        """mdk-inspect prints the same digests for the same camera."""
        if not INSPECT.exists():
            self.skipTest("mdk_inspect not built")
        data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
        proc = subprocess.run(
            [str(INSPECT), "--data-path", data, "--arena-render",
             "TRAVERSE/LEVEL3/LEVEL3.DTI", "--arena", "HMO_1",
             "--start", "-3.16708231", "-7.92468166", "195.058044"],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(GOLDEN_GEOM, proc.stdout)
        self.assertIn(GOLDEN_ORDER, proc.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
