// Phase 7-9 — retained GDExtension bridge into mdk_core.
//
// The C++ core stays authoritative: parsing, ArenaRenderData,
// painter order, camera state, dynamic-object state. This class
// owns a TraversalRuntime, keeps every file buffer that render
// data aliases alive for as long as snapshots are reachable, and
// converts to Godot-space values inside C++ (see mdk_math.h /
// mdk_convert.h — no axis math exists in GDScript).
//
// G3 presentation model:
//   * Arena geometry is presented for the traversal DISPLAY SET —
//     the arenas the reconstructed view path touches: the current
//     arena plus the active partner (rt.cur / rt.partner when
//     rt.partnerActive). Each arena's render/collision bundle lives
//     in an ArenaSet keyed by arena index.
//   * Dynamic objects are presented from the same view set (a
//     geometry-less corridor still shows its objects — e.g. the
//     CHMO_2 XCORDOOR door). Objects get opaque uint64 IDs
//     (mdk_objid.h) — never pointers — stable across frames and
//     arena transfers, dead after despawn.
//   * Every snapshot is copy-out: Dictionaries/Arrays of Godot
//     value types; no C++ aliasing escapes to GDScript.
//
// All methods are designed for Godot's main/scene thread — no
// background work, no shared mutable state.
#pragma once

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/arena_mesh.h"
#include "core/arena_render.h"
#include "core/bni_directory.h"
#include "core/collision_query.h"
#include "core/data_root.h"
#include "core/freefall_runtime.h"
#include "core/freefall_scene.h"
#include "core/frontend_host.h"
#include "core/frontend_machines.h"
#include "core/frontend_resources.h"
#include "core/frontend_shell.h"
#include "core/fti_sprite.h"
#include "core/gameplay_input.h"
#include "core/progression_runtime.h"
#include "core/sni_directory.h"
#include "core/sni_wave.h"
#include "core/traversal_audio_mixer.h"
#include "core/traversal_runtime.h"
#include "core/player_projectiles.h"

#include "frontend_presenter.h"
#include "mdk_objid.h"

namespace godot {

class MdkBridge : public RefCounted {
  GDCLASS(MdkBridge, RefCounted)

 protected:
  static void _bind_methods();

 public:
  MdkBridge() = default;
  ~MdkBridge() override { shutdown(); }

  // Lifecycle — see docs/GODOT_FRONTEND.md.
  bool initialize(const String& data_root_path);
  bool load_level(const String& dti_rel_path);
  // "" resolves to the current (spawn) arena. Ensures that arena's
  // render set exists and shows it until the next stepped frame
  // recomputes the core-driven display set.
  bool load_arena(const String& arena_name);
  // One traversal frame; `action_mask` sets the QA keyboard actions
  // (MdkInputAction bits) — 0 reproduces idle input.
  Dictionary step_frame(double dt_ms, int64_t action_mask);
  // One traversal frame driven by a raw-input dictionary — the
  // neutral G2 input path. Keys (all optional):
  //   "actions"       — MdkInputAction QA mask, same as step_frame's
  //   "keys"          — PackedInt32Array/Array of held internal key
  //                     codes (0..127, the original's key domain)
  //   "mouse_dx"/"mouse_dy"/"mouse_dz" — int per-frame device deltas
  //                     (the DIMOUSESTATE accumulators)
  //   "mouse_buttons" — int nibble, bit i = physical button i held
  // The dictionary only carries device state: the configured axis
  // map, button action masks, and scales live in mdk_core bindings —
  // no gameplay semantics exist on the Godot side.
  Dictionary step_frame_input(double dt_ms, const Dictionary& input);
  Dictionary get_player_snapshot() const;
  Dictionary get_camera_snapshot() const;
  // Phase 16B — the authoritative traversal-Kurt sprite snapshot:
  // the draw gate, blit anchor, scope y-offset, scale, clip flags,
  // main/overlay frame identity (+ dims/hotspot/table name), jitter
  // offsets, muzzle index, and lazily-built palette-expanded
  // ImageTextures. Textures are cached by {table, frame, palette
  // digest} and reused across frames — nothing here recomputes any
  // animation semantics; every field is a verbatim core output.
  Dictionary get_kurt_snapshot();
  // Collision-world debug data for the PRIMARY displayed arena:
  // poly edge line soup (pairs of points, PRIMITIVE_LINES) +
  // counts. All positions already Godot-space. For every displayed
  // arena's soup see get_arena_render_snapshots() "collision_lines".
  Dictionary get_collision_snapshot();
  // The live configured input bindings (factory block — BUILD_A's
  // MDK.CFG carries no overrides): mouse axis letters, scales, and
  // per-button action masks, for QA/debug display.
  Dictionary get_input_config() const;
  // BSP-order digest for the PRIMARY displayed arena — kept for
  // compatibility; prefer get_display_digest() (covers set changes).
  int64_t get_arena_order_digest();
  // Retained snapshot for the PRIMARY displayed arena — same dict
  // shape as each entry of get_arena_render_snapshots().
  Dictionary get_arena_render_snapshot();
  Array get_arena_names() const;
  bool is_level_loaded() const { return rt_ != nullptr; }
  bool is_arena_loaded() const { return arenaLoaded_; }
  String get_last_error() const { return lastError_.c_str(); }
  void shutdown();

