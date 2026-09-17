# Native Skeleton — Phase 3A

Status: implemented and smoke-tested on the Apple Silicon host
(2026-09-17). This is a platform/framebuffer shell only — no gameplay,
no original asset parsing, no original data.

Throughout: ORIGINAL ENGINE OBSERVATION refers to Phase 2B evidence
(`docs/reverse-engineering/EXECUTABLE_MAP.md`); NATIVE PORT PROJECT
DECISION marks our own engineering choices. Do not conflate them.

## Toolchain

| Item | Value |
|---|---|
| OS | macOS 27.0 (Darwin 27.0.0), arm64 Apple Silicon (T8132) |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.4.3 |
| SDL3 | 3.4.16 (Homebrew `/opt/homebrew`, `SDL3::SDL3` CMake target) |
| Frameworks | Metal, QuartzCore (CAMetalLayer), Cocoa |
| Language | C++20; Objective-C++ (ARC) only in `src/renderer/metal_presenter.mm` |

## Build & run

```sh
cmake -S . -B build/native
cmake --build build/native            # produces build/native/mdk-native.app
ctest --test-dir build/native         # native unit tests

# run (windowed):
build/native/mdk-native.app/Contents/MacOS/mdk-native

# smoke test (deterministic, self-terminating):
build/native/mdk-native.app/Contents/MacOS/mdk-native \
    --selftest --frames 120 --data-path original/installed \
    --dump-ppm /tmp/frame.ppm
```

CLI options: `--data-path DIR` (read-only data root), `--frames N`
(quit after N frames), `--selftest` (inject + verify synthetic input
events), `--dump-ppm FILE` (write last presented frame), 
`--no-relative-mouse`, `--help`. `Esc` or closing the window quits.

## Source layout

```
src/
  app/       application.h/.cpp  — lifecycle + loop + arg parsing
             diagnostic_scene.*  — synthetic test scene (no original data)
             main.cpp            — entry point
  core/      compat.h            — evidence-anchored geometry constants
             log.h               — minimal stderr logging
             clock.*             — monotonic frame timing
             framebuffer.*       — 8bpp indexed buffer + 256-entry palette
             viewport.*          — aspect-fit / presentation geometry
             mode_dispatch.*     — PrimaryModeId/SubModeId dispatcher
             data_root.*         --data-path validation (read-only seam)
  input/     input_state.*       — neutral per-frame input state (no SDL)
  platform/  sdl_host.*          — SDL3 init/window/event-pump/rel-mouse
  renderer/  presenter.h         — presentation backend interface
             metal_presenter.mm  — Metal backend (Objective-C++/ARC)
tests/native/test_main.cpp       — unit tests (linked to mdk_core only)
```

`mdk_core` is a static lib of everything platform-neutral; the app links
`mdk_core` + `SDL3::SDL3` + Metal/QuartzCore/Cocoa. Only
`metal_presenter.mm` and `sdl_host.cpp` see SDL/Metal headers.

## Application lifecycle

1. `SDL_Init(VIDEO|EVENTS)`
2. `SDL_CreateWindow` — `SDL_WINDOW_METAL | RESIZABLE |
   HIGH_PIXEL_DENSITY` (960x720 pt ⇒ 1920x1440 drawable on Retina)
3. `createMetalPresenter` — `SDL_Metal_CreateView` → `CAMetalLayer`
4. event pump → `InputState`; mode dispatch; scene render; present
5. shutdown: presenter (Metal view) → window → `SDL_Quit`

## Framebuffer model

- Working surface: 600x360 **8-bit indexed** (`IndexedFramebuffer`) +
  256-entry RGBA `Palette`. (OBSERVED: original ~600x360 indexed back
  buffer `DAT_00541650`, 360-row present copy.)
- Presentation canvas: 640x480, 4:3 (OBSERVED: `SetDisplayMode
  640x480x8`). The 600x360 image is **centered** in the canvas
  (PROJECT DECISION — original in-surface offset is UNKNOWN; candidate
  targeted Phase 2C observation).
- `expandToBGRA` converts indexed→BGRA8 on CPU at present time. The
  indexed buffer is the authoritative working surface; expansion is a
  presentation detail.

## Metal presentation architecture

