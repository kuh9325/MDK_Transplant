# Runtime Oracle — Phase 2A

Status: DOS lane bootstrap complete (menu reached). Win95 lane blocked
(see below). Recorded 2026-09-17.

Everything in this document describes runtime behavior of **BUILD_A**
(`original/installed/`), a third-party repack with self-described
"NoCD-Fixed" executables. BUILD_A is a **PROVISIONAL_RUNTIME_SOURCE** —
never an AUTHORITATIVE_PRISTINE_BUILD. All behavioral observations here are
therefore **PROVISIONAL** until reproduced against a verified-clean
retail/CD/GOG build.

## Evidence vocabulary for runtime observations

| Level | Meaning |
|---|---|
| OBSERVED | Directly seen in a runtime session |
| REPRODUCIBLE | Observed repeatedly from a documented start state |
| BUILD-SPECIFIC | Observed only for one executable variant |
| PROVISIONAL | Observed in BUILD_A; unverified vs a clean build |
| UNKNOWN | Not tested / not established |

Runtime observations constrain *behavior* only. They do not name or imply
original functions, structures, or implementation details.

## Host environment

| Item | Value |
|---|---|
| Host | macOS 27.0 (Darwin 27.0.0), Apple Silicon arm64 (T8132) |
| Rosetta | not installed (x86_64 host binaries cannot run) |
| Screen Recording perm. | denied to terminal — host `screencapture` unusable |
| Accessibility (keystroke) perm. | denied — no host-side input injection |

Because both host-side capture and host-side input injection are
permission-blocked, DOS-lane observation uses a **guest-side observer**
(see below) that runs inside the emulator and needs no host permissions.

## DOS lane — OPERATIONAL

| Item | Value |
|---|---|
| Emulator | DOSBox-X **2026.08.31** SDL2, Homebrew formula, arm64 (`/opt/homebrew/bin/dosbox-x`) |
| Executable | `MDKDOS.EXE` (LE, DOS/4GW-bound; SHA-256 `7471fa6a…df591b`) |
| Runtime copy | `runtime-private/dos/mdk/` — full copy of BUILD_A (ignored) |
| Config | `scripts/runtime/dosbox-x-mdkdos.conf` (`machine=svga_s3`, `memsize=32`, `sbtype=sb16` 220/5/1, `cycles=max`) |
| Copied-config change | `MDK.CFG` in the COPY only: `SoundID=0xE015`, `SoundIRQ=5`, `SoundDMA=1`, `SoundPort=0x220` (SB16 for HMI SOS; see `scripts/runtime/setup-runtime.sh`) |

### Launch procedure

```sh
scripts/runtime/setup-runtime.sh   # first time: build the writable copy
scripts/runtime/run-mdk-dos.sh     # launch (logs to runtime-private/logs/)
scripts/runtime/reset-dos.sh       # clean state: delete copy, rebuild from BUILD_A
```

The conf mounts `runtime-private/dos/mdk` as `C:`, loads the observer TSR,
then runs `MDKDOS.EXE`. There is intentionally no `exit` so post-exit state
stays inspectable.

### Clean state / persistence

- `reset-dos.sh` deletes `runtime-private/dos/mdk/` and rebuilds it from
  `original/installed/` — the source tree is never touched.
- Persistent between runs otherwise: the whole writable copy, including
  `MDK.CFG`, `SAVES/`, and `SHnn.BIN`/`QHnn.BIN` capture files written by
  the observer.
- `runtime-private/captures/` holds decoded captures (ignored).

### Guest-side observer (instrumentation, not part of the game)

`SHOTKEY.COM` (built by `analysis-private/scripts/build_shotkey.py` from
`shotkey.s`, both ignored) is a real-mode TSR loaded before the game. It:

- hooks INT 08h (timer, 18.2 Hz — still reflected to real mode under
  DOS/4GW);
- every ~3 s writes `C:\SHnn.BIN` (then `QHnn.BIN` after 99): BIOS/VESA mode,
  256-color DAC palette, text memory, and VESA banked windows via INT10
  4F05h;
- stuffs scripted BIOS-buffer keys (0040:001A) on a time schedule.