  // --- Phase 9 (G3) — objects + display set ---------------------
  // Per-arena render snapshots for the whole display set, ordered
  // current-first. Each dict carries the G1 fields (mesh, material,
  // positions, uvs, matdesc, poly_order, palette, stats, atlas)
  // plus "role" ("current"/"partner") and "collision_lines".
  Array get_arena_render_snapshots();
  // Traversal-view state: cur/partner indices + names, partner
  // active flag, view-on-partner, last-frame swap/portal fields,
  // counters, the presented arena set, and the object-view set.
  Dictionary get_display_snapshot();
  // Digest over the display set's arena indices + per-arena BSP
  // orders — poll it; rebuild presentation when it changes.
  int64_t get_display_digest();
  // Copy-safe snapshots for every DynamicObject in the object-view
  // set (cur + active partner). Fields are documented in
  // docs/GODOT_FRONTEND.md. "id" is an opaque uint64 — NOT an
  // address — stable across frames and arena transfers.
  Array get_object_snapshots();
  // Immutable local model geometry for one snapshot id: mesh (one
  // surface per element), surface_elems, elem_names, vert/tri/
  // element counts, geom_key. Empty dict for a stale/unknown id.
  Dictionary get_object_geometry(int64_t object_id);
  // NATIVE DIAGNOSTIC — wraps traversalRuntimeDiagnosticStart:
  // re-anchors the player at pos_mdk/yaw inside arena_index.
  // Test/QA path only; not original behavior.
  Dictionary diagnostic_start(int64_t arena_index,
                              const Vector3& pos_mdk,
                              double yaw_deg);
  // NATIVE DIAGNOSTIC — calls the authentic core damage producer
  // (playerDamageApply = FUN_0046771c). The dispatcher consumes the
  // accumulator on the next stepped frame — nothing here writes
  // loco/anim state directly. Test/QA path only.
  Dictionary diagnostic_damage(int64_t amount);
  // NATIVE DIAGNOSTIC — runs the authentic FUN_00458140 death
  // boundary on a live snapshot id (objectDieFacingPlayer — the same
  // entry the shot/punch kill paths reach). Emits the real
  // kObjectDeathScript / kObjectTeardown event; the teardown object
  // record is wiped in place as usual. Test/QA path only.
  Dictionary diagnostic_kill(int64_t object_id);
  // NATIVE DIAGNOSTIC — calls the real FUN_004575fc remnant seam
  // (fxShockwave, the same function the script opcodes dispatch to)
  // with the observed 2.0 scale arg. Emits a kDetonation event.
  // Test/QA path only.
  Dictionary diagnostic_shockwave(int64_t object_id);

  // --- Phase 17A closeout — full save/restore -------------------
  // Serializes the live traversal session through the original
  // full-save stream (core/save_full_write.h — SAVE/THMB/GAME/MORE/
  // PLAY/DAMP/CAME/AREN/ALIE/FAND/BULL x3/SEND). Copy-out only;
  // empty when no traversal session is live. No Godot presentation
  // state participates — combatFx/shard/remnant nodes are transient
  // by design and are never serialized.
  PackedByteArray save_game_full();
  // Restores a full save into the live session via
  // applyFullSaveToTraversal (FUN_00427218 — the same path
  // mdk-inspect --save-restore and the golden tests run): parses,
  // rebuilds a fresh TraversalRuntime in place (level resolved from
  // the save's own level id), rebinds arena/object/CMI references,
  // and returns a restore report dict. On success the previous
  // runtime — and every presentation structure derived from it —
  // is dead; GDScript rebuilds from the first post-restore
  // snapshot. Header-only saves (no MORE packet) are rejected.
  Dictionary restore_save(const PackedByteArray& bytes);

