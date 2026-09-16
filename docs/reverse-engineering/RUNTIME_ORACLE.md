# Runtime Oracle — Phase 2A/2A.1/2A.2

Status: DOS lane interactive baseline complete — automated input, gameplay
entry, in-game save, controlled quit, automatic mouse capture, and wheel
passthrough configuration all verified under DOSBox-X. Win95 lane blocked
(see below). Recorded 2026-09-17 (2A), updated same date for 2A.1/2A.2.

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
| Config | `scripts/runtime/dosbox-x-mdkdos.conf` (`machine=svga_s3`, `memsize=32`, `sbtype=sb16` 220/5/1, `cycles=max`, `autolock=true`, `mouse_emulation=locked`, `mouse_wheel_key=0`, `auxdevice=intellimouse`, `CAPMOUSE /C` in autoexec) |
| Copied-config change | `MDK.CFG` in the COPY only: `SoundID=0xE015`, `SoundIRQ=5`, `SoundDMA=1`, `SoundPort=0x220` (SB16 for HMI SOS; see `scripts/runtime/setup-runtime.sh`) |

### Launch procedure

```sh
scripts/runtime/setup-runtime.sh   # first time: build the writable copy
scripts/runtime/run-mdk-dos.sh     # launch (logs to runtime-private/logs/)
scripts/runtime/reset-dos.sh       # clean state: delete copy, rebuild from BUILD_A
```

The conf mounts `runtime-private/dos/mdk` as `C:` and runs `MDKDOS.EXE`.
There is intentionally no `exit` so post-exit state stays inspectable.

**Headless automated run (verified recipe):**

```sh
# 1. Generate TSRs into the disposable copy:
python3 analysis-private/scripts/build_injkey.py runtime-private/dos/mdk/INJKEY.COM
python3 analysis-private/scripts/bsnap.py       runtime-private/dos/mdk/SNAP.COM 470 5
# 2. For a serial frame dump add to the conf:  serial1 = file file:/tmp/mdk-com1.bin
# 3. Launch headless:
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  dosbox-x -conf scripts/runtime/dosbox-x-mdkdos.conf \
  -defaultdir "$PWD" -debug -log-int21 > runtime-private/logs/run.log 2>&1 &
```

The `-debug -log-int21` log records every guest file open by name —
attract/menu/level transitions are all observable via filenames
(`MDKS_*.GIF` slideshow cycle → `STATS.MTI/BNI` menu → `FALL3D\*` +
`TRAVERSE\LEVEL7\*` level load → `SAVES\*` writes → `EXIT.TXT` quit).
`inb/outb`-level keyboard injection needs no host permissions.

### Clean state / persistence

- `reset-dos.sh` deletes `runtime-private/dos/mdk/` and rebuilds it from
  `original/installed/` — the source tree is never touched.
- Persistent between runs otherwise: the whole writable copy, including
  `MDK.CFG`, `SAVES/`, and `SHnn.BIN`/`QHnn.BIN` capture files written by
  the observer.
- `runtime-private/captures/` holds decoded captures (ignored).

### Guest-side observer / injector (instrumentation, not part of the game)

Two real-mode TSRs generated into the disposable runtime copy (sources in
`analysis-private/scripts/`, all ignored; loaded conditionally by the conf
autoexec via `if exist`):

- `INJKEY.COM` (`injkey.s` / `build_injkey.py`) — hooks INT 08h and injects
  scheduled scancodes through keyboard-controller command `0xD2` (port
  64h) + data port 60h: real IRQ1 → real-mode INT 9 path. This is the
  **working input mechanism** (see Phase 2A.1 results).
- `SNAP.COM` (`snap.s` / `bsnap.py`) — one-shot VESA bank dump streamed to
  COM1; requires `serial1 = file file:<host-path>` in the conf. Pure port
  I/O, no DOS calls in interrupt context.
- `SHOTKEY.COM` (`shotkey.s` / `build_shotkey.py`, deprecated for input) —
  INT 08h video-RAM dumps to `C:\SHnn.BIN` + BIOS-buffer key stuffing.
  Frame capture worked (VESA `0x4101` PNGs); BIOS-buffer input did NOT
  reach the game. The heavy dump path also proved unstable mid-game
  (see Stability). Retained for intro/menu captures only.

`analysis-private/scripts/decode_shots.py` converts SHnn/QHnn dumps to PNG
(VESA 8bpp) or text.

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

## Phase 2A.1 — interactive DOS oracle results

All results are **PROVISIONAL** (BUILD_A repack) and were produced
headless (`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`) with
`-debug -log-int21` file-I/O logging as the observation channel.

### Input mechanism — RESOLVED