`analysis-private/scripts/decode_shots.py` converts dumps to PNG
(VESA 8bpp) or text. The observer perturbs timing slightly and must not be
treated as part of the original runtime; for interactive runs it can be
removed from the conf autoexec.

### Smoke-test results (BUILD_A, PROVISIONAL)

| Check | Result |
|---|---|
| Game starts | **OBSERVED** — process stable, no crash |
| Rendering | **OBSERVED** — VESA mode `0x4101` (640x480x8, LFB flag set); banked readback via window A still works under DOSBox-X |
| Startup sequence | **OBSERVED** — `Checking...` screen (version string `0.3.1`), Shiny Entertainment logo with dissolve, Playmates/Interplay publisher text, then main menu |
| Main menu | **OBSERVED** — `New Game / Saved Game / Options / Quit`, ~7 min into the run; static for ~3 min of observation |
| Intro movie | UNKNOWN — no cinematic frames captured this run (possibly skipped by an injected key, or not in this flow) |
| Keyboard input | UNKNOWN — BIOS-buffer Enter/Esc stuffing produced **no observable effect** on the menu (HYPOTHESIS: game uses a direct INT9/port-60h handler; needs real keyboard or a different injection path) |
| Audio | UNKNOWN — HMI drivers + SB16 config in place; not verifiable headlessly |
| Gameplay entry | Not reached (input not demonstrated) |
| Clean termination | OBSERVED once — emulator exited cleanly ~1 min after the capture schedule ended; mechanism undetermined (possible menu Quit via a late key, or emulator-level event; no crash report) |

### Known DOS-lane limitations

- Host TCC permissions block `screencapture` and keystroke injection —
  all evidence is guest-side framebuffer dumps.
- VESA dumps show occasional torn/partial frames (mid-draw sampling).
- The observer perturbs timing; intro pacing (~7 min to menu) may differ
  from an uninstrumented run.
- Untested: sound output, savegames, level entry, in-game rendering.

## Win95 lane — BLOCKED

| Item | Value |
|---|---|
| Target executable | `MDK95.EXE` (PE32 GUI; SHA-256 `e57bd63b…0ba788`) — generic software-renderer build; accelerator variants (`MDK3DFX`, `MDKPVR`, `MDKRED`) deliberately not attempted first |
| Status | **BLOCKED — no Windows 95 environment** |

### Why blocked

- `MDK95.EXE` imports `DDRAW`/`DSOUND`/`DINPUT`/`WINMM` — needs a real
  Win9x-class OS + DirectX runtime, not just a CPU emulator.
- No Windows 95 install media, VM image, or other licensed OS is present
  in any drop-zone — and acquiring one is a user/legal step (no downloads
  of proprietary OS images from arbitrary sources).
- Wine is not viable here: the `wine-stable` cask is x86-only and
  Gatekeeper-disabled on this host, and no Rosetta exists to run it.
  Modern Wine would also be a poor oracle for 1997 DirectDraw behavior.

### Planned path (documented, not yet executed)

- **QEMU** — `qemu` 11.1.1 is bottled in Homebrew for arm64 and can
  emulate i386 under TCG. Plan: `qemu-system-i386` + user-supplied,
  legally owned Windows 95 install media + DirectX runtime, in a disk
  image under `runtime-private/win95/` (ignored).
- QEMU was deliberately **not installed yet**: with no OS media it cannot
  be demonstrated, and Phase rules discourage unused installs.
- Alternatives considered: UTM (QEMU front-end — same media requirement),
  86Box/PCem (x86_64-only upstream builds; Rosetta absent), Wine (above).

## Comparison baseline

Not applicable yet — only the DOS lane is operational. When the Win95
lane exists, compare only observable startup flow, menus, resolution,
renderer presentation, audio, controls, and level entry — no internal
"why" analysis, no quality ranking.

## Clean-build gate (unresolved requirement)

A verified-clean, legally owned MDK retail/CD/GOG build is required before
any executable-level or behavioral finding from BUILD_A is treated as
authoritative. Until then all runtime observations carry PROVISIONAL.
A new build, when supplied, gets its own identifier and inventory;
BUILD_A metadata is not overwritten.
