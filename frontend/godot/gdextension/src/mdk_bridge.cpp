// Phase 7 (G1) — retained GDExtension bridge into mdk_core.
// See mdk_bridge.h for ownership and threading notes.

#include "mdk_bridge.h"

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/dti_structure.h"
#include "core/fti_directory.h"
#include "core/keyboard_menu.h"
#include "core/mto_directory.h"

#include "arena_presenter.h"
#include "mdk_convert.h"

using namespace godot;

namespace {

// QA action -> keyboard-settings slot (kKeyboardSlotToGlobal maps to
// the 29-dword global block; bindings_.keys holds the bound code).
int slotForAction(std::uint32_t bit) {
  switch (bit) {
    case kActTurnLeft: return 0;    // KeyLeft
    case kActTurnRight: return 1;   // KeyRight
    case kActMoveFwd: return 2;     // KeyUp
    case kActMoveBack: return 3;    // KeyDown
    case kActJump: return 4;        // KeyJump
    case kActTurbo: return 8;       // KeyTurbo
    case kActLookUp: return 10;     // KeyLookUp
    case kActLookDown: return 11;   // KeyLookDown
    case kActStrafeLeft: return 17; // KeySideL
    case kActStrafeRight: return 18;// KeySideR
    default: return -1;
  }
}

}  // namespace

void MdkBridge::_bind_methods() {
  ClassDB::bind_method(D_METHOD("initialize", "data_root_path"),
                       &MdkBridge::initialize);
  ClassDB::bind_method(D_METHOD("load_level", "dti_rel_path"),
                       &MdkBridge::load_level);
  ClassDB::bind_method(D_METHOD("load_arena", "arena_name"),
                       &MdkBridge::load_arena);
  ClassDB::bind_method(D_METHOD("step_frame", "dt_ms", "action_mask"),
                       &MdkBridge::step_frame);
  ClassDB::bind_method(D_METHOD("step_frame_input", "dt_ms", "input"),
                       &MdkBridge::step_frame_input);
  ClassDB::bind_method(D_METHOD("get_player_snapshot"),
                       &MdkBridge::get_player_snapshot);
  ClassDB::bind_method(D_METHOD("get_camera_snapshot"),
                       &MdkBridge::get_camera_snapshot);
  ClassDB::bind_method(D_METHOD("get_collision_snapshot"),
                       &MdkBridge::get_collision_snapshot);
  ClassDB::bind_method(D_METHOD("get_input_config"),
                       &MdkBridge::get_input_config);
  ClassDB::bind_method(D_METHOD("get_arena_order_digest"),
                       &MdkBridge::get_arena_order_digest);
  ClassDB::bind_method(D_METHOD("get_arena_render_snapshot"),
                       &MdkBridge::get_arena_render_snapshot);
  ClassDB::bind_method(D_METHOD("get_arena_names"),
                       &MdkBridge::get_arena_names);
  ClassDB::bind_method(D_METHOD("is_level_loaded"),
                       &MdkBridge::is_level_loaded);
  ClassDB::bind_method(D_METHOD("is_arena_loaded"),
                       &MdkBridge::is_arena_loaded);
  ClassDB::bind_method(D_METHOD("get_last_error"),
                       &MdkBridge::get_last_error);
  ClassDB::bind_method(D_METHOD("shutdown"), &MdkBridge::shutdown);
}

void MdkBridge::setError_(const std::string& msg) {
  lastError_ = msg;
  UtilityFunctions::printerr("MdkBridge: ", msg.c_str());
}

bool MdkBridge::initialize(const String& data_root_path) {
  const std::string path = std::string(data_root_path.utf8().get_data());
  std::string err;
  root_ = mdk::DataRoot::open(path, &err);
  if (!root_) {
    setError_("DataRoot open failed: " + err);
    return false;
  }
  return true;
}