- `SDL_Metal_CreateView(window)` attaches a `CAMetalLayer`-backed
  `NSView`; `SDL_Metal_GetLayer` yields the layer.
- Layer: `MTLPixelFormatBGRA8Unorm`, `displaySyncEnabled = YES` (vsync
  paces `nextDrawable` — the loop cannot busy-spin), `framebufferOnly`.
- Per frame: expand framebuffer → `staging_` → `replaceRegion` into a
  600x360 shared-mode `MTLTexture` → single triangle-strip quad sampled
  with a **nearest** sampler (indexed-era pixel look) into the drawable
  inside a viewport computed by `presentationRect()`.
- Shaders: tiny MSL source compiled at init via
  `newLibraryWithSource:` (no .metallib build step).
- Resize: `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` /
  `SDL_EVENT_WINDOW_METAL_VIEW_RESIZED` → `layer.drawableSize` update;
  the viewport is recomputed every present.

## Scaling policy

Window (any size, HiDPI) → largest centered 4:3 canvas
(letterbox/pillarbox, never stretched) → centered 600x360 sub-rect
(occupies 93.75% x, 75% y of the canvas, matching the observed
360-of-480-row copy). Logical framebuffer dimensions never change with
window size.

## Input seam

`InputState` (platform-neutral) receives, per frame: key down/up/repeat
(platform scancode numbering — SDL values on this seam), accumulated
relative mouse dx/dy, mouse button state, **wheel float + integer ticks**
(preserved even though MDKDOS.EXE exposes no wheel binding — NATIVE
PORT DECISION; not bound to any action yet), and window/quit events
route to host callbacks. Relative mouse capture is enabled at startup
via `SDL_SetWindowRelativeMouseMode` (observed working — no
Accessibility permission needed).

`--selftest` pushes synthetic SDL events (key, motion, button, wheel)
through `SDL_PushEvent` and verifies the full event→InputState path —
needed because host-level synthetic input is TCC-blocked on this
machine (see RUNTIME_ORACLE.md §Host environment).

## Timing seam

`Clock` = `std::chrono::steady_clock`; per frame produces `{index, dt,
elapsed}`. The original accumulates a per-frame delta (OBSERVED) but its
constants are UNKNOWN — none are reproduced. The diagnostic scene is
driven by **frame index**, not wall time, so `--frames N` runs are
deterministic. No fixed-step physics yet.

## Data-path seam

`--data-path DIR` validates existence + is-directory, stores a
canonicalized path, and logs it as read-only. The app launches fine
without it. Nothing under the path is read, parsed, copied, or written —
parsing original formats is a later phase. `--data-path` pointing at
`original/installed/` is verified to be treated read-only.

## Mode-dispatch scaffold

`ModeDispatcher` holds `PrimaryModeId`/`SubModeId` + quit flag and routes
one handler per primary mode — mirroring the observed original loop
shape (primary-mode switch + overlay sub-modes + quit flag). Original
mode numbers are recorded under `mode::observed::*` as **evidence
constants only**; native modes are negative IDs (`nativeBoot=-1`,
`nativeShell=-2`) so they can never collide. Demonstrated path:
`boot → shell → quit`.

## Diagnostic scene

100% synthetic: horizontal hue gradient (palette sweep), animated
checkerboard band, bouncing box (frame-index-driven), 16-bit binary
frame-counter dots, and live mouse-delta/wheel indicators top-right.
Verified visually via `--dump-ppm` (P6 dump of the exact BGRA data
uploaded to Metal).

## Current limitations / non-goals

- No gameplay, no levels, no enemies, no collision, no audio, no video.
- No original-format parsing (`.MTO/.SNI/.MTI/.CMI/.DTI/.FTI/.BNI/.LBB/
  .SAV/.FLC/.MVE` all unimplemented — Phase 3B+).
- Original 600x360→640x480 image offset UNKNOWN (currently centered).
- Window-close quit and resize were code-verified; physical
  keyboard/mouse input could not be host-injected on this machine (TCC),
  so `--selftest` exercises the identical event path instead.
- Single window, single Metal backend, nearest-neighbor only; no
  shaders-as-assets, no .metallib packaging.
- `Esc` quits the shell (native convenience binding, not an original
  key mapping).
