# Local chaingun verification

`MdkChaingunAudit.java` is a read-only navigation helper for an **existing**
analyzed BUILD_A `MDK95.EXE` program. It prints bounded reference/call/flow
metadata, not executable bytes, instruction text, symbols, or decompiled
functions. It does not assign either sound to a weapon or decide what
`FUN_00469668` does. Those findings require the short local UI review below.

Original semantics remain **UNKNOWN — NEEDS LOCAL ORIGINAL-BUILD VERIFICATION**
until supported by the returned findings. See the
[evidence policy](../../docs/reverse-engineering/EVIDENCE_POLICY.md) and
[audio audit](../../docs/reverse-engineering/KURT_CHAINGUN_AUDIO.md).

## 1. Locate and verify the existing inputs

On the owner's Mac, from the repository checkout:

```sh
MDK_REPO="$HOME/Developer/MDK-Native"
MDK_EXE="$MDK_REPO/original/installed/MDK95.EXE"
test -f "$MDK_EXE" && shasum -a 256 "$MDK_EXE"
rg --files --hidden --no-ignore "$MDK_REPO/analysis-private/ghidra" |
  rg '(^|/)(analyzeHeadless|[^/]+\.gpr)$'
```

These locations are expected, not remotely verified. Use the actual paths
if different. Stop if the executable is missing or its hash differs from:

```text
e57bd63ba6af1ce201e3dd28a352e36ffa737267a9be0ed733fb81e6c40ba788
```

**OBSERVED, previously recorded:** this identifies the NoCD-modified BUILD_A
Windows executable, not pristine retail. All following addresses are its
Windows virtual addresses, not file offsets or DOS/PPC addresses. The
helper also checks stored import hash, PE format, `x86:LE:32:default`,
4-byte pointers, base `0x400000`, and mapped anchors. Stored import hash
does **not** prove the analysis program has never had byte patches; use the
matching unpatched analysis database. Do not change a hash or rebase a
different binary to get through the guard.

Use the same Ghidra version that created the existing project (the recorded
toolchain is 12.1.3). In its Project Data UI, note the exact program basename
and any internal folder. Close the project normally before headless access;
do not remove locks, force an upgrade, or overwrite an existing program.

## 2. Run the helper without saving or re-analysis

Set these values from the actual installation/project. The project name
excludes `.gpr`; the internal folder is empty or begins with `/`:

```sh
MDK_GHIDRA='/absolute/path/to/ghidra_12.1.3_PUBLIC'
MDK_PROJECT_PARENT='/absolute/path/containing/the/project.gpr'
MDK_PROJECT_NAME='actual-project-name'
MDK_INTERNAL_FOLDER=''
MDK_PROGRAM_BASENAME='MDK95.EXE'
```

Run in a subshell so guard failures stop this procedure. The `.gpr` and
`.rep` checks prevent accidentally pointing the command at a new project.
The exact `-process` basename must not contain `/`, `*`, or `?`:

```sh
(
  set -eu
  test -x "$MDK_GHIDRA/support/analyzeHeadless"
  test -f "$MDK_PROJECT_PARENT/$MDK_PROJECT_NAME.gpr"
  test -d "$MDK_PROJECT_PARENT/$MDK_PROJECT_NAME.rep"
  case "$MDK_PROGRAM_BASENAME" in ''|*/*|*\**|*\?*) exit 2 ;; esac
  test -d "$MDK_REPO/analysis-private"
  MDK_REPORT_DIR=$(mktemp -d "$MDK_REPO/analysis-private/chaingun-audit.XXXXXX")
  "$MDK_GHIDRA/support/analyzeHeadless" "$MDK_PROJECT_PARENT" \
    "$MDK_PROJECT_NAME$MDK_INTERNAL_FOLDER" \
    -process "$MDK_PROGRAM_BASENAME" -readOnly -noanalysis \
    -scriptPath "$MDK_REPO/tools/ghidra" \
    -log "$MDK_REPORT_DIR/headless.log" \
    -scriptlog "$MDK_REPORT_DIR/script.log" \
    -postScript MdkChaingunAudit.java \
    > "$MDK_REPORT_DIR/console.log" 2>&1
  sed -n 's/.*\(MDK_AUDIO|.*|END\).*/\1/p' "$MDK_REPORT_DIR/script.log" \
    > "$MDK_REPORT_DIR/index.txt"
  cat "$MDK_REPORT_DIR/index.txt"
  printf 'Local logs: %s\n' "$MDK_REPORT_DIR"
)
```

