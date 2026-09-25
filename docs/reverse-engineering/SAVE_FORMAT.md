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
  `+0x00` health (u32), `+0xc4..0xc7` the weapon-indicator bytes
  `0x541618..0x54161b`, `+0xcb..0xe3` the six-dword ammo block
  `0x54161f..0x541633`, `+0xe7` `0x54163b`. Other stat fields inside
  the block remain UNKNOWN.
- `DAMP` (724B) — the `0x540bfc` motion block: `+0x00` player position
  xyz (f32), `+0x0c` previous position. Real saves confirm mid-arena
  coordinates (`403.0,770.28,-66.0` etc.) — full saves restore the
  player pose, they do not respawn at s0.
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
- `mdk-inspect --save-info` / `--save-roundtrip` / `--save-restore` —
  census + round-trip + full-restore diagnostics; all five local real
  saves parse, and both full saves restore and step deterministically
  (`1.SAV` digest `06307535cd8e10ca`, `MDK.SAV` `4243287bfc9fb26a`,
  90 frames each).

### Remaining seam (BOUNDED, not closed)

Full-save **writing** requires reproducing the AREN/ALIE/FAND raw
memory records (the port deliberately uses different object storage —
no raw pointers are ever serialized). On the load side: unmapped
packet regions remain (the `--save-restore` report lists them —
e.g. most of the DAMP middle block and the BULL +0x2c..0xbc region),
and six `+0x114` anim pointers in `1.SAV` are runtime heap addresses
(runtime-allocated anims, shared in triplets — no image backing) that
resolve to null with a diagnostic. Reader, death checkpoint,
header-only save/continue, and full-save loading semantics are
closed for BUILD_A on the local corpus.
