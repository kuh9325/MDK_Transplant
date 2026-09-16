# Research Questions

Every item is **UNKNOWN** unless evidence upgrades it (see EVIDENCE_POLICY.md).
This file is the index of what we do not yet know. Items should be annotated
in place as evidence accumulates; do not remove answered questions — mark them
with their evidence level and a pointer to the supporting document.

## Executable architecture

- [UNKNOWN] Relationship between DOS and Windows game logic — shared core vs divergent codebases?
- [UNKNOWN] Relationship between the Classic Mac (PPC) build and x86 builds — port lineage, shared sources? (no Mac build supplied as of Phase 1)
- [STRONG EVIDENCE: Watcom] Compiler/toolchain fingerprints per build — WATCOM C/C++32 and C/C++16 run-time banners embedded in BUILD_A's `MDKDOS.EXE`/`MDK95.EXE`/`MDKZAP.EXE`; Watcom-style PE layout (`AUTO`/`DGROUP`/`BEGTEXT` sections, `VirtualSize==0`). Exact Watcom version still UNKNOWN; Mac toolchain UNKNOWN.
- [OBSERVED, partial] Which subsystems are statically vs dynamically linked in each build — BUILD_A Win95 exes statically import DDRAW/DSOUND/DINPUT/WINMM (+variant DLLs); DOS LE images have zero import modules (DOS/4GW-bound); HMI `.386` audio drivers and `MDK_FF.DLL` are runtime-loaded; `*.FRC` force effects are RIFF data. Details beyond imports remain UNKNOWN. (See ORIGINAL_BUILDS.md §BUILD_A.)
- [OBSERVED for BUILD_A] Executable format details per build — DOS exes are LE (DOS/4GW-bound), Windows exes are PE32 i386 GUI, `DOSWINKY.VXD` is an LE VxD. Mac format UNKNOWN (no build supplied).
- [OBSERVED] DOS extender identity — DOS/4GW (Rational Systems; `DOS4GW.EXE` bundled; embedded DOS/4G + DOS/16M banners).
- [UNKNOWN] Entry points, subsystem boundaries, and module organization per build

## Asset formats

- [UNKNOWN] Level/mission container format — magic bytes, versioning, layout (BUILD_A `.CMI`/`.DTI`/`.MTO`/`LEVEL*S.MTI` begin with u32 length + `LEVE` tag; semantics UNKNOWN)
- [UNKNOWN] Texture/image formats — bit depth, palettes, compression
- [UNKNOWN] Mesh/model representation — vertex/index formats, hierarchies
- [UNKNOWN] Animation data — skeletal vs vertex vs procedural
- [UNKNOWN] Collision data — per-level, per-object, derived or authored
- [UNKNOWN] Enemy placement/spawn data — embedded in level files vs separate
- [UNKNOWN] Script/event system — existence, format, opcode set
- [UNKNOWN] Audio formats — `.SNI` files exist (tags `FALL`/`LEVE`/`MDKS`) but encoding is UNKNOWN; DOS audio middleware identified as HMI SOS (`.386` drivers + `HMISETUP.INI`)
- [OBSERVED presence] Video/cinematic format — Autodesk FLIC (`.FLC`, magic 0xAF12, 600x360x8) and Interplay MVE (`.MVE`) files ship in `MISC/FLIC/`; no Smacker evidence. In-engine playback path UNKNOWN.
- [OBSERVED, partial] Archive/packing format for shipped data files — most proprietary families begin u32 length (file size−4) + 4-char tag equal to file stem; `.BNI` differs (non-ASCII second field); `.LBB` looks raw. Whether files are further compressed/packed inside is UNKNOWN.
- [UNKNOWN] Endianness consistency across x86 and PPC builds (no PPC build supplied)
- [UNKNOWN] Whether any formats are shared across builds or platform-specific

## Runtime

