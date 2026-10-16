# Kurt normal / Super Chaingun audio audit

Status, 2026-10-15: **NEEDS LOCAL ORIGINAL-BUILD VERIFICATION**.
This change does **not** restore firing audio. It records the implementation
gap, adds synthetic regressions at the known boundaries, and identifies the
evidence needed for a faithful restoration. Neither owner-confirmed defect
is closed: ordinary Kurt firing is silent; Super Chaingun needs its distinct
original sound. No weapon, audio, animation, or resource-loading behavior is
changed by this audit.

Evidence levels follow [EVIDENCE_POLICY.md](EVIDENCE_POLICY.md). Statements
about current source/tests below are implementation observations, not new
observations of an original executable. Original-build claims marked
OBSERVED refer to the existing recorded evidence, which was not re-observed
in this cloud session. Only public repository evidence was available.

## Findings and root causes

| Evidence | Finding | Consequence |
|---|---|---|
| Current source | `playerPunch()` performs the unscoped firing hitscan but emits no gun sound in either ammo branch. `player_animation.cpp` emits other player sounds, but contains no explicit `MULTIFIRE` or `GATTFIRE` emitter. | The normal/charged firing path lacks explicit gun-audio integration. |
| Current source | `playerFireLatch()` counts notification calls on press/release; `playerPunch()` counts another on charge depletion. Other notification seams occur on scope entry, tumble/death, scripted knock, and end-level. | Counters do not implement these side effects. Their presence does **not** prove that `FUN_00469668` plays audio. |
| OBSERVED, recorded [§145](../GAMEPLAY_RECONSTRUCTION.md#145-combat-sound-callsites--classification-only-observed-names) | The BUILD_A sound table binds `MULTIFIRE` to `0x54c5e0` and `GATTFIRE` to `0x54c5e4` in `FUN_0043394c`. | Registration establishes identifiers, not which weapon uses them. |
| UNKNOWN | Normal-versus-Super mapping, actual sound consumer, playback operation, repeat/overlap rule, record loop flags, stop conditions, and depletion switch order. | Do not select a name by its spelling or introduce per-frame play/restart, forced looping, or speculative stops. |
| Current source | §230 claimed the weapon sounds were wired; §233 declared traversal SFX complete. Neither claim covers these missing emitters. | The reconstruction notes now retain a dated correction and reopen this audio seam. |

The missing integration is established; the exact omitted original calls
are **UNKNOWN**. In particular, the direct audio role of `FUN_00469668` must
be verified independently. Its auto-generated name, notification counters,
and existing "HUD pushes" comments are not evidence of its implementation.
The history contains the registration table and counted notifications, but
no sufficient weapon-specific selector or playback description.

## What is already supported

**OBSERVED, recorded §127–128:** `fieldC74` gates normal-mode firing;
mount/exclusion gates also apply. Positive **pre-drain** `ammo[0]` selects
the charged mechanics: subtract `frameStep`, use sixfold damage, and keep
that charged damage on the depletion frame. Depletion clamps ammunition to
zero, removes the first inventory id 6 when present, and reaches the counted
notification after that zero write. The following firing tick is normal
unless charge is replenished.
This establishes gameplay ordering, not when a sound must change.

**OBSERVED, recorded §228/232:** the generic sound API distinguishes these
operations; their applicability to either chaingun is still UNKNOWN:

| Core event | Recorded API semantics |
|---|---|
| `kPlayOnce` | `FUN_004022b8`: unowned spawn; another call can overlap. The record controls looping despite the event's name. |
| `kEnsurePlaying` | `FUN_00402388(sound,0)`: no-op if any matching active instance exists, otherwise spawn. |
| `kRestart` | `FUN_00402388(sound,nonzero)`: stop matching instances, then spawn. |
| `kStop` | `FUN_0040210c`: release every matching record instance, irrespective of ownership. |

Current playback path, verified by source inspection:

1. `traversalAudioEmit()` appends ordered name/op/frame/sequence events.
2. `MdkBridge::drain_audio_fx()` applies the batch to the existing mixer,
   clears the event queue, ticks voices, and drains start/params/stop commands.
3. The bridge resolves level S/O banks, `TRAVERSE/TRAVERSE.SNI`, and
   `MISC/MDKSOUND.SNI`; it decodes the locally supplied record. Authored
   rate/volume/loop metadata is retained. Missing records produce no voice.
4. `main.gd::_drain_audio_fx()` consumes the commands with Godot players.
   PCM8 conversion occurs at the host boundary; PCM16 passes through.

No change to these shared paths is justified by the missing gun emitters.
Existing sniper, impact, enemy, footstep, chute, ambient, and music behavior
is retained. Source inspection and synthetic mixer tests are not an audible
playback test or proof of the original weapons' semantics.

## Regression scope and cloud validation

`tests/native/test_main.cpp` adds:

- `test_player_chaingun_control_boundaries`: normal/charged target damage,
  charge drain, exact and overshoot depletion, the charged depletion frame
  followed by normal damage, inventory removal, trigger release/repress,
  priority suppression, rapid normal/charged transitions, and existing
  standing/running fire event posts. Scoped selection/cadence fields are
  checked for unintended mutation. It does not assert silence
  or invent gun-audio events.
- `test_traversal_named_sni_audio`: synthetic `MULTIFIRE` and `GATTFIRE`
  records parsed/decoded into the mixer, distinct PCM and authored metadata,
  named ensure/stop behavior, non-loop expiry versus loop survival, missing
  either/both records, and preservation of an unrelated active voice.
  Fixture rates, volumes, PCM, and loop bits are deliberately synthetic;
  they are **not** claims about the original resources or weapon mappings.

Portable Linux C++ core/frontend tests and Python tests are run without
original data. The root CMake project is macOS-specific; the cloud test
build uses a separate scratch CMake project with the same core/input/test
sources. GNU compilation needs `-include algorithm` for an existing
missing `<algorithm>` include in `freefall_scene.cpp`; that workaround is
confined to the test build. No production source fix is bundled for it.
Results: native **583866 checks, 0 failures** (82 new assertions), frontend
**340 checks, 0 failures**, CTest **2/2 passed**; Python **17 passed,
16 skipped**. Godot integration tests requiring its built extension/original
data are skipped, not counted as playback passes. `git diff --check` passed.

**Not performed:** macOS native build, original executable analysis, real
SNI resolution/decode, Godot gameplay playback, or audible comparison.
All normal/Super audio acceptance cases remain **NEEDS LOCAL ORIGINAL-BUILD
VERIFICATION**, even when their portable gameplay or synthetic-pipeline
regressions pass.

## Minimal read-only verification on the owner's Mac

2026-10-16: a runnable, guarded Ghidra helper is now available at
[`tools/ghidra/MdkChaingunAudit.java`](../../tools/ghidra/MdkChaingunAudit.java).
Start with its [local procedure and compact return table](../../tools/ghidra/README.md).
It uses an existing project with `-process -readOnly -noanalysis`, prints
reference/call/flow metadata, and never exports instruction/decompiler
bodies. Its index locates candidate consumers; the local UI review must
establish actual arguments, predicates, aliases, and runtime ordering.

Use the existing legal installation and private analysis in place. Do not
upload binaries, SNI payloads, private analysis files, or substantial
disassembly. Return only compact text findings with build hash, address,
predicate, operation/mode, call order, and resource metadata.

First identify the actual installation root and filenames; the supplied
Mac-local DOS installation path has not been verified in this cloud task:

```sh
MDK_ORIGINAL_ROOT='/absolute/path/to/existing/local/mdk'
rg --files --hidden --no-ignore "$MDK_ORIGINAL_ROOT" | rg -i '(mdk95\.exe|mdkdos\.exe|\.sni)$'
```

If `MDK95.EXE` is present, use its actual path:

```sh
MDK_EXE='/absolute/path/to/existing/MDK95.EXE'
shasum -a 256 "$MDK_EXE"
python3 tools/exe_headers.py "$MDK_EXE" --format text
```

**OBSERVED, recorded BUILD_A identity:** 684544-byte PE32 x86 `MDK95.EXE`,
SHA-256 `e57bd63ba6af1ce201e3dd28a352e36ffa737267a9be0ed733fb81e6c40ba788`.
[EXECUTABLE_MAP.md](EXECUTABLE_MAP.md) identifies this as a NoCD-modified
provisional runtime source, not verified pristine retail. A different hash
requires independent address resolution. All addresses below are Windows
BUILD_A virtual addresses, **not file offsets or DOS/PPC addresses**. A
DOS-only installation cannot directly verify these Windows addresses.

Use the existing locally built inspector for directory metadata. Set its
path to the actual local binary; the native output filename is
`mdk-inspect` (CMake target `mdk_inspect`):

```sh
MDK_INSPECT='./build/mdk-inspect'
"$MDK_INSPECT" --data-path "$MDK_ORIGINAL_ROOT" --entries TRAVERSE/TRAVERSE.SNI
"$MDK_INSPECT" --data-path "$MDK_ORIGINAL_ROOT" --entries MISC/MDKSOUND.SNI
```

Inspect the relevant level S/O banks with the same `--entries` command if
needed. Keep only the `MULTIFIRE`/`GATTFIRE` rows and source-bank hash for
the text report. The output includes raw `field0x0c`, payload file offset,
and size; it does not dump waveform bytes. **OBSERVED, recorded §232:**
low 16 bits are flags (bit0 loop, bit1 music class), high 16 bits authored
volume. Record both raw and interpreted values. To inspect WAVE metadata
without exporting PCM, substitute one row's **fileOff** and **size** below
(repeat independently for each sound). This uses the standard Python WAVE
reader on the documented raw RIFF payload; report an error rather than
assuming a format when it rejects the record:

```sh
MDK_BANK='/absolute/path/to/the/bank.SNI'
shasum -a 256 "$MDK_BANK"
python3 - "$MDK_BANK" '<fileOff from row>' '<size from row>' <<'PY'
import io, sys, wave
offset, size = (int(v, 0) for v in sys.argv[2:])
with open(sys.argv[1], 'rb') as bank:
    bank.seek(0, 2)
    assert offset >= 0 and size > 0 and offset + size <= bank.tell()
    bank.seek(offset)
    payload = bank.read(size)
with wave.open(io.BytesIO(payload), 'rb') as sound:
    print('channels=', sound.getnchannels(), 'rateHz=', sound.getframerate(),
          'bits=', sound.getsampwidth() * 8, 'frames=', sound.getnframes(),
          'durationSec=', sound.getnframes() / sound.getframerate())
PY
```

In the matching existing private analysis project, inspect the following
without saving or exporting proprietary analysis into this repository.
The public helper's procedure gives the exact headless command and
explains its guard, incomplete-report, and truncation signals; the older
private scripts are not required.

| Target | Minimal finding needed |
|---|---|
| `0x0043394c` | Verify literal name → slot registration for both sounds. |
| `0x0054c5e0`, `0x0054c5e4` | Enumerate **all** references; distinguish registration/teardown from actual consumers. Follow indirect consumers. |
| `0x00469668` and callers | Determine actual effects independently; follow real callees, retain UI-only effects as UI. |
| `0x00465228` tail, `0x00432f84` | Trace press, hold, release, gates, and charge depletion to the actual sound consumers. |
| Consumer reads of `0x00540c74` / `ammo[0]` (`0x0054161f`) | Exact signed/unsigned/zero predicate and whether selection reads pre- or post-drain ammo. |
| Reached sound APIs (`0x004022b8`, `0x00402388`, `0x0040210c`, positional paths if used) | Actual sound argument, operation/mode, order, volume/rate/owner, and return-value gates. Verify registers/stack; do not trust decompiler argument order. |

The existing `mdk-inspect --script-disasm` command handles game scripts,
not original x86 machine code. No consumer found means **UNKNOWN**; neither
the sound name nor a notification counter supplies the missing proof.

For each gun, return: sound identity; ammo predicate; trigger gate;
operation/mode; repeat/overlap rule; stop conditions; depletion switch
order; loop flag; authored wave metadata; evidence location/build hash.
Classify findings using the evidence policy. This is the minimum needed to
implement and assert weapon-specific audio regressions.

## Owner gameplay acceptance after evidence-backed implementation

Compare the legally owned original and the locally built port at the same
SFX setting, with music low enough to distinguish shots. Record each case
separately; a Dummy-driver smoke is not an audible check.

1. Ordinary fire: one tap, sustained hold, moving fire, and release. Match
   onset, repetition/overlap, and tail/cutoff against the original.
2. Super fire: repeat with positive `ammo[0]`; verify the proven distinct
   original resource and unchanged charge consumption/damage/cadence.
3. Depletion while held: observe the last charged tick and following normal
   tick; verify the exact sound-switch timing and no lingering wrong voice.
4. Rapid transitions: release/repress, collect Super while already firing,
   exhaust it, enter/leave scope, and fire again. Check for duplicated or
   stuck voices and compare original behavior rather than assuming stops.
5. Interruptions: priority suppression, knock/tumble, death, end-level,
   reload/save-restore, and bank teardown. Verify each proven stop path.
6. Missing-resource behavior: use the synthetic regression cases. Do not
   delete or rename original assets. Verify no substitute sound, crash, or
   unrelated audio loss in an isolated test setup.
7. Preservation: check sniper fire/reload, impacts, footsteps, chute,
   enemy/object sounds, ambience, and music alongside the gun sounds.

Until these are performed and the mapping/calls are evidenced, the two
reported audio defects remain open; this draft is not ready to merge as a
restoration.
