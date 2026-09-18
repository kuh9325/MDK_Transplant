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
`--preview-resource FILE RECORD` (Phase 4A/4B: decode + present one
proven BNI visual resource — the paletted `MISC/OPTIONS.BNI MDKOPT`,
or the indexed-only `STREAM/STREAM.BNI BG` whose palette is resolved
by the proven stream-context binding `SYS_PAL[0:64]` + `PAL[64:256]`;
requires `--data-path`),
`--preview-font FILE RECORD [TEXT]` (Phase 4C: decode + present one
proven FTI font record — `MISC/MDKFONT.FTI FONTSML`/`FONTBIG`; atlas
of all mapped glyphs, or TEXT drawn with the proven advance rule;
glyph indices resolve through the record's `SYS_PAL` palette head;
requires `--data-path`),
`--preview-sprite FILE RECORD` (Phase 4D: decode + draw one proven
FTI sprite-table record — `MISC/MDKFONT.FTI ARROW` — over a
diagnostic checkerboard; requires `--data-path`),
`--preview-options` (Phase 4D: compose the one proven static
front-end frame — `MDKOPT` backdrop + `OPT0..OPT4` scaled centered
FONTBIG labels + `ARROW` at the reset mouse position; resolves all
resources itself; requires `--data-path`),
`--preview-options-submenu` (Phase 4F: compose the static options
sub-menu frame — framebuffer clear + `OM_*` centered FONTBIG labels
+ `ARROW` under the system palette head; requires `--data-path`),
`--interactive-frontend` (Phase 4E/4F/4G: run the reconstructed
FUN_0041dc90 root-menu controller, now flowing into the FUN_00420eac
options sub-menu on `OpenOptions` — original-style UP/DOWN/LEFT/
RIGHT selection with key repeat, gated mouse hit-test, scale ramp,
Skill row mutation + dirty latch, Esc/Quit return to root; requires
`--data-path`). `--frontend-root-only` is a test-only switch that
keeps `OpenOptions` deferred so the Phase 4E single-screen
regression snapshot stays reproducible. `--settings-file FILE`
(Phase 4G) gives the native-owned Skill persistence seam its path
(load at startup if present, write on the options-exit dirty gate;
always outside `--data-path` — the original's `C:\MDK.CFG`/
relative-file resolution is deliberately not reproduced). With
`--selftest`, the interactive mode runs a deterministic
injected-event script (real device input is filtered out for the
duration; the timing machine is fed the original's paced regime —
dt = 100/3 ms per frame — so runs and digests are
machine-independent) and reports PASS/FAIL.
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
             indexed_image.*     — decoded indexed visual + blit (4A)
             bni_image.*         — proven BNI bitmap payload decoders (4A/4B)
             stream_context.*    — STREAM.BNI backdrop palette binding (4B)
             fti_font.*          — FONTSML/FONTBIG glyph decode + draw (4C)
                                  + scaled draw (FUN_00414f64 mirror, 4D)
             fti_sprite.*        — FTI sprite-table decode + stream blit
                                  (ARROW format, FUN_00415ff0 mirror, 4D)
             frontend_menu.*    — static front-end frame composition
                                  (FUN_0041dc90 stable state, 4D)
                                  + interactive root controller (4E)
             frontend_machines.h — shared input-machine primitives:
                                  mouse accumulate, repeat queries,
                                  scale ramp, frame timing, the
                                  serialized FrontendMachineState (4F)
             options_menu.*     — options sub-menu controller +
                                  static/dynamic renderers
                                  (FUN_00420eac, 4F)
             frontend_flow.*    — two-screen flow controller:
                                  FUN_00420cf0 enter options,
                                  FUN_00420d68 return to root +
                                  dirty-gated persist sink (4F/4G)
             frontend_settings.*— native-owned Skill persistence:
                                  FrontendSettings + serialize/
                                  parse/file seam — caller-supplied
                                  path, never under --data-path (4G)
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
  640x480x8`). The 600x360 image is **centered** in the canvas —
  CODE-CORROBORATED in Phase 4A: the original present copy
  (`FUN_0046c86c`) lands at column +20, row +60, i.e. exactly
  centered.
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
without it. Phase 3B extends this seam into a full read-only resolver
(case-insensitive, root-confined) — see `DATA_ACCESS.md`; the
diagnostic scene still never reads original data.

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
- Runtime original-data use is limited to the Phase 4A/4B/4C/4D/4E
  front-end paths:
  one named record from one BNI file (`--preview-resource`), decoded by
  the proven paletted-bitmap layout or — for `STREAM/STREAM.BNI BG`
  only — the proven external-palette binding (`SYS_PAL` head + `PAL`
  tail, resolved via `MISC/MDKFONT.FTI`); one FTI font record
  (`--preview-font`, `FONTSML`/`FONTBIG` glyph layout) drawn into the
  indexed framebuffer through the SYS_PAL palette head; one FTI sprite
  record (`--preview-sprite`, the `ARROW` frame-table + command-stream
  format) over a diagnostic checkerboard; the composed static
  front-end frame (`--preview-options`, `MDKOPT` + `OPT0..OPT4` +
  `ARROW` in the proven draw order); and the interactive root-menu
  controller (`--interactive-frontend`, same composition driven by the
  reconstructed FUN_0041dc90 selection/scale/activation state, flowing
  into the FUN_00420eac options sub-menu with the Skill row's real
  mutation + native-owned persistence seam — child screens deferred)
  — see `ENGINE_RECONSTRUCTION.md`. Metadata-only interior parsers for
  `.SNI/.MTI/.MTO/.CMI/.DTI/.FTI/.BNI` exist in `mdk_core`/
  `mdk-inspect` (Phases 3C–3H). `.LBB/.SAV/.FLC/.MVE` remain unparsed.
- Window-close quit and resize were code-verified; physical
  keyboard/mouse input could not be host-injected on this machine (TCC),
  so `--selftest` exercises the identical event path instead.
- Single window, single Metal backend, nearest-neighbor only; no
  shaders-as-assets, no .metallib packaging.
- `Esc` quits the shell (native convenience binding, not an original
  key mapping).
