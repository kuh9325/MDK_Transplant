# MDK Transplant

A preservation-oriented compatibility reimplementation of **MDK (1997)** for modern macOS, with Apple Silicon as the primary target.

MDK Transplant is **not a remaster, redesign, or source port of original game code**. The project reconstructs game behavior and file formats from documented observations and independently written code, while loading game data from a legally owned copy supplied by the user.

> **Status:** active work in progress. The repository already contains substantial engine, gameplay, and presentation reconstruction, but it is **not yet a complete drop-in replacement for the full retail game**.

## Goals

- Reproduce original MDK behavior as faithfully as practical.
- Preserve original gameplay rules, timing, quirks, and data semantics rather than "improving" them.
- Run natively on modern Apple Silicon macOS.
- Keep original MDK executables, assets, archives, audio, video, and other proprietary material out of Git.
- Keep reverse-engineering claims evidence-backed and clearly separated from hypotheses.
- Maintain a platform-neutral core so presentation technology can evolve without moving game semantics out of the reconstructed engine.

## Current Architecture

The project is split into an independently written C++ core and presentation/platform layers.

### Native core

- **C++20** core library (`mdk_core`)
- **CMake 3.24+**
- **SDL3** for the native window/input host
- **Metal** presentation for the native indexed-framebuffer path
- Objective-C++ only at the Metal/platform seam
- Unit tests and diagnostic/inspection tooling

### Godot frontend

A retained **Godot 4 GDExtension** frontend presents snapshots produced by `mdk_core`.

The core remains authoritative for reconstructed game semantics. Godot is used as a presentation host for world geometry, player state, dynamic objects, combat presentation, freefall presentation, HUD/view layers, and input acquisition.

See [docs/GODOT_FRONTEND.md](docs/GODOT_FRONTEND.md) and [docs/GODOT_INTEGRATION_AUDIT.md](docs/GODOT_INTEGRATION_AUDIT.md).

## Implementation Status

The current tree includes evidence-driven work across areas such as:

- read-only original-data access and bounded binary parsing;
- structural parsers for the reconstructed MDK resource families;
- indexed images, palettes, fonts, sprites, and frontend composition;
- root/options/display/sound/mouse/keyboard frontend state machines;
- gameplay input translation and configurable bindings;
- traversal runtime and scripted level state;
- player movement, vertical motion, collision, surface interaction, camera, look, sniper, weapons, and projectiles;
- dynamic objects, movers, animation, enemies, and runtime arena presentation;
- freefall runtime and presentation;
- progression and native save/restore infrastructure;
- reconstructed traversal combat presentation;
- reconstructed traversal HUD and view-mode presentation.

Recent development has reached the Phase 17 series, including traversal combat presentation and HUD/view-mode presentation. Phase numbers are engineering milestones, **not a percentage-complete measure of the retail game**.

For detailed evidence and phase history, see:

- [docs/ENGINE_RECONSTRUCTION.md](docs/ENGINE_RECONSTRUCTION.md)
- [docs/GAMEPLAY_RECONSTRUCTION.md](docs/GAMEPLAY_RECONSTRUCTION.md)
- [docs/reverse-engineering/](docs/reverse-engineering/)
- [docs/DATA_FORMATS.md](docs/DATA_FORMATS.md)

## Original Game Data

**No original MDK game data is included in this repository.**

You must provide files from a legally owned copy of MDK. Original data should remain outside version control. The repository already ignores local drop-zone directories used during development.

Typical local setup:

```text
MDK_Transplant/
  original/
    installed/
      ...your legally owned MDK installation...
```

You can also point tools directly at another directory with `--data-path` or `MDK_DATA_ROOT`.

See [docs/PROPRIETARY_BOUNDARY.md](docs/PROPRIETARY_BOUNDARY.md) for the project boundary.

## Building the Native Target

### Requirements

- Apple Silicon Mac
- Xcode / Apple Clang
- CMake 3.24+
- SDL3

With Homebrew:

```sh
brew install cmake sdl3
```

Configure, build, and test:

```sh
cmake -S . -B build/native
cmake --build build/native
ctest --test-dir build/native
```

The native application is produced as:

```text
build/native/mdk-native.app
```

Run it directly:

```sh
build/native/mdk-native.app/Contents/MacOS/mdk-native
```

Example with original data:

```sh
build/native/mdk-native.app/Contents/MacOS/mdk-native \
  --data-path /path/to/your/MDK/data \
  --interactive-frontend
```

The native executable also exposes deterministic self-tests and resource/runtime inspection modes. See [docs/NATIVE_SKELETON.md](docs/NATIVE_SKELETON.md) for the current command surface.

## Building and Running the Godot Frontend

The actively developed presentation frontend targets **Godot 4.7.x**.

Build the GDExtension:

```sh
frontend/godot/build.sh
```

Then run with either the default ignored data location or an explicit path:

```sh
frontend/godot/run.sh --data-path /path/to/your/MDK/data
```

Or:

```sh
export MDK_DATA_ROOT=/path/to/your/MDK/data
frontend/godot/run.sh
```

A Godot binary can be supplied through `MDK_GODOT_BIN` if it is not on `PATH` or installed at the standard macOS application location.

For smoke tests, screenshots, level selection, freefall mode, and the bridge API, see [docs/GODOT_FRONTEND.md](docs/GODOT_FRONTEND.md).

## Development Principles

Before changing compatibility behavior, read:

- [AGENTS.md](AGENTS.md)
- [docs/reverse-engineering/EVIDENCE_POLICY.md](docs/reverse-engineering/EVIDENCE_POLICY.md)
- [docs/PROPRIETARY_BOUNDARY.md](docs/PROPRIETARY_BOUNDARY.md)

Important rules include:

1. Compatibility takes priority over modernization.
2. Do not silently "fix" original behavior because it looks odd.
3. Do not commit original MDK binaries or assets.
4. Keep observations, corroborated facts, documentation, hypotheses, and unknowns distinct.
5. Prefer small, testable reconstruction steps over speculative rewrites.
6. The reimplementation must not depend on original executable code at runtime.

## Repository Layout

```text
src/                 C++ core, native app, platform and renderer code
frontend/godot/      Godot 4 presentation frontend + GDExtension
tests/               Native regression/unit tests
tools/               Inspection and research utilities
docs/                Architecture, formats, gameplay and reconstruction notes
docs/reverse-engineering/
                     Evidence policy and original-behavior research
```

## Legal / Project Status

MDK and the original game's copyrighted content remain the property of their respective rights holders.

This repository is an independent preservation and compatibility project. It is not affiliated with or endorsed by the original developers, publishers, or rights holders. It does not distribute original MDK executables, disc images, or game assets.

See [docs/PROPRIETARY_BOUNDARY.md](docs/PROPRIETARY_BOUNDARY.md) for the project's handling rules.
