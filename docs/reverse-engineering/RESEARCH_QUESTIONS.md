# Research Questions

Every item is **UNKNOWN** unless evidence upgrades it (see EVIDENCE_POLICY.md).
This file is the index of what we do not yet know. Items should be annotated
in place as evidence accumulates; do not remove answered questions — mark them
with their evidence level and a pointer to the supporting document.

## Executable architecture

- [CORROBORATED: shared core] Relationship between DOS and Windows game logic — 1,068/1,626 MDKDOS strings byte-identical to MDK95 incl. 25 shared `mdksrc\main|share` source-path strings; DOS adds only `mdksrc\dos\opthmi.c`. Homologous main loop, mode orchestrator, and loader functions confirmed by call-graph shape + identical string clusters. Platform layer (DDraw/DInput/DSound vs VESA/INT9/HMI) is per-OS. See EXECUTABLE_MAP.md §DOS↔Win95.
- [UNKNOWN] Relationship between the Classic Mac (PPC) build and x86 builds — port lineage, shared sources? (no Mac build supplied as of Phase 1)
- [STRONG EVIDENCE: Watcom] Compiler/toolchain fingerprints per build — WATCOM C/C++32 and C/C++16 run-time banners embedded in BUILD_A's `MDKDOS.EXE`/`MDK95.EXE`/`MDKZAP.EXE`; Watcom-style PE layout (`AUTO`/`DGROUP`/`BEGTEXT` sections, `VirtualSize==0`). Exact Watcom version still UNKNOWN; Mac toolchain UNKNOWN.
- [OBSERVED, partial] Which subsystems are statically vs dynamically linked in each build — BUILD_A Win95 exes statically import DDRAW/DSOUND/DINPUT/WINMM (+variant DLLs); DOS LE images have zero import modules (DOS/4GW-bound); HMI `.386` audio drivers and `MDK_FF.DLL` are runtime-loaded; `*.FRC` force effects are RIFF data. Details beyond imports remain UNKNOWN. (See ORIGINAL_BUILDS.md §BUILD_A.)
- [OBSERVED for BUILD_A] Executable format details per build — DOS exes are LE (DOS/4GW-bound), Windows exes are PE32 i386 GUI, `DOSWINKY.VXD` is an LE VxD. Mac format UNKNOWN (no build supplied).
- [OBSERVED] DOS extender identity — DOS/4GW (Rational Systems; `DOS4GW.EXE` bundled; embedded DOS/4G + DOS/16M banners).
- [OBSERVED, partial] Entry points, subsystem boundaries, and module organization per build — MDK95 entry `0x47d524`→CRT `0x480042`→WinMain `0x401abc`→main loop `0x40103c`; mode/sub-mode globals `0x541492`/`0x541493`; subsystem candidates mapped in EXECUTABLE_MAP.md. MDKDOS entry obj1+0x81318; main loop body at flat `0x10380–0x1089e` with jump table `0x10320`. Deep internals UNKNOWN.

## Asset formats

- [UNKNOWN] Level/mission container format — magic bytes, versioning, layout (BUILD_A `.CMI`/`.DTI`/`.MTO`/`LEVEL*S.MTI` begin with u32 length + `LEVE` tag; semantics UNKNOWN)
- [UNKNOWN] Texture/image formats — bit depth, palettes, compression
- [UNKNOWN] Mesh/model representation — vertex/index formats, hierarchies
- [UNKNOWN] Animation data — skeletal vs vertex vs procedural
- [UNKNOWN] Collision data — per-level, per-object, derived or authored
- [UNKNOWN] Enemy placement/spawn data — embedded in level files vs separate
- [OBSERVED, partial] Script/event system — `tr_alcmd.c` ("traverse alien commands") exists in both builds; strings `Alien %s looped %d commands`, `Unrecognised controlalien` indicate a per-alien command interpreter. Opcode set/format UNKNOWN.
- [UNKNOWN] Audio formats — `.SNI` files exist (tags `FALL`/`LEVE`/`MDKS`) but encoding is UNKNOWN; DOS audio middleware identified as HMI SOS (`.386` drivers + `HMISETUP.INI`)
- [OBSERVED, partial] Video/cinematic format — Autodesk FLIC (`.FLC`, magic 0xAF12, 600x360x8) and Interplay MVE (`.MVE`) files ship in `MISC/FLIC/`; no Smacker evidence. Playback path located: `FUN_0041d7b4` (MDK12 intro), `FUN_0041ebf4` (PIE), `FUN_0047b0fc` (MDKEND+FINISH.BNI ending), `FUN_0047b674` (MDKBZK.MVE, `Cannot open movie file`); decoder internals UNKNOWN.
- [OBSERVED, partial] Archive/packing format for shipped data files — most proprietary families begin u32 length (file size−4) + 4-char tag equal to file stem; `.BNI` differs (non-ASCII second field); `.LBB` looks raw. Whether files are further compressed/packed inside is UNKNOWN.
- [UNKNOWN] Endianness consistency across x86 and PPC builds (no PPC build supplied)
- [UNKNOWN] Whether any formats are shared across builds or platform-specific

