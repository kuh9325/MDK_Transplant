# Architecture Target (Tentative)

This document sketches **target boundaries only**. Nothing here is implemented
in Phase 0, and nothing here is a reverse-engineered fact — these are design
intentions to be validated against evidence as it accumulates.

## Tentative subsystem boundaries

```text
┌─────────────────────────────────────────────┐
│ game logic        — rules, entities, AI,    │
│                     weapons, progression    │
├─────────────────────────────────────────────┤
│ world             — level state, collision, │
│                     spatial queries         │
├──────────────────┬──────────────────────────┤
│ asset parsing    │ animation                │
│ — original file  │ — playback, blending,    │
│   format loaders │   kinematics             │
├──────────────────┴──────────────────────────┤
│ renderer          — frame production; must  │
│                     reproduce original look │
│                     and limits              │
├─────────────────────────────────────────────┤
│ audio             — mixing, music/SFX       │
│                     playback                │
├─────────────────────────────────────────────┤
│ platform          — windowing, input, file  │
│                     IO, timing, audio dev   │
├─────────────────────────────────────────────┤
│ tools             — offline analysis,       │
│                     format inspection,      │
│                     oracle test harnesses   │
└─────────────────────────────────────────────┘
```

Boundary notes (intentions, not commitments):

- **asset parsing** is deliberately separate from game logic: original formats
  are loaded by evidence-driven parsers; nothing original is embedded.
- **renderer** sits behind an abstraction so the original software-rendering
  look can be reproduced even if the backend is modern.
- **platform** isolates everything OS-specific; target is native arm64 macOS
  first, portability second.

## Technology decisions — status

| Decision | Status |
|---|---|
| Implementation language | IMPLEMENTED (Phase 3A): C++20, Objective-C++ at the Metal seam |
| Platform/windowing lib | IMPLEMENTED (Phase 3A): SDL3 3.4.16 via Homebrew (`SDL3::SDL3`) |
| GPU backend | IMPLEMENTED (Phase 3A): Metal presentation of the software framebuffer |
| Build system | IMPLEMENTED (Phase 3A): CMake ≥3.24, out-of-tree `build/` |
| Data access | IMPLEMENTED (Phase 3B): `DataRoot` read-only resolver + `BinaryReader` + container envelope — see `DATA_ACCESS.md` |
| Resource decoding | STARTED (Phase 4A/4B/4C/4D): `IndexedImage` + four proven decoders (BNI paletted bitmap; BNI indexed-only bitmap with the STREAM context palette; FTI FONTSML/FONTBIG glyph table; FTI ARROW sprite table) + the first static front-end composition — see `ENGINE_RECONSTRUCTION.md` |
| Audio backend | PROJECT DECISION (open) — no audio in Phase 3A/3B/4A–4I; the Sound screen emits proven trigger semantics (`SoundAudioEvent`) but no playback backend exists yet |

> Phase 3A implements the bottom two layers (`platform`, `renderer`
> presentation boundary) plus a neutral input seam and a mode-dispatch
> scaffold. See `NATIVE_SKELETON.md` for the realized structure.

> Phase 3B adds the bottom of `asset parsing` + the file-IO half of
> `platform`: read-only data-root resolution (case-insensitive,
> root-confined), bounded binary reads, and the common u32+name
> envelope parser — top-level only; interior semantics UNKNOWN.
> See `DATA_ACCESS.md`.