  // --- Phase 18B.1 — frontend host services -------------------------
  // The presentation-neutral shell (mdk::FrontendShell — Phase 18A)
  // plus the host seams it needs (mdk::FrontendHostServices): the
  // writable save root, the LASTGAME probe, slot inspect/enumerate,
  // save writes routed through the existing writers, and the
  // MISC\MDKS_* slide probe. Presentation draws menus from
  // frontend_snapshot() and executes nothing itself — host-ownable
  // requests are dispatched here, presentation-ownable ones stay in
  // the drained queue for the app.
  //
  // `save_dir` is the writable save root — the caller picks it
  // (production: the runtime's SAVES dir; tests: a temp dir). The
  // data root must already be open (initialize()); it is used
  // read-only for MISC\MDKS_*.
  bool frontend_boot(const String& save_dir);
  bool frontend_booted() const;
  // FUN_0041d85c — frontend entry. `returning` is the original's arg
  // (0 = fresh boot entry, 1 = returning from gameplay).
  void frontend_enter(bool returning);
  Dictionary frontend_snapshot() const;
  // One frame. `input` keys mirror mdk::FrontendMenuInput
  // (prev/next/confirm/attract/left/right/cancel as bools; "typed" +
  // key edges as ints; "mouse_dx/dy/dz"/"mouse_buttons" as ints;
  // "raw_edges" PackedInt32Array of four ints — the four raw edge
  // bytes). Missing keys = inactive.
  Dictionary frontend_update(const Dictionary& input);
  // The end-of-frame ramp/timing update + the OBSERVED idle/attract
  // cadence — call once per rendered frame after frontend_update.
  void frontend_end_frame(double dt_ms);
  // Drained queues: requests as Dictionaries
  // {"request","name","header_only"}; fx as ints (mdk::FrontendFx).
  Array frontend_drain_requests();
  Array frontend_drain_fx();
  // Host seams (the same functions the shell's seams call):
  bool frontend_lastgame_exists();
  Array frontend_enumerate_saves();
  Dictionary frontend_inspect_slot(const String& stem);
  // Direct write — {"name": stem, "header_only": bool}. Full saves
  // serialize from the live traversal session through
  // saveGameWriteFull; header-only through saveGameWriteHeaderOnly
  // with the session's checkpoint fields. Equivalent to what the
  // shell's write seam invokes when armed.
  bool frontend_write_save(const Dictionary& request);
  // The OBSERVED MISC\MDKS_%03d.GIF attract probe — the dictionary
  // carries exists/path/dims/bytes (exists = the 600x360 gate
  // verdict); raw bytes come separately for the caller's decoder.
  Dictionary frontend_slide_probe(int64_t index);
  PackedByteArray frontend_slide_data(int64_t index);
  // The transition seam: presentation calls this when the transition
  // the TransitionArmed fx requested has played — clears the
  // Esc-abort suppression + the idle/attract blend gate
  // (FUN_0041ebf4's clear point).
  void frontend_transition_complete();
  void frontend_notify_load_result(bool ok);
  // Executes every drained request the host owns — LoadSave /
  // ContinueLastGame (envelope parse + restore),
  // AbortToFrontend (gameplay teardown + fresh frontend entry),
  // StartNewGame (campaign start), ResumeTraversal + WriteSaveDone
  // (no-ops for the bridge — the modes resume on their own frames).
  // Returns a per-request report Array of Dictionaries
  // {"request","handled","owner","ok"} — presentation/app-ownable
  // requests (Quit, CycleBrightness, CaptureUtility,
  // OpenLegacyScreen) report handled=false with the owner tag.
  Array frontend_dispatch_requests();
  // --- Phase 18B.2A — frontend presentation ----------------------
  // Composes the authoritative shell state into the shared core
  // renderers (mdkbridge::FrontendPresenter) and returns the frame:
  //   "w"/"h"     — 600x360
  //   "rgba"      — PackedByteArray RGBA8 (600*360*4)
  //   "thmb"      — {"x","y","w","h","rgba"} existing-save THMB
  //                 overlay, only when the selected slot carries the
  //                 3648-byte record (decode-only; no new capture)
  //   "screen"    — diagnostic tag for the composed surface
  // Requires frontend_boot() (which loads FrontendResources).
  Dictionary frontend_frame();
  // Progression pump for the modes that have no runtime of their own
  // (5 intermission / 6 loader / 7 traversal-only entry / 8
  // cinematic). `input` carries the presentation seam:
  //   "stage_done" / "confirm" — the tally/briefing/cinematic
  //   finished this frame (the original's stage-complete input).
  // Returns {"pumped","error","mode","sess_mode","level_id","ok"} —
  // on a kOk transition the bridge installs the next runtime itself
  // (freefall scene+init for mode 2, the traversal load for mode 3,
  // frontend re-entry for mode 0) so the caller just keeps pumping
  // until mode_ lands on a presented mode.
  Dictionary frontend_progression_step(const Dictionary& input);

