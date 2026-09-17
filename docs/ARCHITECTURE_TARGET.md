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
| Audio backend | PROJECT DECISION (open) — no audio in Phase 3A/3B/4A |

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
> state verbatim between screens. Settings mutation stays deferred.
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