## Runtime

- [OBSERVED, partial] Player update loop structure — main loop pumps messages, polls input, accumulates frame delta `0x49b6e8`, dispatches mode handlers (traversal frame `FUN_00436100` for mode 3). Order: pump→tick/input→overlay-dialog check→mode dispatch→(present inside handlers). In-frame order inside traversal UNKNOWN.
- [OBSERVED, partial] Fixed vs variable timestep — a per-frame delta global (`0x49b6e8`) is accumulated into timing counters; suggests variable-step accumulation, but exact integration semantics UNKNOWN.
- [UNKNOWN] Collision detection approach and tolerances
- [UNKNOWN] AI structure — state machines, scripts, pathfinding
- [UNKNOWN] Camera model — third-person follow behavior, constraints
- [UNKNOWN] Weapon/projectile system — hitscan vs simulated projectiles, spread, damage
- [UNKNOWN] RNG — generator, seeding, determinism
- [OBSERVED, partial] Save-game structures and versioning — F2 traversal save produces `SAVES\<name>.SAV` (~33 KB, `SAVE` tag at offset 8); `LASTGAME.SAV` written on level exit. Phase 2B adds: `savegame.c` exists in both builds; loader diagnostics reveal a header+version check then a sequence of `(4-char tag, u32 size)` packets (`SAVE CORRUPT: want %c%c%c%c:%d, found %c%c%c%c:%d`, `packet %d not found`). Per-packet field semantics UNKNOWN
- [UNKNOWN] Physics model — gravity, terminal velocity, sniper-mode mechanics
- [OBSERVED, partial] Level transition / streaming behavior — New Game loads `FALL3D\*` (freefall) then a `TRAVERSE\LEVEL*` dataset (BUILD_A shows `LEVEL7` for the entry level — internal numbering vs displayed order UNKNOWN); level session ends back at intro/menu with `LASTGAME.SAV` write. Phase 2B adds: mode-2→6/7 transition path statically confirmed (`FUN_004346e8`→`FUN_00433d40` traversal loader; `FUN_0040ef28` fall3d init; orchestrator `FUN_0041dc90`) — trigger (death vs quit vs timeout) still not separated
- [OBSERVED] Attract-mode structure — intro FLIC → looping `MISC\MDKS_001..011.GIF` slideshow that requires a keypress to break (verified by no-key control run); menu files `STATS.MTI/BNI` on break
- [OBSERVED, partial] DOS input path — game consumes real INT9-level scancodes (8042-injected input drives menus/gameplay); BIOS buffer stuffing has no effect; `MDK.CFG` remaps gameplay keys (`KeyUp=17` W etc.); menu nav uses arrows; F2/F3 = save/load (traversal only per MDKDOS.TXT); captured (locked) mouse gives correct relative movement in-game (user-verified); DOS mouse config exposes only X/Y axes — no Z-axis/wheel binding exists in MDKDOS.EXE (user-observed); sniper zoom = original keyboard controls (A/Z default; R/F in BUILD_A cfg); Z-axis/wheel question deferred to Win95 lane
- [UNKNOWN] Freefall control model — whether steering is required/possible, or the fall auto-completes to traversal
- [UNKNOWN] Difficulty scaling parameters

## Rendering

