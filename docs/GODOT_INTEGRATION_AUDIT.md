# Godot 4 Integration Audit — prior-art survey + boundary design

Status: DESIGN SUPERSEDED BY IMPLEMENTATION — the audit/design below
was recorded 2026-09-21 against MDK-Native `3f23778` (Phase 5N) and is
retained as the decision record. Phase 7 (G1, arena rendering) and
Phase 8 (G2, player presentation) implemented the designed boundary:
the retained frontend lives in `frontend/godot/` and is documented in
`docs/GODOT_FRONTEND.md`. Where the two disagree, the frontend doc
describes what actually shipped.

Evidence levels follow `reverse-engineering/EVIDENCE_POLICY.md`. Claims
about original MDK behavior remain as marked in
`GAMEPLAY_RECONSTRUCTION.md` / `DATA_FORMATS.md`; this document adds no
new original-behavior claims.

## 1. Executive decision

A future Godot 4 frontend is **feasible and compatible** with the
existing architecture. `mdk_core` is already a platform-neutral static
library (no SDL, no Metal, no Godot); the SDL3/Metal application,
`mdk-inspect`, and `mdk_tests` are three independent hosts of that same
core. Godot becomes a **fourth host** through a GDExtension that owns a
`TraversalRuntime` and trades small POD snapshots across the boundary.

Non-negotiable ownership rule (project policy, follows AGENTS.md rule 1):

- The MDK-Native C++ core remains the **sole authority** for original
  data interpretation and all reconstructed gameplay semantics:
  movement, collision, traversal, script VM, camera, combat.
- Godot owns **presentation only**: windowing, rendering, input
  acquisition, audio playback, HUD/menus, animation/effect display,
  packaging. Godot consumes snapshots and events; it never runs a
  parallel simulation and never reinterprets original formats.
- Godot physics (`CharacterBody3D`, `move_and_slide`, `RayCast3D`,
  physics areas) must **never** make gameplay decisions. See §11.

The prior-art project (§2) demonstrates both why this rule matters and
that it is achievable: its one real subsystem (the player controller) is
a Godot-physics approximation precisely because it had no reconstructed
core to consume. We have the core.

## 2. godot-mdk repository audit

| Field | Value |
|---|---|
| URL | <https://github.com/Calinou/godot-mdk> |
| Audited HEAD | `5257b9435d5120e62957a59633da062a1333ae4b` |
| Branch | `master` (only branch) |
| Commits | 30 |
| First commit | 2021-02-02 "Initial commit" |
| Last meaningful development | 2022-01-21 "Add MDK data notes" (bulk of work is Feb–Mar 2021; later commits are fixes/docs) |
| Godot version | Godot **3.4beta4** (`config_version=4`, GDScript 1.0 — `Spatial`, `KinematicBody`, `CPUParticles`, `PoolByteArray`, `File`/`Directory` APIs) |
| Self-described status | "**It is not in a playable state yet.**" (README) |
| License | MIT — © 2021 Hugo Locurcio and contributors (`LICENSE.md`) |
| Bundled third-party | `addons/smoothing` v1.0.3 (MIT, Lawnjelly); fonts Anton + Inter (SIL OFL 1.1) |
| Data acquisition | user-supplied install; probes `res://MDK|Mdk|mdk`, `C:/GOG Games/MDK`, `~/.wine/.../GOG Games/MDK`; no proprietary data in repo |

Audit location was a disposable clone (`/tmp/godot-mdk-audit`); nothing
was vendored, submoduled, or pushed.

### What the project actually is

A ~1,700-line GDScript prototype: autoloads (`Settings`, `Sound`,
`MDKData`, `Game`), a main/options/pause menu, a HUD using two decoded
textures, a placeholder level (`levels/3.tscn` = hand-placed cube floor
+ wall + noise texture), and a `KinematicBody` player with a hitscan
prototype. Its real content is one autoload — `autoload/mdk_data.gd`
(462 lines) — which parses the `.BNI` and `.SNI` archive directories and
decodes sprite payloads and embedded RIFF/WAVE samples.

## 3. License / code-reuse decision

godot-mdk is MIT-licensed. MIT permits copy/modify/redistribute with
attribution (copyright + license text must accompany copies or
substantial portions). There is no legal obstacle to reuse. The question
is whether anything is *worth* reusing — see §4.

The bundled pieces have their own licenses: the Smoothing addon is MIT
(Lawnjelly); the two fonts are SIL OFL 1.1. Both are compatible, but
neither is needed (§4).

Proprietary-boundary note: godot-mdk follows the same "no original data
in repo" rule we do. No boundary issues.

## 4. Reusable / reference-only components