**DOCUMENTED:** `-process -readOnly -noanalysis` processes the existing
program without saving changes or running analysis. It is not a zero-write
filesystem sandbox: Ghidra can create project locks, script caches, and
logs. The helper itself uses only database getters/iterators and printing.
No import, overwrite, commit, recursive, or analysis commands are needed.
See the [official headless documentation](https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/RuntimeScripts/support/analyzeHeadlessREADME.md).

Require a `GUARD_OK` row **and** a `COMPLETE` row; launcher exit status alone
is insufficient. A refusal, exception, cancellation, or absent completion
means no complete report. Keep raw logs local; return only the compact
facts described below. Do not add generated output or private analysis to
GitHub. If project/version/guard checks fail, return the reason rather than
importing a replacement or altering the existing project.

## 3. Read the index, then verify the actual consumers locally

The default index covers:

- All **stored** references to each of the four bytes at `0x0054c5e0`
  (`MULTIFIRE` slot) and `0x0054c5e4` (`GATTFIRE` slot), including references
  from data or outside recognized functions. Zero references is reported.
- The existing functions at `0x00469668`, `0x00432f84`, `0x00465228`, plus
  functions containing those slot references. Registration/writes are
  candidates to inspect, not automatically playback consumers.
- Entry references, calls/jumps, branch successors, fallthrough, overrides,
  and references to the two slots, `0x00540c74` (latch), and `0x0054161f`
  (`ammo[0]`) **inside the selected functions**. It does not enumerate all
  ammo/latch uses across the executable.

Rows are existing analysis-database facts, not a recovered execution trace.
`CALL`/`EDGE` gives a callsite/target, not argument values. `api=` labels come
from the recorded public sound API map. Cross-body `JUMP` edges are tail-call
candidates; computed edges, thunks, indirect calls, and return-value gates
need review. An address-sorted list does not establish runtime ordering.
Flow data can include analysis/user overrides; `override=` exposes them.

The 400-row cap is explicit: `TRUNCATED` lists an omitted-row count.
`INCOMPLETE` means missing definitions or indirect/unresolved flows;
`OK_DB_ONLY` means database iteration completed, **never** original behavior
verified. All reports retain `weapon_semantics=UNKNOWN`. Even a complete
index misses unrecorded aliases, pointer copies, array-base and wider
overlapping accesses. Inspect those locally before claiming full coverage.

To narrow a truncated report or follow a wrapper, repeat the command with
one to eight **exact function-entry VAs** after the script name, for example:

```text
-postScript MdkChaingunAudit.java 0x00469668 0x00432f84
```

This replaces the default function-detail selection; the sound-slot xref
index is still included. Undefined entries remain gaps; the helper never
creates functions or disassembles bytes.

In Ghidra's Listing/Decompiler UI, inspect only the identified sites:

1. Verify the two name-to-slot assignments in `FUN_0043394c`; distinguish
   registration writes from reads which feed actual playback calls.
2. For every slot read, follow the handle through registers/stack, aliases,
   and wrapper calls to the sound API. Record the consumer address, exact
   ammo/latch predicate, and actual operation/mode. Check instructions
   locally to verify Watcom argument conventions; decompiler parameter
   order is not sufficient. Do this independently for both sounds.
3. Inspect `FUN_00469668` and its actual callees without assuming an audio
   role. If it only changes UI/animation state, report that and continue
   following the slot references to the real audio consumer.
