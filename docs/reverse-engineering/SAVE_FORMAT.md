# MDK .SAV Format — Phase 14B reconstruction

Evidence levels follow `EVIDENCE_POLICY.md`. All addresses are BUILD_A
(`MDK95.EXE`). Five real saves validate every claim marked OBSERVED —
`original/installed/SAVES/{1,2}.SAV` (win32 lane) and
`runtime-private/dos/mdk/SAVES/{MDK,2,3}.SAV` (DOS lane) all parse with
zero checksum/packet errors through the native reader
(`src/core/save_game.*`).

## 1. Envelope (OBSERVED — FUN_004264f0 reader / FUN_00426440 writer close)

| offset | size | content |
|---|---|---|
| 0x00 | u32 LE | file size — must equal the physical file length |
| 0x04 | u32 LE | checksum — sum of raw bytes `[8, size)` |
| 0x08 | …    | packet stream (see below) |

There is **no version field** — the earlier "version = 2" note was the
SAVE packet's own payload size. Version compatibility is enforced by
the packet registry sizes alone (`Mismatch Version` diagnostics belong
to the container/`.BNI` reader family, not `.SAV`).

## 2. Packet stream (OBSERVED — FUN_00427970 writer / FUN_00427ab4 reader)

Each packet is `{u32 tag, u32 size, byte payload[size]}` — tag is a
little-endian fourcc (`"SAVE"` = `0x45564153`). Readers ask for a
specific tag through `FUN_00427ab4(expectedTag, dest)`; the tag/size
are compared against the expected pair and the static registry
(expectation failure → `"SAVE CORRUPT: %c%c%c%c:%d …"` diagnostics;
a `DEND` tag returns failure silently — it is the demo-stream end
sentinel).

### Obfuscation (OBSERVED — FUN_00426668 read byte / FUN_00426380 write byte)

The leading SAVE packet — tag, size, and its 2-byte payload — is
stored **clear**. Its payload is the cipher seed `u16`. Every byte
after it (stream offset 10 onward, file offset 0x12) is XORed against
a rolling key:

```
key   = seed & 0xff      (state byte 0x54be40)
delta = seed >> 8        (state byte 0x54be41)
per byte: plain = raw ^ key;  key = (key + delta) & 0xff
```

The writer seeds it from `rand()×2` (`FUN_0047d2b5` — the shared CRT
LCG; save bytes are nondeterministic across identical writes).

### Registry (OBSERVED — `0x49b2ac`, 16 {tag,size} records)

| tag | size | role | classification |
|---|---:|---|---|
| `DEMO` | 0 | demo playback header | stream control (dormant in BUILD_A) |
| `DEND` | 0 | demo-stream end sentinel | stream control |
| `KEYS` | 20 | demo input frame | stream control |
| `RATE` | 4 | demo playback pacing | stream control |
| `SAVE` | 2 | save header + cipher seed | envelope |
| `SEND` | 0 | save terminator | envelope |
| `GAME` | 24 | mode/levelId/health/state | A. campaign/progression |
| `THMB` | 3648 | 768B palette + 64×45 8bpp image | F. presentation (preview) |
| `MORE` | 52 | session/timer globals | A/G — partially mapped |
| `PLAY` | 239 | `0x541554..0x541642` player block | B/E — verbatim copy |
| `DAMP` | 724 | `0x540bfc` player motion block | B — transform at head |
| `CAME` | 200 | `0x540b28` camera block | F — pose at head |
| `AREN` | 1126 | `0x466` arena record | C |
| `ALIE` | 814 | `0x32e` dynamic-object record | D |
| `FAND` | 72 | `0x48` fan/volume record | C |
| `BULL` | 252 | `0x540ed4` shot-pool slot (×3) | E |

### Observed packet orders

```
header-only (LASTGAME, briefing saves):
    SAVE THMB GAME SEND
full (traversal F2 saves):
    SAVE THMB GAME MORE PLAY DAMP CAME (AREN ALIE* FAND*)* BULL BULL BULL SEND
```