| godot-mdk component | Verdict | Reason |
|---|---|---|
| `.BNI` directory reader (`read_textures`) | REFERENCE ONLY | Functionally correct (16-byte `{name[12], u32 imgOff}` records — **corroborates** our PROVEN Phase 3H layout), but ours (`bni_directory.*`) is evidence-backed and already integrated with `DataRoot`. Their version trusts watto.org spec comments; ours was verified against the original loader. |
| `.SNI` directory reader (`read_sounds`) | REFERENCE ONLY | Same story: `{name[12], u16, u16, u32 off, u32 size}` ≈ our PROVEN `{name[12], u32, blobOff, size}` 24-byte record (their two skipped u16s = our opaque u32). Corroborating, not needed. Known-broken cases (`LEVEL4S.SNI`, `LEVEL6S.SNI` "fails to load") are handled correctly by ours — those files contain `K_*` sentinel records their parser can't see. |
| RIFF/WAVE → audio sample scanner (`parse_wav`) | REFERENCE ONLY | Linear 100-byte chunk-tag scan; serviceable. Our SNI payloads are OBSERVED to be mostly RIFF — a proper bounded chunk walk is trivial to write when the audio phase arrives. |
| `COLOR_PALETTE` (74 hand-estimated colors) | DO NOT USE | Manually reverse-engineered from screenshots; ~25 entries are `# TODO` cyan placeholders. Superseded by our OBSERVED SYS_PAL/`PAL` palette bindings and the DTI s3 RGB table. |
| Texture decode (`parse_texture`) | DO NOT USE | Guesses `{u16 w, u16 h}` + raw indexed payload; works on the TRAVSPRT sprites it loads but is not a proven record format. Our `bni_image.*` decoders are byte-exact CODE-CORROBORATED. |
| Player controller (`player.gd`) | DO NOT USE (anti-pattern) | `KinematicBody.move_and_slide` + Doom-style friction + 4 corner RayCasts + `RUN_SPEED=2`/`GRAVITY=34`/`MOUSE_SENSITIVITY` constants — a hand-tuned approximation of MDK feel, not the reconstructed semantics. This is the "BAD" pattern this project exists to avoid. |
| Smoothing addon (Lawnjelly, MIT) | DO NOT VENDOR | Fixed-timestep render interpolation. Our timing model (§9) does not need it — the original's own virtual-clock machine already handles arbitrary frame rates. If interpolation is ever wanted, MIT makes it legal; the concept, not the code, is the takeaway. |
| Sound autoload (`sound.gd`) | REFERENCE ONLY | Fire-and-forget `AudioStreamPlayer`/`2D`/`3D` helpers — 50 lines of obvious Godot idiom. Useful as a pattern sketch for the future event→audio layer. |
| Menu/settings/HUD scene idioms | REFERENCE ONLY | `ConfigFile` persistence, `TextureProgress` HUD bars, focus-on-show menu pattern. Standard Godot; no MDK semantics to reuse. |
| Fonts (Anton/Inter, OFL) | NOT NEEDED | We decode the original FTI fonts (evidence-backed). |
| GOG-path auto-detection list | REFERENCE ONLY | The probe-path list idea is worth copying conceptually into future Godot-side data-root discovery. |

**Bottom line: nothing is copied.** Every subsystem godot-mdk
implemented is one we have already reconstructed with strictly stronger
evidence. Its residual value is (a) independent corroboration of the
`.BNI`/`.SNI` directory layouts, and (b) prior-art proof that MDK data
is reachable from a Godot project without conversion. Reuse category:
**REFERENCE ONLY** (license-wise it would be SAFE TO COPY under MIT —
there is simply nothing to copy).

## 5. Subsystem inventory — godot-mdk vs MDK-Native

`ABSENT` = not present in code. `PARTIAL` = present but incomplete or
approximated. "Evidence quality" is judged against our EVIDENCE_POLICY
vocabulary; godot-mdk's parser comments cite watto.org format specs, not
binary verification.