| Mechanism | Result |
|---|---|
| Host synthetic input (CGEvent/HID tap) | **BLOCKED** — `CGPreflightPostEventAccess`/`AXIsProcessTrusted` both false; TCC |
| DOSBox-X `AUTOTYPE` | **UNUSABLE for scheduling** — present and functional, but blocks the shell while running and caps `-w` at 30 s; cannot be scheduled to fire minutes into the game from autoexec |
| BIOS keyboard-buffer stuffing (SHOTKEY) | **NO EFFECT** — game does not poll INT 16h for gameplay input |
| **8042 KBC command `0xD2` via guest TSR (INJKEY)** | **WORKING** — injects raw set-1 scancodes (explicit make/break) through port 64h/60h, raising real IRQ1 → game's own INT9 handler. Proven at DOS prompt (injected `exit` quit the shell) and in-game |

INJKEY.COM hooks INT 08h, counts ticks (~18.2 Hz emulated), and fires a
compile-time schedule of make/break scancode pairs. Emulated-tick to
wall-time ratio is load-dependent (≈1:1.6–1:3); schedule values are
nominal emulated seconds.

### Verified input sequence (REPRODUCIBLE — 2 independent runs)

Nominal-tick schedule (seconds × 18.2):

| t (nom s) | Keys | Effect observed |
|---|---|---|
| 250 | Enter | attract slideshow stops → main menu (STATS.MTI/BNI open) |
| 268/278 | Down, Up | menu navigation window |
| 292, 310 | Enter ×2 | **New Game** → `FALL3D\*` + `MISC\LOAD_7.LBB` + `TRAVERSE\LEVEL7\*` load burst |
| 345–430 | arrow/W holds | freefall steering attempts (fall appears to auto-complete) |
| 450 | F2 + "SMKT" + Enter | **save dialog → `SAVES\SMKT.SAV` created, 33,041 bytes, `SAVE` header** — F2 is traversal-only per MDKDOS.TXT, so traversal gameplay was live |
| 475–516 | W hold, Ctrl, Alt, Space | movement/fire/jump/sniper-mode inputs delivered |
| 560–605 | Esc, Enter ×2 | **level exits → `LASTGAME.SAV` written → menu → clean quit to DOS** (`EXIT.TXT`) |

Differential control (no keys injected): attract slideshow looped
indefinitely (21+ cycles, >2× normal menu window) — proves the transitions
above were caused by injected input, not timers.

### Results table (Phase 2A.1)

