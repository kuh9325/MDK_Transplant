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
#include <godot_cpp/variant/string.hpp>

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
  Dictionary get_player_snapshot() const;
  Dictionary get_camera_snapshot() const;
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
  // Rebuilds tris_ for the current core camera position.
  bool rebuildOrder_();
  void setError_(const std::string& msg);

  std::optional<mdk::DataRoot> root_;
  std::unique_ptr<mdk::TraversalRuntime> rt_;
  mdk::FrontendTimingState timing_;
  mdk::GameplayInputBindings bindings_;
  mdk::TraversalFrameResult last_;
  bool hasFrame_ = false;

  // File buffers the render data aliases (MTO bytes live inside
  // rt_->level; the shared MTI bank is loaded here alongside it).
  std::vector<std::byte> sharedMtiBytes_;
  std::vector<std::byte> ftiBytes_;
  std::span<const std::uint8_t> sysPalHead_;  // into ftiBytes_
  std::string levelStem_;
  std::string levelDir_;

  // Current arena render state (all owned/aliased storage above).
  mdk::CollisionArena arenaCol_;
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