| Subsystem | godot-mdk | MDK-Native @ 3f23778 | Ahead | Action |
|---|---|---|---|---|
| Project bootstrap | PARTIAL — working Godot 3.4 project, autoloads, data-dir probe | — (no Godot project yet) | godot-mdk | Reference layout only |
| Level loading | ABSENT — `levels/3.tscn` is hand-placed cubes | PROVEN — `traversalRuntimeLoad` on real LEVEL3–8 (DTI+CMI+MTO, 19-arena LEVEL3 verified) | us | — |
| `.MTI` | ABSENT — spec comment only, never loaded | PROVEN directory + record classes + payload header | us | — |
| `.MTO` | ABSENT — spec comment only | PROVEN directory + overlay regions A/B/C + collision blob | us | — |
| `.SNI` | PARTIAL — directory + WAV scan; fails on sentinel-bearing files | PROVEN directory; payloads OBSERVED RIFF, not yet decoded | us | — |
| `.BNI` | PARTIAL — directory + guessed `{w,h}` sprite decode + hand palette | PROVEN directory + proven paletted/indexed bitmap decoders + real palettes | us | — |
| `.FTI` | ABSENT | PROVEN directory + FONTSML/FONTBIG glyph + ARROW sprite decode | us | — |
| `.CMI` | ABSENT | PROVEN directory + enemy/model table + arena script records | us | — |
| `.DTI` | ABSENT | PROVEN five-section structure + arena table + palette + grid | us | — |
| Geometry (arena visual) | ABSENT | PROVEN — shared region-C poly records + BSP submission order (`src/core/arena_render.*`, Phase 6A) | us | — |
| Textures/materials | PARTIAL — L8→RGBA via hand palette, unfiltered | PROVEN — MTI records → 0x34 material records, matlkup bank-A-then-B name resolution, NULL→0xff fallback, region-B palette | us | — |
| Models | ABSENT | PROVEN — `FUN_00428400` geometry + `FUN_004555bc`/`FUN_00455890` vertex-delta/ref-point/rigid animation driver (Phase 11A) | us | Godot presentation remains (G5) |
| Sprites | PARTIAL — BNI sprites as flat textures | PROVEN BNI/FTI sprite decode into indexed fb | us | — |
| Animations | ABSENT | PROVEN — object anim driver + records (`FUN_004555bc`/`FUN_00455890`/`FUN_00455c48`, BNI record lookup, Phase 11A) | us | Godot presentation remains |
| Collision | ABSENT — Godot physics on placeholder geometry | PROVEN — `FUN_004630d4` swept box-vs-BSP + object pass + floor probe | us | — |
| Player | PARTIAL — KinematicBody approximation | PROVEN — motion/vertical/look/camera/sniper/fire dispatch | us | — |
| Input | PARTIAL — InputMap + captured mouse | PROVEN — `FUN_00419370`/`FUN_00406f14` internal-domain merge | us | — |
| Camera | PARTIAL — pivot + FOV toggle | PROVEN — pose/basis/M1/M2/obstruction/nudge | us | — |
| Traversal | ABSENT | PROVEN — runtime, portal/trigger scans, arena streaming seams | us | — |
| Scripts/events | ABSENT | PARTIAL — tr_alcmd VM core + ~25-opcode traversal subset | us | RE tail remains |
| Enemies | ABSENT | PROVEN — spawn/placement + native gameplay runtime (gravity/collide/subtype/path/orbit/runner/command dispatch/mover incl. `FUN_004585c4` child lifecycle + `SW_H150`/`SW_SEAL`/`SW_SBONE` branches, Phase 11A–11C; G5 native CLOSED for BUILD_A) | us | Godot presentation remains (G5) |
| Weapons | PARTIAL — hitscan raycast + particle puff | PARTIAL — fire dispatch + shot-pool creation proven; **flight/damage UNKNOWN** | us | RE needed (G4) |
| UI/HUD | PARTIAL — menu skeleton + vitals textures | PARTIAL — full menu flow proven; HUD internals UNKNOWN | mixed | RE needed (G6) |
| Audio | PARTIAL — WAV→AudioStreamSample + players | PARTIAL — semantic `SoundAudioEvent`s, no backend | godot-mdk (marginally) | Ordinary coding (G6) |
| Level transitions | ABSENT | PROVEN — full campaign loop native-closed (Phase 14A: traversal→mode-5 intermission→mode-6 loader advance→mode-7 traversal-only→mode-8 terminal; `ProgressionSession` + step APIs); save + presentation remain | us | — |
| Save/load | ABSENT | PROVEN — envelope+packet model closed (Phase 14B): native reader validates all 5 real saves, header-only writer + `SaveStore`, death→`LASTGAME.SAV`→Continue loop native-closed in `ProgressionSession`; Phase 14C loads full saves into `TraversalRuntime` (all packet families + the `FUN_004321dc`/`FUN_0045a3b0` element-set rebind) and steps both local saves deterministically; Phase 14D closes the write side — `saveGameWriteFull` emits the original-compatible stream (`SAVE THMB GAME MORE PLAY DAMP CAME (AREN ALIE* FAND*)* BULL×3 SEND`) with FUN_00426738-faithful token fixups, re-parsing + re-restoring with zero authoritative loss (`--save-write-full`; THMB capture remains a host seam); see `docs/reverse-engineering/SAVE_FORMAT.md` | us | — |

### Overlapping findings (the only real overlap is formats)

- `.BNI` directory: their 16-byte `{name[12], u32}` records —
  **agreement** with our PROVEN layout.
- `.SNI` directory: their `{name[12], u16, u16, u32 off, u32 size}` —
  **agreement** with ours (`u32 @+0x0c` split as two u16s).
- `.MTI`/`.MTO` headers: watto spec comments match our OBSERVED
  envelope (`u32 = size−4`, `.MAT` internal name) — **agreement** on
  the parts they cite.
- Sprites are 8-bit indexed + palette — **agreement in kind**, but
  their palette is hand-estimated and ours is the real SYS_PAL/PAL/DTI
  provenance.