| Check | Result |
|---|---|
| Automated input | **REPRODUCIBLE** — KBC 0xD2 injection, guest TSR |
| Menu navigation | **REPRODUCIBLE** — attract break, item selection, quit-confirm |
| Gameplay entry | **REPRODUCIBLE** — New Game → freefall (FALL3D) → traversal level data (LEVEL7 dir naming is BUILD_A's internal layout; provisional) |
| In-game input | **REPRODUCIBLE** — F2 save dialog + text entry + confirm produced `SMKT.SAV` (33,041 B) in two runs |
| Player control | **PARTIAL** — movement/fire/jump/sniper keys delivered on schedule while in-game; screen-space effect not captured (frame dump torn; see below) |
| Audio | **OBSERVED at hardware level** — `SBLASTER:Raising IRQ` streams through menu and gameplay (452 IRQs in the traversal window); audible host output unverified (headless dummy driver) |
| Frame capture | **PARTIAL** — SNAP.COM streamed 320 KB VESA banks over COM1 mid-traversal; decode shows real pixel data but torn/banded (mid-draw sampling / bank-granularity) |
| Controlled quit | **REPRODUCIBLE** — Esc + Enter → game exits to DOS → `EXIT.TXT` marker |

### Runtime stability notes

- Bare `MDKDOS.EXE` (no TSR) is stable ≥9 min headless; game self-quits
  only via menu/quit input.
- INJKEY/SNAP (lightweight int8 TSRs, no DOS calls in context) survived
  full ~10-min nominal schedules including gameplay and clean quit.
- The earlier heavy dump TSR (int21 file I/O + int10 bank-switch inside
  int8) crashed nondeterministically with descriptor faults / wild writes
  — InDOS reentrancy is the likely mechanism; it is deprecated.
- Two further non-fatal flake modes observed with TSRs resident: rare
  early descriptor faults (`E_Exit: JMP Illegal descriptor type N`), and
  two slideshow-phase hangs ending `WARN SBLASTER:DMA ended when previous
  IRQ had not yet been acked`. Both are emulated-edge instabilities, not
  consistent — treat long automated runs as needing retry budget.
- Emulated-time vs wall-time ratio drifts (≈1:1.6–1:3 under load);
  schedules must be expressed in emulated ticks, not wall time.

### Known DOS-lane limitations (post-2A.1)

- Host TCC permissions block `screencapture` and keystroke injection —
  all evidence is guest-side: file-I/O logs, serial-channel dumps, and
  filesystem artifacts (`.SAV`, `EXIT.TXT`).
- No clean gameplay screenshot yet: serial bank-dump frames are torn;
  the int21-based dump TSR is too unstable mid-game.
- Audible audio unverified headless; emulated SB16 IRQ/DMA activity is
  the strongest current evidence.
- What ends the traversal session (death vs quit key vs timeout) is not
  fully separated — the quit keys land near the same window.
- `MDK.CFG` in BUILD_A maps movement to WASD-style scancodes
  (`KeyUp=17` W, `KeyDown=31` S, `KeySideL=30` A, `KeySideR=32` D);
  arrows still drive menus.

## Phase 2A.2 — mouse capture, wheel passthrough, audio, stability

### Mouse capture (automatic)

| Setting | Value | Notes |
|---|---|---|
| `[sdl] autolock` | `true` | locks the pointer on first window click; Ctrl+F10 releases |
| `[sdl] mouse_emulation` | `locked` | relative-motion emulation while captured (DOSBox-X default, made explicit) |
| autoexec `CAPMOUSE /C` | — | DOSBox-X internal command; requests capture at launch so no click is needed |

Manual Ctrl+F10 is no longer required in the documented launch path.
**OBSERVED (user-verified): with the pointer locked, MDK's mouse movement
works correctly** — aim/look in gameplay responds to relative motion.
Unconditional OS-level grab before first focus is not guaranteeable from
config alone; `CAPMOUSE /C` + `autolock` covers launch and click cases.

### Mouse wheel / Z-axis passthrough

| Setting | Value | Notes |
|---|---|---|
| `[sdl] mouse_wheel_key` | `0` | never convert wheel to keys — wheel goes to the emulated mouse |
| `[keyboard] auxdevice` | `intellimouse` | wheel-capable PS/2 AUX device (default, made explicit) |

Probe evidence (guest `MPROBE.COM` over COM1): INT 33h `AX=0011h` returns
`AX=574Dh` (`'WM'` signature) + `CX=0001` — this DOSBox-X build exposes a
**wheel-capable INT 33h API** (REPRODUCIBLE). So physical wheel events can
reach a guest that queries the INT33 wheel extension.

**MDK consumption: UNKNOWN.** No supported mechanism can synthesize wheel
events without host input (AUTOTYPE types guest keys only; mapper events
are host-side; KBC `0xD3` AUX injection feeds int15h subscribers, not the
INT33 wheel counter). Whether MDK's `MouseWAxesMap`/`MouseDAxesMap`
(`A0G`, semantics UNKNOWN) routes wheel→sniper zoom requires a physical
wheel test or static analysis — documented as the one UNVERIFIED item.
Keyboard zoom (`KeyZoomIn=19` R, `KeyZoomOut=33` F in BUILD_A's cfg) is the
proven fallback path.

### MDK mouse configuration

Writable-copy `MDK.CFG` already carries mouse bindings:
`MouseWAxesMap=A0G`, `MouseDAxesMap=A0G`, `MouseWButtMapD=32768`,
`MouseDButtMapC=0`, `MouseDButtMapD=32768`. No offline edit needed —
mouse works with the shipped values once the pointer is captured
(user-observed). The map encoding is UNKNOWN; not modified.

### Smoke test under the new config (REPRODUCIBLE — 3rd reproduction)

Headless run with `-debug -log-int21` + `serial1=file` reproduced the full
chain: attract → menu (`STATS.MTI`) → New Game (`FALL3D_1.MTI` →
`LOAD_7.LBB` → `LEVEL7.CMI` + full LEVEL7 dataset) → traversal save
(`SMKT.SAV`, 33,041 B, `SAVE` magic — third independent write) → serial
frame dump (327,689 B = `FRAM`+mode `0x69`+5×64KB+`FEND`) → clean game
exit (`int21 ah=4c`, text mode restored, emulator idle at DOS prompt).
~11 min wall, no crash, no descriptor fault this run.

### Audio

- `SBLASTER:Raising IRQ` + DSP/DMA activity stream through the session
  (676 IRQs in the smoke run) — emulated SB16 hardware path REPRODUCIBLE.
- WAV/AVI capture: **not achievable headless** in this build.
  `-avistart` parses with a warning and arms the recorder (`USING
  AVI+ZMBV`) but never opens a capture file even windowed with
  `output=surface`; `mapper_recwave`/`recmtwave` are host-input mapper
  events unreachable from guest-side injection, and host keystroke
  synthesis is TCC-blocked. Audible output remains UNVERIFIED.

### Stability (2A.2)

- Full smoke session: ~11 min wall, clean exit, no termination anomaly.
- No recurrence of the earlier rare descriptor faults or slideshow hangs
  this run; they remain documented emulated-edge flakes with retry budget.

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
