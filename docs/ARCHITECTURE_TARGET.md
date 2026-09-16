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
| Implementation language | PROJECT DECISION (tentative): modern C++ (C++20) with a portable platform layer |
| Platform/windowing lib | PROJECT DECISION (open): SDL3 is available via Homebrew; not yet chosen |
| GPU backend | PROJECT DECISION (open): Metal is the obvious native fit; not yet chosen |
| Build system | PROJECT DECISION (tentative): CMake (installed: 4.4.3) |
| Audio backend | PROJECT DECISION (open) |

> SDL3/Metal/CMake are **engineering choices for the reimplementation** — they
> say nothing about what the original game used. Do not conflate the two.

## Non-goals for now

- No engine source tree yet (no src/ is created in Phase 0).
- No renderer, no gameplay, no decoders.
- Subsystem boundaries will be revised once the original builds' actual
  module organization is observed.