`AREN+0x0c` is the following `ALIE` count and `AREN+0x10` the `FAND`
count — corroborated by the packet walk on both full saves (e.g.
`DANT_1` has 5 objects → exactly 5 ALIE packets follow it).

## 3. GAME packet (OBSERVED — writer FUN_00426e98, reader FUN_004278c0)

24 bytes, six little-endian dwords:

| offset | field | semantics |
|---|---|---|
| +0x00 | modeField | `<1000` → header-only save, value = `0x541492` clamped to `(mode==6)?6:3`. `>=1000` → full save, value = mode + 1000; the reader recovers `mode = (field + 0x18) & 0xff` |
| +0x04 | levelId | → `0x541498`; validated `[0,6)` |
| +0x08 | field8 | uninitialized stack dword in the original writer — real saves carry code-address garbage (`0x42ed31`, `0x2127ad`); never read |
| +0x0c | health | → `0x541554`; validated `1..150` (real saves carry 150) |
| +0x10 | deathCount | → `0x541637` |
| +0x14 | field54163b | → `0x54163b` |

Writer floor (header-only only): `health < 0x65` → stores `100`.
Health `>= 101` stores verbatim.

## 4. Field-level map of the other packets

- `PLAY` (239B) — byte copy of `0x541554..0x541642`:
  `+0x00` health (u32), `+0x04` HUD/item timer `0x541558`,
  `+0x08..+0xbb` the five 0x24-byte inventory slot records
  `0x54155c..0x541610` (`{i32 id, i32 charges, f32 anim[4],
  i32 slotX, i32 slotY, i32 aux}` — HUD item rows; both real saves
  carry populated records), `+0xbc` inventory count `0x541610`,
  `+0xc0` inventory selection `0x541614`, `+0xc2..+0xc5` the packed
  weapon-select/cadence dword `0x541616` — its low u16 (`+0xc2`) is
  restored as the selection aux, while the high u16 (`+0xc4/+0xc5` =
  `0x541618/19` = wpnSel0/wpnSel1) is reset by the loader post-copy
  along with `0x54161a/1b` (`+0xc6/+0xc7`), `+0xcb..+0xe2` the
  six-dword ammo block `0x54161f..0x541633`, `+0xe3` death counter
  `0x541637`, `+0xe7` `0x54163b`.
  The script VM's `0xaf` opcode scans the restored table and the
  punch drain consumes id-6 records (FUN_0046a3d8 semantics).
- `DAMP` (724B) — the `0x540bfc` motion block: `+0x00` player position
  xyz (f32), `+0x0c` previous position. Real saves confirm mid-arena
  coordinates (`403.0,770.28,-66.0` etc.) — full saves restore the
  player pose, they do not respawn at s0. The whole packet is field-
  classified in `save_full_restore.cpp::applyDamp`: every gameplay
  field is mapped (transform, collision, arena refs at `+0x4c/+0xa8/
  +0x134/+0x164/+0x168`, motion channels `+0x14c..+0x160`, script
  globals `+0x18c..+0x19c`, slide/anim state, counters, pending view),
  loader-cleared dwords are documented (`+0x114..+0x124`,
  `+0x140/+0x144`, `+0x16c`, `+0x1d4..+0x210`, `+0x2ac`), ambient-
  sound fades/channel records are carried dormant (audio seam), and
  the only unmapped spans are proven derived scratch, diagnostic
  counters, no-xref padding, and stale heap tokens.
- `CAME` (200B) — the `0x540b28` camera block: `+0x00` camera position,
  then the basis/orientation tail.
- `MORE` (52B, 13 dwords) — session/timer globals (`0x5414a0/a4/a8`
  transition timers, counters `0x5414d8`/`0x541518`, overhead-view aux
  `0x49b740..0x49b74c` family). `+0x00` is the level data length used
  by the loader's identity check. Individual names CORROBORATED by the
  writer/reader field order; per-field semantics partial.