- Their note that "levels are numbered 3 to 8" matches BUILD_A's
  `LEVEL3`–`LEVEL8` corpus — **agreement**.
- No disagreements found; their parser simply stops where ours
  continues (payloads, regions, semantics).

## 6. Recommended Godot version

| Field | Decision |
|---|---|
| Version | **Godot 4.7.x** — verified locally: `4.7.2.stable.official.ed1daf0bf`, universal binary (`x86_64` + `arm64` slices), runs natively on this M4 host with no Rosetta |
| Why not godot-mdk's 3.x | Godot 3 is EOL-era; GDScript 1.0, `Spatial`/`KinematicBody`, GLES3-only. Nothing from that codebase ports mechanically anyway — it is reference-only, so its version is irrelevant. |
| Binding tech | **GDExtension (C++) via `godot-cpp`** — see §7 |
| Local install | none present before this audit; spike used the official zip extracted under `/tmp` (no global install performed) |

## 7. GDExtension vs alternatives — decision

**Decision: GDExtension (`godot-cpp`), no custom engine module, no
pure-GDScript bridge.**

- *Pure GDScript*: cannot host `mdk_core` (no C++ linkage), and
  reimplementing the core in GDScript is explicitly forbidden (it would
  fork the authoritative semantics — the exact failure mode godot-mdk
  exhibits). Eliminated.
- *Custom engine module*: would require building Godot from source and
  maintaining a forked engine build forever; buys nothing GDExtension
  lacks for this use case (we need data + object marshalling, not new
  engine internals). Eliminated for now; revisit only if a hard
  profiler/editor-integration need appears.
- *GDExtension*: designed exactly for this — load `mdk_core` code into
  the engine process as a `.dylib`, expose a small class surface to
  GDScript, keep all semantics in C++. Proven working by the §17 spike.
  godot-cpp `master` builds with CMake against API `4.7` (no SCons
  needed); pin a tagged godot-cpp release when `godot-4.7.x-stable`
  tags appear.

## 8. State ownership across the boundary

**Core owns (never crosses as mutable references):**

- `DataRoot` — read-only resolver over the user's data dir.
- `TraversalRuntime` — level bytes, parsed views, arenas, player
  subsystem states, script VM state, seam counters. Internal pointers
  (`CollisionPoly*`, `DynamicObject*`, `DtiArenaRecord*`, `unique_ptr`
  arena addresses used as tokens) are semantically meaningful inside
  the core and **must never cross the FFI**.
- `FreefallRuntime` (Phase 13A) — the FALL3D mode-2 course state:
  399-record object pool + LIFO freelist, timers, difficulty block,
  camera/zoom state, deterministic LCG. Presentation surface = the
  object snapshot (type/model/pos/anim fields) + `FreefallEvent`
  sound/grant tags; input surface = `FreefallInput` (four digital
  channels + two analog axes — semantics documented OBSERVED in
  `freefall_runtime.h`).
- `FrontendTimingState` — the reconstructed `FUN_0042fcd0` clock.
- `GameplayInputState` + bindings — the `0x4ce6e0` control-block path.
- `ProgressionSession` + `ProgressionHandoff` (Phase 13B, extended
  Phase 14A) — the orchestrator-owned cross-mode state
  (mode/course id/health/skill/ammo/rand + `victoryPhase`/
  `loaderSub`/`terminalDone`/`transitionCount`) and the transition
  APIs (`progressionFreefallHandoff`, `progressionTraversalComplete`,
  `progressionStepIntermission`/`Loader`/`Mode7`,
  `progressionEnterCinematic`/`StepCinematic`). Mode-5 tally, mode-6
  briefing pages and the mode-8 cinematic are semantic-completion
  inputs (`tallyDone`/`stageDone`/`cinematicDone`) — Godot drives the
  presentation clocks, the core owns every mode/level-id decision.
  Godot consumes the records; it never decides the route.

**Bridge owns (the seam):**

- The `TraversalRuntime` instance lifetime (a C++ object behind a
  `RefCounted` handle — never copied, never moved; arena addresses are
  tokens and must stay stable).
- Frame snapshots: POD copies of `TraversalFrameResult` (plus, later,
  enumerated dynamic-object/shots summaries) packed into Godot
  `Dictionary`/typed arrays. Snapshot data is **read-only** for Godot.
- The input adapter: Godot events → `RawGameplayInput` (§"input" of §7
  API below).
- The coordinate conversion helpers (§10) — the *only* place MDK
  coordinates become Godot coordinates.

**Godot owns:**

- Scene tree, nodes, meshes/materials built from snapshots, camera
  node, audio players, HUD controls. Everything downstream of
  snapshots/events; free to create/destroy presentation resources at
  will.

## 9. Tick / timing ownership — exact model

The original is **not** a fixed-tick engine. The reconstructed
`FUN_0042fcd0`/`FUN_0042fdc8` machine (`FrontendTimingState` +
`frontendTimingUpdate` in `frontend_machines.h`) is a virtual-clock chase:

- per rendered frame, `dtMs` from the real clock;
- `rawDelta = (nowMs − virtualMs)·120/1000` (integer, truncating);
- `smoothed` = EMA toward `rawDelta·0.25` (≈1.0 at a steady 30 Hz);
- `deltaSec = smoothed/30`;
- `frameStep = clamp(stepAccum>>2, 1, 4)` — the catch-up sub-step count.

So one gameplay frame runs per displayed frame, with `frameStep`/`smoothed`
absorbing the actual cadence — that is the original's own
frame-rate-dependent behavior, including its quirks (1–4 clamp, EMA lag,
integer truncation).

**Godot mapping (decided):**

- Drive from `_process(delta)` — Godot's per-rendered-frame callback —
  **not** `_physics_process` (its fixed 60 Hz accumulator is a different
  model that would falsify the original's variable-step behavior).
- Each `_process` call = one `step_frame(delta·1000)`; the bridge feeds
  `dtMs` into `frontendTimingUpdate` inside the core. At 60 Hz the
  machine naturally produces `smoothed≈0.5, frameStep=1`; at 144 Hz
  `smoothed≈0.208`; below 30 Hz `frameStep` climbs 2–4 — all original
  semantics, no reimplementation.
- **No render interpolation** (no Smoothing-addon-style blending). The
  original does not interpolate; every displayed frame is a distinct
  simulation state.
- Deterministic headless path is preserved unchanged: the selftests/
  `mdk-inspect` feed a fixed `dtMs` (100/3 ms) — the identical call
  sequence runs inside Godot when a test script feeds the same constant.
  Frame-rate-dependent original quirks live in the core's machine, so
  they reproduce identically under any host cadence.

## 10. Coordinate / matrix / camera contract

Observed MDK convention (`player_camera.h`, `dynamic_objects.h`):
`+X` = forward at yaw 0, `+Y` = left, `+Z` = up; angles in degrees;
positive yaw turns left (view direction sweeps `+X → +Y`); matrices are
row-major 3×4 world→camera (`t` in element `[3]`); `viewYaw = 90 − yaw`.

Godot convention: `−Z` = forward, `+X` = right, `+Y` = up; radians;
`Basis` columns are the local axes in parent space; `Camera3D` looks
down its `−Z`.

Canonical change-of-basis (a proper rotation, determinant +1 — no
mirroring):

```text
v_godot = (−v_mdk.y, v_mdk.z, −v_mdk.x)          // position & vectors
yaw_godot_deg = yaw_mdk_deg                       // same sign (both
                                                  //   turn left)
Basis columns for a camera (from M2 snapshot rows right|down|back):
  X = P·right,  Y = −(P·down),  Z = P·back       // orthonormalized
origin = P·camPos
World scale: 1 MDK unit = 1 Godot unit (player box half-extents
  {0.6,0.6,2.5} → ~5-unit-tall character — plausible as-is).
```

**Placement decision: the conversion lives inside the C++ bridge**, so
GDScript never sees MDK coordinates and no sign flips/axis swaps can
accumulate in scene scripts. Helpers (`mdkToGodotPosition/Basis/Camera`)
are the single implementation; the spike verified them against
`mdk-inspect` ground truth (§17).

Projection: the original stores no FOV angle — only the folded scale
pair `scaleX = 1/(zoom·0.5)`, `scaleY = 1/(zoom·(H/W)·0.5)`. The bridge
exposes the scales; the presentation side derives the equivalent
`Camera3D.fov` (vertical `fov = 2·atan(1/scaleY_eff)`-style mapping, to
be finalized when the renderer boundary is exercised) — presentation
detail, not semantics.

## 11. Collision / physics authority — policy

**Godot physics is never authoritative for gameplay.** Specifically:

- No `CharacterBody3D`/`RigidBody3D`/`move_and_slide`/`move_and_collide`
  in the gameplay path. The player is a plain `Node3D` positioned from
  snapshots.
- No `RayCast3D`/`ShapeCast`/`Area3D` queries may feed simulation
  inputs. All gameplay collision answers come from `collision_query.*`
  (`FUN_004630d4` sweep, `FUN_00435eec` floor probe, object AABB pass).
- Permitted non-authoritative uses (clearly marked, never read back
  into state): editor/debug visualization (collision-wireframe
  overlay), mouse picking for dev tools/inspectors, cosmetic VFX
  placement tests.
- The original's own quirks (the `0.75/0.5` slide scales, `+0.01/+0.5`
  lift margins, the `9.99` landing hover) are reproduced in the core;
  approximating them in Godot physics would silently falsify them.

## 12. Asset / render path

**Asset interface decision: option A — Godot requests decoded data from
the C++ parsers through the bridge.** (B = offline conversion; C =
custom `ResourceFormatLoader`.)