  // --- Phase 17A — traversal combat presentation ----------------
  // All of this is copy-out presentation state produced by the core
  // combat system (playerShotVisuals + TraversalRuntime::combatFx).
  // Nothing here mutates gameplay.

  // The 3-slot player-shot pool. Top-level dict:
  //   "scoped"     — playerShotRenderGate (flagC9c && phase>1)
  //   "hud_active" — 0x5414d4 HUD gate
  //   "shots"      — Array of 3 dicts, each:
  //     slot, state, type, class_idx, mesh_renderable (state==1
  //     world-mesh gate), window_active (state!=0 && lifetime>0 —
  //     the bullet-cam gate), pos/pos_mdk, tail/tail_mdk, tail_len,
  //     yaw_deg, pitch_deg, billboard_yaw_deg, billboard_pitch_deg,
  //     spin_deg, render_scalar (+0xcc), ribbon_bound, hud_frame,
  //     arena_index, and cam_transform — the FUN_0045e9a0 bullet-cam
  //     pose (origin = tail, basis from the billboard yaw/pitch)
  //     converted to Godot space.
  Dictionary get_shot_snapshots() const;

  // Drains TraversalRuntime::combatFx — the core accumulates; each
  // event reaches GDScript exactly once. Dict fields: kind (the
  // CombatFxKind int), mode, variant, aux, pos/pos_mdk, obj_id
  // (opaque; 0 when none — do NOT expect it to resolve for a
  // torn-down object), scale, facing_deg, bank_deg, model_name.
  Array drain_combat_fx();

  // The shot's bound model: class_idx < 0 -> the built-in slot-1
  // record (STREAM.BNI "KURT"); >= 0 -> the level enemy-table model.
  // Same dict shape as get_object_geometry. Empty when unresolvable.
  Dictionary get_shot_geometry(int64_t class_idx);

  // A level enemy-table model by name — the class-record identity
  // the corpse/remnant binds ("EXPLODE"). Same dict shape as
  // get_object_geometry. Empty when the name is not in the table.
  Dictionary get_named_geometry(const String& name);

  // --- Phase 17C.2 — traversal audio presentation ----------------
  // Drains TraversalRuntime::audioFx through the C++ voice pool
  // (TraversalAudioMixer — the BUILD_A instance-pool semantics) and
  // returns the player command batch for the GDScript presenter.
  // Each dict: "op" ("start"/"params"/"stop"), "id" (voice slot
  // 0..62), "name"; start adds "stream" (Ref<AudioStreamWAV>) and
  // "loop"; start/params add "db" (Godot volume_db), "pan"
  // (AudioEffectPanner -1..1), "pitch" (pitch_scale). The event
  // stream is consumed exactly once — repeated calls see only new
  // work, like drain_combat_fx.
  Array drain_audio_fx();
  // Development counters: resolved/missing records, stream cache
  // size, active voice count, pool-exhaustion count.
  Dictionary get_audio_stats() const;

  // The active display palette (768 RGB bytes — the palette the
  // shard colors and HUD indicator fills index into).
  PackedByteArray get_active_palette();

  // FUN_00406a0c — the shard tick's contact stab: `from`->`to` in
  // Godot space against the record-arena BSP (arena_index, from the
  // event's "arena_index"), falling back to the displayed current
  // arena then the active partner (0x540c48/0x540ca4 order, gated on
  // !carrierBusy — 0x540d3c, OBSERVED). Returns {} on a miss, else
  // {"pos", "normal", "arena_index"} — the crossing point and the
  // hit node's split-plane normal, both Godot space. Presentation
  // query only — reads collision, writes nothing.
  Dictionary fx_stab(const Vector3& from, const Vector3& to,
                     int64_t arena_index);

