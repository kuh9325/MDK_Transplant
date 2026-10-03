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


def have_stream_data():
    data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
    return (Path(data) / "STREAM" / "STREAM.BNI").exists()


@unittest.skipUnless(*have_prereqs())
class GodotStreamSmoke(unittest.TestCase):
    """Phase 19B.1 — mode-5 intermission presentation smoke.

    Runs the canonical launcher with --stream on the two boundary
    courses: c0 (alive exit -> white fill 0xff, mode-6 loader route)
    and c4 (final course — death latch -> black fill 0x00, mode-0
    frontend route). Asserts the presentation counters and deferred
    model/ribbon census via the smoke's own checks; the diagnostic
    line pins the golden event census verbatim.
    """

    def run_smoke(self, course, skill, seed):
        return subprocess.run(
            [str(RUN_SH), "--smoke", "--stream", str(course),
             "--skill", str(skill), "--seed", str(seed)],
            capture_output=True, text=True, timeout=600)

    def check_course(self, course, extra=()):
        if not have_stream_data():
            self.skipTest("no STREAM data in data root")
        proc = self.run_smoke(course, 1, 0xC0FFEE)
        out = proc.stdout + proc.stderr
        self.assertNotIn("SCRIPT ERROR", out)
        m = re.search(r"smoke\(stream\): (\d+) failure", out)
        self.assertIsNotNone(m, f"no smoke verdict in output:\n{out}")
        self.assertEqual(
            proc.returncode, 0,
            f"godot exited {proc.returncode}:\n{out}")
        self.assertEqual(m.group(1), "0", f"smoke failures:\n{out}")
        for pat in extra:
            self.assertIn(pat, out)

    def test_stream_course0(self):
        # Alive exit: fill 0xff, mode 6; census matches the 19A
        # golden draw-summary (spr/hud/mdl/rib verbatim). 19B.1A —
        # the sprite outcome split: zero true misses; every non-drawn
        # sprite is a faithful FUN_00403a40 raster outcome on the
        # LIGHT debris image (zero-size >>8 collapse, full clip, or
        # an all-pen-0 sampled footprint). 19B.3B1 — the audio census:
        # WIND singleton loop (1 play/1 stop), the ensure/restart
        # split, the listener feed == the core's counted 026f8 seam,
        # zero misses/drops/leaks.
        self.check_course(0, extra=(
            "fill=0xff",
            "exit handoff -> mode 6",
            "spr=6120 drawn=3047",
            "miss=0(res=0,meta=0) zsize=283 clip=1523 key=1267",
            "rib=139162",
            "pal=02b99a68d9993a25",
            "audio0: ev=81 plays=80(ensure=79,restart=1,loop=1,pos=0)"
            " stops=1 lstn=465 active=0 cap=0 miss=0/0/0"
            " cmds=78 starts=39 stops=39 live=0 wind=1/1",
            "asnd WIND plays=1 stops=1",
            "asnd RESCUE plays=1 stops=0",
            "asnd HITSIDE plays=39 stops=0"))

    def test_stream_course4(self):
        # Final course: the counter drains to the death latch ->
        # black fill 0x00 -> mode 0 frontend route. The death route
        # skips the rescue dock — RESCUE stays unplayed.
        self.check_course(4, extra=(
            "fill=0x00",
            "exit handoff -> mode 0",
            "spr=4764 drawn=3074",
            "miss=0(res=0,meta=0) zsize=111 clip=1291 key=288",
            "rib=97740",
            "pal=9fa9e040e0eedf25",
            "audio4: ev=80 plays=79(ensure=78,restart=1,loop=1,pos=0)"
            " stops=1 lstn=353 active=0 cap=0 miss=0/0/0"
            " cmds=68 starts=34 stops=34 live=0 wind=1/1",
            "asnd WIND plays=1 stops=1",
            "asnd HITSIDE plays=39 stops=0"))


