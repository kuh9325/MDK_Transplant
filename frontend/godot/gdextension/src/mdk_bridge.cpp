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
#include <cstdlib>
#include <cstring>

#include "core/dti_structure.h"
#include "core/enemy_runtime.h"
#include "core/frontend_transition.h"
#include "core/fti_directory.h"
#include "core/indexed_image.h"
#include "core/keyboard_menu.h"
#include "core/mto_directory.h"
#include "core/save_full_restore.h"
#include "core/save_full_write.h"
#include "core/thmb_capture.h"

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
  ClassDB::bind_method(D_METHOD("get_kurt_snapshot"),
                       &MdkBridge::get_kurt_snapshot);
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
  ClassDB::bind_method(D_METHOD("diagnostic_damage", "amount"),
                       &MdkBridge::diagnostic_damage);
  ClassDB::bind_method(D_METHOD("diagnostic_kill", "object_id"),
                       &MdkBridge::diagnostic_kill);
  ClassDB::bind_method(D_METHOD("diagnostic_shockwave", "object_id"),
                       &MdkBridge::diagnostic_shockwave);
  // Phase 17A closeout — full save/restore.
  ClassDB::bind_method(D_METHOD("save_game_full"),
                       &MdkBridge::save_game_full);
  ClassDB::bind_method(D_METHOD("restore_save", "bytes"),
                       &MdkBridge::restore_save);
  // Phase 17A — traversal combat presentation.
  ClassDB::bind_method(D_METHOD("get_shot_snapshots"),
                       &MdkBridge::get_shot_snapshots);
  ClassDB::bind_method(D_METHOD("drain_combat_fx"),
                       &MdkBridge::drain_combat_fx);
  ClassDB::bind_method(D_METHOD("get_shot_geometry", "class_idx"),
                       &MdkBridge::get_shot_geometry);
  ClassDB::bind_method(D_METHOD("get_named_geometry", "name"),
                       &MdkBridge::get_named_geometry);
  ClassDB::bind_method(D_METHOD("get_active_palette"),
                       &MdkBridge::get_active_palette);
  ClassDB::bind_method(D_METHOD("fx_stab", "from", "to", "arena_index"),
                       &MdkBridge::fx_stab);
  // Phase 17C.2 — traversal audio.
  ClassDB::bind_method(D_METHOD("drain_audio_fx"),
                       &MdkBridge::drain_audio_fx);
  ClassDB::bind_method(D_METHOD("get_audio_stats"),
                       &MdkBridge::get_audio_stats);
  // Phase 17B.2 — traversal HUD / view presentation.
  ClassDB::bind_method(D_METHOD("get_hud_snapshot"),
                       &MdkBridge::get_hud_snapshot);
  // Phase 16C — freefall (mode 2).
  ClassDB::bind_method(
      D_METHOD("load_freefall", "course", "skill", "seed"),
      &MdkBridge::load_freefall);
  ClassDB::bind_method(D_METHOD("get_mode"), &MdkBridge::get_mode);
  ClassDB::bind_method(D_METHOD("get_freefall_snapshot"),
                       &MdkBridge::get_freefall_snapshot);
  ClassDB::bind_method(D_METHOD("get_freefall_object_snapshots"),
                       &MdkBridge::get_freefall_object_snapshots);
  ClassDB::bind_method(
      D_METHOD("get_freefall_object_geometry", "pool_slot", "part"),
      &MdkBridge::get_freefall_object_geometry);
  ClassDB::bind_method(D_METHOD("get_freefall_material", "name"),
                       &MdkBridge::get_freefall_material);
  // Phase 18B.1 — frontend host services.
  ClassDB::bind_method(D_METHOD("frontend_boot", "save_dir"),
                       &MdkBridge::frontend_boot);
  ClassDB::bind_method(D_METHOD("frontend_booted"),
                       &MdkBridge::frontend_booted);
  ClassDB::bind_method(D_METHOD("frontend_enter", "returning"),
                       &MdkBridge::frontend_enter);
  ClassDB::bind_method(D_METHOD("frontend_snapshot"),
                       &MdkBridge::frontend_snapshot);
  ClassDB::bind_method(D_METHOD("frontend_update", "input"),
                       &MdkBridge::frontend_update);
  ClassDB::bind_method(D_METHOD("frontend_end_frame", "dt_ms"),
                       &MdkBridge::frontend_end_frame);
  ClassDB::bind_method(D_METHOD("frontend_drain_requests"),
                       &MdkBridge::frontend_drain_requests);
  ClassDB::bind_method(D_METHOD("frontend_drain_fx"),
                       &MdkBridge::frontend_drain_fx);
  ClassDB::bind_method(D_METHOD("frontend_lastgame_exists"),
                       &MdkBridge::frontend_lastgame_exists);
  ClassDB::bind_method(D_METHOD("frontend_enumerate_saves"),
                       &MdkBridge::frontend_enumerate_saves);
  ClassDB::bind_method(D_METHOD("frontend_inspect_slot", "stem"),
                       &MdkBridge::frontend_inspect_slot);
  ClassDB::bind_method(D_METHOD("frontend_write_save", "request"),
                       &MdkBridge::frontend_write_save);
  ClassDB::bind_method(D_METHOD("frontend_slide_probe", "index"),
                       &MdkBridge::frontend_slide_probe);
  ClassDB::bind_method(D_METHOD("frontend_slide_data", "index"),
                       &MdkBridge::frontend_slide_data);
  ClassDB::bind_method(D_METHOD("frontend_transition_complete"),
                       &MdkBridge::frontend_transition_complete);
  ClassDB::bind_method(D_METHOD("frontend_transition_seconds"),
                       &MdkBridge::frontend_transition_seconds);
  ClassDB::bind_method(D_METHOD("frontend_capture_thumbnail"),
                       &MdkBridge::frontend_capture_thumbnail);
  ClassDB::bind_method(D_METHOD("frontend_notify_load_result", "ok"),
                       &MdkBridge::frontend_notify_load_result);
  ClassDB::bind_method(D_METHOD("frontend_dispatch_requests"),
                       &MdkBridge::frontend_dispatch_requests);
  // Phase 18B.2A — frontend presentation.
  ClassDB::bind_method(D_METHOD("frontend_frame", "transition_ms"),
                       &MdkBridge::frontend_frame, DEFVAL(-1.0));
  ClassDB::bind_method(D_METHOD("frontend_progression_step", "input"),
                       &MdkBridge::frontend_progression_step);
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
  // Standalone arena run — no campaign carry-in, so seed the
  // observed live-player health (150, per real saves; same
  // convention as mdk-inspect --traversal-runtime). The
  // damage/death dispatcher (FUN_00463608 dead-check) treats
  // health==0 && gate==0 as dead.
  if (rt_->fieldHealth <= 0) rt_->fieldHealth = 150;
  mode_ = 3;
  // Standalone loads carry no campaign context; seed the session row
  // a full save serializes. GAME+0x04 needs the internal level id —
  // the 0x4999e8 table inverse on the LEVEL<n> dir number, the same
  // convention mdk-inspect --save-write-full uses. A path outside
  // the table leaves levelId=-1 and the writer reports it.
  sess_ = mdk::ProgressionSession{};
  sess_.mode = 3;
  sess_.health = rt_->fieldHealth;
  sess_.field54163b = rt_->field54163b;
  sess_.levelId = -1;
  if (const char* p = std::strstr(stem.c_str(), "LEVEL")) {
    const int levelDir = static_cast<int>(std::strtol(p + 5, nullptr, 10));
    for (int i = 0; i < 8; ++i)
      if (mdk::progressionLevelDir(i) == levelDir) sess_.levelId = i;
  }
  return presentTraversalLevel_(stem, dir);
}

// The presentation tail shared by load_level and the freefall
// handoff: the shared MTI bank + MDKFONT.FTI reads, the SYS_PAL
// head span, the level-fallback palette compose, and the Kurt
// sprite-table decode. Requires rt_ to be the loaded traversal
// runtime for `stem`.
bool MdkBridge::presentTraversalLevel_(const std::string& stem,
                                       const std::string& dir) {
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
  // Phase 16B — the level fallback sprite palette (SYS_PAL head +
  // DTI s3 tail, no arena region-B copy) and the decoded K_ tables.
  // The palette actually applied is refreshKurtPalette_()'s pick:
  // the primary displayed arena's composed palette when one exists.
  {
    const auto& dti = rt_->level.dti;
    std::span<const std::uint8_t> s3;
    if (dti.paletteBytes.fileStart + 768 <=
        rt_->level.dtiBytes.size()) {
      s3 = std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(
              rt_->level.dtiBytes.data()) +
              dti.paletteBytes.fileStart,
          768);
    }
    std::array<std::uint8_t, 768> lp{};
    mdk::arenaPaletteCompose(sysPalHead_, s3, {}, dti.paletteCount,
                             lp.data());
    levelPalette_ = lp;
  }
  kurtPalette_ = levelPalette_;
  kurtPalKey_ = 0;
  kurtTex_.clear();
  decodeKurtTables_();
  // Phase 17C.2 — the SNI sound bank set for this level (level bank
  // first — the observed record-list order; global banks after).
  loadSoundBanks_();
  return true;
}

// ---------------------------------------------------------------------------
// Phase 17C.2 — traversal audio: SNI banks, WAVE cache, voice pool
// ---------------------------------------------------------------------------

// The sound-bank set is rebuilt on every level presentation (load,
// restore, freefall->traversal handoff): voices die with the session,
// stream/lookup caches are level-scoped, and the byte buffers the SNI
// directories index stay alive until the next boundary.
void MdkBridge::loadSoundBanks_() {
  audioBanks_.clear();
  audioBankStore_.clear();
  audioEntries_.clear();
  audioMixer_.reset();
  if (!rt_) return;
  // OBSERVED record-list order: the level bank, then the traversal
  // bank (FUN_0041b7b4's load pair), then the global MDKSOUND bank
  // (FUN_0042322c). FUN_00402fe8's first-match walk makes list order
  // the shadow order — the corpus carries zero cross-bank name
  // collisions, so the order is belt-and-suspenders either way.
  if (!rt_->level.sniBytes.empty()) {
    AudioBank_ b;
    b.bytes = std::span<const std::byte>(rt_->level.sniBytes);
    b.dir = mdk::inspectSniDirectory(b.bytes);
    audioBanks_.push_back(b);
  }
  for (const char* rel : {"TRAVERSE/TRAVERSE.SNI",
                         "MISC/MDKSOUND.SNI"}) {
    std::string err;
    auto bytes = root_->readFile(rel, 1 << 28, &err);
    if (!bytes) continue;   // absent bank -> resolves report missing
    audioBankStore_.push_back(std::move(*bytes));
    AudioBank_ b;
    b.bytes = std::span<const std::byte>(audioBankStore_.back());
    b.dir = mdk::inspectSniDirectory(b.bytes);
    audioBanks_.push_back(b);
  }
}

// Resolve + decode a record (memoized). Sentinel records and the
// flags-bit1 music class never enter the SFX pool (OBSERVED: those
// records drive the FUN_0041d774 song path). Returns the stable map
// entry — nullptr only for a truly absent name.
const MdkBridge::AudioEntry_* MdkBridge::audioEntry_(
    const std::string& name) {
  if (const auto it = audioEntries_.find(name);
      it != audioEntries_.end()) {
    return &it->second;
  }
  AudioEntry_ e;
  for (const AudioBank_& b : audioBanks_) {
    if (b.dir.status != mdk::SniDirectoryStatus::kOk) continue;
    const mdk::SniEntry* rec = nullptr;
    for (const mdk::SniEntry& en : b.dir.entries) {
      if (!en.isSentinel() && en.name() == name) {
        rec = &en;
        break;
      }
    }
    if (!rec) continue;
    const std::uint32_t fld = rec->fieldAt0x0C;
    const int flags = static_cast<int>(fld & 0xffffu);
    if (flags & 0x2) break;    // music-class — out of SFX scope
    const std::uint64_t off = rec->payloadFileOffset();
    const std::uint64_t end = rec->payloadFileEnd();
    if (off >= end || end > b.bytes.size()) break;
    mdk::SniWave wv;
    std::string derr;
    const mdk::SniWaveStatus st = mdk::decodeSniWave(
        b.bytes.subspan(static_cast<std::size_t>(off),
                        static_cast<std::size_t>(end - off)),
        &wv, &derr);
    if (st != mdk::SniWaveStatus::kOk) {
      UtilityFunctions::printerr(
          "MdkBridge: SNI wave '", String(name.c_str()),
          "' decode failed: ", mdk::sniWaveStatusName(st).data(),
          " — ", derr.c_str());
      break;
    }
    e.def.volume = static_cast<int>((fld >> 16) & 0xffffu);
    e.def.rateHz = wv.rateHz;
    e.def.frames = static_cast<std::uint32_t>(wv.frames);
    e.def.loop = (flags & 0x1) != 0;
    Ref<AudioStreamWAV> wav;
    wav.instantiate();
    wav->set_format(wv.bitsPerSample == 8
                        ? AudioStreamWAV::FORMAT_8_BITS
                        : AudioStreamWAV::FORMAT_16_BITS);
    wav->set_stereo(false);
    wav->set_mix_rate(wv.rateHz);   // verbatim — no resampling
    PackedByteArray data;
    data.resize(static_cast<int64_t>(wv.pcm.size()));
    std::memcpy(data.ptrw(), wv.pcm.data(), wv.pcm.size());
    wav->set_data(data);
    if (e.def.loop) {
      wav->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
      wav->set_loop_begin(0);
      wav->set_loop_end(static_cast<int64_t>(wv.frames));
    }
    e.stream = wav;
    e.resolved = true;
    break;
  }
  const auto [it, inserted] =
      audioEntries_.emplace(name, std::move(e));
  return &it->second;
}

bool MdkBridge::audioResolve_(const std::string& name,
                              mdk::TraversalAudioSoundDef& def) {
  const AudioEntry_* e = audioEntry_(name);
  if (!e || !e->resolved) return false;
  def = e->def;
  return true;
}