  // --- Phase 17B.2 — traversal HUD / view presentation ----------
  // The already-composed core HUD (FUN_00436d60's tail, run per
  // frame inside stepTraversalRuntime — see core/traversal_hud.h)
  // copied out for the frontend: pen indices + palette-mapped
  // ImageTextures + the view/scope gates. Nothing here mutates the
  // runtime; no HUD semantics exist on the GDScript side.
  //
  // Returns {} unless mode_ == 3, a frame has been stepped, and the
  // HUD bound (traversalHudBind ran). Fields:
  //   "w"/"h"        — 600x360 (fb dims)
  //   "fb"           — PackedByteArray, the composed pen indices
  //                    (216000 bytes; pen 0 = transparent)
  //   "nz"           — nonzero pen count (diagnostic)
  //   "digest"       — fnv1a64 over the pen bytes — the SAME fold
  //                    mdk-inspect's `hud: ... dg=` prints, so a
  //                    given runtime state cross-checks directly
  //   "pal_key"      — fnv over the active display palette (the
  //                    768-byte pick get_active_palette returns)
  //   "tex"          — persistent palette-expanded RGBA8
  //                    ImageTexture; pen 0 -> alpha 0. Re-created
  //                    only on (content|palette) change — the same
  //                    object identity is returned every call.
  //   "scoped"       — playerShotRenderGate (c9c && ca0>1): the
  //                    scope gate that also runs the shot windows
  //   "sniper_view"  — c9c && ca0!=0: the camera's alt-rect gate
  //                    (ce.sniperViewport — transition phase 1
  //                    already adopts the aperture pose)
  //   "hud_active"   — 0x5414d4
  //   "view_rect"    — the camera pose's authored rect
  //                    (0,0,600,360 / 107,79,384,280 — the
  //                    viewport-register form)
  //   "scope_rect"   — the fb-space scope aperture constants
  //                    (108,80,384,280 — kHudScope*)
  //   "win_fill"     — PackedInt32Array[3]: per-slot hudFrame
  //                    (the FUN_0045ee7c mode-1 shotWinFill channel)
  //   "bezel"        — SNIPERS1 dict: "w","h" (640x480), "px" (the
  //                    raw indexed buffer), "key" (fnv of px),
  //                    "tex" (persistent opaque RGBA texture),
  //                    "fb_ofs" (Vector2i — the (20,55) fb-in-bezel
  //                    HYPOTHESIS offset, see traversal_hud.h)
  //   Scalar echoes for diagnostics/tests only (verbatim fields —
  //   the composed fb above remains the authoritative visual):
  //   "health" "field_dac" "field_eb8" "loco_state" "wpn0" "wpn1"
  //   "ammo" (PInt32[6]) "inv_count" "inv_sel" "inv_timer"
  //   "timer" "timer_max" "timer_latch" "level_id"
  Dictionary get_hud_snapshot();

  // --- Phase 16C — freefall (mode 2) ------------------------------
  // FUN_0040ef28's domain: loads the FALL3D course bundle (BNI +
  // FALL3D_<course+1>.MTI + FALLP_<course+1> palette + FALLPU pickups)
  // into a FreefallScene, inits FreefallRuntime, and arms the
  // progression session so the freefall-exit handoff routes to
  // TRAVERSE/LEVEL<table[course]> exactly like the campaign path.
  // `skill` is 0..2 (54147a); `seed` loads the shared LCG state.
  bool load_freefall(int64_t course, int64_t skill, int64_t seed);
  // The bridge's active mode — the 0x541492 values: 0 = nothing
  // loaded (frontend), 2 = freefall, 3 = traversal.
  int64_t get_mode() const { return mode_; }
  // Mode-2 frame state — verbatim FreefallRuntime fields (phase,
  // intro countdown/progress, zoom sprite indices, timeline, health,
  // fade accumulator + target, radar/pickup/missile timers, the
  // camera block 0x4ce69c..0x540b28) plus the FUN_004123f4 camera
  // converted for Godot. The palette fade reaches GDScript as raw
  // `fade` (1.0 = full bright, 0 = black, >1 = the damage flash).
  Dictionary get_freefall_snapshot();
  // Copy-safe snapshots for every object on the active list
  // (listHead chain — the FUN_004109d8 entry domain): pool slot,
  // type, model tag/resolved slot, the FUN_0046b2f8 transform
  // (Godot-space), anim fields, chute/explode flags. The dicts carry
  // only presentation inputs — gameplay stays in FreefallRuntime.
  Array get_freefall_object_snapshots();
  // Live kind-2 geometry for one bound pool slot: the twin's
  // animated RuntimeModel as an ArrayMesh grouped one surface per
  // (element, material index), pixel-space UVs, the material name
  // per surface, vert/tri counts and a geom_key digest that changes
  // whenever the anim driver mutates verts (rebuild trigger — same
  // contract as traversal's get_object_geometry). `part` 1 returns
  // the CHUTE attachment model (the +0x306 entry — same object
  // basis, own mesh). Empty dict for an unbound slot.
  Dictionary get_freefall_object_geometry(int64_t pool_slot,
                                          int64_t part);
  // One FALL3D material by name-table string: palette-expanded
  // ImageTexture + {w, h, frames} for payload records; a flat
  // `palette_color` (and `palette_index`) for index/pen records;
  // `palette_index` 256 = the NONE no-draw marker. Unresolvable
  // names return an empty dict.
  Dictionary get_freefall_material(const String& name);