bool MdkBridge::load_level(const String& dti_rel_path) {
  if (!root_) {
    setError_("initialize() first");
    return false;
  }
  shutdown();  // drops rt_/buffers on reload; root_ is kept
  const std::string dti = std::string(dti_rel_path.utf8().get_data());
  const auto slash = dti.find_last_of("/\\");
  const auto dot = dti.find_last_of('.');
  if (dot == std::string::npos) {
    setError_("not a .DTI path: " + dti);
    return false;
  }
  const std::size_t nameOff = slash == std::string::npos ? 0 : slash + 1;
  const std::string stem = dti.substr(nameOff, dot - nameOff);
  const std::string dir = slash == std::string::npos
                              ? ""
                              : dti.substr(0, slash + 1);
  const std::string cmi = dir + stem + ".CMI";
  const std::string mto = dir + stem + "O.MTO";
  const std::string mti = dir + stem + "S.MTI";

  std::string err;
  auto mtiBytes = root_->readFile(mti, 1 << 28, &err);
  if (!mtiBytes) {
    setError_("shared material bank read failed: " + mti + " — " + err);
    return false;
  }
  auto fti = root_->readFile("MISC/MDKFONT.FTI", 1 << 28, &err);
  if (!fti) {
    setError_("MISC/MDKFONT.FTI read failed (SYS_PAL): " + err);
    return false;
  }

  rt_ = std::make_unique<mdk::TraversalRuntime>();
  timing_ = mdk::FrontendTimingState{};
  hasFrame_ = false;
  std::string detail;
  const auto le = mdk::traversalRuntimeLoad(*root_, dti, cmi, mto, *rt_,
                                            &detail);
  if (le != mdk::TraversalLoadError::kOk) {
    setError_(std::string("traversal load failed: ") +
              mdk::traversalLoadErrorName(le) + " — " + detail);
    rt_.reset();
    return false;
  }

  levelStem_ = stem;
  levelDir_ = dir;
  sharedMtiBytes_ = std::move(*mtiBytes);
  ftiBytes_ = std::move(*fti);
  // SYS_PAL record head (192 bytes) — FUN_0040163c's source.
  sysPalHead_ = {};
  const auto ftiDir = mdk::inspectFtiDirectory(
      std::span<const std::byte>(ftiBytes_.data(), ftiBytes_.size()));
  if (const mdk::FtiRecord* rec = mdk::findFtiRecord(ftiDir, "SYS_PAL")) {
    if (rec->payloadEnd - rec->payloadFileOffset >= 192) {
      sysPalHead_ = std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(ftiBytes_.data()) +
              rec->payloadFileOffset,
          192);
    }
  }
  if (sysPalHead_.empty()) {
    UtilityFunctions::printerr(
        "MdkBridge: SYS_PAL not found in MDKFONT.FTI — palette head "
        "degrades to black");
  }
  return true;
}