// 0x20000 live-pos refresh — the original dereferences inst+0x10 (the
// owner position pointer). ownerKey IS that pointer in this process:
// DynamicObject storage is stable (std::list + retained records), so
// scanning finds it; a miss freezes the source at its last position.
bool MdkBridge::audioOwnerPos_(int cat, const void* key,
                               float pos[3]) {
  if (!rt_ || !key) return false;
  if (cat == static_cast<int>(mdk::TraversalAudioOwner::kPlayer)) {
    pos[0] = rt_->cs.pos[0];
    pos[1] = rt_->cs.pos[1];
    pos[2] = rt_->cs.pos[2];
    return true;
  }
  for (const auto& a : rt_->arenas) {
    if (!a) continue;
    for (const auto& up : a->dyn.storage) {
      if (up.get() == key) {
        pos[0] = up->pos[0];
        pos[1] = up->pos[1];
        pos[2] = up->pos[2];
        return true;
      }
    }
  }
  return false;
}

// Drain rt_->audioFx once per presented frame: apply the batch, run
// one mixer pass (FUN_004026f8 cadence) over the voice pool, then emit
// the player commands. The conversions to Godot units happen here —
// dB via the DS millibel table (FUN_0046c27c + the FUN_0040202c master
// scale), pan to the +-1 panner domain, pitch as freqHz/recRate.
Array MdkBridge::drain_audio_fx() {
  Array out;
  if (!rt_ || mode_ != 3) return out;
  // Listener — the mixer copies the 0x540bb0 view snapshot
  // (rt.camera.pose.basis), reads 0x540b58 (zoom), 0x49b6f0
  // (smoothed frame scalar), and the global mode select (byte1 of
  // 0x49ff58 — the scope paths write 2; 0x540ca0's nonzero phases
  // are exactly those paths' mirror).
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      audioListener_.m[r][c] = rt_->camera.pose.basis[r][c];
  audioListener_.zoom = rt_->camera.zoom;
  audioListener_.frame = timing_.smoothed;
  audioListener_.mode3d = (rt_->transitionPhase != 0);
  audioMixer_.setListener(audioListener_);

  const auto res = [this](const std::string& n,
                          mdk::TraversalAudioSoundDef& d) {
    return audioResolve_(n, d);
  };
  const auto posFn = [this](int cat, const void* key, float p[3]) {
    return audioOwnerPos_(cat, key, p);
  };
  for (const auto& ev : rt_->audioFx) audioMixer_.applyEvent(ev, res);
  rt_->audioFx.clear();
  audioMixer_.tick(lastDtSec_, posFn);

  std::vector<mdk::TraversalAudioCmd> cmds;
  audioMixer_.drain(cmds);
  for (const mdk::TraversalAudioCmd& c : cmds) {
    Dictionary d;
    d["id"] = int64_t(c.handle);
    d["name"] = String(c.name.c_str());
    switch (c.op) {
      case mdk::TraversalAudioCmdOp::kStart: {
        d["op"] = "start";
        const AudioEntry_* e = audioEntry_(c.name);
        if (e && e->stream.is_valid()) {
          d["stream"] = e->stream;
        }
        d["loop"] = c.loop;
        break;
      }
      case mdk::TraversalAudioCmdOp::kParams:
        d["op"] = "params";
        break;
      case mdk::TraversalAudioCmdOp::kStop:
        d["op"] = "stop";
        break;
    }
    if (c.op != mdk::TraversalAudioCmdOp::kStop) {
      // vol domain -> FUN_0040202c master scale -> mB -> dB.
      d["db"] = double(mdk::traversalAudioVolDb(
          mdk::traversalAudioScaledVol(c.vol, audioSfxPct_)));
      d["pan"] = double(mdk::traversalAudioPanUnit(c.pan));
      d["pitch"] = c.rateHz > 0
                       ? double(c.freqHz) / double(c.rateHz)
                       : 1.0;
    }
    out.push_back(d);
  }
  return out;
}

Dictionary MdkBridge::get_audio_stats() const {
  Dictionary d;
  d["active"] = int64_t(audioMixer_.activeCount());
  d["resolved"] = int64_t(audioMixer_.resolvedCount());
  d["missing"] = int64_t(audioMixer_.missingCount());
  d["pool_exhausted"] = int64_t(audioMixer_.poolExhaustedCount());
  d["cache"] = int64_t(audioEntries_.size());
  d["banks"] = int64_t(audioBanks_.size());
  return d;
}

// ---------------------------------------------------------------------------
// Phase 16B — Kurt sprite tables + texture cache
// ---------------------------------------------------------------------------

void MdkBridge::decodeKurtTables_() {
  kurt_ = KurtSprites{};
  if (!rt_) return;
  // Directory views over the level's two sprite banks. The slot
  // order is PlayerAnimTables' field order (playerAnimTableName);
  // the 23 traversal tables live in TRAVSPRT.BNI, the slide/surf
  // tables in the level's *S.SNI sentinel records — name lookups
  // mirror playerAnimBindTables' filters exactly.
  mdk::BniDirectory bd;
  if (!rt_->level.travsprtBytes.empty()) {
    bd = mdk::inspectBniDirectory(
        std::span<const std::byte>(rt_->level.travsprtBytes));
  }
  mdk::SniDirectory sd;
  if (!rt_->level.sniBytes.empty()) {
    sd = mdk::inspectSniDirectory(
        std::span<const std::byte>(rt_->level.sniBytes));
  }
  for (int i = 0; i < 29; ++i) {
    const char* nm = mdk::playerAnimTableName(i);
    std::span<const std::byte> span;
    // TRAVSPRT.BNI — FUN_004039ec returns payload+4; the table
    // (frame count at +0) runs to the record's payload end.
    if (bd.status == mdk::BniDirectoryStatus::kOk) {
      if (const mdk::BniRecord* r = mdk::findBniRecord(bd, nm)) {
        const auto* base = rt_->level.travsprtBytes.data();
        span = std::span<const std::byte>(
            base + r->payloadFileOffset + 4,
            r->payloadEnd - r->payloadFileOffset - 4);
      }
    }
    // LEVEL<n>S.SNI — FUN_004289a0's imageBase+ofs+4 sentinel
    // records carry no byte count; the span is bounded by the next
    // record's stored position or the 12-byte name trailer.
    if (span.empty() &&
        sd.status == mdk::SniDirectoryStatus::kOk) {
      for (const mdk::SniEntry& e : sd.entries) {
        if ((e.fieldAt0x0C & 0x8000u) == 0) continue;
        if (e.name() != nm) continue;
        const std::uint64_t start = e.payloadFileOffset() + 4;
        if (start > rt_->level.sniBytes.size()) break;
        std::uint64_t end = rt_->level.sniBytes.size();
        if (sd.trailerPresent && end >= 12) end -= 12;
        for (const mdk::SniEntry& o : sd.entries) {
          const std::uint64_t p = o.payloadFileOffset();
          if (p > start && p < end) end = p;
        }
        span = std::span<const std::byte>(
            rt_->level.sniBytes.data() + start, end - start);
        break;
      }
    }
    if (span.empty()) {
      ++kurt_->missing;   // table name absent — normal per level
      continue;
    }
    std::string err;
    auto sp = mdk::decodeSpriteTable(span, &err);
    if (!sp) {
      kurt_->errors[std::size_t(i)] = err;
      UtilityFunctions::printerr("MdkBridge: K_ table decode failed: ",
                                 nm, " — ", err.c_str());
      continue;
    }
    kurt_->tables[std::size_t(i)] = std::move(*sp);
    ++kurt_->decoded;
  }
}

const std::uint8_t* MdkBridge::activePalette_() const {
  // The displayed arena's composed palette, else the level fallback
  // (SYS_PAL head + region-B + DTI-s3 compose). Shared by the Kurt
  // sprite path, get_active_palette(), and the HUD expand.
  const std::uint8_t* pal = levelPalette_.data();
  if (arenaIndex_ >= 0) {
    if (auto it = arenaSets_.find(arenaIndex_);
        it != arenaSets_.end()) {
      pal = it->second->palette.data();
    }
  }
  return pal;
}

void MdkBridge::refreshKurtPalette_() {
  const std::uint8_t* pal = activePalette_();
  if (std::memcmp(pal, kurtPalette_.data(), 768) != 0) {
    std::memcpy(kurtPalette_.data(), pal, 768);
    kurtPalKey_ = fnvAppend(0xcbf29ce484222325ull, pal, 768);
  } else if (kurtPalKey_ == 0) {
    kurtPalKey_ = fnvAppend(0xcbf29ce484222325ull, pal, 768);
  }
}

Ref<ImageTexture> MdkBridge::kurtTexture_(int tableIdx, int frameIdx) {
  Ref<ImageTexture> tex;
  if (!kurt_ || tableIdx < 0 || tableIdx >= 29) return tex;
  const auto& tab = kurt_->tables[std::size_t(tableIdx)];
  if (!tab) return tex;
  const mdk::FtiSpriteFrame* f = tab->frame(std::size_t(frameIdx));
  if (!f || f->width == 0 || f->height == 0) return tex;

  const std::uint64_t key =
      (kurtPalKey_ << 32) |
      (std::uint64_t(std::uint32_t(tableIdx)) << 16) |
      std::uint64_t(std::uint32_t(frameIdx));
  if (auto it = kurtTex_.find(key); it != kurtTex_.end()) {
    return it->second;
  }
  if (kurtTex_.size() >= 4096) kurtTex_.clear();   // bounded cache

  // Expand the stream into palette-mapped RGBA — FUN_00415ff0's
  // semantics minus the framebuffer edge rules (the texture IS the
  // frame rectangle): literal bytes write nonzero, run packets fill
  // or skip, byte 0 is transparent, 0xfe next row, 0xff end. Packet
  // spill continues linearly into the next row — the stream is
  // trusted exactly as the original trusted it; writes are bounded
  // to the frame rectangle (the decoder proved 0xff termination).
  const int w = f->width, hgt = f->height;
  const std::size_t npix = std::size_t(w) * std::size_t(hgt);
  PackedByteArray px;
  px.resize(static_cast<int64_t>(npix) * 4);
  std::uint8_t* dst = px.ptrw();
  std::memset(dst, 0, npix * 4);
  const auto& s = f->stream;
  int x = 0, y = 0;
  for (std::size_t i = 0; i < s.size();) {
    const std::uint8_t cmd = s[i++];
    if (cmd == mdk::kFtiSpriteStreamEnd) break;
    if (cmd == mdk::kFtiSpriteRowBreak) {
      x = 0;
      ++y;
      continue;
    }
    if (cmd < 0x80) {   // literal packet: cmd+1 bytes follow
      const int n = cmd + 1;
      for (int k = 0; k < n && i < s.size(); ++k) {
        const std::uint8_t v = s[i++];
        const std::size_t pos = std::size_t(y) * w + x;
        if (v != 0 && pos < npix) {
          dst[pos * 4 + 0] = kurtPalette_[v * 3 + 0];
          dst[pos * 4 + 1] = kurtPalette_[v * 3 + 1];
          dst[pos * 4 + 2] = kurtPalette_[v * 3 + 2];
          dst[pos * 4 + 3] = 255;
        }
        ++x;
      }
      continue;
    }
    // run packet: cmd-0x7c copies of one value byte
    const int n = cmd - mdk::kFtiSpriteRunBase;
    const std::uint8_t v = i < s.size() ? s[i++] : 0;
    if (v != 0) {
      for (int k = 0; k < n; ++k) {
        const std::size_t pos = std::size_t(y) * w + x;
        if (pos < npix) {
          dst[pos * 4 + 0] = kurtPalette_[v * 3 + 0];
          dst[pos * 4 + 1] = kurtPalette_[v * 3 + 1];
          dst[pos * 4 + 2] = kurtPalette_[v * 3 + 2];
          dst[pos * 4 + 3] = 255;
        }
        ++x;
      }
    } else {
      x += n;           // transparent run — advance only
    }
  }
  Ref<Image> img = Image::create_from_data(
      w, hgt, false, Image::FORMAT_RGBA8, px);
  tex = ImageTexture::create_from_image(img);
  kurtTex_[key] = tex;
  return tex;
}

Dictionary MdkBridge::kurtFrameDict_(int tableIdx, int frameIdx) {
  Dictionary d;
  if (!kurt_ || tableIdx < 0 || tableIdx >= 29) return d;
  const auto& tab = kurt_->tables[std::size_t(tableIdx)];
  if (!tab) return d;
  const mdk::FtiSpriteFrame* f = tab->frame(std::size_t(frameIdx));
  if (!f) return d;
  d["table"] = tableIdx;
  d["table_name"] = mdk::playerAnimTableName(tableIdx);
  d["frame"] = frameIdx;
  d["frame_count"] = static_cast<int64_t>(tab->frames.size());
  d["w"] = f->width;
  d["h"] = f->height;
  d["hot_x"] = f->hotspotX;
  d["hot_y"] = f->hotspotY;
  // The whole-table digest is the frame's stable identity — a
  // presentation check can assert texture reuse from
  // {digest, frame, pal_key} alone.
  d["digest"] = static_cast<int64_t>(mdk::ftiSpriteDigest(*tab));
  d["tex"] = kurtTexture_(tableIdx, frameIdx);
  return d;
}

Dictionary MdkBridge::get_kurt_snapshot() {
  Dictionary out;
  if (!rt_ || !hasFrame_) return out;
  refreshKurtPalette_();
  // Every field below is a verbatim TraversalFrameResult copy — the
  // core already owns animation selection, the draw gate, the
  // anchor projection, the scale probe and the overlay offsets.
  out["drawn"] = last_.animDrawn;
  out["registered"] = last_.animRegistered;
  out["anchor_x"] = last_.animAnchorX;
  out["anchor_y"] = last_.animAnchorY;
  out["scope_ofs"] = last_.animScopeOfs;   // added to the blit y
  out["scale"] = last_.animScale;          // 0x540dbc probe result
  out["depth"] = last_.animDepth;          // view z' (0x540c1c)
  out["view_x"] = static_cast<double>(last_.animViewX);
  out["view_y"] = static_cast<double>(last_.animViewY);
  out["screen_x"] = static_cast<double>(last_.animScreenX);
  out["screen_y"] = static_cast<double>(last_.animScreenY);
  out["probe_y"] = static_cast<double>(last_.animProbeY);
  out["clip"] = last_.animClipFlags;       // 0x540c28
  out["table"] = last_.animTableIdx;
  out["frame"] = last_.animFrameIdx;
  out["anim_frame"] = last_.animFrame;     // 0x540cb4 counter
  out["anim_phase"] = static_cast<double>(last_.animPhase);
  out["loco_state"] = last_.locoState;
  out["ofs_x"] = last_.animOfsX;           // overlay jitter offsets
  out["ofs_y"] = last_.animOfsY;
  out["muzz_idx"] = last_.animMuzzIdx;     // muzzle parity index
  out["pal_key"] = static_cast<int64_t>(kurtPalKey_);
  out["tex_cache"] = static_cast<int64_t>(kurtTex_.size());
  if (kurt_) {
    out["decoded_tables"] = kurt_->decoded;
    out["missing_tables"] = kurt_->missing;
    Array errs;
    for (int i = 0; i < 29; ++i) {
      if (!kurt_->errors[std::size_t(i)].empty()) {
        errs.push_back(String(mdk::playerAnimTableName(i)) + ": " +
                       kurt_->errors[std::size_t(i)].c_str());
      }
    }
    out["table_errors"] = errs;
  }
  out["main"] = kurtFrameDict_(last_.animTableIdx, last_.animFrameIdx);
  out["overlay"] = kurtFrameDict_(last_.animOverlayTableIdx,
                                  last_.animOverlayFrameIdx);
  return out;
}