 private:
  // One arena's complete presentation bundle — collision parse,
  // render data, palette-composed textures, ordered tris, and the
  // camera-independent Godot resources. Keyed by arena index.
  struct ArenaSet {
    int index = -1;
    std::string name;
    std::string role;               // "current" | "partner"
    mdk::CollisionArena col;
    std::uint32_t counts[3]{};      // nodes / polys / verts
    PackedVector3Array colLines;    // debug soup, built once
    mdk::ArenaRenderData rd;
    std::array<std::uint8_t, 768> palette{};
    mdk::ArenaMeshTextures texs;
    std::vector<mdk::ArenaMeshTri> tris;
    std::vector<std::uint32_t> order;
    std::uint64_t orderDigest = 0;
    std::uint32_t clsCount[6]{};
    std::uint64_t geomDigest = 0;
    Ref<Image> atlasImage;
    Ref<ImageTexture> atlasTex;
    Ref<ShaderMaterial> mat;
  };

  // Shared frame step behind step_frame/step_frame_input.
  Dictionary stepCore_(double dt_ms, int64_t action_mask,
                       const Dictionary* input);
  // The shared QA-mask/raw-dict -> RawGameplayInput fold (device
  // state only; the per-mode readers own channel semantics).
  mdk::RawGameplayInput buildRawInput_(int64_t action_mask,
                                       const Dictionary* input);
  void setError_(const std::string& msg);

  // Arena lookup helpers.
  mdk::TraversalArena* arenaByIndex_(int idx);
  int indexOfArena_(const mdk::DynamicArena* dyn) const;
  // Inverse of the above for a raw CollisionArena — linear scan of
  // rt_->arenas matching `&a->dyn.col` (the combat-FX events carry
  // the CollisionArena the FUN_00403f6c record bound, not the
  // TraversalArena).
  int indexOfColArena_(const mdk::CollisionArena* col) const;
  // The traversal view set: {cur} + {partner when partnerActive
  // and partner != cur}. Objects are enumerated from this set even
  // when an arena has no MTO render block (corridors).
  std::vector<mdk::TraversalArena*> viewArenas_() const;
  // Build-or-fetch the render bundle for `a` (nullptr when the
  // arena has no MTO block / parse fails; failures are cached in
  // arenaSetFailed_ so a corridor doesn't retry every frame).
  ArenaSet* ensureArenaSet_(mdk::TraversalArena& a);
  // Recompute displaySet_ from the core view set + build any
  // missing sets. `primary` = first member (the current arena's
  // block, else the partner's — a corridor shows its partner's
  // geometry, matching the core's carrier-arena role).
  void updateDisplaySet_();
  // Re-evaluate painter order for every displayed set at the
  // current core camera; re-emit tris only when the order digest
  // actually changed.
  void refreshOrders_();
  // The retained per-arena snapshot dict for `s`.
  Dictionary arenaSnapshotDict_(ArenaSet& s);
  // One copy-safe object dict for `o` (mints/reuses its opaque id).
  Dictionary objectSnapshot_(mdk::TraversalArena& arena,
                             mdk::DynamicObject& o);
  // Identity digest over spawn-stable fields — used by the ID map
  // to detect same-address reuse. Excludes all mutable state.
  std::uint64_t objectFingerprint_(const mdk::DynamicObject& o);

  // --- Phase 16B — Kurt sprite decode + texture cache -----------
  // The 29 bound K_ tables (TRAVSPRT.BNI + LEVEL<n>S.SNI) decoded at
  // level load via decodeSpriteTable, in PlayerAnimTables slot order.
  struct KurtSprites {
    std::array<std::optional<mdk::FtiSprite>, 29> tables;
    std::array<std::string, 29> errors;   // decode failures only
    int decoded = 0;
    int missing = 0;                      // name absent from banks
  };
  void decodeKurtTables_();
  // The sprite palette = the primary displayed arena's composed
  // palette (the global DAC the original blitted into); falls back
  // to the level palette (SYS_PAL + DTI s3, no region-B copy).
  void refreshKurtPalette_();
  // Lazily expands one frame's RLE stream into a palette-mapped
  // RGBA ImageTexture; cached by {table, frame, palette digest}.
  Ref<ImageTexture> kurtTexture_(int tableIdx, int frameIdx);
  Dictionary kurtFrameDict_(int tableIdx, int frameIdx);