- [OBSERVED, partial] Software renderer path — arena-based level render: `draw_arena` (`FUN_00431300`, "Overflowed MaxObjects in draw_arena"), `BSPShow %s not found` (`FUN_00432e2c`), `arena %s not found` (`FUN_00432ec4`), `Too many polygons for current sort list` (poly sort list ≈ z-order), `3 Cooridors not allowed for arena` (corridor-connected arenas). Output = ~600×360 8bpp back buffer `0x541650` presented via DirectDraw (`FUN_0046c86c/0x46ca84`). Rasterizer internals (texture mapping, perspective correction) UNKNOWN.
- [OBSERVED] DirectDraw/Direct3D usage in the Windows build — `FUN_0047aa74` calls `DirectDrawCreate`, queries caps, sets cooperative level, `SetDisplayMode 640×480×8bpp`, creates primary+back surfaces; `FUN_0042277c` calls `DirectDrawEnumerateA` (options/perf screen). `MDKD3D.EXE` reaches Direct3D via DirectDraw COM (no separate D3D import; readme documents DirectX 3.0+). Renderer-variant DLLs: `glide2x.dll` ×34 (3DFX), `sgl.dll` ×12 (PVR), `verite.dll` ×20 + `redline.dll` ×31 and NO DDRAW (RED). Feature-set internals UNKNOWN.
- [UNKNOWN] Classic Mac renderer — QuickDraw/Rave/custom?
- [OBSERVED, partial] Visibility/culling — per-arena BSP evidence (`BSPShow`, `arena` resources, corridor topology limit). Whether portal-style traversal or BSP-ordering drives culling UNKNOWN.
- [UNKNOWN] Lighting model — per-vertex, lightmaps, gouraud, flat
- [UNKNOWN] Fog/atmospheric effects implementation
- [UNKNOWN] Texture handling — filtering, mip selection, palette management
- [UNKNOWN] Resolution and aspect handling per build
- [UNKNOWN] HUD/menus rendering path vs 3D path

## Platform / IO

- [OBSERVED] Input abstraction — DOS build reads INT9/port-60h scancodes directly (8042 cmd 0xD2 injection works; BIOS buffer stuffing does not); Win95 `FUN_0046bd14` creates DirectInput keyboard+mouse device objects (error strings `Cannot initialise mouse/keyboard from directinput`), polled per frame by `FUN_0046bc18`/`FUN_0046b688`; joystick via `joyGetPosEx` (`FUN_0046b9b4`). Cross-build: same engine consumes platform-produced input state; per-platform acquisition differs.
- [OBSERVED] Audio output APIs per build — DOS build uses HMI Sound Operating System (`hmidet.386`/`hmidrv.386`, `MISC\HMISETUP.INI` listing SB/GUS/ESS/etc. device IDs; `dos\opthmi.c`); Windows `FUN_0046c198` calls `DirectSoundCreate` + COM setup (`DirectSound Init Failed`, `SoundInit Failed: Resources in Use/Invalid Parameter`). Shared `soundset.c` loads `MISC\MDKSOUND.SNI`; `SndPlay failed for %s`, `Out of sound samples`, `Unknown sound mode %x` in both. Mac UNKNOWN.
- [OBSERVED, partial] CD-ROM vs installed-data layout and runtime file resolution order — `MDK.CFG` contains `cddata`/`hddata` path keys (both `.\` in BUILD_A = full HDD install); `MDKDOS.EXE` references `MISC\FLIC\SHINY.FLC`, absent from the tree (CD-only content? UNKNOWN). Resolution order UNKNOWN.
- [OBSERVED, partial] Configuration file formats — `MDK.CFG` is text key=value (sound device/IRQ/DMA/port, mouse mappings, `ForcePCorrect`, `D3DOptions`); `HMISETUP.INI` is INI-format. Phase 2B adds: `FUN_00425de4`/`FUN_004260ac` read `C:\MDK.CFG` then relative `MDK.CFG` at startup; `mdkfopen` (`FUN_0041ae50`) resolves `cddata`/`hddata` roots. Registry/preferences usage UNKNOWN.

## Methodology questions

- [HYPOTHESIS, supported] Which build is cheapest to run reproducibly for oracle experiments — DOS under emulation remains the leading hypothesis; BUILD_A already ships with a preconfigured DOSBox 0.72 setup (renamed `MDK.EXE` + `DOSBOX.CONF`), which is convenient but NOT a verified-pristine reference environment (repack, NoCD-fixed binaries).
- [UNKNOWN] Whether any official technical documentation, SDK leftovers, or contemporaneous interviews describe internals