bool MdkBridge::load_arena(const String& arena_name) {
  if (!rt_) {
    setError_("load_level() first");
    return false;
  }
  const std::string name = std::string(arena_name.utf8().get_data());
  mdk::TraversalArena* arena = nullptr;
  if (name.empty()) {
    arena = rt_->cur;
  } else {
    for (auto& a : rt_->arenas) {
      if (a->name == name) {
        arena = a.get();
        break;
      }
    }
  }
  if (!arena) {
    setError_("arena not found: '" + name + "'");
    return false;
  }
  // MTO block lookup by 8-char name — the same search
  // traversalArenaLoadGeometry performs (FUN_00432404's lookup).
  const mdk::MtoBlock* block = nullptr;
  for (std::size_t i = 0; i < rt_->level.mto.entries.size(); ++i) {
    if (rt_->level.mto.entries[i].name() == arena->name) {
      block = &rt_->level.mto.blocks[i];
      break;
    }
  }
  if (!block) {
    setError_("no MTO block named " + arena->name);
    return false;
  }

  // Region-C parse — the same call traversalArenaLoadGeometry /
  // mdk-inspect run (block->regionCOffset == fileOffset+4+fieldAt0x0C).
  const std::uint8_t* mb = reinterpret_cast<const std::uint8_t*>(
      rt_->level.mtoBytes.data());
  const std::size_t mn = rt_->level.mtoBytes.size();
  if (block->regionCOffset >= mn) {
    setError_("region-C offset out of file for " + arena->name);
    return false;
  }
  std::uint32_t counts[4] = {};
  mdk::CollisionArena col;
  if (!mdk::collisionBlobParse(mb + block->regionCOffset,
                             mn - block->regionCOffset, &col, counts)) {
    setError_("collision blob parse failed for " + arena->name);
    return false;
  }

  mdk::ArenaRenderData rd;
  if (!mdk::arenaRenderDataBuild(
          std::span<const std::byte>(rt_->level.mtoBytes.data(), mn),
          *block, col, counts[1], counts[2], counts[3],
          std::span<const std::byte>(sharedMtiBytes_.data(),
                                     sharedMtiBytes_.size()),
          &rd)) {
    setError_("arenaRenderDataBuild failed for " + arena->name);
    return false;
  }

  // Effective palette = SYS_PAL head + region B + DTI s3 tail
  // (arena_mesh.h documents the OBSERVED call chain).
  const auto& dti = rt_->level.dti;
  const std::uint8_t* dtiBytes =
      reinterpret_cast<const std::uint8_t*>(rt_->level.dtiBytes.data());
  std::span<const std::uint8_t> s3;
  if (dti.paletteBytes.fileStart + 768 <= rt_->level.dtiBytes.size()) {
    s3 = std::span<const std::uint8_t>(dtiBytes + dti.paletteBytes.fileStart,
                                       768);
  }
  if (!mdk::arenaPaletteCompose(sysPalHead_, s3, rd.paletteRgb,
                                dti.paletteCount, palette_.data())) {
    setError_("palette compose failed for " + arena->name);
    return false;
  }
  if (sysPalHead_.empty()) {
    UtilityFunctions::printerr(
        "MdkBridge: composing palette without SYS_PAL head");
  }

  arenaCol_ = col;
  // Collision debug line soup — every poly edge of the proven
  // collision blob, converted once per arena (presentation only;
  // nothing here feeds back into collision queries).
  colNodeCount_ = counts[1];
  colPolyCount_ = counts[2];
  colVertCount_ = counts[3];
  colLines_.clear();
  colLines_.resize(int64_t(colPolyCount_) * 6);
  for (std::uint32_t p = 0; p < colPolyCount_; ++p) {
    const mdk::CollisionPoly& cp = arenaCol_.polys[p];
    Vector3 v[3];
    for (int k = 0; k < 3; ++k) {
      v[k] = mdkToGodotVec(arenaCol_.verts + std::size_t(cp.v[k]) * 3);
    }
    const int64_t o = int64_t(p) * 6;
    colLines_.set(o + 0, v[0]);
    colLines_.set(o + 1, v[1]);
    colLines_.set(o + 2, v[1]);
    colLines_.set(o + 3, v[2]);
    colLines_.set(o + 4, v[2]);
    colLines_.set(o + 5, v[0]);
  }
  rd_ = std::move(rd);
  atlasImage_.unref();
  atlasTex_.unref();
  arenaMat_.unref();
  std::fill_n(clsCount_, 6, 0u);
  for (std::size_t p = 0; p < rd_.polys.size(); ++p) {
    ++clsCount_[static_cast<int>(rd_.polyMaterialClass(p))];
  }
  {  // geometry digest — the mdk-inspect fold over the decoded view
    auto fnv = [](std::uint64_t h, const void* p, std::size_t n) {
      const auto* b = static_cast<const std::uint8_t*>(p);
      for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ b[i]) * 0x100000001b3ull;
      }
      return h;
    };
    geomDigest_ = fnv(0xcbf29ce484222325ull, rd_.verts,
                      std::size_t(rd_.vertCount) * 12);
    geomDigest_ = fnv(geomDigest_, rd_.polys.data(),
                      rd_.polys.size() * sizeof(mdk::ArenaRenderPoly));
  }
  if (!mdk::arenaMeshTexturesBuild(
          rd_, std::span<const std::uint8_t>(palette_.data(), 768),
          &texs_)) {
    setError_("texture expansion failed for " + arena->name);
    return false;
  }
  arenaName_ = arena->name;
  arenaIndex_ = arena->index;
  arenaLoaded_ = true;
  return rebuildOrder_();
}