  std::optional<KurtSprites> kurt_;
  std::unordered_map<std::uint64_t, Ref<ImageTexture>> kurtTex_;
  std::array<std::uint8_t, 768> kurtPalette_{};
  std::array<std::uint8_t, 768> levelPalette_{};
  std::uint64_t kurtPalKey_ = 0;   // FNV-64 of kurtPalette_

  // --- Phase 17B.2 — HUD/bezel texture cache --------------------
  // The displayed palette pick — the primary displayed arena's
  // composed palette, else the level fallback (the same rule
  // refreshKurtPalette_ and get_active_palette use).
  const std::uint8_t* activePalette_() const;
  // Persistent expand targets — one Image + one ImageTexture each
  // for the HUD overlay and the SNIPERS1 bezel, re-uploaded only
  // when (contentDigest, paletteKey) changes. Bounded: 2 textures.
  Ref<Image> hudImage_;
  Ref<ImageTexture> hudTex_;
  std::uint64_t hudTexKey_ = 0;    // mix(fb digest, pal key) uploaded
  int64_t hudTexUploads_ = 0;      // real re-uploads only (boundedness)
  Ref<Image> bezelImage_;
  Ref<ImageTexture> bezelTex_;
  std::uint64_t bezelTexKey_ = 0;  // mix(bezel key, pal key)
  int64_t bezelTexUploads_ = 0;

  // --- Phase 16C — freefall (mode 2) ------------------------------
  // The presentation tail load_level and the freefall handoff share:
  // shared MTI bank + MDKFONT.FTI reads, SYS_PAL head, level palette
  // compose, Kurt table decode. Requires rt_ to be the loaded
  // traversal runtime for `stem`.
  bool presentTraversalLevel_(const std::string& stem,
                              const std::string& dir);
  // The mode-2 step behind stepCore_ when mode_ == 2: folds input
  // through the same QA/binding path, runs freefallStep with the
  // frontend timing block, steps the scene twins with the same
  // dtSec, and drives the one-shot progression handoff on `done`.
  Dictionary stepFreefall_(double dt_ms, int64_t action_mask,
                           const Dictionary* input);
  // FUN_0040fa68 + the 0x4014bc health branch via
  // progressionFreefallHandoff: on the traversal route the loaded
  // TraversalRuntime becomes rt_ and the presentation tail runs so
  // the display set/arena data stay coherent; on the frontend
  // route the mode flips to 0. One-shot per freefall entry.
  void freefallHandoff_();
  // Palette-expands one freefall material's frame 0 into an
  // ImageTexture (cached by material name — the FALLP palette is
  // fixed for the loaded course).
  Ref<ImageTexture> freefallTexture_(const mdk::FreefallMaterial& m);

  int mode_ = 0;                     // 0x541492 domain
  std::unique_ptr<mdk::FreefallRuntime> ff_;
  std::unique_ptr<mdk::FreefallScene> ffScene_;
  mdk::ProgressionSession sess_{};
  bool ffHandoffDone_ = false;
  int ffHandoffRoute_ = -1;          // ProgressionRoute, or -1
  std::string ffHandoffDetail_;
  std::unordered_map<std::string, Ref<ImageTexture>> ffTex_;

  std::optional<mdk::DataRoot> root_;
  std::unique_ptr<mdk::TraversalRuntime> rt_;
  mdk::FrontendTimingState timing_;
  mdk::GameplayInputBindings bindings_;
  mdk::TraversalFrameResult last_;
  bool hasFrame_ = false;
  // Previous frame's keyLevel — the original's keyEdge is
  // level & ~prev (FUN_0046b688's latch diff).
  std::array<std::uint32_t, mdk::kGameplayKeyBitmapWords>
      prevKeyLevel_{};

  // File buffers the render data aliases (MTO bytes live inside
  // rt_->level; the shared MTI bank is loaded here alongside it).
  std::vector<std::byte> sharedMtiBytes_;
  std::vector<std::byte> ftiBytes_;
  std::span<const std::uint8_t> sysPalHead_;  // into ftiBytes_
  std::string levelStem_;
  std::string levelDir_;

  // Display state — the traversal view set's render bundles.
  std::unordered_map<int, std::unique_ptr<ArenaSet>> arenaSets_;
  std::unordered_set<int> arenaSetFailed_;   // no MTO block / parse
  std::vector<int> displaySet_;              // ordered, cur first
  int arenaIndex_ = -1;                      // displaySet_[0] or -1
  std::string arenaName_;                    // primary arena name
  bool arenaLoaded_ = false;                 // displaySet_ non-empty