> Phase 4A adds the first decoded-resource path: one proven BNI visual
> payload (`MISC/OPTIONS.BNI` `MDKOPT`) flows through `DataRoot` →
> directory parser → `decodeBniPalettedImage` → `IndexedImage` → the
> indexed framebuffer → Metal. Phase 4B adds the indexed-only variant:
> `STREAM/STREAM.BNI` `BG` → `decodeStreamBackdrop` (SYS_PAL head +
> `PAL` tail, the proven consumer binding) → `decodeBniIndexedImage`.
> Phase 4C adds the first glyph path: `MISC/MDKFONT.FTI` `FONTSML` →
> `decodeFtiFont` (256-entry byte-indexed table + `{s8,s8,u8,px}`
> glyphs) → `drawFtiGlyph`/`drawFtiText` into the indexed framebuffer,
> palette = the record's resident `SYS_PAL` head.
> Phase 4D adds the first composed frame: `MDKOPT` backdrop +
> `OPT0..OPT4` scaled FONTBIG labels + the `ARROW` sprite-table cursor
> (`decodeFtiSprite` → `blitFtiSpriteFrame`, the `FUN_00415ff0`
> command-stream format) drawn in the original order by
> `renderFrontendMenuFrame` — one static evidence-backed state only,
> no interaction.
> Phase 4E makes the root menu interactive: `FrontendMenuController`
> reproduces `FUN_0041dc90`'s selection, key-repeat, gated mouse
> hit-test, scale ramp, and activation dispatch.
> Phase 4F adds the second reconstructed screen and the first
> inter-screen flow: `OptionsMenuController` reproduces
> `FUN_00420eac` (black-background `OM_*` list, system-palette
> binding, LEFT/RIGHT skill cycle, Esc return), and
> `FrontendFlowController` performs the proven `FUN_00420cf0` entry /
> `FUN_00420d68` exit transitions, carrying the shared input-machine
> state verbatim between screens. Phase 4G adds the first real
> settings mutation + persistence: the Skill row's ±1 wrap latches
> the `DAT_00541486` dirty flag, and `FrontendSettings`
> (`frontend_settings.*`) reproduces the `FUN_004260ac` delta-write
> contract — `Skill = %d` emitted iff != factory default 1 — behind
> a caller-supplied path that never touches the read-only
> `DataRoot`. Phase 4H adds the first real child screen:
> `DisplayMenuController` (`display_menu.*`) reproduces
> `FUN_0041d1e0` entered via `FUN_0041d020` from options row 7 —
> three rows (`Brightness` ±1 wrap [0,7], `ForcePCorrect` toggle,
> `Quit`), the SYS_PAL-head-plus-4×48-ramp palette composition
> with the `FUN_0046d208` `min(c+level·16,255)` upload lift
> (`frontend_palette.h`), the `FUN_0041cf80` swatch grid, and the
> `FUN_0041d144` return that resumes options at selection 7.
> Phase 4I adds the second real child screen:
> `SoundMenuController` (`sound_menu.*`) reproduces
> `FUN_004233d8` entered via `FUN_0042322c` from options row 1 —
> three rows (`SoundFX`/`SoundMusic` ±10 clamp [0,100],
> `Done`), the inherited options palette (no upload of its
> own), the proven volume-bar geometry (`FUN_00416aa8`
> inclusive rectfill, `x = 210..210+vol·280/100`), and the
> `FUN_00423280` return that resumes options at selection 1 —
> with the proven `OPTSONG`/`OPTBUTT`/ambient-song triggers
> emitted as semantic `SoundAudioEvent`s (actual playback
> deferred). Phase 4J adds the third real child screen:
> `MouseMenuController` (`mouse_menu.*`) reproduces
> `FUN_004217e8` entered via `FUN_00421664` from options row 3 —
> the 23-row machine (four `JOY_*`/`M_*` left rows + the `JOY_B`
> 4×16 button-binding grid + three axis bars + the test
> indicator), the truncated-toward-zero hit-test bands, the
> axis-letter cycler (`'0'`,`'A'`–`'H'`, bounded repair), the
> button-bit exclusivity table, the `FUN_00414b28` blink
> bracket, and the inline mode-0x0b return that resumes options
> at selection 3. Mouse mutates settings-table entries 49–68
> only — `MouseOn`, the `MouseYReversed` float slot toggled
> through raw integer bits (denormal `1.4013e-45` emission
> preserved), the W-set axis/button maps — while the flow
> overlays them on `baseSettings_` so the D-set/scales round-
> trip untouched. Phase 4K adds the fourth real child screen:
> `KeyboardMenuController` (`keyboard_menu.*`) reproduces
> `FUN_0041f18c` entered via `FUN_0041f030` from options row 4 —
> the 21-row machine (19 binding rows in two columns +
> `KM_RESET` + `KM_QUIT`), the corrected entry selection 20,
> the raw-key capture FSM over the DirectInput-derived 0..127
> internal key domain (lowest-set-bit edge pick via the
> `FUN_00419168` contract, `FUN_0041925c` right-modifier fold,
> Esc-cancel before poll, duplicates allowed, same-key no-op
> leaves dirty alone), and `FUN_00425db0`'s full 29-dword
> reset — all 19 visible bindings plus the 10 hidden
> weapon-hotkey globals — that always latches dirty. Keyboard
> mutates settings-table entries 69–87 only; the hidden 10
> live in the flow's `keyGlobals_` block for Phase 5 without
> becoming fake `MDK.CFG` entries. The flow is now Root →
> Options → {Display|Sound|Mouse|Keyboard} → Options → Root
> and the settings seam persists all proven entries in table
> order on the options exit.
> See `ENGINE_RECONSTRUCTION.md`.

> SDL3/Metal/CMake are **engineering choices for the reimplementation** — they
> say nothing about what the original game used. Do not conflate the two.

## Non-goals for now

- No gameplay, no level/enemy/collision systems, no original-format
  parsers, no audio — all deferred past Phase 3A.
- Subsystem boundaries will be revised once the original builds' actual
  module organization is observed.

## Phase 2B cross-check (ORIGINAL ENGINE OBSERVATION)

`EXECUTABLE_MAP.md` supports the tentative boundaries above:

- A **platform layer** per OS is real in the original: identical engine
  source-path strings (`mdksrc\main\*`, `mdksrc\share\*`) in DOS and Win95
  builds, with only platform modules swapped (`dos\opthmi.c`+VESA+INT9 vs
  DirectDraw/DirectInput/DirectSound). ⇒ planned `platform` boundary matches.
- The original is **mode-dispatch monolithic**, not cleanly layered: one
  main loop (`MDK95 FUN_0040103c`) switches on a primary-mode global with
  modal overlay sub-modes. Native ports should reproduce this state machine
  rather than impose a modern loop shape.
- Renderer is modular at link time (software / D3D / Glide / SGL / Vérité
  variants share the same engine image except the render module), which
  supports keeping `renderer` behind an abstraction.
- `asset parsing` (chunk/`readbin`/`mdkfopen`/`setupob`) is a distinct
  shared layer — consistent with the tentative split.

No boundary changes are warranted yet; game-logic internals (AI, physics,
camera) remain insufficiently mapped to validate `game logic`/`world`
subdivision.