bool MdkBridge::rebuildOrder_() {
  if (!arenaLoaded_) return false;
  // Painter order is evaluated at the current core camera position.
  const float* cam = hasFrame_ ? last_.camera.pos
                               : rt_->camera.pose.pos;
  mdk::arenaRenderOrder(arenaCol_, cam, false, &order_);
  return mdk::arenaMeshTrisEmit(rd_, texs_, order_, &tris_);
}

Dictionary MdkBridge::step_frame(double dt_ms, int64_t action_mask) {
  return stepCore_(dt_ms, action_mask, nullptr);
}

Dictionary MdkBridge::step_frame_input(double dt_ms,
                                       const Dictionary& input) {
  return stepCore_(dt_ms, int64_t(input.get("actions", 0)), &input);
}

Dictionary MdkBridge::stepCore_(double dt_ms, int64_t action_mask,
                                const Dictionary* input) {
  Dictionary out;
  if (!rt_) {
    setError_("no level loaded");
    return out;
  }
  mdk::frontendTimingUpdate(timing_, dt_ms);
  mdk::RawGameplayInput raw{};
  // QA action mask -> the bound internal key codes (level state).
  for (std::uint32_t bit = 1; bit; bit <<= 1) {
    if (!(action_mask & bit)) continue;
    const int slot = slotForAction(bit);
    if (slot < 0) continue;
    const int gi = mdk::kKeyboardSlotToGlobal[slot];
    const int code = bindings_.keys[gi];
    if (code > 0 && code < mdk::kGameplayKeyCount) {
      raw.keyLevel[code >> 5] |= 1u << (code & 31);
    }
  }
  if (input != nullptr) {
    // "keys" — held internal key codes (0..127, the original
    // FUN_0046b688 domain). The frontend translates its device key
    // events into these codes; configured bindings stay in core.
    if (input->has("keys")) {
      const Variant kv = (*input)["keys"];
      PackedInt32Array codes;
      if (kv.get_type() == Variant::PACKED_INT32_ARRAY) {
        codes = kv;
      } else if (kv.get_type() == Variant::ARRAY) {
        const Array arr = kv;
        codes.resize(arr.size());
        for (int64_t i = 0; i < arr.size(); ++i) {
          codes.set(i, int64_t(arr[i]));
        }
      }
      for (int64_t i = 0; i < codes.size(); ++i) {
        const int code = codes[i];
        if (code > 0 && code < mdk::kGameplayKeyCount) {
          raw.keyLevel[code >> 5] |= 1u << (code & 31);
        }
      }
    }
    // DIMOUSESTATE deltas + the 4-button nibble — forwarded raw;
    // the core's W-set axis letters/scales and per-button action
    // masks own all semantics (FUN_00406f14).
    raw.mouseDx = int32_t(int64_t(input->get("mouse_dx", 0)));
    raw.mouseDy = int32_t(int64_t(input->get("mouse_dy", 0)));
    raw.mouseDz = int32_t(int64_t(input->get("mouse_dz", 0)));
    raw.mouseButtons =
        uint32_t(int64_t(input->get("mouse_buttons", 0))) & 0xf;
  }
  // keyEdge = level & ~prev — the original's per-poll new-press
  // bitmap (FUN_0046b688 latch diff).
  for (int w = 0; w < mdk::kGameplayKeyBitmapWords; ++w) {
    raw.keyEdge[w] = raw.keyLevel[w] & ~prevKeyLevel_[w];
    prevKeyLevel_[w] = raw.keyLevel[w];
  }

  last_ = mdk::stepTraversalRuntime(*rt_, raw, bindings_, timing_);
  hasFrame_ = true;

  // Traversal may portal the player into a different arena — swap
  // the displayed static arena to match when a block exists. A
  // corridor (CHMO_*, no MTO block) leaves the previous display in
  // place and marks the desync instead of failing the frame.
  if (last_.curArenaIndex != arenaIndex_ &&
      last_.curArenaIndex != arenaSwitchAttempt_) {
    arenaSwitchAttempt_ = last_.curArenaIndex;
    for (auto& a : rt_->arenas) {
      if (a->index == last_.curArenaIndex) {
        if (!load_arena(a->name.c_str())) {
          UtilityFunctions::printerr(
              "MdkBridge: arena switch to '", a->name.c_str(),
              "' unavailable — keeping '", arenaName_.c_str(),
              "' displayed (desync)");
        }
        break;
      }
    }
  }
  if (arenaLoaded_) rebuildOrder_();

  out["frame"] = last_.frame;
  out["arena"] = last_.curArenaIndex;      // core arena
  out["arena_display"] = arenaIndex_;      // displayed arena (-1 none)
  out["player_pos"] = mdkToGodotVec(last_.pos);
  out["yaw_deg"] = last_.yawDeg;
  out["pitch_deg"] = last_.pitchDeg;
  out["grounded"] = last_.grounded;
  out["camera"] = mdkToGodotCameraTransform(last_.camera);
  out["order_digest"] =
      static_cast<int64_t>(mdk::arenaOrderDigest(order_));

  // Merged-control echo — the just-consumed GameplayInputFrame
  // (0x4ce block), for tests/QA that need to observe which semantic
  // action the raw input produced. Proven fields only.
  const mdk::GameplayInputFrame& cf = rt_->prevFrame;
  Dictionary inp;
  inp["fire"] = cf.fire != 0;
  inp["jump"] = cf.jump != 0;
  inp["sniper_pulse"] = cf.sniperPulse != 0;
  inp["item_use"] = cf.itemUse != 0;
  inp["item_next"] = cf.itemNext != 0;
  inp["item_prev"] = cf.itemPrev != 0;
  inp["look_up"] = cf.lookUp != 0;
  inp["look_down"] = cf.lookDown != 0;
  inp["turn_axis"] = cf.turnAxis;
  inp["move_axis"] = cf.moveAxis;
  inp["move_digital"] = cf.moveDigital;
  inp["strafe_axis"] = cf.strafeAxis;
  inp["side_step_held"] = cf.sideStepHeld;
  inp["turbo_latched"] = cf.turboLatched;
  inp["zoom_accumulator"] = cf.zoomAccumulator;
  inp["mouse_turn_active"] = cf.mouseTurnActive != 0;
  out["input"] = inp;
  return out;
}