- Decode authority stays in `mdk_core` where the evidence lives;
  conversion pipelines (B) would fork the format knowledge and could
  drift; `ResourceFormatLoader` (C) is a viable *later* convenience for
  editor inspection but adds importer complexity for no gameplay
  benefit — revisit after G1.
- Concretely: level load → bridge exposes decoded surfaces — indexed
  pixel buffers + palette → `Image`/`ImageTexture` (nearest filter),
  mesh vertex/index data → `ArrayMesh`/`ImmediateMesh`, WAV bytes →
  `AudioStreamWAV`. All materialized in-memory; the data root stays
  read-only and nothing proprietary is written to `res://`.
- Smallest faithful render target: unshaded materials
  (`BaseMaterial3D` `shading_mode=unshaded`, `texture_filter=nearest`),
  palette-expanded RGBA8 textures, original geometry once its visual
  format is reconstructed, sprites as camera-facing quads or in the HUD
  layer, 4:3 logical view (600×360 content in a 640×480 canvas per the
  OBSERVED present copy) letterboxed inside the window.
- Explicitly avoided for now: PBR relighting, texture upscaling,
  remaster shaders, redesigned assets, engine lighting redesign. The
  original was a software renderer; the faithful target is "the same
  pixels, at native resolution," not a relit scene. (The OBSERVED
  modular-renderer evidence — software/D3D/Glide/SGL/Vérité variants
  sharing one engine image — supports "renderer behind an abstraction";
  Godot is simply another backend.)
- Open RE dependency: the arena *visual* geometry/material path
  (`draw_arena`/`FUN_00431300`, texture mapping, lighting model) is
  UNKNOWN — that reconstruction is a prerequisite inside phase G1
  (§19).

## 13. Audio / UI responsibility

- Audio: Godot `AudioStreamPlayer`/`AudioStreamPlayer3D` driven by
  presentation events from the core (`SoundAudioEvent` already exists
  on the menu side; traversal fire/notify seams are counted today —
  including the object-VM voice rebind op `0x6b`, counted in
  `seams.fireSoundCalls`; the `FUN_0042f310` X_STRIKB/
  X_STRIKD held-bomb prop manager is likewise a deferred presentation
  seam — the weapon-5 gameplay spawn `FUN_0045a4dc` itself is ported).
  Object-VM op `0x6d` is **not** audio/presentation — it is
  `FUN_00467888` player damage (`FUN_0046771c` body-identical),
  fully ported in the native core.
  SNI payloads are OBSERVED mostly RIFF/WAVE — decode is ordinary
  coding, no RE risk. Positional audio must not acquire gameplay
  meaning (attenuation is presentation).
- UI/HUD/menus: Godot `Control` tree renders decoded FTI/BNI resources
  (fonts `FONTSML`/`FONTBIG`, `ARROW` cursor, HUD sprites) positioned
  per the reconstructed layout constants. The *menu logic* stays in
  `mdk_core` (`frontend_*` controllers are proven state machines) —
  Godot draws their output; it does not reimplement their transitions.
  Mouse-menu semantics (hit-test bands, axis letters) are core-owned.

## 14. Threading constraints

- `mdk_core` is single-threaded; `DataRoot` builds its per-directory
  indexes lazily and is not thread-safe. **All bridge calls occur on
  Godot's scene/main thread** (GDExtension calls from `_process`/
  `_ready`/signals are main-thread). No `WorkerThreadPool` use against
  the bridge, ever.
- Snapshots are POD copies, so presentation code may read them freely
  after the call returns; nothing in the snapshot aliases core memory.
- If level loads ever need backgrounding, the load must still run on a
  thread that exclusively owns the runtime during the call, with
  results published to the main thread — deferred optimization, not
  needed for the plan.

## 15. Deterministic / headless regression strategy

Unchanged and mandatory:

- `mdk_tests` (CTest) — `mdk_core` unit tests; Godot work must not
  touch them.
- `python3 -m pytest tests/` — inventory/manifest tests.
- `mdk-inspect` selftests + the traversal FNV-1a digest (folds frame
  outputs incl. camera pose f32 bit patterns) + the freefall
  `--freefall-runtime` digest (Phase 13A — folds rng/timeline/health
  + per-object type/timer/pos bits).
- The SDL3 app + all `--selftest-*` diagnostics — the SDL/Metal/native
  path remains a first-class headless host. It is not deprecated,
  subordinated, or "legacy."

Added by the Godot path (when G0 lands): a headless extension test —
`godot --headless --rendering-driver dummy` project that steps fixed
`dtMs` frames and recomputes the same digest over the snapshot stream.
Identical input sequence + identical `dtMs` must yield identical
digests in both hosts; that is the regression tripwire proving the
bridge perturbs nothing.

## 16. Repository layout — decision