4. Follow `FUN_00465228` press/hold/release gates. Determine whether held
   ticks spawn, ensure, restart, or make no playback call; identify overlap
   and the exact release/interrupt stop path. Verify the mode argument of
   `FUN_00402388` rather than inferring it from the API address.
5. Follow `FUN_00432f84` through exact and overshoot depletion. Record a
   path-ordered sequence of the ammo decrement/clamp, any notification, and
   actual sound calls, with addresses and branch predicates. Distinguish
   pre-drain charged damage from the ammo value read by the sound consumer.
6. Inspect pickup-while-held, scope, knock/death, end-level and teardown
   paths reached from these consumers. Unknown paths stay UNKNOWN.

Generic API semantics are recorded in §228/232 of the reconstruction;
which calls each weapon reaches still requires the observations above.
Looping also needs each resource's actual flags. The audio audit has
[read-only SNI/WAVE metadata commands](../../docs/reverse-engineering/KURT_CHAINGUN_AUDIO.md#minimal-read-only-verification-on-the-owners-mac)
that print no PCM payload.

## 4. Return this compact result

Return `GUARD_OK`, `COMPLETE`, relevant `SLOT_REF`/`FLOW`/`EDGE` rows, and the
following table filled from the local review. Do not send binaries, SNI
payloads, raw logs, decompiler functions, or instruction listings.

| Finding | Compact text to return |
|---|---|
| Identity | Actual executable SHA-256, Ghidra version, saved program has no byte patches: yes/no/unknown |
| `FUN_00469668` | Proven effects and actual callees; direct audio role yes/no/unknown; evidence addresses |
| Ordinary fire | Exact ammo/latch predicate; sound name/slot; API/callsite; operation/mode; repeat/overlap; stop condition |
| Super fire | Same fields, independently verified |
| Depletion | Exact/overshoot branch; ordered ammo writes and sound calls; first following tick; evidence addresses |
| Transitions | Pickup while held, release/repress, scope, knock/death/end-level/teardown; calls or UNKNOWN |
| Resources | Bank SHA-256 and both raw flags/volume words; loop bit, rate/channels/bits/frame count |
| Gaps | Unresolved indirect references, truncation, missing definitions, or uncertainty |

Prefix each behavioral conclusion with OBSERVED, CORROBORATED, DOCUMENTED,
HYPOTHESIS, or UNKNOWN. Include the decisive branch/call addresses and
argument values in prose (e.g. "predicate …; callsite …; sound slot …;
mode …"), not a copied code body. Resource names alone prove no mapping.

Once these findings establish the mapping and operations, the next change
on the existing PR branch is the runtime audio implementation and
weapon-specific regression tests. The owner then performs the documented
macOS audible acceptance cases; another audit is not a substitute for that
implementation.

## Synthetic validation (maintainers, Linux)

The repeatable test uses public Ghidra 12.1.3 APIs, JDK 21, and GNU
`as`/`objcopy`/`ld`. It builds a hand-authored PE32 fixture under `work/`;
it downloads nothing and needs no original files:

```sh
python3 tests/ghidra/run_synthetic.py \
  --ghidra-home /path/to/ghidra_12.1.3_PUBLIC \
  --java-home /path/to/jdk21
```

The fixture setup intentionally mocks the stored import hash to exercise
the production guard. Before any fixture mutation it verifies a synthetic
basename, a fixed synthetic marker, and the actual generated-file hash;
it refuses the original MDK hash. Never run the **test setup** script on
the owner's analysis project. Only `MdkChaingunAudit.java` belongs in that
project's verification procedure.

Tests cover complete/indirect reports, SHA/format/base/mapping guards,
missing functions, argument handling, truncation, the setup's own guard,
and a saved synthetic project reopened with the documented `-process
-readOnly -noanalysis` command. Program modification/listing snapshots
must remain identical across the production helper. These tests establish
helper behavior only, not original weapon mappings, macOS operation, or
audible playback.