Dictionary MdkBridge::get_player_snapshot() const {
  Dictionary out;
  if (!hasFrame_) return out;
  out["pos"] = mdkToGodotVec(last_.pos);
  out["pos_mdk"] = Vector3(last_.pos[0], last_.pos[1], last_.pos[2]);
  out["yaw_deg"] = last_.yawDeg;
  out["pitch_deg"] = last_.pitchDeg;
  out["grounded"] = last_.grounded;
  out["arena"] = last_.curArenaIndex;
  out["frame"] = last_.frame;
  // G2 presentation fields — every value a verbatim copy of a
  // proven core output (TraversalFrameResult / collision state).
  out["transform"] = mdkToGodotPlayerTransform(last_.pos,
                                               last_.yawDeg);
  // box = the standing body extents (0x540c30..44 as the mode-3 tail
  // rebuilt it); query_box = the LAST per-query AABB collisionApply
  // wrote — volatile, exposed for diagnostics only.
  out["box"] = mdkToGodotAabb(last_.playerBodyBox);
  out["box_mdk"] = Array::make(
      last_.playerBodyBox[0], last_.playerBodyBox[1],
      last_.playerBodyBox[2], last_.playerBodyBox[3],
      last_.playerBodyBox[4], last_.playerBodyBox[5]);
  out["query_box"] = mdkToGodotAabb(last_.playerBox);
  out["query_box_mdk"] = Array::make(
      last_.playerBox[0], last_.playerBox[1], last_.playerBox[2],
      last_.playerBox[3], last_.playerBox[4], last_.playerBox[5]);
  out["loco_state"] = last_.locoState;              // 0x540cac
  out["move_vel"] = last_.moveVel;                  // 0x540d48
  out["strafe_vel"] = last_.strafeVel;              // 0x540d4c
  out["turn_vel"] = last_.turnVel;                  // 0x540d50
  out["vert_vel"] = last_.vertVel;                  // 0x540c78
  out["contact"] = last_.contactObj != 0;           // 0x540e4c != 0
  out["contact_normal"] = mdkToGodotVec(last_.contactNormal);
  out["position_changed"] = last_.positionChanged;
  out["look_offset_deg"] = last_.lookOffsetDeg;     // 0x540d58
  out["event_type"] = last_.eventType;              // 0x54cb00
  out["event_mag"] = last_.eventMag;                // 0x54cb08
  out["arena_display"] = arenaIndex_;               // shown arena
  return out;
}