**`frontend/godot/` inside this repository** (task option C). Rejected:
a sibling repository (would vendor or submodule `mdk_core` — either
drifts; this project must never host two gameplay implementations) and
repo-root placement (pollutes the native app root).

```text
frontend/godot/
  extension/            C++ GDExtension sources (bridge only —
                        no gameplay logic lives here)
    CMakeLists.txt      optional target MDK_BUILD_GODOT_EXT=OFF default
    mdk_bridge.*        the seam: input adapter + snapshots + conversion
  project.godot         Godot 4 project
  mdk_bridge.gdextension
  scenes/ scripts/      presentation only
  bin/                  built .dylib (gitignored)
  .godot/               editor cache (gitignored)
```

The extension links `libmdk_core.a` from the existing build (all
objects are position-independent on arm64 macOS — verified by the
spike link). `frontend/godot/bin`, `.godot/`, `*.import`, and the
`godot-cpp` checkout are gitignored; `godot-cpp` is fetched by the
build, never vendored.

## 17. Feasibility spike — performed and passing

Disposable project at `/tmp/mdk-godot-spike` (not committed; recipe
recorded here):

1. Godot `4.7.2-stable` official macOS universal zip → runs arm64
   slice natively.
2. `godot-cpp` master (`GODOTCPP_API_VERSION=4.7`, `GODOTCPP_TARGET=
   editor`) built via CMake → `libgodot-cpp.macos.editor.arm64.a`.
3. `MdkBridge` (`RefCounted`) — `initialize(dataRoot)` /
   `load_level("TRAVERSE/LEVEL3/LEVEL3.DTI")` / `step_frame(dtMs)` →
   `Dictionary{player_pos: Vector3, yaw_deg, grounded, camera:
   Transform3D, arena}`; coordinate conversion per §10, inside the
   extension.
4. Linked: `libmdk_core.a` (Phase 5N) + godot-cpp →
   `libmdkbridge.dylib` (Mach-O arm64). **Zero changes to MDK-Native
   sources.**

Result (`godot --headless --rendering-driver dummy`):

```text
MdkBridge: level loaded, arenas=19
f=0 arena=0 pos=(0.0, 190.0, 4.0) yaw=96.00 grounded=true
    cam=(7.924682, 195.058, 3.167082)
…
spike: OK — mdk_core ran inside Godot 4 GDExtension
```

Cross-check vs `mdk-inspect --traversal-runtime` on the same level:
spawn `(-4, 0, 190) yaw=96` → Godot `(0,190,4)`; camera `(-3.17,-7.92,
195.06)` → Godot `(7.92,195.06,3.17)`. **Exact match under the §10
transform.** 19 arenas, `grounded=true` — the full
DataRoot→DTI/CMI/MTO→collision→traversal-step→snapshot chain runs
inside the engine process. Player/camera transforms were applied to a
real `Node3D` marker and `Camera3D` (headless — asserted numerically,
not visually).

Findings / gotchas recorded for G0:

- The headless *editor* binary crashes in this environment inside
  MoltenVK SPIRV→MSL shader conversion (SIGSEGV) — run projects with
  `--headless --rendering-driver dummy`, and treat editor launches as
  an environment quirk to revisit; headless *game* mode is unaffected.
- godot-cpp `InitObject` now takes a non-const
  `GDExtensionClassLibraryPtr` (needs a `const_cast` at the entry
  point); godot-cpp has no `4.7` release tag yet — `master` + API 4.7
  works, pin a tag when published.
- macOS arm64 needs no `-fPIC` gymnastics: the existing `mdk_core`
  archive linked straight into the `.dylib`.
- Rosetta status: not installed; the extension is arm64-only and
  loaded — the chain is provably native.

## 18. Risks

