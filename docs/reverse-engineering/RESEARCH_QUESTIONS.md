# Research Questions

Every item is **UNKNOWN** unless evidence upgrades it (see EVIDENCE_POLICY.md).
This file is the index of what we do not yet know. Items should be annotated
in place as evidence accumulates; do not remove answered questions — mark them
with their evidence level and a pointer to the supporting document.

## Executable architecture

- [UNKNOWN] Relationship between DOS and Windows game logic — shared core vs divergent codebases?
- [UNKNOWN] Relationship between the Classic Mac (PPC) build and x86 builds — port lineage, shared sources?
- [UNKNOWN] Compiler/toolchain fingerprints per build (e.g., Watcom, MSVC, CodeWarrior — all unverified)
- [UNKNOWN] Which subsystems are statically vs dynamically linked in each build
- [UNKNOWN] Executable format details per build (MZ/LE/LX for DOS extender? PE for Windows? PEF for Mac?) — no files inspected yet
- [UNKNOWN] DOS extender identity, if any (DOS/4GW, CWSDPMI, other)
- [UNKNOWN] Entry points, subsystem boundaries, and module organization per build

## Asset formats

- [UNKNOWN] Level/mission container format — magic bytes, versioning, layout
- [UNKNOWN] Texture/image formats — bit depth, palettes, compression
- [UNKNOWN] Mesh/model representation — vertex/index formats, hierarchies
- [UNKNOWN] Animation data — skeletal vs vertex vs procedural
- [UNKNOWN] Collision data — per-level, per-object, derived or authored
- [UNKNOWN] Enemy placement/spawn data — embedded in level files vs separate
- [UNKNOWN] Script/event system — existence, format, opcode set
- [UNKNOWN] Audio formats — music (tracker? streamed?), SFX encoding
- [UNKNOWN] Video/cinematic format, if any
- [UNKNOWN] Archive/packing format for shipped data files
- [UNKNOWN] Endianness consistency across x86 and PPC builds
- [UNKNOWN] Whether any formats are shared across builds or platform-specific

## Runtime

- [UNKNOWN] Player update loop structure and order of operations
- [UNKNOWN] Fixed vs variable timestep; frame-rate dependence of logic
- [UNKNOWN] Collision detection approach and tolerances
- [UNKNOWN] AI structure — state machines, scripts, pathfinding
- [UNKNOWN] Camera model — third-person follow behavior, constraints
- [UNKNOWN] Weapon/projectile system — hitscan vs simulated projectiles, spread, damage
- [UNKNOWN] RNG — generator, seeding, determinism
- [UNKNOWN] Save-game structures and versioning
- [UNKNOWN] Physics model — gravity, terminal velocity, sniper-mode mechanics
- [UNKNOWN] Level transition / streaming behavior
- [UNKNOWN] Difficulty scaling parameters

## Rendering

- [UNKNOWN] Software renderer path — rasterizer structure, perspective correction
- [UNKNOWN] DirectDraw/Direct3D usage in the Windows build (version, feature set)
- [UNKNOWN] Classic Mac renderer — QuickDraw/Rave/custom?
- [UNKNOWN] Visibility/culling approach — BSP, portal, per-polygon
- [UNKNOWN] Lighting model — per-vertex, lightmaps, gouraud, flat
- [UNKNOWN] Fog/atmospheric effects implementation
- [UNKNOWN] Texture handling — filtering, mip selection, palette management
- [UNKNOWN] Resolution and aspect handling per build
- [UNKNOWN] HUD/menus rendering path vs 3D path

## Platform / IO

- [UNKNOWN] Input abstraction differences across builds
- [UNKNOWN] Audio output APIs per build (Sound Blaster/General MIDI on DOS? DirectSound on Windows? Sound Manager on Mac?)
- [UNKNOWN] CD-ROM vs installed-data layout and runtime file resolution order
- [UNKNOWN] Configuration file formats and registry/preferences usage

## Methodology questions

- [UNKNOWN] Which build is cheapest to run reproducibly for oracle experiments (DOS under emulation is the leading HYPOTHESIS)
- [UNKNOWN] Whether any official technical documentation, SDK leftovers, or contemporaneous interviews describe internals