Dictionary MdkBridge::get_camera_snapshot() const {
  Dictionary out;
  if (!rt_) return out;
  const mdk::PlayerCameraPose& p =
      hasFrame_ ? last_.camera : rt_->camera.pose;
  const mdk::PlayerCameraState& st = rt_->camera;
  out["transform"] = mdkToGodotCameraTransform(p);
  out["position"] = mdkToGodotVec(p.pos);
  out["pos_mdk"] = Vector3(p.pos[0], p.pos[1], p.pos[2]);
  // Normal-viewport divisors (OBSERVED projector constants for the
  // 600x360 view rect): x=299.95, y=180.4.
  const float xDiv = p.viewW > 0 ? float(p.viewW) * 0.4999f : 299.95f;
  const float yDiv = p.viewH > 0 ? float(p.viewH) * 0.5011f : 180.4f;
  out["fov_deg"] = mdkfront::mdkCameraFovYDeg(p.scaleY,
                                              float(p.viewH), yDiv);
  out["aspect"] = mdkfront::mdkCameraAspect(
      p.scaleX, p.scaleY, float(p.viewW), float(p.viewH), xDiv, yDiv);
  out["scale_x"] = p.scaleX;
  out["scale_y"] = p.scaleY;
  out["scale_z"] = p.scaleZ;
  out["zoom"] = st.zoom;
  out["view_rect"] =
      Rect2i(p.viewOX, p.viewOY, p.viewW, p.viewH);
  return out;
}

Dictionary MdkBridge::get_collision_snapshot() {
  Dictionary out;
  if (!arenaLoaded_) {
    setError_("load_arena() first");
    return out;
  }
  out["arena"] = arenaName_.c_str();
  out["arena_index"] = arenaIndex_;
  out["poly_count"] = int64_t(colPolyCount_);
  out["vert_count"] = int64_t(colVertCount_);
  out["node_count"] = int64_t(colNodeCount_);
  // Pairs of points -> Mesh.PRIMITIVE_LINES.
  out["lines"] = colLines_;
  return out;
}

Dictionary MdkBridge::get_input_config() const {
  Dictionary out;
  out["mouse_on"] = bindings_.mouseOn;
  out["mouse_axes_map"] = bindings_.mouseAxesMap.c_str();
  out["mouse_y_reversed"] = bindings_.mouseYReversedBits != 0;
  PackedFloat32Array scales;
  scales.resize(3);
  for (int i = 0; i < 3; ++i) scales.set(i, bindings_.mouseScale[i]);
  out["mouse_scale"] = scales;
  PackedInt32Array masks;
  masks.resize(4);
  for (int i = 0; i < 4; ++i) masks.set(i, bindings_.mouseButtMask[i]);
  out["mouse_button_masks"] = masks;
  out["joy_on"] = bindings_.joyOn;
  return out;
}

int64_t MdkBridge::get_arena_order_digest() {
  if (!arenaLoaded_) return -1;
  return static_cast<int64_t>(mdk::arenaOrderDigest(order_));
}