  // Opaque object IDs (see mdk_objid.h for lifetime rules).
  mdkfront::MdkObjectIds objIds_;

  // --- Phase 17C.2 — traversal audio ------------------------------
  // One SNI sound bank, search-ordered (level S.SNI first — the same
  // record-list order the original's FUN_00402fe8 first-match sees,
  // OBSERVED via the FUN_0041b7b4/0042322c load order).
  struct AudioBank_ {
    std::span<const std::byte> bytes;   // aliases audioBankStore_ /
                                        // rt_->level.sniBytes
    mdk::SniDirectory dir;
  };
  // Memoized record resolve+decode — keyed by name (zero cross-bank
  // collisions OBSERVED in the corpus; rebuilt per level anyway).
  struct AudioEntry_ {
    bool resolved = false;
    mdk::TraversalAudioSoundDef def;    // vol/rate/frames/loop
    Ref<AudioStreamWAV> stream;         // null on decode failure
  };
  void loadSoundBanks_();
  const AudioEntry_* audioEntry_(const std::string& name);
  bool audioResolve_(const std::string& name,
                     mdk::TraversalAudioSoundDef& def);
  // 0x20000 live-pos refresh — ownerKey is a real DynamicObject* (or
  // the kPlayer tag) in THIS process; scanned across arena storage.
  bool audioOwnerPos_(int cat, const void* key, float pos[3]);

  std::deque<std::vector<std::byte>> audioBankStore_;
  std::vector<AudioBank_> audioBanks_;
  std::unordered_map<std::string, AudioEntry_> audioEntries_;
  mdk::TraversalAudioMixer audioMixer_;
  mdk::TraversalAudioListener audioListener_;
  double lastDtSec_ = 0.0;             // stepCore_'s dt — mixer playhead

  // --- Phase 18B.1 — frontend host ----------------------------------
  std::unique_ptr<mdk::FrontendHostServices> feHost_;
  std::unique_ptr<mdk::FrontendShell> feShell_;
  // Phase 18B.2A — the shared decoded frontend resources + the
  // presentation composer (loaded inside frontend_boot).
  mdk::FrontendResources feRes_;
  bool feResLoaded_ = false;
  mdkbridge::FrontendPresenter fePresenter_;
  // The write seam's content source — the original's writers read
  // live globals at commit time; this snapshots sess_/rt_.
  std::optional<mdk::FrontendSaveData> produceFrontendSave_();
  // Dictionary -> FrontendMenuInput (see frontend_update's doc).
  mdk::FrontendMenuInput frontendInput_(const Dictionary& input) const;
  // FUN_00427f94 route for LoadSave/ContinueLastGame: parse, then
  // GAME's mode field picks header-only (progressionApplyGamePacket
  // + a fresh level load when mode==3) or full (the same
  // applyFullSaveToTraversal rebuild restore_save runs).
  bool frontendLoadSaveFile_(const std::string& stem,
                             std::string& detail);
  // Per-mode teardown before a fresh frontend entry (the
  // FUN_004031b8 abort-yes path) — frees rt_/ff_ so no gameplay
  // structure survives into mode 0.
  void frontendTeardown_();
  Dictionary frontendSnapshot_() const;
  // Campaign runtime installs for frontend_progression_step's
  // kOk edges — mode 2 freefall entry (scene load + init carrying
  // the session's rng/skill) and mode 3 traversal entry (the same
  // load+present tail frontendLoadSaveFile_ runs).
  bool campaignFreefallEnter_(std::string& detail);
  bool campaignTraversalEnter_(std::string& detail);
  // FUN_0040202c's SFX master (0x541308) — the SoundFX menu scalar;
  // the frontend never parses the user's MDK.CFG, so the OBSERVED
  // factory default (70 — frontend_settings' @0x49b0fc table).
  int audioSfxPct_ = 70;

  std::string lastError_;
};

// QA input bits for step_frame's action_mask — mapped onto the
// factory-default keyboard bindings (KeyLeft/Right/Up/Down,
// KeySideL/R, KeyJump, KeyTurbo, KeyLookUp/Down).
enum MdkInputAction : std::uint32_t {
  kActTurnLeft = 1u << 0,
  kActTurnRight = 1u << 1,
  kActMoveFwd = 1u << 2,
  kActMoveBack = 1u << 3,
  kActStrafeLeft = 1u << 4,
  kActStrafeRight = 1u << 5,
  kActJump = 1u << 6,
  kActTurbo = 1u << 7,
  kActLookUp = 1u << 8,
  kActLookDown = 1u << 9,
};

}  // namespace godot