- `AREN`/`ALIE`/`FAND` — raw arena/object/fan memory records; restore
  includes pointer fixups by saved object id (FUN_00427218's
  `FUN_00426f34`/`FUN_004262c8` passes). Script-VM context (`+0x108`
  PC, `+0x22c` wait, `+0x230` resume, `+0x248` call stack) lives inside
  the ALIE record — it persists verbatim.
- `BULL` (252B ×3) — the `0x540ed4` three-slot projectile pool, one
  packet per slot.

RNG: the shared CRT rand state is **not** in any packet — no packet
payload covers its address and no reader writes it. Loading does not
reseed; the stream just continues (CORROBORATED).

Skill (`0x54147a`): **not persisted** — it lives before the `0x541554`
block boundary, is absent from GAME, and is not in the MORE field
list (CORROBORATED).

## 5. Writer / loader call graph (OBSERVED)

```
write entry   FUN_00427ed4(path, thumbCtx, headerOnly)     ; EBX→0x54be2c
  FUN_004262dc(path)                    ; open "wb" + envelope state
  rand()×2 → u16 seed
  FUN_00427970("SAVE", &seed)           ; stored clear
  cipher arm: 0x54be40 = seed
  FUN_00427970("THMB", thumbCtx)        ; 0x49f010 — filled by
                                        ; FUN_00427e8c thumbnail gen
  FUN_00426e98                          ; GAME (+ world when !be2c)
    FUN_00426a0c                        ; MORE PLAY DAMP CAME AREN/ALIE/
                                        ; FAND BULL×3 — full only
  FUN_00427970("SEND", NULL)
  FUN_00426440                          ; flush cipher, patch size+sum

load entry    FUN_00427f94(path)
  FUN_00426618/FUN_004264f0             ; envelope: size, checksum,
                                        ; clear SAVE packet, arm cipher
  FUN_00427ab4("THMB", buf)             ; preview thumb (required)
  FUN_004278c0                          ; GAME → 0x54be2c = field<1000;
                                        ; restore 5 globals; validate
                                        ; levelId<6, 0<health<0x97
    header-only → early return
    full → FUN_00427218                 ; MORE→PLAY→level identity→
                                        ; fresh traversal load→
                                        ; DAMP CAME AREN/ALIE/FAND
                                        ; BULL + pointer fixups
  route: mode==3 → FUN_0041b7b4+FUN_004346e8 (traversal)
         mode==6 → FUN_00429200 (briefing)
         else   → FUN_0041d85c (frontend)
```

### File family / paths (OBSERVED)

`FUN_0041ab50` builds `<base 0x541270>\SAVES\<name>`; callers append
`%s.SAV` (`0x495c64`). Files seen in BUILD_A:

| path | writer | reader |
|---|---|---|
| `SAVES\<name>.SAV` | manual save — traversal F2 name-entry `FUN_00422bc0(0)` → `FUN_00422dec` → `FUN_00422d84` → `FUN_00427ed4(hdr=0)` = **full**; briefing-prompt `FUN_00422bc0(1)` (arg → `0x54bdb4`) = **header-only**, default name `%d` = levelId+1 | `FUN_004202cc` list → `FUN_004206d0` dialog → `FUN_00427f94` |
| `SAVES\LASTGAME.SAV` | traversal death — FUN_00463608 `0x464070` block, `EBX=1` → **header-only**, mode field 3 | frontend Continue — `FUN_0041dc90`→`FUN_00427f94`, gated by `FUN_00428290` exists-check |

Quit edge: `FUN_0040103c` tail `0x401174` builds `SAVES\LASTGAME.SAV`
and calls `FUN_0047d1a0` = `DeleteFileA` — a clean quit deletes the
death checkpoint, so Continue only survives a *death*, not a quit.

## 6. Traversal death route (OBSERVED — FUN_00463608 + dispatcher)

The `0x5414d0` demo-restore latch is **dormant in BUILD_A** — its file
context `0x5414c0/0x5414c4` and demo streams `0x49b284/0x49b288` have
no writers; `FUN_004090fc` (the `0x5414d0` consumer) early-outs after
clearing the latch. Real traversal death instead runs:

```
frame 0  0x541554==0 && 0x541510==0 && gates clear
         → event {0xa, 0x3ea} posts → 0x540cac = 0x3ea (death state)
per frame (0x540cac==0x3ea):
         0x540dac += rint(0x49b6f4 * 100.0f)   ; 0x4986ec = 100.0f
fade >255 → 0x464070 block:
         0x540dac = 0; ++0x541637
         FUN_00427e8c(0x49f010)                 ; thumbnail capture
         FUN_0041ab50 → "SAVES\LASTGAME.SAV"
         FUN_00427ed4(path, 0x49f010, EBX=1)    ; header-only write
         FUN_004167a0(0x3c)                     ; death sfx seam
         FUN_0046ca84; FUN_004371bc             ; traversal teardown
         FUN_0041d85c                           ; → mode 0 frontend
```

Death **never advances `0x541498`** — the checkpoint stores the
current level id, and Continue reloads that level fresh at s0 with
health 100 (the writer floor), ammo/session state carried from the
live globals (a header-only save stores no ammo — it survives in
memory only, which is also the original's behavior since death does
not reload the ammo block).

The demo-latch writers (`0x419d1f` stream-exhaustion, `0x464195`
death-during-demo) only matter for the `DEMO_SET.TXT`/`demo_%s`
machinery that BUILD_A never arms — classified vestigial.

## 7. Error / corruption behavior (OBSERVED diagnostics)

| condition | original behavior |
|---|---|
| open/read failure | `"Cannot load game %s"` → return 0 |
| `u32@0` != file length | header reject → `"SAVE CORRUPT: %s has incorrect header"` |
| checksum mismatch | same |
| first packet != `SAVE`/2 | same |
| packet tag/size != expected | `"SAVE CORRUPT: want %c%c%c%c:%d, found %c%c%c%c:%d"` → 0; `DEND` tag → silent 0 |
| `GAME.levelId` outside `[0,6)` | `FUN_004278c0` → 0 (no diag) |
| `GAME.health` outside `(0,150]` | → 0 |
| truncation | `FUN_00427ab4` read failure → corrupt path |

The native reader maps these onto `SaveError` codes one-for-one.

## 8. Native implementation status

- `src/core/save_game.*` — full-stream reader (envelope, cipher,
  registry-validated packet walk, GAME decode + FUN_004278c0 checks),
  typed views for the proven packet fields, header-only writer
  (LASTGAME/briefing shape — byte-layout-identical modulo seed and
  thumbnail content), `SaveStore` SAVES-dir model with the
  write/exists/delete lifecycle.
- `src/core/progression_runtime.*` — `ProgressionSession` gained
  `deathCount`/`field54163b`/`godMode`/`deathPhase`/`deathFade`/
  `lastgameArmed`/`lastgame`; `progressionStepDeath` (the FUN_00463608
  edge), `progressionContinue` (the FUN_0041dc90 → FUN_00427f94 route),
  `progressionApplyGamePacket`, `progressionDeleteCheckpoint` (the
  quit-delete edge).
- `src/core/save_full_restore.*` — Phase 14C: the FUN_00427218 world
  apply — MORE/PLAY/DAMP/CAME globals, AREN 0x466-record image apply,
  ALIE object records + FUN_00426f34-equivalent fixups (0x466-strided
  arena offsets, CMI image offsets with `imageBase=4`, sequential
  object ids), FAND owner/name tokens, BULL slots (state-dormant in
  observed saves), then the attach tail `FUN_00432c34 → FUN_00432d9c
  (cur) → FUN_00432d9c (partner)`.
- The `+0x0c` lifecycle (OBSERVED — see GAMEPLAY_RECONSTRUCTION §181):
  the fixup clears it; `FUN_004321dc` (end of `FUN_00432980` pull-in)
  rebinds every named object in cur+partner via `FUN_0045a3b0`;
  `FUN_0045a2d0` releases it on migration out. Port:
  `objectArenaActivate` (gate `col.elements == nullptr`) +
  `traversalMigrateInto` tail loop; the embedded pseudo-object binds
  model 0's element view (`arena+0x124 = 0x4edcc0` equivalent).
