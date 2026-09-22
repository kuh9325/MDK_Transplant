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
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/arena_mesh.h"
#include "core/arena_render.h"
#include "core/collision_query.h"
#include "core/data_root.h"
#include "core/frontend_machines.h"
#include "core/gameplay_input.h"
#include "core/traversal_runtime.h"

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
  void setError_(const std::string& msg);

  // Arena lookup helpers.
  mdk::TraversalArena* arenaByIndex_(int idx);
  int indexOfArena_(const mdk::DynamicArena* dyn) const;
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