Dictionary MdkBridge::get_arena_render_snapshot() {
  Dictionary out;
  if (!arenaLoaded_) {
    setError_("load_arena() first");
    return out;
  }
  PackedVector3Array positions;
  PackedVector2Array uvs;
  PackedFloat32Array matDesc;
  Ref<ArrayMesh> mesh = arenaArrayMesh(texs_, tris_, &positions, &uvs,
                                       &matDesc);
  // Atlas + material are camera-independent — build once per arena
  // and reuse across painter-order rebuilds.
  if (atlasImage_.is_null()) {
    atlasImage_ = arenaAtlasImage(texs_, palette_, &atlasTex_);
  }
  if (arenaMat_.is_null()) {
    arenaMat_.instantiate();
    Ref<Shader> shader =
        ResourceLoader::get_singleton()->load(
            "res://shaders/arena_unshaded.gdshader");
    if (shader.is_valid()) {
      arenaMat_->set_shader(shader);
      if (atlasTex_.is_valid())
        arenaMat_->set_shader_parameter("atlas", atlasTex_);
    } else {
      UtilityFunctions::printerr(
          "MdkBridge: arena_unshaded.gdshader missing");
    }
  }

  Ref<ShaderMaterial> mat = arenaMat_;
  Ref<Image> atlas = atlasImage_;

  PackedInt32Array polyOrder;
  polyOrder.resize(static_cast<int64_t>(tris_.size()));
  for (std::size_t i = 0; i < tris_.size(); ++i) {
    polyOrder.set(static_cast<int64_t>(i),
                  static_cast<int32_t>(tris_[i].poly));
  }
  PackedByteArray palette;
  palette.resize(768);
  std::memcpy(palette.ptrw(), palette_.data(), 768);

  out["arena"] = arenaName_.c_str();
  out["arena_index"] = arenaIndex_;
  out["mesh"] = mesh;
  out["material"] = mat;
  out["atlas_image"] = atlas;
  out["positions"] = positions;
  out["uvs"] = uvs;
  out["matdesc"] = matDesc;
  out["poly_order"] = polyOrder;
  out["palette"] = palette;

  Dictionary stats;
  stats["vert_count"] = int64_t(rd_.vertCount);
  stats["node_count"] = int64_t(rd_.nodeCount);
  stats["poly_count"] = int64_t(rd_.polys.size());
  stats["submitted_count"] = int64_t(tris_.size());
  stats["name_count"] = int64_t(rd_.materialNames.size());
  stats["texture_count"] = int64_t(texs_.textures.size());
  std::int64_t resolved = 0;
  for (int s : rd_.materialOfName) resolved += s >= 0;
  stats["resolved"] = resolved;
  stats["missing"] =
      int64_t(rd_.materialOfName.size()) - resolved;
  stats["textured"] = int64_t(clsCount_[0]);
  stats["unresolved"] = int64_t(clsCount_[1]);
  stats["pen"] = int64_t(clsCount_[2]);
  stats["fx770"] = int64_t(clsCount_[3]);
  stats["fxe94"] = int64_t(clsCount_[4]);
  stats["fx12970"] = int64_t(clsCount_[5]);
  stats["geom_digest"] = int64_t(geomDigest_);
  stats["order_digest"] =
      int64_t(mdk::arenaOrderDigest(order_));
  // Hex forms for display/comparison (the int64 values may read
  // negative in GDScript).
  char hexBuf[24];
  std::snprintf(hexBuf, sizeof(hexBuf), "%016llx",
                (unsigned long long)geomDigest_);
  stats["geom_digest_hex"] = hexBuf;
  std::snprintf(hexBuf, sizeof(hexBuf), "%016llx",
                (unsigned long long)mdk::arenaOrderDigest(order_));
  stats["order_digest_hex"] = hexBuf;
  stats["atlas_w"] = int64_t(texs_.atlasW);
  stats["atlas_h"] = int64_t(texs_.atlasH);
  stats["lut_x"] = int64_t(texs_.lutX);
  stats["lut_y"] = int64_t(texs_.lutY);
  out["stats"] = stats;
  return out;
}

Array MdkBridge::get_arena_names() const {
  Array out;
  if (!rt_) return out;
  for (const auto& a : rt_->arenas) {
    out.push_back(String(a->name.c_str()));
  }
  return out;
}

void MdkBridge::shutdown() {
  arenaLoaded_ = false;
  arenaIndex_ = -1;
  arenaSwitchAttempt_ = -1;
  arenaName_.clear();
  tris_.clear();
  order_.clear();
  texs_ = mdk::ArenaMeshTextures{};
  rd_ = mdk::ArenaRenderData{};
  arenaCol_ = mdk::CollisionArena{};
  colNodeCount_ = colPolyCount_ = colVertCount_ = 0;
  colLines_.clear();
  sharedMtiBytes_.clear();
  ftiBytes_.clear();
  sysPalHead_ = {};
  atlasImage_.unref();
  atlasTex_.unref();
  arenaMat_.unref();
  prevKeyLevel_ = {};
  rt_.reset();
  hasFrame_ = false;
}