- `mdk-inspect --save-info` / `--save-roundtrip` / `--save-restore`
  (`--save-activate N` exercises the dormant-arena activation route on
  a restored save; `--save-write-full out.SAV` emits the Phase 14D
  full-save stream after the frame steps — deterministic under
  `--seed`, refuses to overwrite its input — re-parses, re-restores,
  and reports key-state equivalence) — census + round-trip +
  full-restore + full-write diagnostics; all five local real saves
  parse, and both full saves restore and step deterministically.
  Phase 14C.1 digests (post-restore / 1 frame / 90 frames): `1.SAV`
  `1375600551be9d99` / `fa92f4a340280874` / `06307535cd8e10ca`;
  `MDK.SAV` `034f2771a215c7a8` / `85ac853650977ca6` /
  `fbf41127b6aba8f6` (the `MDK.SAV` 90-frame digest changed from
  `4243287bfc9fb26a` because the corrected DAMP field mapping now
  restores nonzero state — `frameCounter=157`, `animPhase=15.03`,
  `fieldD0c=999`, `turboLatch=1`, `lruB=-1` — that the buggy offsets
  had dropped).

### Phase 14D writer (OBSERVED — FUN_00427ed4 → FUN_00426e98 →
### FUN_00426a0c → FUN_00427970)

- `src/core/save_full_write.*` — `saveGameWriteFull(rt, sess, in)`
  builds the whole packet stream `SAVE THMB GAME MORE PLAY DAMP CAME
  (AREN ALIE* FAND*)* BULL×3 SEND` in memory and hands it to the
  shared `saveGameEnvelope` (cipher armed after the 2B seed, size +
  checksum patch) — the same envelope helper the header-only writer
  uses. `SaveWriteFullInput.seed` forces the cipher seed (tests /
  diagnostics only); `thumbnail` accepts a caller blob, else zeros.
- **GAME**: full writes stamp `modeField = 0x3eb` (mode+1000) and
  health verbatim — the <0x65→100 floor is header-only-branch only.
  `+0x08` emits 0 (the original carried an uninit stack dword).
- **Ids**: sequential `+0x7c` stamping, 1-based, no gaps — per arena
  the embedded `+0x118` record first, then the live list in order
  (OBSERVED in the FUN_00426a0c staging loop).
- **Token encoders** (the FUN_004262b0 family, inverse of the loader's
  FUN_004262c8): arena refs → `position × 0x466`, `-1` = null; object
  refs → stamped `+0x7c` id, `0` = null (the fixup never writes -1
  into object slots); CMI pointers → `ptr − imageBase`, `-1` = null;
  CMI strings → the char-position offset located by image search
  (operand `{len}{chars}{NUL}` first, bare NUL-terminated fallback;
  non-CMI strings → `-1` + warning — the original saved a wild
  `ptr−base` that only resolved inside its own address space).
- **FUN_00426738 record fixup, mirrored**: `+0x0c`/`+0x158` zeroed;
  `+0x60`/`+0x2bc`/conn `+0x302` arena tokens; `+0xec`, `+0x108`,
  `+0x10c`, `+0x110`, `+0x114`, `+0x230`, retPc `+0x24c..`, savedPc
  `+0x25c..`, conn `+0x306`/`+0x30a`/`+0x316..+0x322`, homing `+0x302`,
  `+0x140`/`+0x15c` strings → CMI tokens; `+0x138`/`+0x278`/`+0x2b8`/
  mover `+0x312` → object ids. The `+0x302..+0x32d` union emits the
  verbatim lane views first, then overlays flag-owned views in the
  original's fixup order (mover → connector → homing prefix).
- **DAMP**: `+0x1d4..+0x20f` emitted zero (the original writer scrubs
  the weapon-5 block too); `+0x134`/`+0x170` emit the current arena's
  token — the loader clears both post-attach but the original's live
  globals still held the load target (OBSERVED `0` in real saves);
  `+0x250` emits nonzero→sentinel like the object `+0x2b0`/`+0x2b4`.