- [UNKNOWN] Player update loop structure and order of operations
- [UNKNOWN] Fixed vs variable timestep; frame-rate dependence of logic
- [UNKNOWN] Collision detection approach and tolerances
- [UNKNOWN] AI structure — state machines, scripts, pathfinding
- [UNKNOWN] Camera model — third-person follow behavior, constraints
- [UNKNOWN] Weapon/projectile system — hitscan vs simulated projectiles, spread, damage
- [UNKNOWN] RNG — generator, seeding, determinism
- [OBSERVED, partial] Save-game structures and versioning — F2 traversal save produces `SAVES\<name>.SAV` (~33 KB, `SAVE` tag at offset 8); `LASTGAME.SAV` written on level exit; field semantics UNKNOWN
- [UNKNOWN] Physics model — gravity, terminal velocity, sniper-mode mechanics
- [OBSERVED, partial] Level transition / streaming behavior — New Game loads `FALL3D\*` (freefall) then a `TRAVERSE\LEVEL*` dataset (BUILD_A shows `LEVEL7` for the entry level — internal numbering vs displayed order UNKNOWN); level session ends back at intro/menu with `LASTGAME.SAV` write — trigger (death vs quit vs timeout) not yet separated
- [OBSERVED] Attract-mode structure — intro FLIC → looping `MISC\MDKS_001..011.GIF` slideshow that requires a keypress to break (verified by no-key control run); menu files `STATS.MTI/BNI` on break
- [OBSERVED, partial] DOS input path — game consumes real INT9-level scancodes (8042-injected input drives menus/gameplay); BIOS buffer stuffing has no effect; `MDK.CFG` remaps gameplay keys (`KeyUp=17` W etc.); menu nav uses arrows; F2/F3 = save/load (traversal only per MDKDOS.TXT); captured (locked) mouse gives correct relative movement in-game (user-verified); DOS mouse config exposes only X/Y axes — no Z-axis/wheel binding exists in MDKDOS.EXE (user-observed); sniper zoom = original keyboard controls (A/Z default; R/F in BUILD_A cfg); Z-axis/wheel question deferred to Win95 lane
- [UNKNOWN] Freefall control model — whether steering is required/possible, or the fall auto-completes to traversal
- [UNKNOWN] Difficulty scaling parameters

## Rendering

- [UNKNOWN] Software renderer path — rasterizer structure, perspective correction
- [OBSERVED, partial] DirectDraw/Direct3D usage in the Windows build — BUILD_A Win95 exes import DDRAW/DSOUND/DINPUT; `MDKD3D.EXE` reaches Direct3D via DirectDraw (no separate D3D import; its readme documents DirectX 3.0+). Renderer-variant DLLs: `glide2x.dll`, `sgl.dll`, `verite.dll`+`redline.dll`. Feature-set internals UNKNOWN.
- [UNKNOWN] Classic Mac renderer — QuickDraw/Rave/custom?
- [UNKNOWN] Visibility/culling approach — BSP, portal, per-polygon
- [UNKNOWN] Lighting model — per-vertex, lightmaps, gouraud, flat
- [UNKNOWN] Fog/atmospheric effects implementation
- [UNKNOWN] Texture handling — filtering, mip selection, palette management
- [UNKNOWN] Resolution and aspect handling per build
- [UNKNOWN] HUD/menus rendering path vs 3D path

## Platform / IO

- [OBSERVED, partial] Input abstraction — DOS build reads INT9/port-60h scancodes directly (8042 cmd 0xD2 injection works; BIOS buffer stuffing does not); Win95 imports DINPUT; cross-build comparison UNKNOWN
- [OBSERVED, partial] Audio output APIs per build — DOS build uses HMI Sound Operating System (`hmidet.386`/`hmidrv.386`, `MISC\HMISETUP.INI` listing SB/GUS/ESS/etc. device IDs); Windows builds import `DSOUND.dll` (DirectSoundCreate). Mac UNKNOWN.
- [OBSERVED, partial] CD-ROM vs installed-data layout and runtime file resolution order — `MDK.CFG` contains `cddata`/`hddata` path keys (both `.\` in BUILD_A = full HDD install); `MDKDOS.EXE` references `MISC\FLIC\SHINY.FLC`, absent from the tree (CD-only content? UNKNOWN). Resolution order UNKNOWN.
- [OBSERVED, partial] Configuration file formats — `MDK.CFG` is text key=value (sound device/IRQ/DMA/port, mouse mappings, `ForcePCorrect`, `D3DOptions`); `HMISETUP.INI` is INI-format. Registry/preferences usage UNKNOWN.

## Methodology questions

- [HYPOTHESIS, supported] Which build is cheapest to run reproducibly for oracle experiments — DOS under emulation remains the leading hypothesis; BUILD_A already ships with a preconfigured DOSBox 0.72 setup (renamed `MDK.EXE` + `DOSBOX.CONF`), which is convenient but NOT a verified-pristine reference environment (repack, NoCD-fixed binaries).
- [UNKNOWN] Whether any official technical documentation, SDK leftovers, or contemporaneous interviews describe internals
