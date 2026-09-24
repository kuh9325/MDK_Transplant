// Phase 7-9 — retained GDExtension bridge into mdk_core.
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
#include "object_presenter.h"

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

std::uint64_t fnvAppend(std::uint64_t h, const void* p,
                        std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    h = (h ^ b[i]) * 0x100000001b3ull;
  }
  return h;
}

std::uint64_t fnvU64(std::uint64_t h, std::uint64_t v) {
  return fnvAppend(h, &v, sizeof(v));
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
  // Phase 9 (G3).
  ClassDB::bind_method(
      D_METHOD("get_arena_render_snapshots"),
      &MdkBridge::get_arena_render_snapshots);
  ClassDB::bind_method(D_METHOD("get_display_snapshot"),
                       &MdkBridge::get_display_snapshot);
  ClassDB::bind_method(D_METHOD("get_display_digest"),
                       &MdkBridge::get_display_digest);
  ClassDB::bind_method(D_METHOD("get_object_snapshots"),
                       &MdkBridge::get_object_snapshots);
  ClassDB::bind_method(
      D_METHOD("get_object_geometry", "object_id"),
      &MdkBridge::get_object_geometry);
  ClassDB::bind_method(
      D_METHOD("diagnostic_start", "arena_index", "pos_mdk",
               "yaw_deg"),
      &MdkBridge::diagnostic_start);
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

// ---------------------------------------------------------------------------
// Display set — arenas the traversal view currently presents
// ---------------------------------------------------------------------------

mdk::TraversalArena* MdkBridge::arenaByIndex_(int idx) {
  if (!rt_) return nullptr;
  for (auto& a : rt_->arenas) {
    if (a->index == idx) return a.get();
  }
  return nullptr;
}

int MdkBridge::indexOfArena_(const mdk::DynamicArena* dyn) const {
  if (dyn == nullptr || dyn->owner == nullptr) return -1;
  return dyn->owner->index;
}

std::vector<mdk::TraversalArena*> MdkBridge::viewArenas_() const {
  std::vector<mdk::TraversalArena*> out;
  if (!rt_ || !rt_->cur) return out;
  out.push_back(rt_->cur);
  // The original's draw pair is c48+ca4 — partner counts only when
  // the carrier flag is set (prefetches stay invisible, 0x540ca8).
  if (rt_->partnerActive && rt_->partner &&
      rt_->partner != rt_->cur) {
    out.push_back(rt_->partner);
  }
  return out;
}

MdkBridge::ArenaSet* MdkBridge::ensureArenaSet_(
    mdk::TraversalArena& a) {
  const int idx = a.index;
  if (auto it = arenaSets_.find(idx); it != arenaSets_.end()) {
    return it->second.get();
  }
  if (arenaSetFailed_.count(idx)) return nullptr;   // static failure

  // MTO block lookup by 8-char name — the same search
  // traversalArenaLoadGeometry performs (FUN_00432404's lookup).
  const mdk::MtoBlock* block = nullptr;
  for (std::size_t i = 0; i < rt_->level.mto.entries.size(); ++i) {
    if (rt_->level.mto.entries[i].name() == a.name) {
      block = &rt_->level.mto.blocks[i];
      break;
    }
  }
  if (!block) {
    arenaSetFailed_.insert(idx);
    return nullptr;   // corridor — no direct render block
  }

  // Region-C parse — the same call traversalArenaLoadGeometry /
  // mdk-inspect run (block->regionCOffset == fileOffset+4+fieldAt0x0C).
  const std::uint8_t* mb = reinterpret_cast<const std::uint8_t*>(
      rt_->level.mtoBytes.data());
  const std::size_t mn = rt_->level.mtoBytes.size();
  if (block->regionCOffset >= mn) {
    arenaSetFailed_.insert(idx);
    return nullptr;
  }
  auto set = std::make_unique<ArenaSet>();
  set->index = idx;
  set->name = a.name;
  std::uint32_t counts4[4] = {};
  if (!mdk::collisionBlobParse(mb + block->regionCOffset,
                             mn - block->regionCOffset, &set->col,
                             counts4)) {
    arenaSetFailed_.insert(idx);
    return nullptr;
  }
  set->counts[0] = counts4[1];
  set->counts[1] = counts4[2];
  set->counts[2] = counts4[3];

  if (!mdk::arenaRenderDataBuild(
          std::span<const std::byte>(rt_->level.mtoBytes.data(), mn),
          *block, set->col, counts4[1], counts4[2], counts4[3],
          std::span<const std::byte>(sharedMtiBytes_.data(),
                                     sharedMtiBytes_.size()),
          &set->rd)) {
    arenaSetFailed_.insert(idx);
    return nullptr;
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
  if (!mdk::arenaPaletteCompose(sysPalHead_, s3, set->rd.paletteRgb,
                                dti.paletteCount, set->palette.data())) {
    arenaSetFailed_.insert(idx);
    return nullptr;
  }

  // Collision debug line soup — every poly edge of the proven
  // collision blob, converted once per arena (presentation only;
  // nothing here feeds back into collision queries).
  const std::uint32_t polyCount = set->counts[1];
  set->colLines.resize(int64_t(polyCount) * 6);
  for (std::uint32_t p = 0; p < polyCount; ++p) {
    const mdk::CollisionPoly& cp = set->col.polys[p];
    Vector3 v[3];
    for (int k = 0; k < 3; ++k) {
      v[k] = mdkToGodotVec(set->col.verts + std::size_t(cp.v[k]) * 3);
    }
    const int64_t o = int64_t(p) * 6;
    set->colLines.set(o + 0, v[0]);
    set->colLines.set(o + 1, v[1]);
    set->colLines.set(o + 2, v[1]);
    set->colLines.set(o + 3, v[2]);
    set->colLines.set(o + 4, v[2]);
    set->colLines.set(o + 5, v[0]);
  }

  std::fill_n(set->clsCount, 6, 0u);
  for (std::size_t p = 0; p < set->rd.polys.size(); ++p) {
    ++set->clsCount[static_cast<int>(set->rd.polyMaterialClass(p))];
  }
  {  // geometry digest — the mdk-inspect fold over the decoded view
    set->geomDigest = fnvAppend(0xcbf29ce484222325ull, set->rd.verts,
                                std::size_t(set->rd.vertCount) * 12);
    set->geomDigest = fnvAppend(set->geomDigest, set->rd.polys.data(),
                                set->rd.polys.size() *
                                    sizeof(mdk::ArenaRenderPoly));
  }
  if (!mdk::arenaMeshTexturesBuild(
          set->rd, std::span<const std::uint8_t>(set->palette.data(),
                                                 768),
          &set->texs)) {
    arenaSetFailed_.insert(idx);
    return nullptr;
  }
  ArenaSet* out = set.get();
  arenaSets_[idx] = std::move(set);
  return out;
}

void MdkBridge::updateDisplaySet_() {
  displaySet_.clear();
  const auto view = viewArenas_();
  for (mdk::TraversalArena* a : view) {
    ArenaSet* s = ensureArenaSet_(*a);
    if (s != nullptr) {
      s->role = (a == rt_->cur) ? "current" : "partner";
      displaySet_.push_back(a->index);
    }
  }
  arenaLoaded_ = !displaySet_.empty();
  if (arenaLoaded_) {
    mdk::TraversalArena* prim = arenaByIndex_(displaySet_.front());
    arenaName_ = prim ? prim->name : "";
    arenaIndex_ = displaySet_.front();
  } else {
    arenaIndex_ = -1;
    arenaName_.clear();
  }
}

void MdkBridge::refreshOrders_() {
  if (!rt_) return;
  const float* cam = hasFrame_ ? last_.camera.pos
                             : rt_->camera.pose.pos;
  for (int idx : displaySet_) {
    ArenaSet* s = arenaSets_[idx].get();
    mdk::arenaRenderOrder(s->col, cam, false, &s->order);
    const std::uint64_t d = mdk::arenaOrderDigest(s->order);
    if (d != s->orderDigest || s->tris.empty()) {
      s->orderDigest = d;
      mdk::arenaMeshTrisEmit(s->rd, s->texs, s->order, &s->tris);
    }
  }
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
  // Manual load reports the lookup failure verbatim — a corridor
  // still surfaces "no MTO block named X" to interactive callers.
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
  // Clear any cached static failure so an explicit request retries.
  arenaSetFailed_.erase(arena->index);
  ArenaSet* s = ensureArenaSet_(*arena);
  if (s == nullptr) {
    setError_("arena render build failed for " + arena->name);
    return false;
  }
  // Pin the display set to this arena until the next stepped frame
  // recomputes the core view set (pre-step priming path).
  s->role = (arena == rt_->cur) ? "current" : "partner";
  displaySet_.clear();
  displaySet_.push_back(arena->index);
  arenaName_ = arena->name;
  arenaIndex_ = arena->index;
  arenaLoaded_ = true;
  refreshOrders_();
  return true;
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

  // The display set is core-driven every frame: portal swaps,
  // partner attach/detach, and corridor (geometry-less) arenas all
  // recompute here — the G1 single-arena desync is gone. A corridor
  // current arena yields no set of its own; the partner's geometry
  // stays up and the corridor's objects still present.
  updateDisplaySet_();
  refreshOrders_();

  out["frame"] = last_.frame;
  out["arena"] = last_.curArenaIndex;      // core arena
  out["arena_display"] = arenaIndex_;      // primary displayed (-1)
  out["player_pos"] = mdkToGodotVec(last_.pos);
  out["yaw_deg"] = last_.yawDeg;
  out["pitch_deg"] = last_.pitchDeg;
  out["grounded"] = last_.grounded;
  out["camera"] = mdkToGodotCameraTransform(last_.camera);
  out["order_digest"] = static_cast<int64_t>(
      arenaIndex_ >= 0 ? arenaSets_[arenaIndex_]->orderDigest : 0);

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
  out["arena_display"] = arenaIndex_;               // primary shown
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
  if (!arenaLoaded_ || arenaIndex_ < 0) {
    setError_("load_arena() first");
    return out;
  }
  ArenaSet* s = arenaSets_[arenaIndex_].get();
  out["arena"] = s->name.c_str();
  out["arena_index"] = s->index;
  out["poly_count"] = int64_t(s->counts[1]);
  out["vert_count"] = int64_t(s->counts[2]);
  out["node_count"] = int64_t(s->counts[0]);
  // Pairs of points -> Mesh.PRIMITIVE_LINES.
  out["lines"] = s->colLines;
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
  if (!arenaLoaded_ || arenaIndex_ < 0) return -1;
  return static_cast<int64_t>(arenaSets_[arenaIndex_]->orderDigest);
}

int64_t MdkBridge::get_display_digest() {
  if (!rt_) return -1;
  std::uint64_t h = 0xcbf29ce484222325ull;
  h = fnvU64(h, std::uint64_t(displaySet_.size()));
  for (int idx : displaySet_) {
    h = fnvU64(h, std::uint64_t(std::uint32_t(idx)));
    h = fnvU64(h, arenaSets_[idx]->orderDigest);
  }
  h = fnvU64(h, std::uint64_t(std::uint32_t(
      rt_->cur ? rt_->cur->index : -1)));
  h = fnvU64(h, std::uint64_t(std::uint32_t(
      (rt_->partnerActive && rt_->partner) ? rt_->partner->index
                                          : -1)));
  return static_cast<int64_t>(h);
}

Dictionary MdkBridge::arenaSnapshotDict_(ArenaSet& s) {
  Dictionary out;
  PackedVector3Array positions;
  PackedVector2Array uvs;
  PackedFloat32Array matDesc;
  Ref<ArrayMesh> mesh = arenaArrayMesh(s.texs, s.tris, &positions,
                                       &uvs, &matDesc);
  // Atlas + material are camera-independent — built lazily once per
  // arena set and reused across painter-order rebuilds.
  if (s.atlasImage.is_null()) {
    s.atlasImage = arenaAtlasImage(s.texs, s.palette, &s.atlasTex);
  }
  if (s.mat.is_null()) {
    s.mat.instantiate();
    Ref<Shader> shader =
        ResourceLoader::get_singleton()->load(
            "res://shaders/arena_unshaded.gdshader");
    if (shader.is_valid()) {
      s.mat->set_shader(shader);
      if (s.atlasTex.is_valid())
        s.mat->set_shader_parameter("atlas", s.atlasTex);
    } else {
      UtilityFunctions::printerr(
          "MdkBridge: arena_unshaded.gdshader missing");
    }
  }

  PackedInt32Array polyOrder;
  polyOrder.resize(static_cast<int64_t>(s.tris.size()));
  for (std::size_t i = 0; i < s.tris.size(); ++i) {
    polyOrder.set(static_cast<int64_t>(i),
                  static_cast<int32_t>(s.tris[i].poly));
  }
  PackedByteArray palette;
  palette.resize(768);
  std::memcpy(palette.ptrw(), s.palette.data(), 768);

  out["arena"] = s.name.c_str();
  out["arena_index"] = s.index;
  out["role"] = s.role.c_str();
  out["mesh"] = mesh;
  out["material"] = s.mat;
  out["atlas_image"] = s.atlasImage;
  out["positions"] = positions;
  out["uvs"] = uvs;
  out["matdesc"] = matDesc;
  out["poly_order"] = polyOrder;
  out["palette"] = palette;
  out["collision_lines"] = s.colLines;

  Dictionary stats;
  stats["vert_count"] = int64_t(s.rd.vertCount);
  stats["node_count"] = int64_t(s.rd.nodeCount);
  stats["poly_count"] = int64_t(s.rd.polys.size());
  stats["submitted_count"] = int64_t(s.tris.size());
  stats["name_count"] = int64_t(s.rd.materialNames.size());
  stats["texture_count"] = int64_t(s.texs.textures.size());
  std::int64_t resolved = 0;
  for (int x : s.rd.materialOfName) resolved += x >= 0;
  stats["resolved"] = resolved;
  stats["missing"] =
      int64_t(s.rd.materialOfName.size()) - resolved;
  stats["textured"] = int64_t(s.clsCount[0]);
  stats["unresolved"] = int64_t(s.clsCount[1]);
  stats["pen"] = int64_t(s.clsCount[2]);
  stats["fx770"] = int64_t(s.clsCount[3]);
  stats["fxe94"] = int64_t(s.clsCount[4]);
  stats["fx12970"] = int64_t(s.clsCount[5]);
  stats["geom_digest"] = int64_t(s.geomDigest);
  stats["order_digest"] = int64_t(s.orderDigest);
  // Hex forms for display/comparison (the int64 values may read
  // negative in GDScript).
  char hexBuf[24];
  std::snprintf(hexBuf, sizeof(hexBuf), "%016llx",
                (unsigned long long)s.geomDigest);
  stats["geom_digest_hex"] = hexBuf;
  std::snprintf(hexBuf, sizeof(hexBuf), "%016llx",
                (unsigned long long)s.orderDigest);
  stats["order_digest_hex"] = hexBuf;
  stats["atlas_w"] = int64_t(s.texs.atlasW);
  stats["atlas_h"] = int64_t(s.texs.atlasH);
  stats["lut_x"] = int64_t(s.texs.lutX);
  stats["lut_y"] = int64_t(s.texs.lutY);
  out["stats"] = stats;
  return out;
}

Dictionary MdkBridge::get_arena_render_snapshot() {
  Dictionary out;
  if (!arenaLoaded_ || arenaIndex_ < 0) {
    setError_("load_arena() first");
    return out;
  }
  return arenaSnapshotDict_(*arenaSets_[arenaIndex_]);
}

Array MdkBridge::get_arena_render_snapshots() {
  Array out;
  for (int idx : displaySet_) {
    out.push_back(arenaSnapshotDict_(*arenaSets_[idx]));
  }
  return out;
}

Dictionary MdkBridge::get_display_snapshot() {
  Dictionary out;
  if (!rt_) return out;
  out["cur_arena"] = rt_->cur ? rt_->cur->index : -1;
  out["cur_name"] = rt_->cur ? String(rt_->cur->name.c_str())
                           : String();
  out["partner_arena"] =
      (rt_->partnerActive && rt_->partner) ? rt_->partner->index
                                           : -1;
  out["partner_name"] =
      (rt_->partnerActive && rt_->partner)
          ? String(rt_->partner->name.c_str()) : String();
  out["partner_active"] = rt_->partnerActive;
  out["view_on_partner"] = rt_->viewOnPartner;
  out["swapped"] = hasFrame_ ? last_.currentArenaSwapped : false;
  out["portal_candidate"] =
      hasFrame_ ? last_.portalCandidate : -1;
  out["portals_crossed"] = rt_->seams.portalsCrossed;
  out["object_migrations"] = rt_->seams.objectMigrations;
  out["primary"] = arenaIndex_;
  Array arenas;
  for (int idx : displaySet_) {
    ArenaSet* s = arenaSets_[idx].get();
    mdk::TraversalArena* a = arenaByIndex_(idx);
    Dictionary d;
    d["index"] = idx;
    d["name"] = s->name.c_str();
    d["role"] = s->role.c_str();
    d["has_geometry"] = true;
    d["object_count"] =
        a ? int64_t(a->dyn.storage.size()) : int64_t(0);
    arenas.push_back(d);
  }
  out["arenas"] = arenas;
  // The object-view set — may include geometry-less corridors that
  // never appear in `arenas` (their objects still present).
  Array objArenas;
  for (mdk::TraversalArena* a : viewArenas_()) {
    Dictionary d;
    d["index"] = a->index;
    d["name"] = a->name.c_str();
    d["role"] = (a == rt_->cur) ? "current" : "partner";
    d["has_geometry"] = arenaSets_.count(a->index) != 0;
    d["object_count"] = int64_t(a->dyn.storage.size());
    objArenas.push_back(d);
  }
  out["object_arenas"] = objArenas;
  return out;
}

// ---------------------------------------------------------------------------
// Dynamic objects (G3)
// ---------------------------------------------------------------------------

std::uint64_t MdkBridge::objectFingerprint_(
    const mdk::DynamicObject& o) {
  // Spawn-stable identity only — nothing mutable (pos/flags/AABB)
  // may feed this or arena transfers/mover updates would re-mint.
  std::uint64_t h = 0xcbf29ce484222325ull;
  h = fnvU64(h, o.enemyIndex);
  h = fnvU64(h, o.spawnId);
  h = fnvU64(h, o.scriptVariant);
  h = fnvU64(h, o.scriptOff);
  const std::string mn = o.model.modelName();
  h = fnvAppend(h, mn.data(), mn.size());
  h = fnvAppend(h, o.scriptClass.data(), o.scriptClass.size());
  h = fnvAppend(h, o.scriptName.data(), o.scriptName.size());
  return h;
}

Dictionary MdkBridge::objectSnapshot_(mdk::TraversalArena& arena,
                                      mdk::DynamicObject& o) {
  Dictionary d;
  const std::uint64_t id =
      objIds_.idFor(&o, objectFingerprint_(o));
  d["id"] = int64_t(id);
  d["arena"] = arena.index;
  d["arena_name"] = String(arena.name.c_str());
  const std::string mn = o.model.modelName();
  d["model"] = String(mn.c_str());
  // Enemy-table (DTI sub-record) name — e.g. the HMO_9 record "XGS"
  // whose resolved RuntimeModel is internally named XG_BOD.
  if (rt_ && o.enemyIndex < rt_->level.enemies.entries.size()) {
    d["enemy_name"] =
        String(rt_->level.enemies.entries[o.enemyIndex].name.c_str());
  } else {
    d["enemy_name"] = String();
  }
  d["enemy_index"] = int64_t(o.enemyIndex);
  d["spawn_id"] = int64_t(o.spawnId);
  d["pos"] = mdkToGodotVec(o.pos);
  d["pos_mdk"] = Vector3(o.pos[0], o.pos[1], o.pos[2]);
  // The COMPLETE core transform — CollisionObject::xform is the
  // authoritative 3x3 (Euler or raw-matrix path, scale baked) and
  // origin is +0x78 (pos, or pos+zBias on the raw path). Conversion
  // is the change of basis P*M*P^T — no Euler recompute here.
  const mdkfront::Vec3 org =
      {o.col.origin[0], o.col.origin[1], o.col.origin[2]};
  d["transform"] = mdkToGodotObjectTransform(
      mdkfront::mdkTransformToGodot(o.col.xform, org));
  d["aabb"] = mdkToGodotAabb(o.col.aabb);
  d["aabb_mdk"] = PackedFloat32Array{
      o.col.aabb[0], o.col.aabb[1], o.col.aabb[2],
      o.col.aabb[3], o.col.aabb[4], o.col.aabb[5]};
  d["yaw_deg"] = o.yawDeg;
  d["pitch_deg"] = o.pitchDeg;
  d["bank_deg"] = o.bankDeg;
  d["scale"] = o.col.scale;
  d["health"] = int64_t(o.health);
  d["flags148"] = int64_t(o.col.flags148);
  d["flags149"] = int64_t(o.col.flags149);
  d["flags14a"] = int64_t(o.col.flags14a);
  d["mover"] = (o.col.flags14a & 0x20) != 0;
  d["connector"] = (o.col.flags14a & 0x10) != 0;
  d["mountable"] = (o.col.flags14a & 0x80) != 0;
  d["ride_capable"] = (o.col.flags149 & 0x01) != 0;
  d["conn_state"] = int64_t(o.connState);
  d["conn_state_hi"] = int64_t(o.connStateHi);
  d["conn_anim_active"] = o.animRec != nullptr;   // +0x114 active anim
  d["pending_arena"] = indexOfArena_(o.pendingArena);
  d["elem_count"] = int64_t(o.model.elems.size());
  d["elem_mask"] = int64_t(o.col.elemMaskB);
  std::int64_t vc = 0, tc = 0;
  for (std::size_t e = 0; e < o.model.elems.size(); ++e) {
    vc += int64_t(o.model.elemVerts[e].size() / 3);
    tc += int64_t(o.model.elemTris[e].size() / 0x24);
  }
  d["vert_count"] = vc;
  d["tri_count"] = tc;
  d["geom_key"] = int64_t(objectGeomKey(o.model));
  d["script_class"] = String(o.scriptClass.c_str());
  d["script_name"] = String(o.scriptName.c_str());
  return d;
}

Array MdkBridge::get_object_snapshots() {
  Array out;
  if (!rt_) return out;
  // Live-set pass over EVERY arena's storage — the ID map must see
  // despawned objects even when their arena leaves the view set.
  // +0x06==0 records are corpses pending the post-pass FUN_0045cf18
  // sweep: the original unlinks+frees them, so they neither mark a
  // live id nor enumerate (a script-pass kill can leave one in
  // storage until the next frame's sweep).
  objIds_.beginPass();
  for (auto& a : rt_->arenas) {
    for (auto& up : a->dyn.storage) {
      if (up->col.named) objIds_.markLive(up.get());
    }
  }
  for (mdk::TraversalArena* a : viewArenas_()) {
    for (auto& up : a->dyn.storage) {
      if (!up->col.named) continue;
      out.push_back(objectSnapshot_(*a, *up));
    }
  }
  objIds_.endPass();
  return out;
}

Dictionary MdkBridge::get_object_geometry(int64_t object_id) {
  Dictionary out;
  if (!rt_ || object_id <= 0) return out;
  const void* p = objIds_.find(std::uint64_t(object_id));
  if (p == nullptr) return out;   // stale/unknown — empty, not error
  const auto& o = *static_cast<const mdk::DynamicObject*>(p);
  const ObjectGeometry g = objectGeometryFromModel(o.model);
  out["mesh"] = g.mesh;
  out["surface_elems"] = g.surfaceElems;
  out["elem_names"] = g.elemNames;
  out["vert_count"] = g.vertCount;
  out["tri_count"] = g.triCount;
  out["elem_count"] = int64_t(o.model.elems.size());
  out["geom_key"] = int64_t(g.geomKey);
  out["model"] = String(o.model.modelName().c_str());
  return out;
}

Dictionary MdkBridge::diagnostic_start(int64_t arena_index,
                                       const Vector3& pos_mdk,
                                       double yaw_deg) {
  Dictionary out;
  if (!rt_) {
    setError_("no level loaded");
    return out;
  }
  const float pos[3] = {float(pos_mdk.x), float(pos_mdk.y),
                        float(pos_mdk.z)};
  std::string detail;
  const auto e = mdk::traversalRuntimeDiagnosticStart(
      *rt_, int(arena_index), pos, float(yaw_deg), &detail);
  out["ok"] = (e == mdk::TraversalLoadError::kOk);
  out["detail"] = String(detail.c_str());
  if (e != mdk::TraversalLoadError::kOk) {
    setError_(std::string("diagnostic_start: ") +
              mdk::traversalLoadErrorName(e) + " — " + detail);
    return out;
  }
  // The display set recomputes from the re-anchored cur immediately
  // so presentation reflects the diagnostic arena before the next
  // stepped frame.
  updateDisplaySet_();
  refreshOrders_();
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
  arenaName_.clear();
  arenaSets_.clear();
  arenaSetFailed_.clear();
  displaySet_.clear();
  objIds_ = mdkfront::MdkObjectIds{};
  sharedMtiBytes_.clear();
  ftiBytes_.clear();
  sysPalHead_ = {};
  prevKeyLevel_ = {};
  rt_.reset();
  hasFrame_ = false;
  lastError_.clear();
}
