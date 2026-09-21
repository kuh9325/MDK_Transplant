// Phase 7 (G1) — retained GDExtension bridge into mdk_core.
//
// The C++ core stays authoritative: parsing, ArenaRenderData,
// painter order, camera state. This class owns a TraversalRuntime,
// keeps every file buffer that ArenaRenderData aliases alive for as
// long as the snapshot is reachable, and converts to Godot-space
// values inside C++ (see mdk_math.h / mdk_convert.h — no axis math
// exists in GDScript).
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

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/arena_mesh.h"
#include "core/arena_render.h"
#include "core/collision_query.h"
#include "core/data_root.h"
#include "core/frontend_machines.h"
#include "core/gameplay_input.h"
#include "core/traversal_runtime.h"

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
  // "" resolves to the current (spawn) arena.
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
  // Collision-world debug data for the displayed arena: poly edge
  // line soup (pairs of points, PRIMITIVE_LINES) + counts. All
  // positions are already converted to Godot space.
  Dictionary get_collision_snapshot();
  // The live configured input bindings (factory block — BUILD_A's
  // MDK.CFG carries no overrides): mouse axis letters, scales, and
  // per-button action masks, for QA/debug display.
  Dictionary get_input_config() const;
  // Cheap BSP-order digest for the current camera position — poll it
  // and rebuild the mesh only when it changes.
  int64_t get_arena_order_digest();
  // Full retained snapshot: ordered vertex/uv/material-descriptor
  // arrays, the palette-expanded atlas image, the LUT strip, and
  // the deterministic stats/digests (copy-safe — no C++ aliasing
  // escapes to GDScript).
  Dictionary get_arena_render_snapshot();
  Array get_arena_names() const;
  bool is_level_loaded() const { return rt_ != nullptr; }
  bool is_arena_loaded() const { return arenaLoaded_; }
  String get_last_error() const { return lastError_.c_str(); }
  void shutdown();

 private:
  // Shared frame step behind step_frame/step_frame_input.
  Dictionary stepCore_(double dt_ms, int64_t action_mask,
                       const Dictionary* input);
  // Rebuilds tris_ for the current core camera position.
  bool rebuildOrder_();
  void setError_(const std::string& msg);

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

  // Current arena render state (all owned/aliased storage above).
  mdk::CollisionArena arenaCol_;
  std::uint32_t colNodeCount_ = 0;
  std::uint32_t colPolyCount_ = 0;
  std::uint32_t colVertCount_ = 0;
  PackedVector3Array colLines_;  // debug line soup, built per arena
  mdk::ArenaRenderData rd_;
  std::array<std::uint8_t, 768> palette_{};
  mdk::ArenaMeshTextures texs_;
  std::vector<mdk::ArenaMeshTri> tris_;
  std::vector<std::uint32_t> order_;
  std::uint32_t clsCount_[6] = {};
  std::uint64_t geomDigest_ = 0;
  // Camera-independent Godot resources, built lazily on the first
  // snapshot of each arena load and reused across order rebuilds.
  Ref<Image> atlasImage_;
  Ref<ImageTexture> atlasTex_;
  Ref<ShaderMaterial> arenaMat_;

  std::string arenaName_;
  int arenaIndex_ = -1;
  // Traversal-driven arena switches retry once per NEW index so a
  // block-less corridor doesn't re-fail the lookup every frame.
  int arenaSwitchAttempt_ = -1;
  bool arenaLoaded_ = false;

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