- **BULL**: `+0x18` arena token, `+0xd4` the FUN_00461724 fly-callback
  enum inverse, `+0xd8` home object id, `+0xdc` nonzero→the element
  rebind gate, `+0xe0` verbatim (ribbon-path alias is a dead slot),
  `+0xe4` the `flags&1`-selected `ribbonT`/`speedH` view.
- **Restore-order fix**: the loader materializes ALIE records in
  stream order — `DynamicArena::allocBack()` now appends so the
  post-load `+0x68` walk order matches the original (a per-record
  `allocFront` had reversed it). `objFlag148` restores the embedded
  record's `+0x30` low byte; `ribbonT` mirrors `+0xe4` on load.
- **Golden (local real save)**: `MDK.SAV` → restore → 1 frame →
  write → reparse → restore → equivalence OK. Byte-level packet diff
  vs the original: identical in every gameplay-authoritative field;
  remaining deltas are the cipher seed, the THMB capture (no renderer
  seam — zeros), `GAME+0x08` uninit dword, `MORE+0x0c` (the live
  `0x5414a8` had decayed past the saved `+0x08` value; loader ignores
  it), loader-skipped scratch/pointer fields (DAMP `+0x18..+0x2b`,
  `+0x50..+0x57`, stat counters, AREN `+0x14..+0x43`/`+0x5c..+0x6a`/
  `+0x446..+0x461`, ALIE `+0x00`/`+0x64..+0xab`), pointer sentinels
  (DAMP `+0x250`/`+0x254`, ALIE `+0x2b0`/`+0x2b4`), and one frame of
  authored drift (positions, anim accumulators, `frameCounter`).

### Phase 14C.1 load-closure audit (OBSERVED)

Every `PLAY`/`DAMP`/`BULL` packet byte is now classified:

- `PLAY +0x04..+0xbb` was the unknown span: it is the five-record
  inventory table `0x54155c` + count + selection, restored verbatim
  (the original loader resets only `0x541618..0x54161b`).
- `DAMP` mid-region: a systematic +4 offset defect (`+0xe4..+0x170`
  had been mapped as if `+0x100 ≡ 0x540d00`) was corrected against
  the writer/loader disassembly; all fields now map to their true
  `0x540bfc`-anchored addresses.
- `BULL +0x2c..+0xbc` (144B/slot): `FUN_0045f670` rewrites the whole
  span every frame for `state==1` shots — screen AABB `+0x2c..+0x43`,
  render transform `+0x44..+0xa3`, vertex block `+0xa4..+0xbb` —
  pure derived render scratch, never read by tick/fly/collision.
- Six `+0x114` values in `1.SAV` are stale runtime heap pointers
  (`0x3b4f0c`/`0x3db980` triplets in dormant arenas DANT_1/DANT_2).
  The original's `FUN_004262b0` remap returns **−1** for them
  (value < image base), so the original object stores the 0xffXX-
  family sentinel — semantically the same "anim done/no record"
  state as the port's `animRec == nullptr` (`animDone()` gate).
- Synthetic packet-application coverage (`test_save_full_restore`)
  exercises MORE/PLAY/DAMP/CAME + 2 AREN + 2 ALIE + FAND + 3 BULL on
  a generated level: cross-arena refs, inventory/ammo, script
  resume (global-flag observable), wait resume, path continuation,
  anim-record resolution, active-shot advance, deterministic
  re-apply, and MORE-identity rejection.

### Remaining seam

Reader, death checkpoint, header-only save/continue, and manual
full-save **loading and writing** are closed for BUILD_A on the local
corpus — the emitted stream re-parses and re-restores with zero
reference failures and identical authoritative state, and the
packet-image diff against a real save contains no gameplay-
authoritative difference. Known intentional deviations (all
load-neutral): `THMB` is a zero fill unless the caller supplies a
blob; non-CMI-resident strings save as `-1`; the `MORE+0x0c` live
value cannot be reconstructed from the loaded state; pointer identity
sentinels and loader-ignored scratch emit `0`/`1`.
