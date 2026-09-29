#!/usr/bin/env python3
"""Phase 7 (G1) — deterministic Godot-frontend smoke orchestration.

Drives the CANONICAL launcher (`frontend/godot/run.sh --smoke`) so the
test exercises the same path users run: Godot discovery, dylib check,
editor-free extension-list seeding, headless dummy-renderer launch.
Skipped unless all preconditions exist:

  * a Godot 4.7 binary — $MDK_GODOT_BIN, `godot` on PATH, or
    /Applications/Godot.app;
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
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GODOT_PROJECT = ROOT / "frontend" / "godot"
RUN_SH = GODOT_PROJECT / "run.sh"
DYLIB_GLOB = "bin/*/libmdkbridge.dylib"
DEFAULT_DATA = ROOT / "original" / "installed"
INSPECT = ROOT / "build" / "mdk-inspect"
GOLDEN_GEOM = "f1cc72cbe4056174"   # LEVEL3 HMO_1, camera-independent
GOLDEN_ORDER = "9ff16337ea1582ec"  # frame-0 spawn camera


def find_godot():
    env = os.environ.get("MDK_GODOT_BIN")
    if env and Path(env).exists():
        return env
    on_path = shutil.which("godot")
    if on_path:
        return on_path
    app = "/Applications/Godot.app/Contents/MacOS/Godot"
    return app if Path(app).exists() else None


def have_prereqs():
    if not find_godot():
        return False, "no Godot binary (MDK_GODOT_BIN/PATH//Applications)"
    if not RUN_SH.exists():
        return False, "frontend/godot/run.sh missing"
    if not list(GODOT_PROJECT.glob(DYLIB_GLOB)):
        return False, "extension not built (frontend/godot/build.sh)"
    data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
    if not (Path(data) / "TRAVERSE" / "LEVEL3" / "LEVEL3.DTI").exists():
        return False, f"no original data at {data}"
    return True, ""


@unittest.skipUnless(*have_prereqs())
class GodotFrontendSmoke(unittest.TestCase):
    def run_smoke(self):
        return subprocess.run(
            [str(RUN_SH), "--smoke"],
            capture_output=True, text=True, timeout=120)

    def test_headless_smoke(self):
        proc = self.run_smoke()
        out = proc.stdout + proc.stderr
        # A parse error here means the extension did not load —
        # fail loudly rather than pattern-matching around it.
        self.assertNotIn("SCRIPT ERROR", out)
        m = re.search(r"smoke: (\d+) failure", out)
        self.assertIsNotNone(m, f"no smoke verdict in output:\n{out}")
        self.assertEqual(
            proc.returncode, 0,
            f"godot exited {proc.returncode}:\n{out}")
        self.assertEqual(m.group(1), "0", f"smoke failures:\n{out}")
        # The digest must match the mdk-inspect fold for the same
        # camera — the bridge prints it as order_digest_hex.
        self.assertIn(GOLDEN_GEOM, out)
        self.assertIn(GOLDEN_ORDER, out)

    def test_inspect_crosscheck(self):
        """mdk-inspect prints the same digests for the same camera."""
        if not INSPECT.exists():
            self.skipTest("mdk-inspect not built")
        data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
        proc = subprocess.run(
            [str(INSPECT), "--data-path", data, "--arena-render",
             "TRAVERSE/LEVEL3/LEVEL3.DTI", "--arena", "HMO_1",
             "--start", "-3.16708231", "-7.92468166", "195.058044"],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(GOLDEN_GEOM, proc.stdout)
        self.assertIn(GOLDEN_ORDER, proc.stdout)


@unittest.skipUnless(*have_prereqs())
class GodotFrontendMenu(unittest.TestCase):
    """Phase 18B.2A — Godot frontend menu presentation smoke.

    Drives the canonical launcher with --frontend: the authoritative
    FrontendShell runs under the bridge while GDScript only presents
    and routes. The A-O scenario set covers root nav, save list,
    options/skill, help/abort (incl. the OBSERVED same-frame Esc
    self-cancel), transition ack, mode visibility, and the
    LASTGAME/Continue route — all under a dedicated user:// save
    root, never the original tree.

    --smoke-real-saves additionally points the store at the
    installed SAVES dir for the §24 read-only corpus check
    (full 1.SAV + header-only 2.SAV, fingerprint-verified).
    """

    def run_smoke(self, *extra):
        return subprocess.run(
            [str(RUN_SH), "--smoke", "--frontend", *extra],
            capture_output=True, text=True, timeout=300)

    def check_smoke(self, verdict, pats, *extra):
        proc = self.run_smoke(*extra)
        out = proc.stdout + proc.stderr
        self.assertNotIn("SCRIPT ERROR", out)
        m = re.search(verdict + r": (\d+) failure", out)
        self.assertIsNotNone(m, f"no verdict in output:\n{out}")
        self.assertEqual(
            proc.returncode, 0,
            f"godot exited {proc.returncode}:\n{out}")
        self.assertEqual(m.group(1), "0", f"failures:\n{out}")
        for pat in pats:
            self.assertIn(pat, out)

    def test_frontend_smoke(self):
        self.check_smoke(
            r"smoke\(frontend\)",
            ("smoke: frontend",
             "O: F10 in gameplay arms abort",
             "D: progression reached freefall (mode 2)",
             "temp: no writes into the data root"))

    def test_real_saves_readonly(self):
        data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
        if not (Path(data) / "SAVES" / "1.SAV").exists():
            self.skipTest("no installed SAVES corpus")
        self.check_smoke(
            r"smoke\(real-saves\)",
            ("R: full-save classification",
             "R: header-only classification",
             "R: SAVES dir untouched"),
            "--smoke-real-saves")


def have_freefall_data():
    data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
    return (Path(data) / "FALL3D" / "FALL3D.BNI").exists()


@unittest.skipUnless(*have_prereqs())
class GodotFreefallSmoke(unittest.TestCase):
    """Phase 16C — mode-2 freefall presentation smoke.

    Runs the canonical launcher over representative courses: c0/c2
    cover the model/anim/material/camera contract plus the
    traversal handoff; c4 additionally gates the bones flyby. The
    exit route is health-gated by the runtime (success -> mode 3,
    death -> mode 0); the smoke asserts route consistency itself.
    """

    def run_smoke(self, course, skill, seed):
        return subprocess.run(
            [str(RUN_SH), "--smoke", "--freefall", str(course),
             "--skill", str(skill), "--seed", str(seed)],
            capture_output=True, text=True, timeout=600)

    def check_course(self, course, skill, seed, extra=()):
        if not have_freefall_data():
            self.skipTest("no FALL3D data in data root")
        proc = self.run_smoke(course, skill, seed)
        out = proc.stdout + proc.stderr
        self.assertNotIn("SCRIPT ERROR", out)
        m = re.search(r"smoke\(freefall\): (\d+) failure", out)
        self.assertIsNotNone(m, f"no smoke verdict in output:\n{out}")
        self.assertEqual(
            proc.returncode, 0,
            f"godot exited {proc.returncode}:\n{out}")
        self.assertEqual(m.group(1), "0", f"smoke failures:\n{out}")
        for pat in extra:
            self.assertIn(pat, out)

    def test_freefall_course0(self):
        # Survival at this seed/skill -> traversal handoff.
        self.check_course(0, 0, 0xC0FFEE,
                          extra=("freefall handoff -> mode 3",))

    def test_freefall_course2(self):
        self.check_course(2, 1, 0xC0FFEE,
                          extra=("freefall handoff -> mode 3",))

    def test_freefall_course4(self):
        # Bones flyby (course>=4); the health gate decides the route.
        self.check_course(4, 2, 0xC0FFEE,
                          extra=("bones flyby on course>=4",))


def have_level(num):
    data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
    d = f"LEVEL{num}"
    return (Path(data) / "TRAVERSE" / d / f"{d}.DTI").exists()


@unittest.skipUnless(*have_prereqs())
class GodotCombatCloseout(unittest.TestCase):
    """Phase 17A — traversal combat presentation closeout.

    LEVEL6/LEVEL8 run the generic smoke + the bounded combat exercise
    (scope, state-1 shot mesh, bullet-cam window, impact shards,
    death boundary -> teardown burst + EXPLODE remnant, ttl reap).
    The save->restore golden runs on LEVEL3: a live mid-combat shot
    round-trips through the native full-save stream and presentation
    rebuilds from the restored authoritative state only.
    """

    def run_smoke(self, *extra):
        return subprocess.run(
            [str(RUN_SH), "--smoke", *extra],
            capture_output=True, text=True, timeout=600)

    def check_level_combat(self, level_dti, arena):
        proc = self.run_smoke("--level", level_dti, "--arena", arena)
        out = proc.stdout + proc.stderr
        self.assertNotIn("SCRIPT ERROR", out)
        self.assertNotIn("FAIL ", out)
        m = re.search(
            r"smoke\(generic\): \d+ object\(s\) enumerated, "
            r"(\d+) failure", out)
        self.assertIsNotNone(m, f"no smoke verdict in output:\n{out}")
        self.assertEqual(
            proc.returncode, 0,
            f"godot exited {proc.returncode}:\n{out}")
        # The combat exercise ran and passed — a level-load alone is
        # not the validation.
        self.assertIn("combat(generic): scoped after MMB pulse", out)
        self.assertIn("combat(generic): impact spawned the shard "
                      "burst", out)
        self.assertIn("combat(generic): kObjectTeardown event "
                      "drained", out)
        self.assertIn("combat(generic): unscoped", out)

    def test_level6_combat(self):
        if not have_level(6):
            self.skipTest("no LEVEL6 data in data root")
        self.check_level_combat(
            "TRAVERSE/LEVEL6/LEVEL6.DTI", "OLYM_1")

    def test_level8_combat(self):
        if not have_level(8):
            self.skipTest("no LEVEL8 data in data root")
        self.check_level_combat(
            "TRAVERSE/LEVEL8/LEVEL8.DTI", "GUNT_1")

    def test_save_restore_golden(self):
        proc = self.run_smoke("--save-restore")
        out = proc.stdout + proc.stderr
        self.assertNotIn("SCRIPT ERROR", out)
        m = re.search(r"smoke\(restore\): (\d+) failure", out)
        self.assertIsNotNone(m, f"no verdict in output:\n{out}")
        self.assertEqual(
            proc.returncode, 0,
            f"godot exited {proc.returncode}:\n{out}")
        self.assertEqual(m.group(1), "0", f"failures:\n{out}")
        # Spot-check the golden's load-bearing lines.
        for pat in ("restore: BULL carried >=1 active shot",
                    "restore: restored shot at its serialized pos",
                    "restore: no pre-restore combat events replay",
                    "restore: post-restore remnant exactly once"):
            self.assertIn(pat, out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
