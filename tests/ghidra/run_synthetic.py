#!/usr/bin/env python3
"""Compile and exercise the Ghidra helper using hand-authored PE32 fixtures.

Requires Python 3, GNU as/objcopy/ld, Ghidra 12.1.3, and JDK 21. No downloads,
original files, or original behavior claims. All generated artifacts use work/.
Example: python3 tests/ghidra/run_synthetic.py --ghidra-home /path/to/ghidra \
    --java-home /path/to/jdk
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ghidra-home", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--saved-process-only", action="store_true")
    args = parser.parse_args()
    scripts = Path(__file__).resolve().parent
    repo = scripts.parents[1]
    work = (args.work_dir or repo / "work/ghidra-audit-tests").resolve()
    ghidra = args.ghidra_home.resolve()
    java = args.java_home.resolve()
    work.mkdir(parents=True, exist_ok=True)
    (work / "classes").mkdir(exist_ok=True)
    (work / "projects").mkdir(exist_ok=True)

    # Compile against the actual public APIs, never hand-written API stubs.
    classpath = os.pathsep.join(str(p) for p in sorted(ghidra.rglob("*.jar")))
    with (work / "javac.log").open("w") as log:
        subprocess.run([str(java / "bin/javac"), "-proc:none", "-Xlint:all,-path",
                        "-cp", classpath, "-d", str(work / "classes"),
                        str(repo / "tools/ghidra/MdkChaingunAudit.java"),
                        *map(str, sorted(scripts.glob("*.java")))],
                       stdout=log, stderr=subprocess.STDOUT, check=True)

    fixtures = {}
    for name, resolved in (("flow", 0), ("resolved", 1)):
        elf, coff = work / f"{name}-elf.o", work / f"{name}-pe.o"
        exe = work / f"SYNTHETIC-MDK-AUDIT-{name}.exe"
        subprocess.run(["as", "--32", "--defsym", f"RESOLVED={resolved}",
                        str(scripts / "synthetic_mdk_audit.S"), "-o", str(elf)], check=True)
        subprocess.run(["objcopy", "-O", "pe-i386", str(elf), str(coff)], check=True)
        subprocess.run(["ld", "-m", "i386pe", "--no-insert-timestamp",
                        "--image-base", "0x400000", "--section-start", ".text=0x401000",
                        "--section-start", ".data=0x540000", "--entry", "synthetic_start",
                        "--disable-dynamicbase", "--disable-reloc-section",
                        str(coff), "-o", str(exe)], check=True)
        fixtures[name] = (exe, hashlib.sha256(exe.read_bytes()).hexdigest())

    # name, setup mode, fixture, audit arguments, expected final record substring
    cases = [
        ("resolved", "pass", "resolved", [], "COMPLETE|status=OK_DB_ONLY"),
        ("indirect", "pass", "flow", [], "COMPLETE|status=INCOMPLETE"),
        ("wrong-sha", "wrong-sha", "flow", [], "REFUSED|reason=import_sha256"),
        ("wrong-format", "wrong-format", "flow", [], "REFUSED|reason=format_language"),
        ("wrong-base", "wrong-base", "flow", [], "REFUSED|reason=format_language"),
        ("unmapped-global", "unmapped-global", "flow", [], "REFUSED|reason=unmapped_global"),
        ("missing-core", "missing-core", "flow", [], "COMPLETE|status=INCOMPLETE"),
        ("invalid-argument", "pass", "flow", ["not_hex"], "REFUSED|reason=invalid_hex"),
        ("too-many-arguments", "pass", "flow", ["401000"] * 9,
         "REFUSED|reason=at_most_eight"),
        ("undefined-entry", "pass", "flow", ["401123"], "COMPLETE|status=INCOMPLETE"),
        ("custom-entry", "pass", "resolved", ["00465228"], "COMPLETE|status=OK_DB_ONLY"),
        ("truncate", "truncate", "flow", [], "COMPLETE|status=TRUNCATED"),
        ("setup-self-guard", "pass", "flow", [], "REFUSED|reason=import_sha256"),
    ]

    def run_case(case):
        name, mode, fixture, audit_args, expected = case
        exe, sha = fixtures[fixture]
        home, temp = work / "userhome" / name, work / "tmp" / name
        home.mkdir(parents=True, exist_ok=True)
        temp.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["JAVA_HOME"] = str(java)
        env["PATH"] = str(java / "bin") + os.pathsep + env.get("PATH", "")
        env["JAVA_TOOL_OPTIONS"] = env.get("JAVA_TOOL_OPTIONS", "") + (
            f' "-Duser.home={home}" "-Djava.io.tmpdir={temp}"')
        env["GHIDRA_HEADLESS_MAXMEM"] = "512M"
        cmd = [str(ghidra / "support/analyzeHeadless"), str(work / "projects"), name,
               "-import", str(exe), "-noanalysis", "-readOnly", "-deleteProject",
               "-log", str(work / f"{name}-application.log"),
               "-scriptlog", str(work / f"{name}-script.log"),
               "-scriptPath", str(scripts) + ";" + str(repo / "tools/ghidra")]
        if name == "setup-self-guard":
            cmd += ["-preScript", "SyntheticAuditSnapshot.java", "before"]
            sha = "0" * 64
        cmd += ["-preScript", "SyntheticMdkAuditSetup.java", mode, sha]
        if name != "setup-self-guard":
            cmd += ["-postScript", "SyntheticAuditSnapshot.java", "before"]
        cmd += ["-postScript", "MdkChaingunAudit.java", *audit_args,
                "-postScript", "SyntheticAuditSnapshot.java", "after"]
        log_path = work / f"{name}-console.log"
        with log_path.open("w") as log:
            subprocess.run(cmd, env=env, cwd=repo, stdout=log,
                           stderr=subprocess.STDOUT, timeout=150, check=True)
        text = log_path.read_text()
        assert "ERROR" not in text, log_path
        records = re.findall(r"MDK_AUDIO\|[^\r\n]*?\|END", text)
        assert records and expected in records[-1], (name, records[-1:])
        snapshots = re.findall(r"SYNTHETIC_INVARIANT\|(before|after)\|([0-9|]+)", text)
        assert len(snapshots) == 2 and snapshots[0][1] == snapshots[1][1], name
        if "REFUSED" in expected:
            assert not any("|COMPLETE|" in r for r in records), name
        if name == "setup-self-guard":
            assert "SYNTHETIC_SETUP_REFUSED|fixture_identity_mismatch" in text
        if name == "indirect":
            assert "|gaps=0|indirect_or_unresolved=1|" in records[-1]
        if name == "truncate":
            assert len(records) == 401 and "|rows=400|" in records[-1]
            assert "|omitted=0|" not in records[-1]
        if name == "resolved":
            assert any("|kind=JUMP|" in r for r in records)
            assert any("|global=ammo0|" in r for r in records)
            assert any("|api=spawn|outside_body=true|" in r for r in records)
            assert "|gaps=0|indirect_or_unresolved=0|" in records[-1]
        report = "\n".join(records)
        assert not any(token in report for token in ("bytes=", "mnemonic=", "decompile=",
                                                      "SYNTHETIC_")), name
        return name

    if not args.saved_process_only:
        with ThreadPoolExecutor(max_workers=2) as pool:
            for name in pool.map(run_case, cases):
                print(f"PASS {name}", flush=True)

    # Exercise the owner's documented workflow on a saved scratch project.
    # A fresh directory prevents this test from opening an existing project.
    project_dir = Path(tempfile.mkdtemp(prefix="saved-process-", dir=work / "projects"))
    home, temp = work / "userhome/saved-process", work / "tmp/saved-process"
    home.mkdir(parents=True, exist_ok=True)
    temp.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["JAVA_HOME"] = str(java)
    env["PATH"] = str(java / "bin") + os.pathsep + env.get("PATH", "")
    env["JAVA_TOOL_OPTIONS"] = env.get("JAVA_TOOL_OPTIONS", "") + (
        f' "-Duser.home={home}" "-Djava.io.tmpdir={temp}"')
    env["GHIDRA_HEADLESS_MAXMEM"] = "512M"
    exe, sha = fixtures["resolved"]
    base = [str(ghidra / "support/analyzeHeadless"), str(project_dir), "fixture",
            "-noanalysis", "-scriptPath", str(scripts) + ";" + str(repo / "tools/ghidra")]
    with (work / "saved-import-console.log").open("w") as log:
        subprocess.run(base + ["-import", str(exe), "-preScript",
                               "SyntheticMdkAuditSetup.java", "pass", sha],
                       env=env, cwd=repo, stdout=log, stderr=subprocess.STDOUT,
                       timeout=150, check=True)
    script_log = work / "saved-process-script.log"
    log_path = work / "saved-process-console.log"
    with log_path.open("w") as log:
        subprocess.run(base + ["-process", exe.name, "-readOnly",
                               "-log", str(work / "saved-process-application.log"),
                               "-scriptlog", str(script_log),
                               "-postScript", "SyntheticAuditSnapshot.java", "before",
                               "-postScript", "MdkChaingunAudit.java",
                               "-postScript", "SyntheticAuditSnapshot.java", "after"],
                       env=env, cwd=repo, stdout=log, stderr=subprocess.STDOUT,
                       timeout=150, check=True)
    text = log_path.read_text()
    assert "ERROR" not in text, log_path
    snapshots = re.findall(r"SYNTHETIC_INVARIANT\|(before|after)\|([0-9|]+)", text)
    assert len(snapshots) == 2 and snapshots[0][1] == snapshots[1][1]
    extracted = subprocess.run(["sed", "-n", r"s/.*\(MDK_AUDIO|.*|END\).*/\1/p",
                                str(script_log)], capture_output=True, text=True, check=True)
    (work / "saved-process-index.txt").write_text(extracted.stdout)
    lines = extracted.stdout.splitlines()
    assert any(line.startswith("MDK_AUDIO|GUARD_OK|") for line in lines)
    assert lines[-1].startswith("MDK_AUDIO|COMPLETE|status=OK_DB_ONLY|")
    assert all(line.startswith("MDK_AUDIO|") and line.endswith("|END") for line in lines)
    print("PASS saved-project-process-readonly", flush=True)
    count = 1 if args.saved_process_only else len(cases) + 1
    print(f"{count} synthetic Ghidra cases passed; no original behavior established.")


if __name__ == "__main__":
    main()