// QA action mask + raw input dictionary -> RawGameplayInput (the
// shared keyboard/mouse fold — mode-independent device state; the
// per-mode readers consume the channels they own).
mdk::RawGameplayInput MdkBridge::buildRawInput_(
    int64_t action_mask, const Dictionary* input) {
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
  return raw;
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

int MdkBridge::indexOfColArena_(const mdk::CollisionArena* col) const {
  if (col == nullptr || !rt_) return -1;
  for (const auto& a : rt_->arenas) {
    if (&a->dyn.col == col) return a->index;
  }
  return -1;
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
  // Mode routing (0x541492): mode 2 runs the freefall core, mode 3
  // (and the standalone load_level path) runs traversal.
  if (mode_ == 2) return stepFreefall_(dt_ms, action_mask, input);
  if (!rt_) {
    setError_("no level loaded");
    return out;
  }
  mdk::frontendTimingUpdate(timing_, dt_ms);
  lastDtSec_ = dt_ms / 1000.0;   // mixer playhead cadence
  const mdk::RawGameplayInput raw =
      buildRawInput_(action_mask, input);

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
  // The kill-plane threshold this object is subject to in
  // FUN_0045bac0: pos.z < home deepFloorZ - 200 (deepFloorZ = the
  // arena's real AABB minZ since Phase 15A — not a flat 0).
  d["floor_plane"] =
      static_cast<double>(o.arena ? o.arena->col.deepFloorZ - 200.0f
                                  : 0.0f);
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

Dictionary MdkBridge::diagnostic_damage(int64_t amount) {
  Dictionary out;
  if (!rt_) {
    setError_("no level loaded");
    return out;
  }
  // The exact producer the enemy/projectile/splash paths call —
  // FUN_0046771c. The dispatch tail consumes the accumulator on the
  // next step; nothing here touches loco/anim state.
  const float pt[3] = {rt_->cs.pos[0], rt_->cs.pos[1],
                       rt_->cs.pos[2]};
  mdk::playerDamageApply(*rt_, static_cast<int>(amount), pt);
  out["ok"] = true;
  out["health"] = static_cast<int64_t>(rt_->fieldHealth);
  out["accum"] = static_cast<double>(rt_->vert.landingAccum);
  out["suppress"] = static_cast<double>(rt_->fieldE10);
  out["fade"] = static_cast<int64_t>(rt_->fieldEb8);
  out["loco_state"] = static_cast<int64_t>(rt_->locoState);
  out["event_priority"] = static_cast<int64_t>(rt_->eventPriority);
  return out;
}

Dictionary MdkBridge::diagnostic_kill(int64_t object_id) {
  Dictionary out;
  out["ok"] = false;
  if (!rt_ || object_id <= 0) return out;
  const void* p = objIds_.find(std::uint64_t(object_id));
  if (p == nullptr) return out;   // stale/unknown id — not an error
  auto& o = *const_cast<mdk::DynamicObject*>(
      static_cast<const mdk::DynamicObject*>(p));
  // The authentic death boundary — FUN_004581a4's die-facing wrapper
  // -> FUN_00458140. The +0x110-script handoff or the FUN_00457cf4
  // teardown emits the corresponding combat event; the record wipe
  // is the core's own semantics, not the diagnostic's.
  const std::size_t before = rt_->combatFx.size();
  mdk::objectDieFacingPlayer(*rt_, o);
  out["ok"] = true;
  out["events"] = int64_t(rt_->combatFx.size() - before);
  if (!rt_->combatFx.empty()) {
    out["kind"] = int64_t(rt_->combatFx.back().kind);
  }
  return out;
}

Dictionary MdkBridge::diagnostic_shockwave(int64_t object_id) {
  Dictionary out;
  out["ok"] = false;
  if (!rt_ || object_id <= 0) return out;
  const void* p = objIds_.find(std::uint64_t(object_id));
  if (p == nullptr) return out;
  auto& o = *const_cast<mdk::DynamicObject*>(
      static_cast<const mdk::DynamicObject*>(p));
  // The real FUN_004575fc seam with the observed 2.0 arg (the shot
  // detonation callsite) — emits the kDetonation remnant event.
  const std::size_t before = rt_->combatFx.size();
  mdk::fxShockwave(*rt_, o, 2.0f);
  out["ok"] = true;
  out["events"] = int64_t(rt_->combatFx.size() - before);
  if (!rt_->combatFx.empty()) {
    out["kind"] = int64_t(rt_->combatFx.back().kind);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Phase 17A closeout — full save/restore
// ---------------------------------------------------------------------------

PackedByteArray MdkBridge::save_game_full() {
  PackedByteArray out;
  if (mode_ != 3 || !rt_) {
    setError_("save_game_full: no traversal session is live");
    return out;
  }
  mdk::SaveWriteFullInput in;   // seed 0, no thumbnail preview
  mdk::FullWriteReport rep;
  std::string detail;
  const auto bytes =
      mdk::saveGameWriteFull(*rt_, sess_, in, &rep, &detail);
  for (const std::string& w : rep.warnings)
    UtilityFunctions::printerr("MdkBridge: save write — ", w.c_str());
  if (bytes.empty()) {
    setError_("save write failed: " + detail);
    return out;
  }
  out.resize(static_cast<int64_t>(bytes.size()));
  std::memcpy(out.ptrw(), bytes.data(), bytes.size());
  return out;
}

Dictionary MdkBridge::restore_save(const PackedByteArray& bytes) {
  Dictionary out;
  out["ok"] = false;
  if (!root_) {
    setError_("restore_save: initialize() first");
    return out;
  }
  mdk::SaveGame sg;
  const mdk::SaveError pe = mdk::saveGameParse(
      reinterpret_cast<const std::byte*>(bytes.ptr()),
      std::size_t(bytes.size()), sg, true);
  out["parse"] = String(mdk::saveErrorName(pe));
  if (pe != mdk::SaveError::kOk) {
    setError_(std::string("restore parse: ") + mdk::saveErrorName(pe));
    return out;
  }
  if (!sg.game.full()) {
    setError_("restore_save: header-only save — no world packets");
    return out;
  }
  // The authoritative path — FUN_00427218 rebuilds the whole runtime
  // (fresh level load + packet application) inside `trav`.
  auto trav = std::make_unique<mdk::TraversalRuntime>();
  mdk::FullRestoreReport rep;
  std::string detail;
  const mdk::SaveError re = mdk::applyFullSaveToTraversal(
      sg, *root_, sess_, *trav, &rep, &detail);
  out["restore"] = String(mdk::saveErrorName(re));
  out["detail"] = String(detail.c_str());
  if (re != mdk::SaveError::kOk) {
    setError_("restore failed: " + detail);
    return out;   // the old session stays live — restore is atomic
  }
  // Install. The fresh runtime carries none of the discarded
  // session's transient state (combatFx, pending lists, stale
  // object identity) — the loader's own semantics.
  rt_ = std::move(trav);
  mode_ = 3;
  hasFrame_ = false;
  timing_ = mdk::FrontendTimingState{};
  last_ = mdk::TraversalFrameResult{};
  prevKeyLevel_ = {};
  ff_.reset();
  ffScene_.reset();
  ffTex_.clear();
  ffHandoffDone_ = false;
  ffHandoffRoute_ = -1;
  ffHandoffDetail_.clear();
  // Presentation state derived from the discarded runtime — ids
  // re-mint, arena sets re-parse, the display set rebinds from the
  // restored cur/partner.
  objIds_ = mdkfront::MdkObjectIds{};
  arenaSets_.clear();
  arenaSetFailed_.clear();
  displaySet_.clear();
  arenaIndex_ = -1;
  arenaName_.clear();
  arenaLoaded_ = false;
  const int dir = mdk::progressionLevelDir(sess_.levelId);
  char stemBuf[16], dirBuf[32];
  std::snprintf(stemBuf, sizeof(stemBuf), "LEVEL%d", dir);
  std::snprintf(dirBuf, sizeof(dirBuf), "TRAVERSE/LEVEL%d/", dir);
  if (!presentTraversalLevel_(stemBuf, dirBuf)) {
    // rt_ is authoritatively restored — only the presentation tail
    // degraded (missing shared-MTI/FTI bytes). Report, keep runtime.
    out["detail"] = String(lastError_.c_str());
    return out;
  }
  updateDisplaySet_();
  refreshOrders_();
  out["ok"] = true;
  out["mode"] = mode_;
  out["level_id"] = int64_t(sess_.levelId);
  out["level_dir"] = int64_t(dir);
  out["identity_ok"] = rep.identityOk;
  out["aren_applied"] = int64_t(rep.arenApplied);
  out["objects_allocated"] = int64_t(rep.objectsAllocated);
  out["shot_slots"] = int64_t(rep.shotSlots);
  out["shots_active"] = int64_t(rep.shotsActive);
  out["cur_arena_index"] = int64_t(rep.curArenaIndex);
  out["partner_arena_index"] = int64_t(rep.partnerArenaIndex);
  out["health"] = int64_t(rep.health);
  out["player_pos_mdk"] =
      Vector3(rep.playerPos[0], rep.playerPos[1], rep.playerPos[2]);
  out["warnings"] = int64_t(rep.warnings.size());
  return out;
}

// ---------------------------------------------------------------------------
// Phase 17A — traversal combat presentation
// ---------------------------------------------------------------------------

// FUN_0045e9a0's camera-block basis: view yaw/pitch -> right/down/back
// rows, bank 0 (same construction as the player camera's banked-up
// path at 0x4304ed..0x430594 with sinB=0/cosB=1). Only used for the
// shot bullet-cam pose.
static void shotCamRows_(float yawDeg, float pitchDeg,
                         float rows[3][4]) {
  constexpr float kRd = 3.14159265358979323846f / 180.0f;
  const float sy = std::sin(yawDeg * kRd);
  const float cy = std::cos(yawDeg * kRd);
  const float sp = std::sin(pitchDeg * kRd);
  const float cp = std::cos(pitchDeg * kRd);
  const float back[3] = {-sy * cp, -cy * cp, sp};
  const float up[3] = {sy * sp, cy * sp, cp};
  const float right[3] = {up[1] * back[2] - up[2] * back[1],
                          up[2] * back[0] - up[0] * back[2],
                          up[0] * back[1] - up[1] * back[0]};
  const float down[3] = {-(back[1] * right[2] - back[2] * right[1]),
                         -(back[2] * right[0] - back[0] * right[2]),
                         -(back[0] * right[1] - back[1] * right[0])};
  rows[0][0] = right[0]; rows[0][1] = right[1];
  rows[0][2] = right[2]; rows[0][3] = 0.0f;
  rows[1][0] = down[0];  rows[1][1] = down[1];
  rows[1][2] = down[2];  rows[1][3] = 0.0f;
  rows[2][0] = back[0];  rows[2][1] = back[1];
  rows[2][2] = back[2];  rows[2][3] = 0.0f;
}

Dictionary MdkBridge::get_shot_snapshots() const {
  Dictionary out;
  Array shots;
  out["shots"] = shots;
  if (!rt_) return out;
  out["scoped"] = mdk::playerShotRenderGate(*rt_);
  out["hud_active"] = rt_->hudActive != 0;
  // 0x54150c — the FX/debris-enable cheat flag: FUN_00437444's
  // select-0 shard pen is 3 when set, 0xd when clear.
  out["fx_enable"] = rt_->fxEnable != 0;
  const auto vis = mdk::playerShotVisuals(*rt_);
  for (const auto& v : vis) {
    Dictionary d;
    d["slot"] = int64_t(v.slot);
    d["state"] = int64_t(v.state);
    d["type"] = int64_t(v.type);
    d["class_idx"] = int64_t(v.classIdx);
    d["mesh_renderable"] = v.meshRenderable;
    d["window_active"] = v.worldRenderable;
    d["pos"] = mdkToGodotVec(v.pos);
    d["pos_mdk"] = Vector3(v.pos[0], v.pos[1], v.pos[2]);
    d["tail"] = mdkToGodotVec(v.tail);
    d["tail_mdk"] = Vector3(v.tail[0], v.tail[1], v.tail[2]);
    d["tail_len"] = double(v.tailLen);
    d["yaw_deg"] = double(v.yawDeg);
    d["pitch_deg"] = double(v.pitchDeg);
    d["billboard_yaw_deg"] = double(v.billboardYawDeg);
    d["billboard_pitch_deg"] = double(v.billboardPitchDeg);
    d["spin_deg"] = double(v.spinDeg);
    d["render_scalar"] = double(v.fieldCc);
    d["ribbon_bound"] = v.ribbonBound;
    d["hud_frame"] = int64_t(v.hudFrame);
    d["arena_index"] =
        v.arena ? indexOfArena_(&v.arena->dyn) : -1;
    // FUN_0045f8b8's model submit (0x431503 gate, OBSERVED): the
    // class record mesh placed by buildObjectMatrix — type 0 banks on
    // spinDeg at scale 1.0; types 1-4 fix bank 90 / pitch 0 at scale
    // 0.5; all yaw = yawDeg + 180 about pos.
    float xf[9], org[3];
    mdk::buildObjectMatrix(v.type == 0 ? v.spinDeg : 0.0f,
                           v.type == 0 ? 0.0f : 90.0f,
                           v.yawDeg + 180.0f,
                           v.type == 0 ? 1.0f : 0.5f, v.pos, xf, org);
    const mdkfront::Vec3 vorg{org[0], org[1], org[2]};
    d["mesh_transform"] = mdkToGodotObjectTransform(
        mdkfront::mdkTransformToGodot(xf, vorg));
    // FUN_0045e9a0's camera block: origin = tail, view basis from
    // (billboardYawDeg, billboardPitchDeg), scales {1.0, 2.0, -1.0}.
    // The 140x70 window's projector divisors {69.95, 34.95} give a
    // ~90x90 degree frustum — the 2:1 window is an anisotropic
    // squash, so GDScript renders a square SubViewport and stretches.
    float rows[3][4];
    shotCamRows_(v.billboardYawDeg, v.billboardPitchDeg, rows);
    d["cam_transform"] = Transform3D(
        mdkToGodotBasis(mdkfront::mdkCameraBasisToGodot(rows))
            .orthonormalized(),
        mdkToGodotVec(v.tail));
    shots.push_back(d);
  }
  return out;
}

Array MdkBridge::drain_combat_fx() {
  Array out;
  if (!rt_) return out;
  for (const auto& ev : rt_->combatFx) {
    Dictionary d;
    d["kind"] = int64_t(ev.kind);
    d["mode"] = int64_t(ev.mode);
    d["variant"] = int64_t(ev.variant);
    d["aux"] = int64_t(ev.aux);
    d["pos"] = mdkToGodotVec(ev.pos);
    d["pos_mdk"] = Vector3(ev.pos[0], ev.pos[1], ev.pos[2]);
    // Non-minting lookup: a torn-down subject's id may already be
    // re-minted or freed — 0 is always safe, never misleading.
    d["obj_id"] = int64_t(ev.obj ? objIds_.lookup(ev.obj) : 0u);
    d["arena_index"] = indexOfColArena_(ev.arena);
    d["scale"] = double(ev.scale);
    d["facing_deg"] = double(ev.facingDeg);
    d["bank_deg"] = double(ev.bankDeg);
    d["model_name"] = String(ev.modelName.c_str());
    // The remnant/corpse spawn (FUN_004575fc / the FUN_00457cf4
    // teardown path, OBSERVED): a dead EXPLODE-class object placed
    // at pos, yaw = facingDeg (+0x4c/+0x50), bank = bankDeg
    // (+0x13c — the camera-tilt), scale = ev.scale (+0x58). The
    // object build matrix is the same FUN_0046b2f8 convention.
    if (ev.kind == mdk::CombatFxKind::kDetonation ||
        ev.kind == mdk::CombatFxKind::kObjectTeardown) {
      float xf[9], org[3];
      mdk::buildObjectMatrix(0.0f, ev.bankDeg, ev.facingDeg,
                             ev.scale, ev.pos, xf, org);
      const mdkfront::Vec3 vorg{org[0], org[1], org[2]};
      d["transform"] = mdkToGodotObjectTransform(
          mdkfront::mdkTransformToGodot(xf, vorg));
    }
    out.push_back(d);
  }
  rt_->combatFx.clear();
  return out;
}

Dictionary MdkBridge::get_shot_geometry(int64_t class_idx) {
  Dictionary out;
  if (!rt_) return out;
  const mdk::RuntimeModel* m =
      mdk::traversalShotModel(rt_->level, int(class_idx));
  if (m == nullptr) return out;
  const ObjectGeometry g = objectGeometryFromModel(*m);
  out["mesh"] = g.mesh;
  out["surface_elems"] = g.surfaceElems;
  out["elem_names"] = g.elemNames;
  out["vert_count"] = g.vertCount;
  out["tri_count"] = g.triCount;
  out["elem_count"] = int64_t(m->elems.size());
  out["geom_key"] = int64_t(g.geomKey);
  out["model"] = String(m->modelName().c_str());
  return out;
}

Dictionary MdkBridge::get_named_geometry(const String& name) {
  Dictionary out;
  if (!rt_) return out;
  const mdk::RuntimeModel* m =
      mdk::traversalNamedModel(rt_->level, name.utf8().get_data());
  if (m == nullptr) return out;
  const ObjectGeometry g = objectGeometryFromModel(*m);
  out["mesh"] = g.mesh;
  out["surface_elems"] = g.surfaceElems;
  out["elem_names"] = g.elemNames;
  out["vert_count"] = g.vertCount;
  out["tri_count"] = g.triCount;
  out["elem_count"] = int64_t(m->elems.size());
  out["geom_key"] = int64_t(g.geomKey);
  out["model"] = String(m->modelName().c_str());
  return out;
}

PackedByteArray MdkBridge::get_active_palette() {
  PackedByteArray out;
  out.resize(768);
  // Same pick as refreshKurtPalette_(): the displayed arena's
  // composed palette, else the level fallback (SYS_PAL head +
  // region-B + DTI-s3 compose). The FUN_00437444 shard pens and the
  // HUD rectfill both index this active display palette.
  std::memcpy(out.ptrw(), activePalette_(), 768);
  return out;
}

Dictionary MdkBridge::fx_stab(const Vector3& from, const Vector3& to,
                              int64_t arena_index) {
  Dictionary out;
  if (!rt_) return out;
  // Godot->MDK is the inverse of mdkVecToGodot: (-gz, -gx, gy).
  const float f[3] = {-from.z, -from.x, from.y};
  const float t[3] = {-to.z, -to.x, to.y};
  // FUN_00406a0c's arena order (OBSERVED): the record's bound arena,
  // then 0x540c48 (cur) when different, then 0x540ca4 (partner) when
  // non-null, !0x540d3c (carrierBusy), and different from the bound.
  const mdk::CollisionArena* order[3] = {};
  int n = 0;
  mdk::TraversalArena* bound = arenaByIndex_(int(arena_index));
  const mdk::CollisionArena* boundCol =
      bound != nullptr ? &bound->dyn.col : nullptr;
  if (boundCol != nullptr) order[n++] = boundCol;
  if (rt_->cur != nullptr && &rt_->cur->dyn.col != boundCol)
    order[n++] = &rt_->cur->dyn.col;
  if (rt_->partner != nullptr && rt_->cs.carrierBusy == 0 &&
      &rt_->partner->dyn.col != boundCol &&
      (n == 0 || &rt_->partner->dyn.col != order[n - 1]))
    order[n++] = &rt_->partner->dyn.col;
  float hit[3];
  const mdk::CollisionPoly* poly = nullptr;
  for (int i = 0; i < n; ++i) {
    const mdk::CollisionNode* node =
        mdk::collisionStabFull(*order[i], f, t, hit, &poly);
    if (node == nullptr) continue;
    out["pos"] = mdkToGodotVec(hit);
    const float nm[3] = {node->nx, node->ny, node->nz};
    out["normal"] = mdkToGodotVec(nm);
    out["arena_index"] = int64_t(indexOfColArena_(order[i]));
    return out;
  }
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

// ---------------------------------------------------------------------------
// Phase 17B.2 — traversal HUD / view presentation
// ---------------------------------------------------------------------------
// The composed 600x360 pen buffer + the SNIPERS1 bezel, copied out
// verbatim. All composition is mdk_core's (traversalHudCompose runs
// inside the stepped frame); this function only folds state into
// copy-safe values and owns the two persistent expand textures.

Dictionary MdkBridge::get_hud_snapshot() {
  Dictionary out;
  if (mode_ != 3 || !rt_ || !hasFrame_ || !rt_->hud.bound) {
    return out;
  }
  const mdk::TraversalHudState& hud = rt_->hud;
  const mdk::IndexedFramebuffer& fb = hud.fb;
  const std::size_t npix = fb.pixelCount();
  const std::uint8_t* pal = activePalette_();
  const std::uint64_t palKey =
      fnvAppend(0xcbf29ce484222325ull, pal, 768);

  // Copy-out the pen buffer verbatim — the frontend never aliases
  // runtime memory.
  PackedByteArray pens;
  pens.resize(static_cast<int64_t>(npix));
  std::memcpy(pens.ptrw(), fb.pixels(), npix);
  out["fb"] = pens;
  out["w"] = int64_t(fb.width());
  out["h"] = int64_t(fb.height());
  int nz = 0;
  for (std::size_t i = 0; i < npix; ++i) nz += fb.pixels()[i] != 0;
  out["nz"] = int64_t(nz);
  // The identical fold mdk-inspect's `hud: dg=` prints — a given
  // runtime state cross-checks against the native diagnostic.
  const std::uint64_t dg = mdk::fnv1a64(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(fb.pixels()), npix));
  out["digest"] = static_cast<int64_t>(dg);
  out["pal_key"] = static_cast<int64_t>(palKey);

  // Palette-expanded overlay texture — pen 0 -> alpha 0 (the
  // overlay's transparency is the indexed contract). One Image +
  // one ImageTexture persist across calls; re-upload only when the
  // (content, palette) pair actually changed.
  const std::uint64_t texKey = dg ^ (palKey * 0x9e3779b97f4a7c15ull);
  if (texKey != hudTexKey_ || hudTex_.is_null()) {
    PackedByteArray px;
    px.resize(static_cast<int64_t>(npix) * 4);
    std::uint8_t* dst = px.ptrw();
    for (std::size_t i = 0; i < npix; ++i) {
      const std::uint8_t v = fb.pixels()[i];
      if (v == 0) continue;              // RGBA stays 0 -> alpha 0
      dst[i * 4 + 0] = pal[v * 3 + 0];
      dst[i * 4 + 1] = pal[v * 3 + 1];
      dst[i * 4 + 2] = pal[v * 3 + 2];
      dst[i * 4 + 3] = 255;
    }
    if (hudImage_.is_null()) {
      hudImage_ = Image::create_from_data(fb.width(), fb.height(),
                                          false, Image::FORMAT_RGBA8,
                                          px);
    } else {
      hudImage_->set_data(fb.width(), fb.height(), false,
                          Image::FORMAT_RGBA8, px);
    }
    if (hudTex_.is_null()) {
      hudTex_ = ImageTexture::create_from_image(hudImage_);
    } else {
      hudTex_->update(hudImage_);
    }
    hudTexKey_ = texKey;
    ++hudTexUploads_;
  }
  out["tex"] = hudTex_;
  // Boundedness counters — the texture object persists across
  // uploads; this serial only advances on a real re-upload, never
  // on a snapshot call with unchanged content.
  out["tex_uploads"] = hudTexUploads_;

  // View/scope gates — verbatim core state, no derived logic.
  out["scoped"] = mdk::playerShotRenderGate(*rt_);
  out["sniper_view"] =
      rt_->flagC9c != 0 && rt_->transitionPhase != 0;
  out["hud_active"] = rt_->hudActive != 0;
  const mdk::PlayerCameraPose& pose =
      hasFrame_ ? last_.camera : rt_->camera.pose;
  out["view_rect"] =
      Rect2i(pose.viewOX, pose.viewOY, pose.viewW, pose.viewH);
  out["scope_rect"] = Rect2i(mdk::kHudScopeX, mdk::kHudScopeY,
                             mdk::kHudScopeW, mdk::kHudScopeH);
  // shotWinFill — FUN_0045ee7c's mode-1 per-slot indicator select
  // (the window pen fills are a documented deferred seam; the
  // frontend draws them from this channel).
  {
    PackedInt32Array wf;
    wf.resize(3);
    const auto vis = mdk::playerShotVisuals(*rt_);
    for (int i = 0; i < 3; ++i) {
      wf[i] = static_cast<int32_t>(vis[std::size_t(i)].hudFrame);
    }
    out["win_fill"] = wf;
  }

  // SNIPERS1 — the 640x480 scope bezel verbatim (raw indexed buffer
  // at payload+0, the FUN_004039c8 bind). Presented beneath the fb
  // content; opaque — the fb-space layers above own the apertures.
  if (!hud.bezelPx.empty()) {
    Dictionary bz;
    bz["w"] = int64_t(mdk::kHudBezelW);
    bz["h"] = int64_t(mdk::kHudBezelH);
    PackedByteArray bpx;
    bpx.resize(static_cast<int64_t>(hud.bezelPx.size()));
    std::memcpy(bpx.ptrw(), hud.bezelPx.data(), hud.bezelPx.size());
    bz["px"] = bpx;
    const std::uint64_t bk = mdk::fnv1a64(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(hud.bezelPx.data()),
        hud.bezelPx.size()));
    bz["key"] = static_cast<int64_t>(bk);
    bz["fb_ofs"] = Vector2i(mdk::kHudBezelFbOfsX, mdk::kHudBezelFbOfsY);
    const std::uint64_t bkey = bk ^ (palKey * 0x9e3779b97f4a7c15ull);
    if (bkey != bezelTexKey_ || bezelTex_.is_null()) {
      const std::size_t bnp = hud.bezelPx.size();
      PackedByteArray px;
      px.resize(static_cast<int64_t>(bnp) * 4);
      std::uint8_t* dst = px.ptrw();
      for (std::size_t i = 0; i < bnp; ++i) {
        const std::uint8_t v = hud.bezelPx[i];
        dst[i * 4 + 0] = pal[v * 3 + 0];
        dst[i * 4 + 1] = pal[v * 3 + 1];
        dst[i * 4 + 2] = pal[v * 3 + 2];
        dst[i * 4 + 3] = 255;
      }
      if (bezelImage_.is_null()) {
        bezelImage_ = Image::create_from_data(
            mdk::kHudBezelW, mdk::kHudBezelH, false,
            Image::FORMAT_RGBA8, px);
      } else {
        bezelImage_->set_data(mdk::kHudBezelW, mdk::kHudBezelH, false,
                              Image::FORMAT_RGBA8, px);
      }
      if (bezelTex_.is_null()) {
        bezelTex_ = ImageTexture::create_from_image(bezelImage_);
      } else {
        bezelTex_->update(bezelImage_);
      }
      bezelTexKey_ = bkey;
      ++bezelTexUploads_;
    }
    bz["tex"] = bezelTex_;
    bz["tex_uploads"] = bezelTexUploads_;
    out["bezel"] = bz;
  }

  // Verbatim scalar echoes — diagnostics/tests read these to prove
  // the presented pixels move with core state; presentation draws
  // only the composed fb.
  out["health"] = int64_t(rt_->fieldHealth);
  out["field_dac"] = int64_t(rt_->fieldDac);
  out["field_eb8"] = int64_t(rt_->fieldEb8);
  out["loco_state"] = int64_t(rt_->locoState);
  out["wpn0"] = int64_t(rt_->wpnSel0);
  out["wpn1"] = int64_t(rt_->wpnSel1);
  {
    PackedInt32Array am;
    am.resize(6);
    for (int i = 0; i < 6; ++i)
      am[i] = static_cast<int32_t>(rt_->ammo[std::size_t(i)]);
    out["ammo"] = am;
  }
  out["inv_count"] = int64_t(rt_->inventoryCount);
  out["inv_sel"] = int64_t(rt_->inventorySel);
  out["inv_timer"] = int64_t(rt_->invHudTimer);
  out["timer"] = static_cast<double>(rt_->fadeTimer5414a0);
  out["timer_max"] = static_cast<double>(rt_->fadeTimer5414a4);
  out["timer_latch"] = static_cast<double>(rt_->fadeTimer5414a8);
  out["level_id"] = int64_t(rt_->field541498);
  return out;
}

// ---------------------------------------------------------------------------
// Phase 16C — freefall (mode 2)
// ---------------------------------------------------------------------------

bool MdkBridge::load_freefall(int64_t course, int64_t skill,
                              int64_t seed) {
  if (!root_) {
    setError_("initialize() first");
    return false;
  }
  shutdown();   // drops rt_/ff_/buffers on reload; root_ is kept
  if (course < 0 || course > 4 || skill < 0 || skill > 2) {
    setError_("load_freefall: course 0..4, skill 0..2");
    return false;
  }

  // FUN_0040ef28's presentation loads — BNI records, the per-course
  // MTI material bank, the FALLP palette, the FALLPU pickup list.
  ffScene_ = std::make_unique<mdk::FreefallScene>();
  std::string detail;
  const auto se = mdk::freefallSceneLoad(*root_, int(course),
                                         ffScene_.get(), &detail);
  if (se != mdk::FreefallSceneError::kOk) {
    setError_(std::string("freefall scene load failed: ") +
              mdk::freefallSceneErrorName(se) + " — " + detail);
    ffScene_.reset();
    return false;
  }

  ff_ = std::make_unique<mdk::FreefallRuntime>();
  mdk::FreefallCourseData data;
  data.course = int(course);
  data.skill = int(skill);
  data.pickups = ffScene_->pickups;
  // explodeAnimFrames keeps the runtime default — the EXPLODE
  // model-slot anims-table entry (+0xc >> 16) is an undecoded seam
  // (mdk-inspect's digest runs use the same default).
  mdk::freefallInit(*ff_, data, std::uint32_t(seed));

  // The orchestrator globals: levelId IS the freefall course (541498
  // selects both FALL3D_<c+1> and the traversal 0x4999e8 table); the
  // LCG state is the shared stream the handoff syncs back out.
  sess_ = mdk::ProgressionSession{};
  sess_.levelId = int(course);
  sess_.skill = int(skill);
  sess_.health = 100;
  sess_.rng = std::uint32_t(seed);
  mdk::progressionEnterFreefall(sess_);

  timing_ = mdk::FrontendTimingState{};
  prevKeyLevel_ = {};
  ffHandoffDone_ = false;
  ffHandoffRoute_ = -1;
  ffHandoffDetail_.clear();
  ffTex_.clear();
  mode_ = 2;
  hasFrame_ = false;
  return true;
}

Dictionary MdkBridge::stepFreefall_(double dt_ms,
                                    int64_t action_mask,
                                    const Dictionary* input) {
  Dictionary out;
  if (!ff_ || !ffScene_) {
    setError_("load_freefall() first");
    return out;
  }
  mdk::frontendTimingUpdate(timing_, dt_ms);
  const mdk::RawGameplayInput raw =
      buildRawInput_(action_mask, input);

  // Mode-2 input — FUN_00407e50's pre-fold state: the bound
  // direction keys (KeyLeft/Right/Up/Down — the same keyboard
  // globals the traversal reader uses) plus the analog axes. The
  // dict path may override the digital channels with explicit
  // booleans and supply "axis_x"/"axis_y" (0x54b538/3c) directly.
  mdk::FreefallInput fi{};
  const auto held = [&](int slot) -> bool {
    const int gi = mdk::kKeyboardSlotToGlobal[slot];
    const int code = bindings_.keys[gi];
    return code > 0 && code < mdk::kGameplayKeyCount &&
           ((raw.keyLevel[code >> 5] >> (code & 31)) & 1u) != 0;
  };
  fi.left = held(0);
  fi.right = held(1);
  fi.up = held(2);
  fi.down = held(3);
  if (input != nullptr) {
    fi.left = bool(input->get("left", Variant(fi.left)));
    fi.right = bool(input->get("right", Variant(fi.right)));
    fi.up = bool(input->get("up", Variant(fi.up)));
    fi.down = bool(input->get("down", Variant(fi.down)));
    fi.axisX = float(double(input->get("axis_x", 0.0)));
    fi.axisY = float(double(input->get("axis_y", 0.0)));
  }

  const bool done = mdk::freefallStep(*ff_, fi, timing_.frameStep,
                                      timing_.smoothed,
                                      timing_.deltaSec);
  // Presentation twins — stepped with the SAME dtSec the gameplay
  // step consumed (objectAnimTickDt's rate*animRate*dtSec ==
  // frameUnits for the rate-1.0 records).
  mdk::freefallSceneStep(*ffScene_, *ff_, timing_.deltaSec);
  if (done && !ffHandoffDone_) freefallHandoff_();

  out["mode"] = mode_;
  out["done"] = done;
  out["phase"] = int64_t(ff_->phase);
  out["timeline"] = double(ff_->timeline);
  out["health"] = int64_t(ff_->health);
  out["fade"] = double(ff_->fade);
  out["camera"] = get_freefall_snapshot()["camera"];
  if (ffHandoffDone_) {
    out["handoff_route"] = ffHandoffRoute_;
    out["handoff_detail"] = String(ffHandoffDetail_.c_str());
  }
  Dictionary inp;
  inp["left"] = fi.left;
  inp["right"] = fi.right;
  inp["up"] = fi.up;
  inp["down"] = fi.down;
  inp["axis_x"] = fi.axisX;
  inp["axis_y"] = fi.axisY;
  out["input"] = inp;
  return out;
}

void MdkBridge::freefallHandoff_() {
  ffHandoffDone_ = true;
  if (!ff_ || !root_) return;
  auto trav = std::make_unique<mdk::TraversalRuntime>();
  mdk::ProgressionHandoff ho;
  std::string detail;
  const auto e = mdk::progressionFreefallHandoff(
      *root_, sess_, *ff_, trav.get(), ho, &detail);
  ffHandoffDetail_ = detail;
  if (e != mdk::ProgressionError::kOk) {
    setError_(std::string("freefall handoff: ") +
              mdk::progressionErrorName(e) + " — " + detail);
    return;   // mode stays 2 — the terminal frame keeps presenting
  }
  ffHandoffRoute_ = static_cast<int>(ho.route);
  if (ho.route != mdk::ProgressionRoute::kTraversal) {
    mode_ = 0;   // frontend route — death (health <= 0)
    return;
  }
  // Traversal route — FUN_004346e8/FUN_00433d40 already ran inside
  // the handoff (the runtime is loaded with health/rng/ammo
  // carried). Install it and run the same presentation tail
  // load_level uses so arena sets/materials/Kurt tables exist.
  rt_ = std::move(trav);
  mode_ = 3;
  hasFrame_ = false;
  // ho.dtiPath = TRAVERSE/LEVEL<n>/LEVEL<n>.DTI — stem/dir derive
  // the same way load_level parses them.
  const std::string& dti = ho.dtiPath;
  const auto slash = dti.find_last_of("/\\");
  const auto dot = dti.find_last_of('.');
  const std::size_t nameOff = slash == std::string::npos ? 0 : slash + 1;
  const std::string stem = dti.substr(nameOff, dot - nameOff);
  const std::string dir = slash == std::string::npos
                              ? "" : dti.substr(0, slash + 1);
  if (presentTraversalLevel_(stem, dir)) {
    updateDisplaySet_();
    refreshOrders_();
  }
}

Dictionary MdkBridge::get_freefall_snapshot() {
  Dictionary out;
  if (!ff_ || !ffScene_) return out;
  const mdk::FreefallRuntime& f = *ff_;
  out["mode"] = mode_;
  out["phase"] = int64_t(f.phase);        // 0 intro · 1 play ·
                                        // 2 dead · 3 done
  out["finished"] = f.finished;
  out["died"] = f.died;
  out["intro_countdown"] = int64_t(f.introCountdown);
  out["intro_progress"] = double(f.introProgress);
  out["zoom_frame"] = int64_t(f.zoomFrame);   // ZOOM sprite idx seam
  out["zoom_sub"] = int64_t(f.zoomSub);
  out["timeline"] = double(f.timeline);
  out["health"] = int64_t(f.health);
  out["fade"] = double(f.fade);           // palette-bright factor:
                                          // 1 full · 0 black · >1
                                          // damage flash
  out["fade_target"] = double(f.fadeTarget);
  out["fade_rate"] = double(f.fadeRate);
  out["palette_cycle"] = double(f.palette);
  out["radar_timer"] = int64_t(f.radarTimer);
  out["pickup_timer"] = int64_t(f.pickupTimer);
  out["missile_timer"] = int64_t(f.missileTimer);
  out["missile_budget"] = int64_t(f.missileBudget);
  out["pickups_remaining"] = int64_t(f.pickupsRemaining);
  out["bones_course"] = f.bonesCourse;
  out["finish_latch"] = f.finishLatch;
  out["course"] = int64_t(f.course);
  out["skill"] = int64_t(f.skill);
  out["list_head"] = int64_t(f.listHead);
  out["bones_idx"] = int64_t(f.bonesIdx);
  out["events_total"] = int64_t(f.events.size());
  out["handoff_done"] = ffHandoffDone_;
  out["handoff_route"] = ffHandoffRoute_;

  // The FALLP_<course+1> palette bound at 0x4edc28 — pen/index
  // materials and GDScript debug views resolve colors through it.
  PackedByteArray pal;
  pal.resize(768);
  std::memcpy(pal.ptrw(), ffScene_->palette.data(), 768);
  out["palette"] = pal;
  out["palette_ok"] = ffScene_->paletteOk;

  // FUN_004123f4's camera block, converted. The pose rows are the
  // [right,down,back] convention (mdk_math.h): the raw M2 row2
  // (0,0,-1) is +viewdir because the freefall scaleZ is +1, so the
  // semantic back row is +Z_mdk = (0,0,1). The zoom global 0x540b58
  // has no mode-2 writer in BUILD_A — the boot value 2.4 stands.
  const float zoom = 2.4f;
  mdk::PlayerCameraPose p{};
  p.pos[0] = f.cameraPos[0];
  p.pos[1] = f.cameraPos[1];
  p.pos[2] = f.cameraPos[2];
  p.basis[0][0] = 1.0f;  p.basis[0][1] = 0.0f;  p.basis[0][2] = 0.0f;
  p.basis[0][3] = -f.cameraPos[0];
  p.basis[1][0] = 0.0f;  p.basis[1][1] = -1.0f; p.basis[1][2] = 0.0f;
  p.basis[1][3] = f.cameraPos[1];
  p.basis[2][0] = 0.0f;  p.basis[2][1] = 0.0f;  p.basis[2][2] = 1.0f;
  p.basis[2][3] = -f.cameraPos[2];
  p.scaleX = 1.0f / (zoom * 0.5f);
  p.scaleY = 1.0f / (zoom * 0.3f);
  p.scaleZ = 1.0f;
  p.viewW = 600;
  p.viewH = 360;
  p.viewCX = 300;
  p.viewCY = 180;
  p.viewOX = 0;
  p.viewOY = 0;
  out["camera"] = mdkToGodotCameraTransform(p);
  out["camera_pos"] = mdkToGodotVec(p.pos);
  out["camera_pos_mdk"] =
      Vector3(f.cameraPos[0], f.cameraPos[1], f.cameraPos[2]);
  out["camera_aim_mdk"] =
      Vector3(f.camX, f.camY, f.camZ);      // 0x4ce69c block
  const float xDiv = float(p.viewW) * 0.4999f;
  const float yDiv = float(p.viewH) * 0.5011f;
  out["fov_deg"] = mdkfront::mdkCameraFovYDeg(
      p.scaleY, float(p.viewH), yDiv);
  out["aspect"] = mdkfront::mdkCameraAspect(
      p.scaleX, p.scaleY, float(p.viewW), float(p.viewH), xDiv, yDiv);
  out["scale_x"] = p.scaleX;
  out["scale_y"] = p.scaleY;
  out["scale_z"] = p.scaleZ;
  out["zoom"] = double(zoom);

  // The player record — pool[listHead] is both list anchor and
  // player handle (0x4edaec).
  const mdk::FreefallObject* pl =
      f.listHead >= 0 ? &f.pool[std::size_t(f.listHead)] : nullptr;
  if (pl != nullptr) {
    Dictionary pd;
    pd["pool_slot"] = int64_t(f.listHead);
    pd["pos"] = mdkToGodotVec(&pl->px);
    pd["pos_mdk"] = Vector3(pl->px, pl->py, pl->pz);
    pd["vel_mdk"] = Vector3(pl->vx, pl->vy, pl->vz);
    pd["yaw_deg"] = double(pl->yaw);
    pd["roll_deg"] = double(pl->roll);
    pd["scale"] = double(pl->scale);
    pd["anim_handle"] = int64_t(pl->animHandle);
    pd["anim_acc"] = double(pl->animAcc);
    pd["anim_frame"] = int64_t(pl->animFrame);
    pd["anim_sentinel"] = int64_t(pl->animSentinel);
    pd["alive"] = pl->alive != 0;
    out["player"] = pd;
  }
  return out;
}

Array MdkBridge::get_freefall_object_snapshots() {
  Array out;
  if (!ff_ || !ffScene_) return out;
  // The FUN_004109d8 entry domain: the active list walked from
  // listHead. Depth sorting is the software renderer's painter
  // algorithm — Godot's depth buffer makes it redundant, so the
  // snapshots keep list order.
  for (int i = ff_->listHead; i >= 0; i = ff_->pool[i].next) {
    if (i >= 399) break;   // corrupt-link hardening
    const mdk::FreefallObject& o = ff_->pool[std::size_t(i)];
    const int slot = mdk::freefallObjectModelSlot(*ff_, o);
    const mdk::FreefallScene::Twin* t =
        mdk::freefallSceneTwin(*ffScene_, i);
    Dictionary d;
    d["pool_slot"] = int64_t(i);
    d["type"] = int64_t(o.type);            // 0..5 dispatch
    d["alive"] = o.alive != 0;
    d["model"] = int64_t(o.model);          // FreefallModelTag
    d["model_slot"] = int64_t(slot);
    d["pos"] = mdkToGodotVec(&o.px);
    d["pos_mdk"] = Vector3(o.px, o.py, o.pz);
    d["vel_mdk"] = Vector3(o.vx, o.vy, o.vz);
    d["yaw_deg"] = double(o.yaw);
    d["roll_deg"] = double(o.roll);
    d["scale"] = double(o.scale);
    d["anim_handle"] = int64_t(o.animHandle);
    d["anim_acc"] = double(o.animAcc);
    d["anim_frame"] = int64_t(o.animFrame);
    d["anim_sentinel"] = int64_t(o.animSentinel);
    d["flags148"] = int64_t(o.flags);
    d["timer"] = int64_t(o.timer);
    d["sub_timer"] = int64_t(o.subTimer);
    d["pickup_rec"] = int64_t(o.pickupRec);
    // The kind-4 trail / kind-3 BANG gates — state only (the FX
    // renders stay documented seams).
    d["trail_fx"] = int64_t(o.fx);
    d["explode_flag"] = int64_t(o.explodeFlag);
    // +0x306 — the chute attachment entry (second kind-2 under the
    // same object basis; its geometry comes from part 1).
    d["chute"] = o.chute != 0;
    d["presented"] = t != nullptr;
    if (t != nullptr) {
      const mdk::DynamicObject& tw = t->obj;
      const mdkfront::Vec3 org = {tw.col.origin[0], tw.col.origin[1],
                                  tw.col.origin[2]};
      d["transform"] = mdkToGodotObjectTransform(
          mdkfront::mdkTransformToGodot(tw.col.xform, org));
      d["geom_key"] = int64_t(objectGeomKey(tw.model));
      // The driver's own outputs — distinct from the runtime's
      // gameplay-side animFrame (the twin is the presented frame).
      d["drv_anim_frame"] = int64_t(tw.animFrame);
      d["drv_anim_latch"] = int64_t(tw.animLatch);
      d["model_name"] =
          String(tw.model.modelName().c_str());
      d["elem_count"] = int64_t(tw.model.elems.size());
      std::int64_t vc = 0, tc = 0;
      for (std::size_t e = 0; e < tw.model.elems.size(); ++e) {
        vc += int64_t(tw.model.elemVerts[e].size() / 3);
        tc += int64_t(tw.model.elemTris[e].size() / 0x24);
      }
      d["vert_count"] = vc;
      d["tri_count"] = tc;
      if (o.chute != 0) {
        const mdk::RuntimeModel* cm =
            mdk::freefallSceneChuteModel(*ffScene_);
        if (cm != nullptr) {
          d["chute_geom_key"] = int64_t(objectGeomKey(*cm));
        }
      }
    }
    out.push_back(d);
  }
  return out;
}

Dictionary MdkBridge::get_freefall_object_geometry(
    int64_t pool_slot, int64_t part) {
  Dictionary out;
  if (!ff_ || !ffScene_ || pool_slot < 0 || pool_slot >= 399) {
    return out;
  }
  const mdk::RuntimeModel* m = nullptr;
  std::string tag;
  if (part == 1) {
    // The +0x306 chute attachment — a kind-2 entry exists only while
    // the object's chute flag is set; it renders under the object's
    // own basis (no separate transform).
    if (ff_->pool[std::size_t(pool_slot)].chute == 0) return out;
    m = mdk::freefallSceneChuteModel(*ffScene_);
    tag = "CHUTE";
  } else {
    const mdk::FreefallScene::Twin* t =
        mdk::freefallSceneTwin(*ffScene_, int(pool_slot));
    if (t == nullptr) return out;
    m = &t->obj.model;
    tag = m->modelName();
  }
  if (m == nullptr) return out;
  const FreefallGeometry g = freefallGeometryFromModel(*m);
  out["mesh"] = g.mesh;
  out["surface_elems"] = g.surfaceElems;
  out["surface_mats"] = g.surfaceMats;
  out["surface_mat_idx"] = g.surfaceMatIdx;
  out["surface_pen"] = g.surfacePenIdx;
  out["elem_names"] = g.elemNames;
  out["vert_count"] = g.vertCount;
  out["tri_count"] = g.triCount;
  out["elem_count"] = int64_t(m->elems.size());
  out["geom_key"] = int64_t(g.geomKey);
  out["model"] = String(tag.c_str());
  return out;
}

Dictionary MdkBridge::get_freefall_material(const String& name) {
  Dictionary out;
  if (!ffScene_) return out;
  const std::string nm = std::string(name.utf8().get_data());
  const mdk::FreefallMaterial* m =
      mdk::freefallSceneMaterial(*ffScene_, nm);
  if (m == nullptr) return out;
  out["name"] = name;
  out["valid"] = m->valid;
  out["palette_index"] = int64_t(m->paletteIndex);
  if (m->paletteIndex >= 0) {
    // Flat pen / index record — the color is the palette entry.
    // paletteIndex 256 = NONE (the original's no-draw flat-0xff).
    if (m->paletteIndex < 256 && ffScene_->paletteOk) {
      const std::uint8_t* p =
          ffScene_->palette.data() + std::size_t(m->paletteIndex) * 3;
      out["palette_color"] =
          Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f);
    }
    out["no_draw"] = m->paletteIndex >= 256;
    return out;
  }
  out["w"] = int64_t(m->width);
  out["h"] = int64_t(m->height);
  out["frames"] = int64_t(m->frameCount);
  out["tex"] = freefallTexture_(*m);
  return out;
}

Ref<ImageTexture> MdkBridge::freefallTexture_(
    const mdk::FreefallMaterial& m) {
  Ref<ImageTexture> tex;
  if (m.pixels.empty() || m.width <= 0 || m.height <= 0) return tex;
  if (auto it = ffTex_.find(m.name); it != ffTex_.end()) {
    return it->second;
  }
  // Frame 0 of the indexed strip — palette-mapped RGBA (the FALLP
  // bank is fixed for the loaded course, so the name key suffices).
  const std::size_t npix =
      std::size_t(m.width) * std::size_t(m.height);
  PackedByteArray px;
  px.resize(static_cast<int64_t>(npix) * 4);
  std::uint8_t* dst = px.ptrw();
  for (std::size_t i = 0; i < npix; ++i) {
    const std::uint8_t v = m.pixels[i];
    if (v == 0 || !ffScene_->paletteOk) {
      dst[i * 4 + 3] = v == 0 ? 0 : 255;   // index 0 = transparent
      if (v != 0) {
        dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = v;
      }
      continue;
    }
    dst[i * 4 + 0] = ffScene_->palette[v * 3 + 0];
    dst[i * 4 + 1] = ffScene_->palette[v * 3 + 1];
    dst[i * 4 + 2] = ffScene_->palette[v * 3 + 2];
    dst[i * 4 + 3] = 255;
  }
  Ref<Image> img = Image::create_from_data(
      m.width, m.height, false, Image::FORMAT_RGBA8, px);
  tex = ImageTexture::create_from_image(img);
  ffTex_[m.name] = tex;
  return tex;
}

void MdkBridge::shutdown() {
  arenaLoaded_ = false;
  arenaIndex_ = -1;
  arenaName_.clear();
  arenaSets_.clear();
  arenaSetFailed_.clear();
  displaySet_.clear();
  objIds_ = mdkfront::MdkObjectIds{};
  kurt_.reset();
  kurtTex_.clear();
  kurtPalette_ = {};
  levelPalette_ = {};
  kurtPalKey_ = 0;
  // Phase 17B.2 — HUD/bezel expand cache: the textures keyed on the
  // old runtime's content die with the level/session.
  hudImage_.unref();
  hudTex_.unref();
  hudTexKey_ = 0;
  hudTexUploads_ = 0;
  bezelImage_.unref();
  bezelTex_.unref();
  bezelTexKey_ = 0;
  bezelTexUploads_ = 0;
  sharedMtiBytes_.clear();
  ftiBytes_.clear();
  sysPalHead_ = {};
  prevKeyLevel_ = {};
  // Phase 17C.2 — the voice pool, banks, and stream cache all die
  // with the session (same boundary as the Kurt/HUD caches).
  audioBanks_.clear();
  audioBankStore_.clear();
  audioEntries_.clear();
  audioMixer_.reset();
  lastDtSec_ = 0.0;
  rt_.reset();
  ff_.reset();
  ffScene_.reset();
  sess_ = mdk::ProgressionSession{};
  ffHandoffDone_ = false;
  ffHandoffRoute_ = -1;
  ffHandoffDetail_.clear();
  ffTex_.clear();
  mode_ = 0;
  hasFrame_ = false;
  lastError_.clear();
}

// ---------------------------------------------------------------------------
// Phase 18B.1 — frontend host services
// ---------------------------------------------------------------------------

bool MdkBridge::frontend_boot(const String& save_dir) {
  if (!root_) {
    setError_("frontend_boot: initialize() first");
    return false;
  }
  // The caller picks the writable save root — production passes the
  // runtime's SAVES dir, tests pass a temp dir. The data root stays
  // read-only (the MISC\MDKS_* slide probe).
  feHost_ = std::make_unique<mdk::FrontendHostServices>(
      std::string(save_dir.utf8().get_data()));
  feHost_->setDataRoot(&*root_);
  feShell_ = std::make_unique<mdk::FrontendShell>(feHost_->makeSeams(
      [this] { return produceFrontendSave_(); }));
  // The ctor ran the FUN_0041d85c(0) fresh entry; mirror the live
  // host globals (0x541492 domain + the running flag — the app loop
  // is live whenever this bridge is pumped).
  feShell_->setPrimaryMode(mode_);
  feShell_->setRunning(true);
  feShell_->setLevelIndex(sess_.levelId);
  // Phase 18B.2A: decode the shared frontend resources once — the
  // same loader the SDL app runs (core/frontend_resources.h).
  std::string err;
  if (!mdk::loadFrontendResources(*root_, feRes_, &err)) {
    setError_("frontend_boot: " + err);
    feResLoaded_ = false;
    return false;
  }
  feResLoaded_ = true;
  return true;
}

bool MdkBridge::frontend_booted() const { return feShell_ != nullptr; }

void MdkBridge::frontend_enter(bool returning) {
  if (!feShell_) {
    setError_("frontend_enter: frontend_boot() first");
    return;
  }
  // FUN_0041d85c writes 0x541492 = 0 — mirror it in both stores.
  mode_ = 0;
  sess_.mode = 0;
  feShell_->enterFrontend(returning);
}

std::optional<mdk::FrontendSaveData> MdkBridge::produceFrontendSave_() {
  // FUN_00427ed4's writers read the live 0x541xxx globals at commit
  // time — this source snapshots them (the session is the canonical
  // store; the traversal runtime carries the live health while mode
  // 3 runs). The THMB is the arm-time capture staged by
  // frontend_capture_thumbnail (FUN_00427e8c -> 0x49f010).
  mdk::FrontendSaveData d;
  d.headerOnly.modeField = mode_ != 0 ? mode_ : sess_.mode;
  d.headerOnly.levelId = sess_.levelId;
  d.headerOnly.health = (mode_ == 3 && rt_) ? rt_->fieldHealth
                                          : sess_.health;
  d.headerOnly.deathCount = sess_.deathCount;
  d.headerOnly.field54163b = sess_.field54163b;
  // The staged FUN_00427e8c capture (armed by the SaveNameThumbnailGrab
  // fx) is embedded verbatim — FUN_00422d84 writes whatever the
  // staging buffer holds; an absent grab emits the writer's zeroed
  // record exactly like an original arm that never captured.
  if (armedThmb_.size() == mdk::kThmbRecordBytes) {
    d.headerOnly.thumbnail = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(armedThmb_.data()),
        armedThmb_.size());
  }
  if (mode_ == 3 && rt_) {
    mdk::SaveWriteFullInput in;
    in.thumbnail = d.headerOnly.thumbnail;
    mdk::FullWriteReport rep;
    std::string detail;
    d.full = mdk::saveGameWriteFull(*rt_, sess_, in, &rep, &detail);
    for (const std::string& w : rep.warnings)
      UtilityFunctions::printerr("MdkBridge: save write — ", w.c_str());
    if (d.full.empty()) {
      // No save content -> the write seam reports failure and the
      // OBSERVED FUN_00422dec path keeps the name dialog open.
      setError_("frontend save: " + detail);
    }
  }
  return d;
}

mdk::FrontendMenuInput MdkBridge::frontendInput_(
    const Dictionary& input) const {
  mdk::FrontendMenuInput in;
  in.prevHeld = bool(input.get("prev", false));
  in.nextHeld = bool(input.get("next", false));
  in.confirmEdge = bool(input.get("confirm", false));
  in.attractEdge = bool(input.get("attract", false));
  in.leftHeld = bool(input.get("left", false));
  in.rightHeld = bool(input.get("right", false));
  in.cancelEdge = bool(input.get("cancel", false));
  in.mouseDx = int(input.get("mouse_dx", 0));
  in.mouseDy = int(input.get("mouse_dy", 0));
  in.mouseDz = int(input.get("mouse_dz", 0));
  in.mouseButtons =
      std::uint8_t(int(input.get("mouse_buttons", 0)) & 0xf);
  in.pageUpEdge = bool(input.get("page_up", false));
  in.pageDownEdge = bool(input.get("page_down", false));
  in.homeEdge = bool(input.get("home", false));
  in.endEdge = bool(input.get("end", false));
  in.keyYEdge = bool(input.get("key_y", false));
  in.keyNEdge = bool(input.get("key_n", false));
  in.f1Edge = bool(input.get("f1", false));
  in.f2Edge = bool(input.get("f2", false));
  in.f3Edge = bool(input.get("f3", false));
  in.f10Edge = bool(input.get("f10", false));
  in.f11Edge = bool(input.get("f11", false));
  in.f12Edge = bool(input.get("f12", false));
  in.pauseEdge = bool(input.get("pause", false));
  in.pauseAltEdge = bool(input.get("pause_alt", false));
  in.utilityEdge = bool(input.get("utility", false));
  in.typedChar = char(int(input.get("typed", 0)) & 0xff);
  in.leftEdge = bool(input.get("left_edge", false));
  in.rightEdge = bool(input.get("right_edge", false));
  in.nameBackspaceEdge = bool(input.get("name_bs", false));
  in.nameDeleteEdge = bool(input.get("name_del", false));
  in.nameHomeEdge = bool(input.get("name_home", false));
  in.nameEndEdge = bool(input.get("name_end", false));
  const PackedInt32Array edges = input.get("raw_edges", {});
  for (int i = 0; i < 4 && i < edges.size(); ++i)
    in.rawKeyEdge[i] = std::uint32_t(edges[i]);
  return in;
}

Dictionary MdkBridge::frontend_update(const Dictionary& input) {
  if (!feShell_) {
    setError_("frontend_update: frontend_boot() first");
    return {};
  }
  // Mirror the live host state into the shell's process-global block
  // before the frame. mode_ is authoritative while a runtime is live;
  // sess_.mode covers the progression modes (5/6/7/8) with no
  // presentation runtime.
  feShell_->setPrimaryMode(mode_ != 0 ? mode_ : sess_.mode);
  feShell_->setLevelIndex(sess_.levelId);
  if (input.has("utility_enabled")) {
    feShell_->setUtilityKeyEnabled(bool(input["utility_enabled"]));
  }
  // The manual-save gate inputs (FUN_00422bc0 arg==0 checks):
  // 0x540e9c + 0x540d9c are live traversal globals; the X_STRIKE
  // scan runs over every arena's +0x68 list for a live object of
  // that class (the OBSERVED tag scan).
  bool xStrike = false;
  if (mode_ == 3 && rt_) {
    for (const auto& a : rt_->arenas) {
      for (const auto& up : a->dyn.storage) {
        // The +6/+8 live-object predicate — despawned records keep
        // their class tag until reaped, so col.named alone would
        // over-block.
        if (up->col.named && up->col.model != nullptr &&
            up->scriptClass == "X_STRIKE") {
          xStrike = true;
          break;
        }
      }
      if (xStrike) break;
    }
  }
  feShell_->setSaveBlockFlags(
      mode_ == 3 && rt_ && rt_->fieldE9c != 0,
      (mode_ == 3 && rt_ && rt_->masterMoveGate) ||
          sess_.victoryPhase != 0,
      xStrike);
  feShell_->update(frontendInput_(input));
  return frontendSnapshot_();
}

void MdkBridge::frontend_end_frame(double dt_ms) {
  if (!feShell_) return;
  feShell_->endFrame(dt_ms);
}

Array MdkBridge::frontend_drain_requests() {
  Array out;
  if (!feShell_) return out;
  while (feShell_->pendingRequest() != mdk::FrontendRequest::None) {
    Dictionary d;
    d["request"] = int64_t(feShell_->pendingRequest());
    d["name"] = String(feShell_->requestName().c_str());
    d["header_only"] = feShell_->requestHeaderOnly();
    feShell_->consumeRequest();
    out.push_back(d);
  }
  return out;
}

Array MdkBridge::frontend_drain_fx() {
  Array out;
  if (!feShell_) return out;
  for (const mdk::FrontendFx f : feShell_->drainFx()) {
    // The host-side half of the arm: DAT_0049aa8c's blend gate holds
    // the attract idle timer while the transition plays (the shell
    // surfaced the fx; the host applies the gate here so the timing
    // is observable in the drained event).
    if (f == mdk::FrontendFx::TransitionArmed) {
      mdk::frontendHostTransitionArmed(*feShell_);
    }
    out.push_back(int64_t(f));
  }
  return out;
}

Dictionary MdkBridge::frontendSnapshot_() const {
  Dictionary out;
  if (!feShell_) return out;
  const mdk::FrontendShell& sh = *feShell_;
  out["mode"] = sh.primaryMode();
  out["sub_mode"] = sh.subMode();
  out["quit"] = sh.quitRequested();
  out["paused"] = sh.paused();
  out["transition_byte"] = sh.transitionByte();
  out["suppress_esc_abort"] = sh.suppressEscAbort();
  out["idle_ticks"] = sh.idleTicks();
  out["noise_frame"] = sh.noiseFrameRan();
  out["saves_exist"] = sh.flow().root().savesExist();
  out["help_open"] = sh.helpOpen();
  const char* screen = "root";
  switch (sh.flow().screen()) {
  case mdk::FrontendScreen::Root:     screen = "root";     break;
  case mdk::FrontendScreen::Options:  screen = "options";  break;
  case mdk::FrontendScreen::Display:  screen = "display";  break;
  case mdk::FrontendScreen::Sound:    screen = "sound";    break;
  case mdk::FrontendScreen::Mouse:    screen = "mouse";    break;
  case mdk::FrontendScreen::Keyboard: screen = "keyboard"; break;
  }
  out["screen"] = screen;
  if (sh.flow().screen() == mdk::FrontendScreen::Root) {
    const mdk::FrontendMenuController& root = sh.flow().root();
    out["selection"] = root.selection();
    out["mouse_x"] = root.mouseX();
    out["mouse_y"] = root.mouseY();
    out["attract_state"] = root.attractState();
    out["attract_slide_active"] = root.attractSlideActive();
    out["menu_strings_hidden"] = root.menuStringsHidden();
    out["idle_seconds"] = double(root.idleSeconds());
  }
  // The flow's settings globals (the 0x541xxx block) — presentation
  // reads them to draw the options screens; the shell mutates them.
  const mdk::FrontendFlowController& fl = sh.flow();
  Dictionary settings;
  settings["skill"] = fl.skill();
  settings["dev_hidden"] = fl.devHidden();
  settings["brightness"] = fl.brightness();
  settings["force_p_correct"] = fl.forcePCorrect();
  settings["sound_fx"] = fl.soundFx();
  settings["sound_music"] = fl.soundMusic();
  settings["mouse_on"] = fl.mouseOn();
  settings["dirty"] = fl.settingsDirty();
  out["settings"] = settings;
  if (const mdk::SaveSlotListController* l = sh.saveList()) {
    Dictionary d;
    d["count"] = l->count();
    d["selection"] = l->selection();
    d["top_row"] = l->topRow();
    d["mouse_track"] = l->mouseTrack();
    Array stems;
    for (const std::string& s : l->stems())
      stems.push_back(String(s.c_str()));
    d["stems"] = stems;
    if (const mdk::SaveSlotSummary* s = l->selected()) {
      Dictionary sd;
      sd["name"] = String(s->name.c_str());
      sd["valid"] = s->valid;
      sd["full_save"] = s->fullSave;
      sd["level_id"] = s->levelId;
      sd["mode_field"] = s->modeField;
      sd["health"] = s->health;
      sd["death_count"] = s->deathCount;
      d["selected"] = sd;
    }
    out["save_list"] = d;
  }
  if (const mdk::SaveNameEntryController* n = sh.saveName()) {
    Dictionary d;
    d["name"] = String(n->name().c_str());
    d["cursor"] = n->cursor();
    d["confirm_phase"] = n->confirmPhase();
    d["confirm_selection"] = n->confirmSelection();
    d["header_only"] = n->headerOnly();
    d["write_failed"] = n->writeFailed();
    out["save_name"] = d;
  }
  if (const mdk::AbortConsoleController* a = sh.abortConsole())
    out["abort_selection"] = a->selection();
  return out;
}

Dictionary MdkBridge::frontend_snapshot() const {
  return frontendSnapshot_();
}

bool MdkBridge::frontend_lastgame_exists() {
  // FUN_00428290 — SAVES\LASTGAME.SAV opens AND its envelope is
  // consistent; no packet walk, no world load.
  return feHost_ && feHost_->lastGameExists();
}

Array MdkBridge::frontend_enumerate_saves() {
  Array out;
  if (!feHost_) return out;
  for (const std::string& n : feHost_->enumerateSaves())
    out.push_back(String(n.c_str()));
  return out;
}

Dictionary MdkBridge::frontend_inspect_slot(const String& stem) {
  Dictionary out;
  out["found"] = false;
  if (!feHost_) return out;
  const auto d = feHost_->inspectSlotDetail(
      std::string(stem.utf8().get_data()));
  if (!d) return out;
  out["found"] = true;
  out["error"] = String(mdk::saveErrorName(d->error));
  out["name"] = String(d->summary.name.c_str());
  out["valid"] = d->summary.valid;
  out["full_save"] = d->summary.fullSave;
  out["level_id"] = d->summary.levelId;
  out["mode_field"] = d->summary.modeField;
  out["health"] = d->summary.health;
  out["death_count"] = d->summary.deathCount;
  // The FUN_00428144 THMB capture — the 3648-byte record (768 palette
  // + 64x45 indexed) when the file carries it.
  out["thumbnail_size"] = int64_t(d->thumbnail.size());
  if (!d->thumbnail.empty()) {
    PackedByteArray t;
    t.resize(int64_t(d->thumbnail.size()));
    std::memcpy(t.ptrw(), d->thumbnail.data(), d->thumbnail.size());
    out["thumbnail"] = t;
  }
  return out;
}

bool MdkBridge::frontend_write_save(const Dictionary& request) {
  if (!feHost_) {
    setError_("frontend_write_save: frontend_boot() first");
    return false;
  }
  const std::string stem =
      std::string(String(request.get("name", "")).utf8().get_data());
  const bool headerOnly = bool(request.get("header_only", false));
  // The same body the shell's write seam runs.
  const auto data = produceFrontendSave_();
  if (!data) return false;
  const std::vector<std::byte> bytes =
      headerOnly ? mdk::saveGameWriteHeaderOnly(data->headerOnly)
                 : data->full;
  return !bytes.empty() &&
         feHost_->writeSaveFile(
             stem, {bytes.data(), bytes.size()});
}

Dictionary MdkBridge::frontend_slide_probe(int64_t index) {
  Dictionary out;
  out["exists"] = false;
  if (!feHost_) return out;
  const auto info = feHost_->slideInfo(int(index));
  if (!info) return out;
  out["index"] = int64_t(info->index);
  out["path"] = String(info->relPath.c_str());
  out["bytes"] = int64_t(info->bytes);
  out["width"] = info->width;
  out["height"] = info->height;
  // `exists` reports the OBSERVED verdict — the 600x360 decode gate
  // (FUN_00416e98), not mere file presence.
  out["exists"] = info->exists && info->width == 600 &&
                  info->height == 360;
  return out;
}

PackedByteArray MdkBridge::frontend_slide_data(int64_t index) {
  PackedByteArray out;
  if (!feHost_) return out;
  const auto bytes = feHost_->slideData(int(index));
  if (!bytes) return out;
  out.resize(int64_t(bytes->size()));
  std::memcpy(out.ptrw(), bytes->data(), bytes->size());
  return out;
}

void MdkBridge::frontend_transition_complete() {
  if (!feShell_) return;
  // FUN_0041ebf4's clear point — the transition presentation
  // finished; the Esc-abort suppression + the attract blend gate
  // release together.
  mdk::frontendHostTransitionComplete(*feShell_);
}

double MdkBridge::frontend_transition_seconds() const {
  // FUN_0041e554's nominal timeline — 300 ticks of 1/30. A missing
  // INTRO1A record reports 0 so the caller completes at once rather
  // than reproducing the original's unbound-record crash.
  return feRes_.transition ? mdk::kFrontendTransitionSeconds : 0.0;
}

bool MdkBridge::frontend_capture_thumbnail() {
  // FUN_00427e8c (OBSERVED): at save-name arm the original samples
  // the presented 600x360 indexed frame (fb[(r*8)*600 + 44 + c*8])
  // and snapshots the staged DAC palette (FUN_0046d614). In the
  // original every screen shares the one indexed work buffer; in
  // this port the closest authoritative source is:
  //   * traversal arm (F2): rt_.hud.fb — the core's 600x360 indexed
  //     overlay — under the active level palette. PARTIAL SEAM:
  //     the Godot port has no indexed world buffer, so the world
  //     scene beneath the HUD cannot appear in the thumbnail; the
  //     sample layout/palette semantics are exact.
  //   * frontend arm (autosave/briefing paths): the last composed
  //     frontend frame.
  armedThmb_.assign(mdk::kThmbRecordBytes, 0);
  if (mode_ == 3 && rt_) {
    mdk::captureThumbnail(rt_->hud.fb,
                          {activePalette_(), 768},
                          armedThmb_.data());
    return true;
  }
  if (feShell_ && feResLoaded_) {
    const mdkbridge::FrontendComposedFrame& f = fePresenter_.frame();
    std::uint8_t pal[768];
    for (int i = 0; i < mdk::Palette::size(); ++i) {
      const mdk::Palette::Color c = f.palette.get(i);
      pal[i * 3 + 0] = c.r;
      pal[i * 3 + 1] = c.g;
      pal[i * 3 + 2] = c.b;
    }
    mdk::captureThumbnail(f.fb, pal, armedThmb_.data());
    return true;
  }
  return false;   // no indexed source armed — the staged buffer
                  // keeps the zeroed 3648 bytes the writer emits.
}

void MdkBridge::frontend_notify_load_result(bool ok) {
  if (!feShell_) return;
  feShell_->notifyLoadResult(ok);
}

bool MdkBridge::frontendLoadSaveFile_(const std::string& stem,
                                      std::string& detail) {
  // FUN_00427f94 — the shared Continue/save-list load path.
  if (!root_ || !feHost_) return false;
  mdk::SaveGame sg;
  const mdk::SaveError pe =
      mdk::saveGameLoadFile(feHost_->saves().pathFor(stem), sg);
  if (pe != mdk::SaveError::kOk) {
    detail = std::string("parse: ") + mdk::saveErrorName(pe);
    return false;
  }
  if (!sg.game.full()) {
    // Header-only save — GAME fields verbatim, then the 0x428088
    // mode route: 3 -> a fresh traversal load of the saved level,
    // 6 -> the briefing re-entry, else the frontend fallback.
    mdk::progressionApplyGamePacket(sess_, sg.game);
    if (sess_.mode == 3) {
      auto trav = std::make_unique<mdk::TraversalRuntime>();
      const mdk::ProgressionError e =
          mdk::progressionLoadTraversalForCurrentLevel(
              *root_, sess_, *trav, &detail);
      if (e != mdk::ProgressionError::kOk) {
        detail = std::string("load: ") +
                 mdk::progressionErrorName(e) + " — " + detail;
        return false;
      }
      rt_ = std::move(trav);
      mode_ = 3;
      hasFrame_ = false;
      timing_ = mdk::FrontendTimingState{};
      last_ = mdk::TraversalFrameResult{};
      prevKeyLevel_ = {};
      objIds_ = mdkfront::MdkObjectIds{};
      arenaSets_.clear();
      arenaSetFailed_.clear();
      displaySet_.clear();
      arenaIndex_ = -1;
      arenaName_.clear();
      arenaLoaded_ = false;
      const int dir = mdk::progressionLevelDir(sess_.levelId);
      char stemBuf[16], dirBuf[32];
      std::snprintf(stemBuf, sizeof(stemBuf), "LEVEL%d", dir);
      std::snprintf(dirBuf, sizeof(dirBuf), "TRAVERSE/LEVEL%d/", dir);
      if (!presentTraversalLevel_(stemBuf, dirBuf)) {
        detail = lastError_;
        return false;
      }
      updateDisplaySet_();
      refreshOrders_();
    } else {
      // mode 6 (briefing) / 0 (frontend) — the progression session
      // holds the mode; no presentation runtime exists for them yet.
      mode_ = sess_.mode;
    }
    return true;
  }
  // Full save — the FUN_00427218 rebuild, identical to restore_save.
  auto trav = std::make_unique<mdk::TraversalRuntime>();
  mdk::FullRestoreReport rep;
  const mdk::SaveError re =
      mdk::applyFullSaveToTraversal(sg, *root_, sess_, *trav, &rep,
                                    &detail);
  if (re != mdk::SaveError::kOk) {
    detail = std::string("restore: ") + mdk::saveErrorName(re) +
             " — " + detail;
    return false;
  }
  rt_ = std::move(trav);
  mode_ = 3;
  hasFrame_ = false;
  timing_ = mdk::FrontendTimingState{};
  last_ = mdk::TraversalFrameResult{};
  prevKeyLevel_ = {};
  ff_.reset();
  ffScene_.reset();
  ffTex_.clear();
  ffHandoffDone_ = false;
  ffHandoffRoute_ = -1;
  ffHandoffDetail_.clear();
  objIds_ = mdkfront::MdkObjectIds{};
  arenaSets_.clear();
  arenaSetFailed_.clear();
  displaySet_.clear();
  arenaIndex_ = -1;
  arenaName_.clear();
  arenaLoaded_ = false;
  const int dir = mdk::progressionLevelDir(sess_.levelId);
  char stemBuf[16], dirBuf[32];
  std::snprintf(stemBuf, sizeof(stemBuf), "LEVEL%d", dir);
  std::snprintf(dirBuf, sizeof(dirBuf), "TRAVERSE/LEVEL%d/", dir);
  if (!presentTraversalLevel_(stemBuf, dirBuf)) {
    detail = lastError_;
    return false;
  }
  updateDisplaySet_();
  refreshOrders_();
  return true;
}

void MdkBridge::frontendTeardown_() {
  // The abort-yes per-mode teardown (FUN_004031b8's mode table —
  // FUN_00418de4 / FUN_0040fa68 / FUN_004371bc / FUN_0042c824 /
  // FUN_004295c4 / FUN_0047b0d8): everything derived from the mode's
  // runtime dies. shutdown() is the port's proven superset of that
  // free list (runtime, presentation caches, audio banks, session
  // mirror); the frontend shell and save root are host-level and
  // survive it, matching the original's process-global lifetime.
  shutdown();
}

Array MdkBridge::frontend_dispatch_requests() {
  Array out;
  if (!feShell_) {
    setError_("frontend_dispatch_requests: frontend_boot() first");
    return out;
  }
  while (feShell_->pendingRequest() != mdk::FrontendRequest::None) {
    const mdk::FrontendRequest req = feShell_->pendingRequest();
    const std::string name = feShell_->requestName();
    const bool headerOnly = feShell_->requestHeaderOnly();
    feShell_->consumeRequest();
    Dictionary r;
    r["request"] = int64_t(req);
    r["name"] = String(name.c_str());
    r["header_only"] = headerOnly;
    r["handled"] = true;
    r["ok"] = true;
    switch (req) {
    case mdk::FrontendRequest::Quit:
      // DAT_0054148e — the process quit belongs to the app.
      r["handled"] = false;
      r["ok"] = false;
      r["owner"] = "app";
      break;
    case mdk::FrontendRequest::StartNewGame:
      // FUN_0041b630 — the campaign host's authoritative entry: mode
      // 6 at the briefing stage. The loader/freefall progression is
      // driven by the campaign host frames after this.
      mdk::progressionStartCampaign(sess_, feShell_->flow().skill());
      mode_ = sess_.mode;
      r["owner"] = "progression";
      r["mode"] = sess_.mode;
      break;
    case mdk::FrontendRequest::ContinueLastGame:
    case mdk::FrontendRequest::LoadSave: {
      std::string detail;
      const bool ok = frontendLoadSaveFile_(
          req == mdk::FrontendRequest::ContinueLastGame ? "LASTGAME"
                                                      : name,
          detail);
      feShell_->notifyLoadResult(ok);
      r["owner"] = "host";
      r["ok"] = ok;
      if (!ok) r["detail"] = String(detail.c_str());
      break;
    }
    case mdk::FrontendRequest::WriteSaveDone:
      // The write itself already ran through the seam inside the
      // name dialog (the OBSERVED FUN_00427ed4 callsite). This
      // request is the commit notification.
      r["owner"] = "host";
      break;
    case mdk::FrontendRequest::AbortToFrontend:
      // Per-mode teardown + FUN_0041d85c(0) fresh entry.
      frontendTeardown_();
      feShell_->enterFrontend(false);
      r["owner"] = "host";
      break;
    case mdk::FrontendRequest::ResumeTraversal:
      // FUN_004348d4 unfreeze — frames resume on their own (the
      // bridge drives no overlay freeze of its own).
      r["owner"] = "host";
      r["ok"] = (mode_ == 3 || sess_.mode == 3);
      break;
    case mdk::FrontendRequest::CycleBrightness:
      // The brightness global already wrapped shell-side; the
      // palette upload (FUN_0046c92c) is presentation-owned.
      r["handled"] = false;
      r["ok"] = false;
      r["owner"] = "presentation";
      break;
    case mdk::FrontendRequest::CaptureUtility:
      // FUN_00428340 frame capture — no host seam yet.
      r["handled"] = false;
      r["ok"] = false;
      r["owner"] = "presentation";
      break;
    case mdk::FrontendRequest::OpenLegacyScreen:
      // Sub-modes 3/6 (joystick/perf) — diagnostic only, not ported.
      r["handled"] = false;
      r["ok"] = false;
      r["owner"] = "legacy";
      break;
    case mdk::FrontendRequest::None:
      break;
    }
    out.push_back(r);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Phase 18B.2A — frontend presentation
// ---------------------------------------------------------------------------

Dictionary MdkBridge::frontend_frame(double transition_ms) {
  Dictionary out;
  if (!feShell_ || !feResLoaded_) {
    setError_("frontend_frame: frontend_boot() first");
    return out;
  }
  std::string err;
  // suppressEscAbort is set on the returning-entry transition arm and
  // cleared exactly by frontend_transition_complete — the same
  // lifecycle as the transition presentation window. The elapsed
  // time drives the INTRO1A palette timeline (FUN_0041e554); a caller
  // that keeps reporting ms after the ack gets the normal frame.
  const double transitionSec = feShell_->suppressEscAbort()
                                   ? transition_ms / 1000.0
                                   : -1.0;
  if (!fePresenter_.compose(*feShell_, feRes_, feHost_.get(),
                            transitionSec, &err)) {
    setError_("frontend_frame: " + err);
    return out;
  }
  const mdkbridge::FrontendComposedFrame& f = fePresenter_.frame();
  out["w"] = f.fb.width();
  out["h"] = f.fb.height();
  PackedByteArray rgba;
  rgba.resize(int64_t(f.fb.pixelCount()) * 4);
  std::uint8_t* dst = rgba.ptrw();
  for (std::size_t i = 0; i < f.fb.pixelCount(); ++i) {
    const mdk::Palette::Color c = f.palette.get(f.fb.pixels()[i]);
    dst[i * 4 + 0] = c.r;
    dst[i * 4 + 1] = c.g;
    dst[i * 4 + 2] = c.b;
    dst[i * 4 + 3] = c.a;
  }
  out["rgba"] = rgba;
  return out;
}

bool MdkBridge::campaignFreefallEnter_(std::string& detail) {
  // The mode-6 exit's FALL3D_<levelId> entry — the same loads
  // load_freefall runs, but the session carries through (health/
  // rng/skill are the campaign globals, not a fresh reset).
  ffScene_ = std::make_unique<mdk::FreefallScene>();
  const auto se = mdk::freefallSceneLoad(*root_, sess_.levelId,
                                         ffScene_.get(), &detail);
  if (se != mdk::FreefallSceneError::kOk) {
    detail = std::string("freefall scene load: ") +
             mdk::freefallSceneErrorName(se) + " — " + detail;
    ffScene_.reset();
    return false;
  }
  ff_ = std::make_unique<mdk::FreefallRuntime>();
  mdk::FreefallCourseData data;
  data.course = sess_.levelId;
  data.skill = sess_.skill;
  data.pickups = ffScene_->pickups;
  mdk::freefallInit(*ff_, data, sess_.rng);
  timing_ = mdk::FrontendTimingState{};
  prevKeyLevel_ = {};
  ffHandoffDone_ = false;
  ffHandoffRoute_ = -1;
  ffHandoffDetail_.clear();
  ffTex_.clear();
  mode_ = 2;
  hasFrame_ = false;
  return true;
}

bool MdkBridge::campaignTraversalEnter_(std::string& detail) {
  // The mode-6-exit (levelId >= 5) / mode-7 traversal entry — the
  // same load+present tail frontendLoadSaveFile_ runs.
  auto trav = std::make_unique<mdk::TraversalRuntime>();
  const mdk::ProgressionError e =
      mdk::progressionLoadTraversalForCurrentLevel(*root_, sess_,
                                                 *trav, &detail);
  if (e != mdk::ProgressionError::kOk) {
    detail = std::string("load: ") +
             mdk::progressionErrorName(e) + " — " + detail;
    return false;
  }
  rt_ = std::move(trav);
  mode_ = 3;
  hasFrame_ = false;
  timing_ = mdk::FrontendTimingState{};
  last_ = mdk::TraversalFrameResult{};
  prevKeyLevel_ = {};
  ff_.reset();
  ffScene_.reset();
  ffTex_.clear();
  ffHandoffDone_ = false;
  ffHandoffRoute_ = -1;
  ffHandoffDetail_.clear();
  objIds_ = mdkfront::MdkObjectIds{};
  arenaSets_.clear();
  arenaSetFailed_.clear();
  displaySet_.clear();
  arenaIndex_ = -1;
  arenaName_.clear();
  arenaLoaded_ = false;
  const int dir = mdk::progressionLevelDir(sess_.levelId);
  char stemBuf[16], dirBuf[32];
  std::snprintf(stemBuf, sizeof(stemBuf), "LEVEL%d", dir);
  std::snprintf(dirBuf, sizeof(dirBuf), "TRAVERSE/LEVEL%d/", dir);
  if (!presentTraversalLevel_(stemBuf, dirBuf)) {
    detail = lastError_;
    return false;
  }
  updateDisplaySet_();
  refreshOrders_();
  return true;
}

Dictionary MdkBridge::frontend_progression_step(const Dictionary& input) {
  Dictionary out;
  out["pumped"] = false;
  out["ok"] = true;
  out["mode"] = mode_;
  out["sess_mode"] = sess_.mode;
  out["level_id"] = sess_.levelId;
  if (!feShell_ || !root_) {
    setError_("frontend_progression_step: frontend_boot() first");
    out["ok"] = false;
    return out;
  }
  // The presentation seam: the original's stage-complete input (the
  // tally fade / briefing input / cinematic end). Godot marks the
  // placeholder intermission done with "stage_done" or a confirm.
  const bool stageDone = bool(input.get("stage_done", false)) ||
                        bool(input.get("confirm", false));
  mdk::ProgressionError e = mdk::ProgressionError::kOk;
  const int prevMode = sess_.mode;
  switch (prevMode) {
  case 5:
    e = mdk::progressionStepIntermission(sess_, stageDone);
    break;
  case 6:
    e = mdk::progressionStepLoader(sess_, stageDone);
    break;
  case 7:
    e = mdk::progressionStepMode7(sess_);
    break;
  case 8:
    e = mdk::progressionStepCinematic(sess_, stageDone);
    break;
  default:
    return out;   // no pump for runtime-presented modes
  }
  out["pumped"] = true;
  out["error"] = String(mdk::progressionErrorName(e));
  if (e == mdk::ProgressionError::kStageRunning) {
    out["sess_mode"] = sess_.mode;
    return out;
  }
  if (e != mdk::ProgressionError::kOk) {
    setError_(std::string("progression step: ") +
              mdk::progressionErrorName(e));
    out["ok"] = false;
    return out;
  }
  // The transition completed — install whichever runtime the new
  // mode needs (the dispatcher's tail is data-load, not state).
  std::string detail;
  if (sess_.mode == 2 && !ff_) {
    if (!campaignFreefallEnter_(detail)) {
      setError_("campaign freefall: " + detail);
      out["ok"] = false;
      out["detail"] = String(detail.c_str());
      return out;
    }
  } else if (sess_.mode == 3 && !rt_) {
    if (!campaignTraversalEnter_(detail)) {
      setError_("campaign traversal: " + detail);
      out["ok"] = false;
      out["detail"] = String(detail.c_str());
      return out;
    }
  } else if (sess_.mode == 0) {
    // The campaign's frontend exit. The mode-8 tail calls
    // FUN_0041d85c(nonzero) — the returning-entry arm that runs the
    // entry transition + Esc suppression (OBSERVED); every other
    // mode-0 route is the fresh entry.
    mode_ = 0;
    feShell_->enterFrontend(prevMode == 8);
  }
  mode_ = sess_.mode;
  out["mode"] = mode_;
  out["sess_mode"] = sess_.mode;
  out["level_id"] = sess_.levelId;
  return out;
}