| Risk | Assessment |
|---|---|
| Arena visual geometry/material format — RESOLVED (Phase 6A/6B: poly records, material pipeline, BSP submission order proven painter's back-to-front via `DAT_00499f8c`=1; `src/core/arena_render.*` + `mdk-inspect --arena-render`) | Closed. Remaining P1 fidelity items for G1: effect-drawer semantics (fx770/fx12970/fxe94), `|4` span-variant role, texture-anim frame selection — none block rendering static arenas; see `reverse-engineering/ARENA_RENDER_PIPELINE.md` census. |
| Animation format UNKNOWN | Models parse; how they animate is unreconstructed — gates enemies/player mesh (G5). |
| Enemy AI / projectile flight / damage UNKNOWN | Gates G4/G5. Pure RE; Godot cannot shortcut it. |
| godot-cpp 4.7 tag absent | Pin `master` commit hash in the build docs; switch to tag when released. Low risk. |
| Headless editor MoltenVK crash | Blocks *editor* use headless on this host; game-mode headless fine; interactive editor on real display untested in this audit. Low risk but needs a check in G0. |
| Godot upgrade churn | GDExtension API is stable within 4.x; the bridge surface is tiny. Low. |
| Scope creep toward a "remake" | Mitigated by §1 policy + the non-goals (§20). |
| macOS packaging/signing | Unexplored (G8). Standard export-template path expected to work; not validated here. |

## 19. Migration phases (proposed — NOT started)

`RE` marks packages that require new reverse-engineering evidence
before/within them. Those are the ones that matter for the
2026-10-10 unlimited-availability horizon.

| Phase | Deliverable | Depends on | New RE? | Effort type |
|---|---|---|---|---|
| **G0** — frontend skeleton | `frontend/godot/` project + `mdk_bridge` GDExtension target (versioned API: `initialize`/`load_level`/`step_frame`/`shutdown`), input adapter (Godot keycodes→internal 0..127 table), snapshot dict, headless digest test | this audit; godot-cpp pin | no | ordinary coding (spike already proves it) |
| **G1** — level geometry + camera | `ArenaRenderData` (Phase 6A — DONE: decode + submission order in `mdk_core`) → Godot `ArrayMesh`/`MeshInstance3D` per arena consuming the ordered snapshot; camera node driven by pose snapshot; palette-index materials → 8bpp/RGBA expansion, original-look unshaded | G0 | **PARTIAL** — pipeline proven in Phase 6A; remaining RE tail = occlusion question + effect-drawer semantics (P0/P1 in ARENA_RENDER_PIPELINE.md) | coding + bounded RE |
| **G2** — player presentation — **DONE** | Node3D player driven by traversal snapshots; move/jump/collision all core-side; debug wireframe of the proven collision world | G0 (+G1 for real geometry) | no (movement/collision already proven) | ordinary coding |
| **G3** — dynamic objects / doors / portals — **DONE** | Object snapshot enumeration (opaque id, model, transform, AABB) + real RuntimeModel geometry; mover/door presentation; portal/arena display-set transitions | G1, G2 | no for known types (5E/5I proven); unknown record types 5/8 remain UNKNOWN | mostly coding, small RE tail |
| **G4** — combat presentation | Shot-pool snapshot → projectile visuals; fire/impact sounds; hit feedback | G2 | **YES — projectile flight + damage/death internals** | RE + coding |
| **G5** — enemies + AI | Enemy spawn→visual models, animation, AI behavior driven by core | G1, G3 | **NO for native runtime** — Phase 11A–11C reconstruct the full native enemy/mover gameplay core incl. `FUN_004585c4` (CLOSED FOR BUILD_A); remaining work is Godot presentation only | ordinary coding |
| **G6** — audio / HUD / menus | SNI WAV playback via events; HUD from decoded FTI/BNI; menu screens re-skinned by Godot with logic still in core | G2 | partial — HUD layout details UNKNOWN | coding + bounded RE |
| **G7** — progression / QA | Level transitions, save/load surface, full-game playthrough oracle-vs-native comparison | G1–G6 | partial — full campaign loop CLOSED natively (Phase 14A: mode-5 intermission, mode-6 advance, mode-7, mode-8 terminal); save envelope + death→LASTGAME→Continue CLOSED natively (Phase 14B); full-save LOAD+WRITE native-closed on the local corpus (Phases 14C/14D); mode-5 statistics + presentation seams (save-slot UI, THMB capture) remain | coding + bounded RE |
| **G8** — macOS arm64 packaging | Export/packaging, icon/signing/notarization as applicable | G7 | no | ordinary coding |
| **G9** — freefall presentation | FALL3D course visuals driven by `FreefallRuntime` snapshots: object models (Kurt/missile/radar/pickup+chute/bones/EXPLODE), `cameraPos`+zoom state → camera node, `FreefallEvent` sound/grant tags → audio+HUD seams, FALLPU grant events → inventory surface | G0 (+G1 for model plumbing) | **NO for native runtime** — Phase 13A reconstructs the full mode-2 core incl. difficulty scaling and death/completion; remaining work is Godot presentation only | ordinary coding |

The RE-heavy items (G1 arena visuals, G4 combat tail, plus bosses,
save semantics, HUD layout, remaining tr_alcmd opcodes) are the work
to schedule inside the current availability window; G5 is
presentation-only after Phase 11C, the freefall core is reconstructed
after Phase 13A (G9 is presentation-only), and the other
presentation-only packages are deferrable ordinary coding.

## 20. Explicit non-goals

- No gameplay migration to Godot — ever, under this architecture.
- No `CharacterBody3D`/`RayCast3D` authority (§11).
- No GDScript reimplementation of reconstructed semantics.
- No engine fork / custom module build (§7).
- No vendoring of godot-mdk, godot-cpp, the Smoothing addon, or fonts
  into this repository.
- No writes into the original data root; no proprietary data in the
  Godot project.
- No PBR remaster look, texture upscaling, modern lighting redesign.
- No change to the SDL3/Metal/headless native path, `mdk-inspect`, or
  `mdk_tests` — they remain first-class.
- No Phase 5O or any new RE performed *by* this audit.