def have_campaign_data():
    data = os.environ.get("MDK_DATA_ROOT") or str(DEFAULT_DATA)
    return (Path(data) / "STREAM" / "STREAM.BNI").exists() and \
        (Path(data) / "FALL3D" / "FALL3D.BNI").exists() and \
        all(have_level(d) for d in (3, 4, 5, 6, 7, 8))


@unittest.skipUnless(*have_prereqs())
class GodotCampaignSmoke(unittest.TestCase):
    """Phase 19B.3A — real campaign handoff route smoke.

    Drives the canonical launcher with --campaign N: the course's
    traversal loads, the diagnostic END_LEVEL mailbox arms the
    victory sequence (the same store the script op writes), and the
    mode-3 dispatcher tail runs FUN_004371bc + FUN_0042b270 — a real
    mode-3 -> 5 transition. The stream exits naturally, the mode-6
    loader pump advances to the next freefall, and layer ownership
    is checked at every edge. Courses 2/3 additionally gate the
    BONES.WHITE stale-bank oracle (no traversal .MAT may survive
    the teardown edge); course 4 covers the death -> mode-0 route.
    """

    def run_smoke(self, course):
        return subprocess.run(
            [str(RUN_SH), "--smoke", "--campaign", str(course)],
            capture_output=True, text=True, timeout=1800)

    def check_course(self, course, extra=()):
        if not have_campaign_data():
            self.skipTest("no STREAM/FALL3D/TRAVERSE data in data root")
        proc = self.run_smoke(course)
        out = proc.stdout + proc.stderr
        self.assertNotIn("SCRIPT ERROR", out)
        m = re.search(r"smoke\(campaign\): (\d+) failure", out)
        self.assertIsNotNone(m, f"no smoke verdict in output:\n{out}")
        self.assertEqual(
            proc.returncode, 0,
            f"godot exited {proc.returncode}:\n{out}")
        self.assertEqual(m.group(1), "0", f"smoke failures:\n{out}")
        for pat in extra:
            self.assertIn(pat, out)

    def test_campaign_course0(self):
        # Base route: traversal -> mode 5 -> mode 6 -> freefall
        # (levelId advances 0 -> 1) + in-session repeat entry. The
        # audio census rides the real route — WIND singleton and a
        # zero-miss drain under the carried session state.
        self.check_course(0, extra=(
            "mode-3 dispatcher tail -> mode 5",
            "exit -> mode 6",
            "loader exit -> mode 2 (freefall)",
            "loader advanced levelId -> 1",
            "miss=0/0/0",
            "wind=1/1",
            "asnd WIND plays=1 stops=1"))

    def test_campaign_course2(self):
        # BONES.WHITE stale-bank oracle — the traversal .MAT must not
        # survive the FUN_004371bc -> FUN_0042b270 edge.
        self.check_course(2, extra=(
            "oracle: white_slot=",
            "resolved=false bankB=false",
            "loader exit -> mode 2 (freefall)",
            "miss=0/0/0",
            "wind=1/1",
            "asnd WIND plays=1 stops=1"))

    def test_campaign_course3(self):
        self.check_course(3, extra=(
            "oracle: white_slot=",
            "resolved=false bankB=false",
            "loader exit -> mode 2 (freefall)",
            "miss=0/0/0",
            "wind=1/1",
            "asnd WIND plays=1 stops=1"))

    def test_campaign_course4(self):
        # Final course on the REAL campaign route: the carried
        # traversal globals (health 150, shared rand) keep the hero
        # latch — exit -> mode 7 -> the LEVEL5 traversal
        # continuation. The death->0 arm is the standalone
        # --stream 4 convention (fresh 100/seed) plus the core suite.
        self.check_course(4, extra=(
            "course-4 carried-health exit -> mode 7",
            "mode 7 -> mode 3 (LEVEL5 continuation)",
            "miss=0/0/0",
            "wind=1/1",
            "asnd WIND plays=1 stops=1"))


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
