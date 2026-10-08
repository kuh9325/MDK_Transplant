// Phase 7-9 — retained GDExtension bridge into mdk_core.
// See mdk_bridge.h for ownership and threading notes.

#include "mdk_bridge.h"

#include "mve_player.h"

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

#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/dti_structure.h"
#include "core/ending_cinematic.h"
#include "core/enemy_runtime.h"
#include "core/frontend_transition.h"
#include "core/fti_directory.h"
#include "core/indexed_image.h"
#include "core/keyboard_menu.h"
#include "core/mti_directory.h"
#include "core/mto_directory.h"
#include "core/save_full_restore.h"
#include "core/save_full_write.h"
#include "core/sound_menu.h"
#include "core/stream_context.h"
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
  ClassDB::bind_method(D_METHOD("qa_set_key_global", "index", "code"),
                       &MdkBridge::qa_set_key_global);
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
      D_METHOD("get_object_material", "object_id", "name", "pen"),
      &MdkBridge::get_object_material);
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
  // Phase 19B.3A — the mode-3 -> 5 route: the END_LEVEL mailbox.
  ClassDB::bind_method(D_METHOD("diagnostic_end_level"),
                       &MdkBridge::diagnostic_end_level);
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
  ClassDB::bind_method(D_METHOD("get_freefall_backdrop"),
                       &MdkBridge::get_freefall_backdrop);
  ClassDB::bind_method(D_METHOD("get_freefall_backdrop_frame"),
                       &MdkBridge::get_freefall_backdrop_frame);
  ClassDB::bind_method(D_METHOD("get_freefall_sprites"),
                       &MdkBridge::get_freefall_sprites);
  ClassDB::bind_method(D_METHOD("get_freefall_material", "name"),
                       &MdkBridge::get_freefall_material);
  // Phase 19B.1 — mode-5 StreamScene presentation.
  ClassDB::bind_method(
      D_METHOD("load_stream", "course", "skill", "seed"),
      &MdkBridge::load_stream);
  ClassDB::bind_method(D_METHOD("stream_active"),
                       &MdkBridge::stream_active);
  ClassDB::bind_method(D_METHOD("stream_frame"),
                       &MdkBridge::stream_frame);
  ClassDB::bind_method(D_METHOD("stream_diag"),
                       &MdkBridge::stream_diag);
  // Phase 19E — mode-6 briefing (FUN_00429cb4 family).
  ClassDB::bind_method(D_METHOD("mode6_active"),
                       &MdkBridge::mode6_active);
  ClassDB::bind_method(D_METHOD("mode6_frame"),
                       &MdkBridge::mode6_frame);
  ClassDB::bind_method(D_METHOD("mode6_diag"),
                       &MdkBridge::mode6_diag);
  ClassDB::bind_method(D_METHOD("ff_teletype_frame"),
                       &MdkBridge::ff_teletype_frame);
  ClassDB::bind_method(D_METHOD("ff_teletype_diag"),
                       &MdkBridge::ff_teletype_diag);
  ClassDB::bind_method(D_METHOD("ff_veil_mask"),
                       &MdkBridge::ff_veil_mask);
  // Phase 19D — mode-8 ending cinematic.
  ClassDB::bind_method(D_METHOD("load_ending"),
                       &MdkBridge::load_ending);
  ClassDB::bind_method(D_METHOD("ending_active"),
                       &MdkBridge::ending_active);
  ClassDB::bind_method(D_METHOD("ending_frame"),
                       &MdkBridge::ending_frame);
  ClassDB::bind_method(D_METHOD("ending_drain_audio"),
                       &MdkBridge::ending_drain_audio);
  ClassDB::bind_method(D_METHOD("ending_set_audio_clock", "sec"),
                       &MdkBridge::ending_set_audio_clock);
  ClassDB::bind_method(D_METHOD("ending_diag"),
                       &MdkBridge::ending_diag);
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
  ClassDB::bind_method(D_METHOD("frontend_drain_audio_events"),
                       &MdkBridge::frontend_drain_audio_events);
  ClassDB::bind_method(D_METHOD("frontend_song_stream", "name"),
                       &MdkBridge::frontend_song_stream);
  ClassDB::bind_method(D_METHOD("frontend_volumes"),
                       &MdkBridge::frontend_volumes);
  ClassDB::bind_method(D_METHOD("audio_vol_db", "vol", "pct"),
                       &MdkBridge::audio_vol_db);
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
  // LEVEL<n>O.SNI — the level's ambient/music stream bank: carries
  // the flags&3 records the arena CMI structures name for zone
  // ambience. Level-scoped like the S bank — it joins the first-match
  // walk right behind it.
  if (!rt_->level.sniOBytes.empty()) {
    AudioBank_ b;
    b.bytes = std::span<const std::byte>(rt_->level.sniOBytes);
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
    const std::string& name, bool allowMusicClass) {
  if (const auto it = audioEntries_.find(name);
      it != audioEntries_.end()) {
    if (it->second.resolved || !allowMusicClass) return &it->second;
    audioEntries_.erase(it);   // gated-out entry — retry unlocked
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
    // music-class — out of SFX scope UNLESS the caller is the
    // zone-ambient path: the FUN_00431cf4 fader drives exactly these
    // flags&3 looped records through the instance pool.
    if ((flags & 0x2) && !allowMusicClass) break;
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
    wav->set_stereo(wv.channels == 2);
    wav->set_mix_rate(wv.rateHz);   // verbatim — no resampling
    // RIFF PCM8 is unsigned-biased; FORMAT_8_BITS wants
    // signed. One conversion here at the host boundary
    // (pcmForGodotWav) — wv.pcm stays verbatim, PCM16
    // passes through untouched.
    const std::vector<std::uint8_t> pcm =
        mdkbridge::pcmForGodotWav(wv);
    PackedByteArray data;
    data.resize(static_cast<int64_t>(pcm.size()));
    std::memcpy(data.ptrw(), pcm.data(), pcm.size());
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
                              mdk::TraversalAudioSoundDef& def,
                              bool allowMusicClass) {
  const AudioEntry_* e = audioEntry_(name, allowMusicClass);
  if (!e || !e->resolved) return false;
  def = e->def;
  return true;
}

// 19C.3 — the mode-2 bank set: the FALL3D course bank (FUN_0040ef28
// binds FALL3D.BNI; the SNI sits beside it as the mode's sound
// bank) then the process-global MDKSOUND — the same shadow order
// loadSoundBanks_ uses for traversal (level-scope first, global
// last; zero cross-bank name collisions in the corpus).
void MdkBridge::loadFreefallSoundBanks_() {
  audioBanks_.clear();
  audioBankStore_.clear();
  audioEntries_.clear();
  audioMixer_.reset();
  if (!ff_) return;
  for (const char* rel : {"FALL3D/FALL3D.SNI", "MISC/MDKSOUND.SNI"}) {
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

// The 0x4edc3c..98 slot block (OBSERVED name table): tag ->
// FALL3D.SNI record name. The two tail entries are the rand groups —
// 0x4edc60's 7-slot run (K_HIT1..7) and 0x4edc80's pair (K_COLL1/2)
// — the event's b carries the picked index, not a play mode.
void MdkBridge::freefallDrainAudio_() {
  if (!ff_) return;
  static const char* const names[] = {
      /* kFfSndRStart  */ "R_START",
      /* kFfSndRMove   */ "R_MOVE",
      /* kFfSndMPass   */ "M_PASS",
      /* kFfSndMLnch   */ "M_LNCH",
      /* kFfSndChute   */ "P_CHUTE",
      /* kFfSndPColl   */ "P_COLL",
      /* kFfSndPFall   */ "P_FALL",
      /* kFfSndKHit0   */ "K_HIT1",
      /* kFfSndKHit1   */ "K_HIT2",
      /* kFfSndKSeen   */ "K_SEEN",
      /* kFfSndKFinish */ "K_FINISH",
      /* kFfSndBones   */ "BONES",
  };
  const auto res = [this](const std::string& n,
                          mdk::TraversalAudioSoundDef& d) {
    return audioResolve_(n, d);
  };
  const auto ownerPos = [](int, const void*, float[3]) {
    return false;   // mode 2 emits no positional voices
  };
  for (const mdk::FreefallEvent& ev : ff_->events) {
    if (ev.kind != mdk::kFfEvSound) continue;
    char buf[16];
    const char* nm = nullptr;
    mdk::TraversalAudioEvent tae;
    tae.op = ev.b != 0 ? mdk::TraversalAudioOp::kRestart
                       : mdk::TraversalAudioOp::kEnsurePlaying;
    if (ev.a == mdk::kFfSndExplode) {
      std::snprintf(buf, sizeof buf, "K_HIT%d",
                    (ev.b % 7) + 1);
      nm = buf;
      tae.op = mdk::TraversalAudioOp::kEnsurePlaying;
    } else if (ev.a == mdk::kFfSndKColl) {
      std::snprintf(buf, sizeof buf, "K_COLL%d",
                    (ev.b % 2) + 1);
      nm = buf;
      tae.op = mdk::TraversalAudioOp::kEnsurePlaying;
    } else if (ev.a >= 0 &&
               ev.a < int(std::size(names))) {
      nm = names[ev.a];
    }
    if (nm == nullptr) continue;
    tae.name = nm;
    audioMixer_.applyEvent(tae, res);
  }
  // The FUN_004026f8 pass on the mode-2 cadence — playhead reap
  // exactly like traversal/stream.
  audioMixer_.tick(timing_.deltaSec, ownerPos);
}

// 19B.3B1 — resolve + decode a STREAM.BNI sound record (memoized).
// The BNI records carry raw RIFF/WAVE payloads with no flag/volume
// words: the def's vol/loop come from the OBSERVED 02e2c
// registration args (0x7fff everywhere; the DS-loop bit on WIND).
// BNI-first is the registration-shadow order — the mode-5 binds are
// what the slot-pointer play calls reach (for the one SNI-colliding
// name, APPLE, the two records carry byte-identical PCM and the
// same vol/flags — the order is unobservable).
const MdkBridge::AudioEntry_* MdkBridge::streamSndEntry_(
    const std::string& name) {
  if (const auto it = streamSndEntries_.find(name);
      it != streamSndEntries_.end()) {
    return &it->second;
  }
  AudioEntry_ e;
  const mdk::BniRecord* rec = mdk::findBniRecord(streamBniDir_, name);
  if (rec) {
    const std::span<const std::byte> riff(
        streamBniBytes_.data() + rec->payloadFileOffset,
        static_cast<std::size_t>(rec->payloadEnd -
                                 rec->payloadFileOffset));
    mdk::SniWave wv;
    std::string derr;
    const mdk::SniWaveStatus st =
        mdk::decodeSniWave(riff, &wv, &derr);
    if (st != mdk::SniWaveStatus::kOk) {
      ++streamAudioDecodeMisses_;
      UtilityFunctions::printerr(
          "MdkBridge: stream wave '", String(name.c_str()),
          "' decode failed: ", mdk::sniWaveStatusName(st).data(),
          " — ", derr.c_str());
    } else {
      const mdkbridge::StreamSndReg* reg =
          streamAudio_.soundReg(
              static_cast<int>(rec - streamBniDir_.records.data()));
      e.def.volume = reg ? reg->volume : 0x7fff;
      e.def.loop = reg ? reg->loop : false;
      e.def.rateHz = wv.rateHz;
      e.def.frames = static_cast<std::uint32_t>(wv.frames);
      Ref<AudioStreamWAV> wav;
      wav.instantiate();
      wav->set_format(wv.bitsPerSample == 8
                          ? AudioStreamWAV::FORMAT_8_BITS
                          : AudioStreamWAV::FORMAT_16_BITS);
      wav->set_stereo(false);
      wav->set_mix_rate(wv.rateHz);   // verbatim — no resampling
      // RIFF PCM8 is unsigned-biased; FORMAT_8_BITS wants
      // signed. One conversion here at the host boundary
      // (pcmForGodotWav) — wv.pcm stays verbatim, PCM16
      // passes through untouched.
      const std::vector<std::uint8_t> pcm =
          mdkbridge::pcmForGodotWav(wv);
      PackedByteArray data;
      data.resize(static_cast<int64_t>(pcm.size()));
      std::memcpy(data.ptrw(), pcm.data(), pcm.size());
      wav->set_data(data);
      if (e.def.loop) {
        wav->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
        wav->set_loop_begin(0);
        wav->set_loop_end(static_cast<int64_t>(wv.frames));
      }
      e.stream = wav;
      e.resolved = true;
    }
  }
  const auto [it, inserted] =
      streamSndEntries_.emplace(name, std::move(e));
  return &it->second;
}

bool MdkBridge::streamAudioResolve_(
    const std::string& name, mdk::TraversalAudioSoundDef& def) {
  // The stream registrations first — the slot plays resolve through
  // the mode-5 binds; then the still-loaded SNI banks (the original's
  // process-global record list, which the marker names search too).
  if (const AudioEntry_* e = streamSndEntry_(name); e && e->resolved) {
    def = e->def;
    return true;
  }
  return audioResolve_(name, def);
}

const MdkBridge::AudioEntry_* MdkBridge::cmdAudioEntry_(
    const std::string& name) {
  // The command drain's stream lookup: stream-registered names (and
  // names already decoded from STREAM.BNI) come from the stream
  // bank; everything else is an SNI record. The registration table
  // lives until the next mode-5 entry, so the teardown tail still
  // resolves stream names correctly after the scene dies.
  if (streamAudio_.regForName(name) != nullptr)
    return streamSndEntry_(name);
  if (const auto it = streamSndEntries_.find(name);
      it != streamSndEntries_.end() && it->second.resolved)
    return &it->second;
  // Music-class allowed: the only kStart commands reaching here for
  // flags&2/3 records are the zone-ambient spawns (non-zone events
  // fail the gated resolve before ever queueing a command).
  return audioEntry_(name, /*allowMusicClass=*/true);
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
  if (mode_ == 3 && rt_) {
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

    // Zone-ambient events resolve the flags&3 music-class records the
    // arena CMI structures name — the original fader drives them
    // through the same instance pool.
    const auto res = [this](const std::string& n,
                            mdk::TraversalAudioSoundDef& d,
                            bool allowMusicClass) {
      return audioResolve_(n, d, allowMusicClass);
    };
    const auto posFn = [this](int cat, const void* key, float p[3]) {
      return audioOwnerPos_(cat, key, p);
    };
    for (const auto& ev : rt_->audioFx) {
      const bool zone =
          ev.owner == mdk::TraversalAudioOwner::kZone;
      audioMixer_.applyEvent(
          ev, [zone, &res](const std::string& n,
                           mdk::TraversalAudioSoundDef& d) {
            return res(n, d, zone);
          });
    }
    rt_->audioFx.clear();
    audioMixer_.tick(lastDtSec_, posFn);
  }
  // Mode 5's listener/events/sweep already ran inside stepStream_
  // at the pinned cadence; every other mode only drains the queued
  // tail (the traversal/stream bank-teardown stops). The drain
  // itself is mode-agnostic — the pool is process-global.

  std::vector<mdk::TraversalAudioCmd> cmds;
  audioMixer_.drain(cmds);
  for (const mdk::TraversalAudioCmd& c : cmds) {
    Dictionary d;
    d["id"] = int64_t(c.handle);
    d["name"] = String(c.name.c_str());
    switch (c.op) {
      case mdk::TraversalAudioCmdOp::kStart: {
        d["op"] = "start";
        const AudioEntry_* e = cmdAudioEntry_(c.name);
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

void MdkBridge::syncBindings_() {
  if (!feShell_) return;
  const mdk::FrontendFlowController& f = feShell_->flow();
  bindings_.keys = f.keyGlobals();
  bindings_.mouseOn = f.mouseOn();
  bindings_.mouseAxesMap = f.mouseAxesMap();
  bindings_.mouseButtMask = f.mouseButtMap();
  bindings_.mouseScale = f.mouseScales();
  bindings_.mouseYReversedBits = f.mouseYReversedBits();
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
  // Mode routing (0x541492): mode 2 runs the freefall core, mode 5
  // the StreamScene, mode 3 (and the standalone load_level path)
  // runs traversal.
  syncBindings_();   // live flow table -> gameplay bindings
  if (mode_ == 2) return stepFreefall_(dt_ms, action_mask, input);
  if (mode_ == 5) return stepStream_(dt_ms, action_mask, input);
  if (mode_ == 8) return stepEnding_(dt_ms, action_mask, input);
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

  // Dispatcher mode-3 death tail: 541554 is the shared global — the
  // session reads the live runtime value each frame. Phase 14B's
  // pump (progressionStepDeath) gates itself on health==0 / god flag,
  // stages the 0x540dac fade, and past 255 arms the LASTGAME record,
  // which commits through SaveStore::writeLastgame (FUN_00427ed4)
  // before the FUN_004371bc teardown + FUN_0041d85c frontend return.
  if (sess_.mode == 3 && rt_) {
    sess_.health = rt_->fieldHealth;
    const mdk::ProgressionError de =
        mdk::progressionStepDeath(sess_, timing_.deltaSec);
    if (de == mdk::ProgressionError::kOk) {
      if (sess_.lastgameArmed && feHost_) {
        mdk::SaveWriteInput wi;
        wi.modeField = sess_.lastgame.modeField;
        wi.levelId = sess_.lastgame.levelId;
        wi.health = sess_.lastgame.health;
        wi.deathCount = sess_.lastgame.deathCount;
        wi.field54163b = sess_.lastgame.field54163b;
        (void)feHost_->saves().writeLastgame(wi);
      }
      routeFrom_ = mode_;                 // 3
      traversalTeardown_();
      mode_ = sess_.mode;                 // 0
      routeTo_ = mode_;
      if (feShell_) feShell_->enterFrontend(true);
    }
  }

  // Dispatcher mode-3 tail (0x401497): the runtime's victory latches
  // map onto the session's staged model — endLevelRequest (the
  // 540ebc=-1 consume -> FUN_0040dde0) is victoryPhase 1, da0
  // (takeoffActive) advances 1 -> 2, and 49a030 (takeoffDone)
  // advances 2 -> 3 before the FUN_004371bc teardown + FUN_0042b270
  // mode-5 entry run. 541554 is the shared global — the session
  // syncs the live value at each edge.
  if (sess_.mode == 3 && rt_) {
    if (last_.endLevelRequested && sess_.victoryPhase == 0) {
      sess_.health = rt_->fieldHealth;
      (void)mdk::progressionRequestTraversalEnd(sess_);
    }
    if (last_.takeoffActive && sess_.victoryPhase == 1)
      (void)mdk::progressionAdvanceVictory(sess_);
    if (last_.takeoffDone && sess_.victoryPhase == 2)
      traversalStreamHandoff_();
    if (last_.endingRequested && sess_.mode == 3) {
      // FUN_0047b038 — script op 0x83 0x51 (or the ending cheat):
      // 541492 = 8, 49bd40 = 1 — the first mode-8 frame runs the
      // traversal teardown + FUN_0047b0fc load (stepEnding_'s head).
      (void)mdk::progressionEnterCinematic(sess_);
      mode_ = sess_.mode;                // 8
      routeFrom_ = 3;
      routeTo_ = mode_;
    }
  }

  // The display set is core-driven every frame: portal swaps,
  // partner attach/detach, and corridor (geometry-less) arenas all
  // recompute here — the G1 single-arena desync is gone. A corridor
  // current arena yields no set of its own; the partner's geometry
  // stays up and the corridor's objects still present.
  if (rt_) {
    updateDisplaySet_();
    refreshOrders_();
  }

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
  static const mdk::GameplayInputFrame kEmptyFrame{};
  const mdk::GameplayInputFrame& cf =
      rt_ ? rt_->prevFrame : kEmptyFrame;
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

bool MdkBridge::qa_set_key_global(int64_t index, int64_t code) {
  if (!feShell_) {
    setError_("qa_set_key_global: frontend_boot() first");
    return false;
  }
  if (index < 0 || index >= mdk::kKeyboardGlobalCount) {
    setError_("qa_set_key_global: index out of range");
    return false;
  }
  feShell_->flow().setKeyGlobalForDebug(static_cast<int>(index),
                                      static_cast<int>(code));
  return true;
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
  out["surface_matidx"] = g.surfaceMatIdx;
  out["surface_mats"] = g.surfaceMats;
  out["surface_pen"] = g.surfacePenIdx;
  out["elem_names"] = g.elemNames;
  out["vert_count"] = g.vertCount;
  out["tri_count"] = g.triCount;
  out["elem_count"] = int64_t(o.model.elems.size());
  out["geom_key"] = int64_t(g.geomKey);
  out["model"] = String(o.model.modelName().c_str());
  return out;
}

// Resolve one surface material for a traversal object (oid > 0) or
// a level-model surface (oid <= 0 — shot/named geometry resolved in
// the current display context). The tri record's s16 @+6 selects
// the model name-table slot; negative values bypass it as a flat
// palette pen ((-mi) & 0xff, surfaced through `pen`). Name lookup
// follows matlkup order (FUN_0041a694): shared level MTI bank A
// first, then the arena's embedded .MAT bank B. A miss draws flat
// pen 0xff — the dispatcher's NULL-material arm (OBSERVED
// FUN_0040c860 @0x40c9da for the same dispatch family).
Dictionary MdkBridge::get_object_material(int64_t object_id,
                                          const String& name,
                                          int64_t pen) {
  Dictionary out;
  if (!rt_) return out;

  // Pick the material context. For a live object the bank pair is
  // its own arena's set (the embedded .MAT is arena-local); for
  // oid <= 0 the current display set stands in (level models have
  // no home arena). Corridors have no render block — bank A still
  // resolves through any built set since every set decodes the same
  // shared LEVELnS.MTI bytes.
  int setIdx = -1;
  if (object_id > 0) {
    const void* p = objIds_.find(std::uint64_t(object_id));
    if (p == nullptr) return out;   // stale/unknown id
    const auto& o = *static_cast<const mdk::DynamicObject*>(p);
    setIdx = indexOfArena_(o.arena);
  }
  const ArenaSet* set = nullptr;
  if (setIdx >= 0) {
    if (auto it = arenaSets_.find(setIdx); it != arenaSets_.end()) {
      set = it->second.get();
    }
  }
  if (set == nullptr && arenaIndex_ >= 0) {
    if (auto it = arenaSets_.find(arenaIndex_);
        it != arenaSets_.end()) {
      set = it->second.get();
    }
  }
  const std::uint8_t* pal =
      set ? set->palette.data() : activePalette_();

  const std::string nm = std::string(name.utf8().get_data());
  char keybuf[160];
  std::snprintf(keybuf, sizeof keybuf, "%d:%s:%lld",
                set ? set->index : -1, nm.c_str(),
                static_cast<long long>(pen));
  const String key = String(keybuf);
  out["key"] = key;

  // Negative material index — a flat palette pen; the name table is
  // bypassed entirely. (The effect-class negatives stay folded to
  // pens here — their TRUE drawers are UNKNOWN; documented seam.)
  if (pen >= 0) {
    out["valid"] = true;
    out["palette_index"] = pen;
    if (pen < 256) {
      const std::uint8_t* c = pal + pen * 3;
      out["palette_color"] =
          Color(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f);
    }
    return out;
  }

  // matlkup — bank A (shared level MTI) first, then bank B.
  const mdk::ArenaRenderMaterial* mat = nullptr;
  if (set != nullptr && !nm.empty()) {
    for (const auto& r : set->rd.bankA) {
      if (r.name == nm) { mat = &r; break; }
    }
    if (mat == nullptr) {
      for (const auto& r : set->rd.bankB) {
        if (r.name == nm) { mat = &r; break; }
      }
    }
  }
  if (mat == nullptr || mat->pixels.empty()) {
    // NULL record / index record / empty span — the flat arm. Index
    // records carry their palette index at +0x0c (param0c), the
    // same field freefall reads for GREY*/PEN_*/NONE names.
    const int idx =
        (mat != nullptr && mat->isIndexRecord)
            ? static_cast<int>(mat->param0c)
            : 0xff;
    out["valid"] = true;
    out["palette_index"] = int64_t(idx);
    if (idx >= 0 && idx < 256) {
      const std::uint8_t* c = pal + std::size_t(idx) * 3;
      out["palette_color"] =
          Color(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f);
    }
    out["no_draw"] = idx >= 256;
    return out;
  }

  out["valid"] = true;
  out["palette_index"] = int64_t(-1);
  out["w"] = int64_t(mat->width);
  out["h"] = int64_t(mat->height);
  out["frames"] = int64_t(mat->frameCount);
  // Index 0 is the texture fill's transparent texel key (same as the
  // freefall path — the EXPLODE/FIRE/SB_* FX records are transparent-
  // surround sprites). Report whether any index-0 texel exists so the
  // presenter can gate alpha only where it matters: opaque geometry
  // textures stay in the depth-writing opaque pass.
  {
    bool hasAlpha = false;
    const std::size_t npix =
        std::size_t(mat->width) * std::size_t(mat->height);
    for (std::size_t i = 0; i < npix && i < mat->pixels.size(); ++i) {
      if (mat->pixels[i] == 0) { hasAlpha = true; break; }
    }
    out["has_alpha"] = hasAlpha;
  }
  out["tex"] = objectTexture_(key, *mat, pal);
  return out;
}

Ref<ImageTexture> MdkBridge::objectTexture_(
    const String& key, const mdk::ArenaRenderMaterial& m,
    const std::uint8_t* pal) {
  Ref<ImageTexture> tex;
  if (m.pixels.empty() || m.width <= 0 || m.height <= 0) return tex;
  const std::string ck = std::string(key.utf8().get_data());
  if (auto it = objTexCache_.find(ck); it != objTexCache_.end()) {
    return it->second;
  }
  // Frame 0 of the indexed strip — animated materials (EXPLODE
  // family, frameCount > 1) hold frame*(w*h) slices; the original
  // advances the frame per anim state (deferred — a presentation
  // seam shared with the freefall path).
  const std::size_t npix =
      std::size_t(m.width) * std::size_t(m.height);
  PackedByteArray px;
  px.resize(static_cast<int64_t>(npix) * 4);
  std::uint8_t* dst = px.ptrw();
  for (std::size_t i = 0; i < npix; ++i) {
    const std::uint8_t v = m.pixels[i];
    if (v == 0) {
      dst[i * 4 + 3] = 0;      // index 0 = transparent
      continue;
    }
    dst[i * 4 + 0] = pal[v * 3 + 0];
    dst[i * 4 + 1] = pal[v * 3 + 1];
    dst[i * 4 + 2] = pal[v * 3 + 2];
    dst[i * 4 + 3] = 255;
  }
  Ref<Image> img = Image::create_from_data(
      m.width, m.height, false, Image::FORMAT_RGBA8, px);
  tex = ImageTexture::create_from_image(img);
  objTexCache_[ck] = tex;
  return tex;
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

Dictionary MdkBridge::diagnostic_end_level() {
  Dictionary out;
  out["ok"] = false;
  if (mode_ != 3 || !rt_) {
    setError_("diagnostic_end_level: no live traversal session");
    return out;
  }
  // 0x540ebc = -1 — the mailbox the END_LEVEL script op writes
  // (traversal_script.cpp env.rt->pendingViewSnap). The next stepped
  // frame's FUN_00436100 tail consumes it through FUN_0040dde0 —
  // health floor + masterMoveGate + the victory latches — and the
  // dispatcher tail then runs the real 49a030 edge (traversal
  // teardown -> FUN_0042b270 mode 5). Bounded-test path only: the
  // loaded arena's own trigger object does the same store when the
  // script fires it in normal play.
  rt_->pendingViewSnap = -1;
  out["ok"] = true;
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
  objTexCache_.clear();
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
  out["surface_matidx"] = g.surfaceMatIdx;
  out["surface_mats"] = g.surfaceMats;
  out["surface_pen"] = g.surfacePenIdx;
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
  out["surface_matidx"] = g.surfaceMatIdx;
  out["surface_mats"] = g.surfaceMats;
  out["surface_pen"] = g.surfacePenIdx;
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
  // Size the §4C mask before any ff_veil_mask() call — the first
  // step hasn't run yet but the host may already ask for it.
  ffVeilMask_.clear();

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
  // 19C.3 — the mode-2 sound bank set; the event drain runs per
  // step in stepFreefall_.
  loadFreefallSoundBanks_();
  // FUN_0040ef28 tail — course 0 posts FALL_T1; arm the service
  // (resolve-miss on a course-0 run is a silent no-op per 0x41cb25).
  {
    std::string detail2;
    if (!ffTtEnter_(detail2)) {
      setError_("freefall teletype: " + detail2);
      return false;
    }
  }
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
  // FUN_00410920's frame order — the backdrop pass (FUN_00412530)
  // runs before the object draw walk, fed by the just-updated
  // camera block (0x540b28/2c/30 == rt.cameraPos). The scroll/chunk
  // integrators are dt-scaled (the original's per-frame constants
  // are its 30 fps render cadence); the zoom dither stays per-call.
  mdk::freefallSceneBackdropStep(*ffScene_, ff_->cameraPos[0],
                                 ff_->cameraPos[1], ff_->cameraPos[2],
                                 timing_.deltaSec);
  // The trail veil — FUN_00412970/FUN_0040c860's indexed op
  // (dst = lut[row*256 + dst]) is presented by the Godot-side
  // veil pass: per-object trail meshes (world-space edges already
  // shipped in the object snapshot) depth-test against the 3D
  // bodies and apply the exact LUT remap via screen_texture, so
  // sections composite at their own depth instead of baking under
  // every body. freefallSceneTrailComposite remains the native
  // (headless/test) compositor.
  //
  // §4C — the serial chain. screen_texture snapshots only the opaque
  // pass, so a veil shader reading it sees the surface BELOW every
  // veil, never an earlier veil's output: overlapping veils need
  // L2[L1[p]] and get L2[p] (~86% of (r1,r2,p) triples differ at
  // LUT level). The mask retains the ordered {row,z'} records each
  // veil op covers; the shaders replay them gated by the winning
  // opaque depth (a buried element was overwritten, not remapped).
  mdk::freefallSceneVeilMask(*ffScene_, *ff_,
                           ff_->cameraPos[0], ff_->cameraPos[1],
                           ff_->cameraPos[2], ffVeilMask_);
  // 19C.3 — this frame's kFfEvSound batch into the shared voice
  // pool + the per-step mixer pass (events self-clear at the next
  // step, so the drain runs here, drain-once).
  freefallDrainAudio_();
  // A recorded post drains once the current entry finishes — the
  // OBSERVED queue is a 4-entry ring serviced a frame at a time;
  // course-start FALL_T1 and the pickup-name posts share it. Gating on
  // !ffTtActive_ keeps a queued post from cutting the live one short;
  // a resolve miss drops silently per 0x41cb25.
  if (ff_ && ff_->teletypePost && !ffTtActive_) {
    std::string tdetail;
    (void)ffTtEnter_(tdetail);
  }
  // FUN_0041cb44 — the mode-2 draw block's teletype service; the
  // FALL_T1 entry slides/holds/pages out through it.
  ffTtService_();
  if (done && !ffHandoffDone_) {
    // FUN_0040fa68 — the freefall bank dies at the edge: every live
    // voice releases before the handoff path rebuilds. The queued
    // stops drain on the host's next audio pass (the traversal
    // route's bank reload frees the nodes via the GDScript sweep).
    audioMixer_.stopAll();
    freefallHandoff_();
  }

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
  // The detection->wave->rearm causal loop, counted at its
  // callsites (diagnostic counters — no gameplay effect).
  out["radar_locks"] = int64_t(f.radarLocks);
  out["radar_rearms"] = int64_t(f.radarReArms);
  out["waves_armed"] = int64_t(f.wavesArmed);
  out["missiles_spawned"] = int64_t(f.missilesSpawned);
  {
    // radar_active = live type-3 objects; radar_twin = bound
    // twins (the wedge draw submits when the twin binds);
    // radar_verts = the regenerated ring vert count.
    int active = 0, twins = 0, verts = 0;
    for (int i = f.listHead; i >= 0; i = f.pool[std::size_t(i)].next) {
      if (f.pool[std::size_t(i)].type != 3) continue;
      ++active;
      const mdk::FreefallScene::Twin* t =
          mdk::freefallSceneTwin(*ffScene_, i);
      if (t == nullptr || !t->bound) continue;
      ++twins;
      if (!t->obj.model.elemVerts.empty()) {
        verts += int(t->obj.model.elemVerts[0].size() / 3);
      }
    }
    out["radar_active"] = int64_t(active);
    out["radar_twin"] = int64_t(twins);
    out["radar_verts"] = int64_t(verts);
  }
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
    // Type-3 radar — +0x120 beam point on the scan plane and the
    // +0x1c wander target / +0x24 plane. These are the fields the
    // lock test consumes (player-vs-beam 2D distance, 225 sq).
    d["beam_mdk"] = Vector3(o.tx, o.ty, o.tz);
    d["aux_mdk"] = Vector3(o.aux0, o.aux1, o.aux2);
    // Kind-5 — the launch FLARE (FUN_004109d8 case 5 gate +0x108).
    // subTimer ramps 0->8 while the +0x11c launch timer runs and
    // decays after; the draw selects LUT row 10+count (+srcPx) and
    // always binds FLARE4. Emitted raw — the presenter gates on
    // flare > 0.
    d["flare"] = int64_t(o.subTimer);
    // Kind-1 — the +0x10c marker entry (PICK sprite through the
    // scaled transparent blit, z-1e-5 sort). +0x10c is a relocated
    // sprite-source pointer populated only by the traversal/load
    // fixup (FUN_00426f34; callers sit in 0x42xxxx) — spawned
    // freefall objects get +0x10c=0 from the zeroed alloc and no
    // spawn or tick path writes it, so the marker is never drawn.
    // The pickup's complete visual is the kind-2 medallion (verified
    // against the original freefall AVI: no badge/marker renders).
    d["marker"] = false;
    // The kind-4 trail / kind-3 BANG gates — state only (the FX
    // renders stay documented seams).
    d["trail_fx"] = int64_t(o.fx);
    d["explode_flag"] = int64_t(o.explodeFlag);
    // +0xac — the object basis the kind-2 draw consumes (missiles
    // carry the FUN_0041139c velocity-tracking basis; other objects
    // keep the euler-built one). Emitted raw for pose diagnostics.
    d["basis_mdk"] = PackedFloat32Array{
        o.basis[0], o.basis[1], o.basis[2],
        o.basis[3], o.basis[4], o.basis[5],
        o.basis[6], o.basis[7], o.basis[8]};
    // The element-0 maximum-Y vertex — the missile's model-space
    // nose tip (MISSILE's +Y axis is the heading axis the kind-5
    // basis writes; the tail anchors sit at y~-12.4). Emitted so
    // the presenter can transform actual geometry, not just the
    // basis column, when proving nose-track-velocity.
    if (t != nullptr && !t->obj.model.elemVerts.empty()) {
      const auto& ev = t->obj.model.elemVerts[0];
      float bestY = -1e30f;
      int bi = -1;
      for (std::size_t vi = 0; vi + 2 < ev.size(); vi += 3) {
        if (ev[vi + 1] > bestY) { bestY = ev[vi + 1]; bi = int(vi); }
      }
      if (bi >= 0) {
        d["nose_local"] = Vector3(ev[std::size_t(bi)],
                                  ev[std::size_t(bi) + 1],
                                  ev[std::size_t(bi) + 2]);
      }
    }
    // Kind-4 — the trail ribbon (FUN_0042ee74 over the +0x60 ring).
    // The draw walk iterates `count` slots OLDEST->NEWEST starting
    // at the +0x20 read cursor (mod-32 indexed). Each slot stores
    // world anchors; the drawn edge points taper toward the slot
    // centroid: v_i = centroid + (anchor_i - centroid) * t with
    // t = headRamp[count-1-secIdx] for the newest six slots
    // ({1.0,1.25,1.2,1.1,1.05,1.0} at 0x49b634) then the linear
    // 1-(age-6)/(cap-6) decay. `pens` carries one pen per SECTION
    // (s = 1..count-1 pairing walked slots s-1,s): -1054 for the
    // older body, count-s-1066 (-1058..-1065 -> LUT rows 29-36)
    // for the newest eight (OBSERVED 0x42efba/0x42f096).
    if (const mdk::FreefallScene::Twin::Trail* tr =
            mdk::freefallSceneTrail(*ffScene_, i)) {
      if (tr->count > 1) {
        constexpr float kHeadRamp[6] =
            {1.0f, 1.25f, 1.2f, 1.1f, 1.05f, 1.0f};
        constexpr int cap = mdk::FreefallScene::Twin::kTrailCap;
        PackedVector3Array edges;
        PackedFloat32Array taper;
        PackedInt32Array pens;
        edges.resize(tr->count * 2);
        taper.resize(tr->count);
        pens.resize(tr->count - 1);
        for (int s = 0; s < tr->count; ++s) {
          const int k = (tr->read + s) & (cap - 1);
          const int age = tr->count - 1 - s;
          const float t =
              age < 6 ? kHeadRamp[age]
                      : std::max(0.0f, 1.0f - float(age - 6) /
                                              float(cap - 6));
          const float* l = tr->pts[k][0];
          const float* r = tr->pts[k][1];
          const float cx = (l[0] + r[0]) * 0.5f;
          const float cy = (l[1] + r[1]) * 0.5f;
          const float cz = (l[2] + r[2]) * 0.5f;
          const mdkfront::Vec3 gl = mdkfront::mdkVecToGodot(
              cx + (l[0] - cx) * t,
              cy + (l[1] - cy) * t,
              cz + (l[2] - cz) * t);
          edges[s * 2] = Vector3(gl.x, gl.y, gl.z);
          const mdkfront::Vec3 gr = mdkfront::mdkVecToGodot(
              cx + (r[0] - cx) * t,
              cy + (r[1] - cy) * t,
              cz + (r[2] - cz) * t);
          edges[s * 2 + 1] = Vector3(gr.x, gr.y, gr.z);
          taper[s] = t;
        }
        for (int s = 1; s < tr->count; ++s) {
          pens[s - 1] = int32_t(
              mdk::freefallTrailSectionPen(tr->count, s));
        }
        Dictionary td;
        td["edges"] = edges;
        td["taper"] = taper;
        td["pens"] = pens;
        td["count"] = int64_t(tr->count);
        td["anchors"] = int64_t(tr->anchors);
        d["trail"] = td;
      }
    }
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

// LEVEL%d — the indexed minecrawler surface image bound at 0x4edc24
// for the FUN_00412530 backdrop pass. One-shot upload: the presenter
// expands the pixels through the FALLP palette and builds the
// approach ground texture. Returns {} without a scene/record.
Dictionary MdkBridge::get_freefall_backdrop() {
  Dictionary out;
  if (!ffScene_ || !ffScene_->backdropOk) return out;
  out["w"] = int64_t(ffScene_->backdropW);
  out["h"] = int64_t(ffScene_->backdropH);
  PackedByteArray px;
  px.resize(static_cast<int64_t>(ffScene_->backdropPixels.size()));
  std::memcpy(px.ptrw(), ffScene_->backdropPixels.data(),
              ffScene_->backdropPixels.size());
  out["pixels"] = px;
  out["course"] = int64_t(ffScene_->course);
  return out;
}

// FUN_00412530's per-frame product: the rendered 600x360 indexed
// framebuffer palette-expanded to RGBA, the frame's sampling
// diagnostics, and the LUT keyframe row colors the presenter needs
// to reproduce the trail/flare veil materials.
Dictionary MdkBridge::get_freefall_backdrop_frame() {
  Dictionary out;
  if (!ffScene_ || ffScene_->backdropFrame.empty() ||
      !ffScene_->paletteOk) {
    return out;
  }
  constexpr int kW = 600, kH = 360;
  PackedByteArray rgba;
  rgba.resize(int64_t(kW) * kH * 4);
  const std::uint8_t* idx = ffScene_->backdropFrame.data();
  const std::uint8_t* pal = ffScene_->palette.data();
  std::uint8_t* w = rgba.ptrw();
  for (int i = 0; i < kW * kH; ++i) {
    const std::uint8_t c = idx[i];
    w[i * 4 + 0] = pal[c * 3 + 0];
    w[i * 4 + 1] = pal[c * 3 + 1];
    w[i * 4 + 2] = pal[c * 3 + 2];
    w[i * 4 + 3] = 255;
  }
  out["w"] = int64_t(kW);
  out["h"] = int64_t(kH);
  out["rgba"] = rgba;
  const auto& dg = ffScene_->backdropDiag;
  out["p"] = double(dg.p);
  out["u_start"] = double(dg.uStart);
  out["v_start"] = double(dg.vStart);
  out["scroll_row"] = int64_t(dg.scrollRow);
  out["zoom_table"] = int64_t(dg.zoomTable);
  out["chunk_frame"] = int64_t(dg.chunkFrame);
  out["chunk_x"] = double(dg.chunkX);
  out["chunk_y"] = double(dg.chunkY);
  out["chunk_w"] = double(dg.chunkW);
  out["chunk_h"] = double(dg.chunkH);
  // The LUT's 64 keyframe row colors — kept for the kind-5 FLARE4
  // texture expansion (the trail veil is now composited exactly in
  // index space before upload).
  PackedByteArray keys;
  keys.resize(64 * 3);
  std::memcpy(keys.ptrw(), ffScene_->keyColors.data(), 64 * 3);
  out["key_colors"] = keys;
  // The raw remap table itself — veil materials sample it as a
  // 256x64 R8 texture (texel x = dst index, y = row) to reproduce
  // dst = lut[row*256 + dst] on the GPU. First 64 rows = bank 0,
  // which is the bank every mode-2 veil pen references.
  if (ffScene_->lut.size() >= 64 * 256) {
    PackedByteArray lt;
    lt.resize(64 * 256);
    std::memcpy(lt.ptrw(), ffScene_->lut.data(), 64 * 256);
    out["lut"] = lt;
  }
  // Required-resource readiness — a rejected ZOOM record or missing
  // LEVEL/POD must never ride through as a valid all-black frame.
  // `ready` is the acceptance gate; the per-field flags name the
  // failure for diagnostics.
  const mdk::FreefallBackdropStatus st =
      mdk::freefallSceneBackdropStatus(*ffScene_);
  out["ready"] = st.ready;
  out["res_palette"] = st.palette;
  out["res_level"] = st.level;
  out["res_pod"] = st.pod;
  out["res_lut"] = st.lut;
  out["res_chunks"] = int64_t(st.chunks);
  out["zoom_count"] = int64_t(st.zoom);
  out["zoom_mask"] = int64_t(st.zoomMask);
  out["res_flare4"] = st.flare4;
  out["res_pick"] = st.pick;
  return out;
}

// One-shot upload of the kind-5 FLARE4 and kind-1 PICK indexed
// sprites (BNI {u16 w, u16 h, px} records). The presenter expands
// FLARE4's 0..8 values through the LUT row colors (kind-5 uses
// dst = lut[10 + count + srcPx][dstPx]) and PICK through palette.
Dictionary MdkBridge::get_freefall_sprites() {
  Dictionary out;
  if (!ffScene_) return out;
  auto emit = [](Dictionary& d, const char* key,
                 const mdk::FreefallScene::BackdropSprite& sp) {
    if (sp.px.empty() || sp.w <= 0 || sp.h <= 0) return;
    Dictionary s;
    s["w"] = int64_t(sp.w);
    s["h"] = int64_t(sp.h);
    PackedByteArray px;
    px.resize(static_cast<int64_t>(sp.px.size()));
    std::memcpy(px.ptrw(), sp.px.data(), sp.px.size());
    s["pixels"] = px;
    d[key] = s;
  };
  emit(out, "flare4", ffScene_->flare4);
  emit(out, "pick", ffScene_->pick);
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
  // Index 0 is the texture fill's transparent texel key (the
  // EXPLODE record is a transparent-surround fireball). Report
  // whether any index-0 texel exists so the presenter can gate
  // alpha only where it matters — opaque textures stay in the
  // depth-writing opaque pass.
  bool hasAlpha = false;
  const std::size_t npix =
      std::size_t(m->width) * std::size_t(m->height);
  for (std::size_t i = 0; i < npix && i < m->pixels.size(); ++i) {
    if (m->pixels[i] == 0) { hasAlpha = true; break; }
  }
  out["has_alpha"] = hasAlpha;
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

// ---------------------------------------------------------------------------
// Phase 19B.1 — Mode-5 StreamScene presentation host
//
// The StreamScene core (Phase 19A, CLOSED) owns the simulation; this
// block is the host half of its documented seams: the FUN_0042b270
// resource bind, the 600x360 indexed surface, the 768B DAC surface,
// the toroidal backdrop copy, the FUN_00403a40 scaled sprite blit,
// the FUN_004185fc HUD blit, the TELETYPE line draws, the terminal
// palette fill, and the kPresent/kExitMode frame boundaries. The
// 0c860 model/ribbon raster family stays deferred (Phase 19B.2) —
// the events are consumed and counted, never rasterized/reordered.
// ---------------------------------------------------------------------------

bool MdkBridge::load_stream(int64_t course, int64_t skill,
                            int64_t seed) {
  if (!root_) {
    setError_("initialize() first");
    return false;
  }
  shutdown();   // drops rt_/ff_/stream_ buffers on reload; root_ kept
  if (course < 0 || course > 4 || skill < 0 || skill > 2) {
    setError_("load_stream: course 0..4, skill 0..2");
    return false;
  }
  // The standalone entry arms the same session globals the campaign
  // carries into mode 5 (541492/541498/54147a/541554 + the shared
  // LCG state) — a fresh-session health=100.
  sess_.mode = 5;
  sess_.levelId = int(course);
  sess_.skill = int(skill);
  sess_.rng = std::uint32_t(seed);
  sess_.health = 100;
  std::string detail;
  if (!campaignStreamEnter_(detail)) {
    setError_("load_stream: " + detail);
    return false;
  }
  return true;
}

bool MdkBridge::streamLoadAssets_(std::string& detail) {
  stream_.reset();
  streamPresenter_.reset();
  streamProtoKurt_.reset();
  streamProtoBones_.reset();
  streamProtoProf_.reset();
  streamProtoEsc_.reset();
  streamAssets_ = mdk::StreamAssets{};
  streamImageNames_.clear();
  streamMtiBytes_.clear();
  streamBankA_.clear();

  std::string err;
  auto bni = root_->readFile("STREAM/STREAM.BNI", 1 << 28, &err);
  if (!bni) {
    detail = "STREAM/STREAM.BNI: " + err;
    return false;
  }
  // STREAM.MTI — the mode-5 model material bank (the original binds
  // it via the 496e10 multi-file table, bank A for model+0x10
  // lookups). Fail-soft on a directory miss: models still load and
  // every textured pen falls back to flat 0xff like the original's
  // unresolved path.
  auto mti = root_->readFile("STREAM/STREAM.MTI", 1 << 28, &err);
  if (mti) {
    streamMtiBytes_ = std::move(*mti);
    const std::span<const std::byte> mtiSpan(
        streamMtiBytes_.data(), streamMtiBytes_.size());
    const mdk::MtiDirectory mdir = mdk::inspectMtiDirectory(mtiSpan);
    if (mdir.status == mdk::MtiDirectoryStatus::kOk) {
      streamBankA_.reserve(mdir.entries.size());
      for (const mdk::MtiEntry& e : mdir.entries) {
        mdk::ArenaRenderMaterial rec;
        if (mdk::arenaRenderMaterialDecode(
                mtiSpan, e.payloadFileOffset(), e.fieldAt0x08,
                e.fieldAt0x0C, e.fieldAt0x10, e.nameField, &rec))
          streamBankA_.push_back(std::move(rec));
      }
    }
  }
  auto fti = root_->readFile("MISC/MDKFONT.FTI", 1 << 28, &err);
  if (!fti) {
    detail = "MISC/MDKFONT.FTI: " + err;
    return false;
  }
  // The mode-5 HUD consumes the ENGINE-side image table slots
  // (FUN_00418688 at 0x49a828), not STREAM records — slot 2 = SC_STAT
  // status icon, slot 7 = the SNIP_TXT digit strip (OBSERVED).
  auto hud = root_->readFile("TRAVERSE/TRAVSPRT.BNI", 1 << 28, &err);
  if (!hud) {
    detail = "TRAVERSE/TRAVSPRT.BNI: " + err;
    return false;
  }
  streamBniBytes_ = std::move(*bni);
  streamFtiBytes_ = std::move(*fti);
  streamHudBytes_ = std::move(*hud);

  const auto bdir = mdk::inspectBniDirectory(
      std::span<const std::byte>(streamBniBytes_.data(),
                                 streamBniBytes_.size()));
  if (bdir.status != mdk::BniDirectoryStatus::kOk) {
    detail = std::string("STREAM.BNI: ") +
             std::string(mdk::bniDirectoryStatusName(bdir.status)) +
             " — " + bdir.detail;
    return false;
  }
  const auto hdir = mdk::inspectBniDirectory(
      std::span<const std::byte>(streamHudBytes_.data(),
                                 streamHudBytes_.size()));
  if (hdir.status != mdk::BniDirectoryStatus::kOk) {
    detail = std::string("TRAVSPRT.BNI: ") +
             std::string(mdk::bniDirectoryStatusName(hdir.status)) +
             " — " + hdir.detail;
    return false;
  }
  const auto fdir = mdk::inspectFtiDirectory(
      std::span<const std::byte>(streamFtiBytes_.data(),
                                 streamFtiBytes_.size()));
  if (fdir.status != mdk::FtiDirectoryStatus::kOk) {
    detail = std::string("MDKFONT.FTI: ") +
             std::string(mdk::ftiDirectoryStatusName(fdir.status)) +
             " — " + fdir.detail;
    return false;
  }

  auto tagOf = [&](const mdk::BniDirectory& d,
                   const char* name) -> int {
    const mdk::BniRecord* r = mdk::findBniRecord(d, name);
    return r ? static_cast<int>(r - d.records.data()) : -1;
  };
  auto payload = [&](const mdk::BniDirectory& d,
                     const std::vector<std::byte>& bytes,
                     const char* name) -> std::span<const std::byte> {
    const mdk::BniRecord* r = mdk::findBniRecord(d, name);
    if (!r) return {};
    return {bytes.data() + r->payloadFileOffset,
            static_cast<std::size_t>(r->payloadEnd -
                                     r->payloadFileOffset)};
  };

  // The proven STREAM palette compose (stream_context.h): 192B
  // SYS_PAL head + PAL[0xc0..0x300) — the 768B scene palette.
  std::array<std::uint8_t, 768> streamPal{};
  if (const mdk::FtiRecord* sp =
          mdk::findFtiRecord(fdir, mdk::kStreamSystemRecord)) {
    streamAssets_.paletteGlobal =
        reinterpret_cast<const std::uint8_t*>(
            streamFtiBytes_.data() + sp->payloadFileOffset);
    std::memcpy(streamPal.data(), streamAssets_.paletteGlobal, 192);
  }
  const std::span<const std::byte> pal =
      payload(bdir, streamBniBytes_, "PAL");
  if (!pal.empty()) {
    streamAssets_.palettePal =
        reinterpret_cast<const std::uint8_t*>(pal.data()) +
        mdk::kStreamPaletteTailOffset;
    std::memcpy(streamPal.data() + 192, streamAssets_.palettePal,
                576);
  }
  const std::span<const std::byte> palSpan(
      reinterpret_cast<const std::byte*>(streamPal.data()),
      streamPal.size());
  // 19B.2A — the ribbon raster LUT builds from the composed BASE
  // palette (native 0x4ed758 — the same bytes the core's
  // StreamScene::palette() holds), never the faded DAC surface.
  streamPresenter_.bindRibbonPalette(streamPal.data());

  // The indexed image records — decoded into the presenter's table
  // keyed by the tag the core echoes in its events. Fail-soft per
  // record: a missing image leaves the event counted-but-undrawn.
  auto bindIndexed = [&](const mdk::BniDirectory& d,
                         const std::vector<std::byte>& bytes,
                         const char* name, int tag,
                         int* outW = nullptr,
                         int* outH = nullptr) -> bool {
    const std::span<const std::byte> p = payload(d, bytes, name);
    if (p.empty() || tag < 0) return false;
    std::string derr;
    auto img = mdk::decodeBniIndexedImage(p, palSpan, &derr);
    if (!img) return false;
    if (outW) *outW = img->width;
    if (outH) *outH = img->height;
    streamImageNames_[tag] = name;
    streamPresenter_.bindImage(tag, std::move(*img));
    return true;
  };

  mdk::StreamAssets& a = streamAssets_;
  a.bgTag = tagOf(bdir, "BG");
  bindIndexed(bdir, streamBniBytes_, "BG", a.bgTag);
  int planetW = 0, planetH = 0;
  a.planetTag[0] = tagOf(bdir, "PLANET");
  // The native's PLANET table entry is {img, w, h, w*h} — the draw
  // record reads [1]/[2] as the source dims (the e55c tail).
  if (bindIndexed(bdir, streamBniBytes_, "PLANET", a.planetTag[0],
                  &planetW, &planetH)) {
    a.planetTag[1] = planetW;
    a.planetTag[2] = planetH;
    a.planetTag[3] = planetW * planetH;
  }
  a.lightTag = tagOf(bdir, "LIGHT");
  bindIndexed(bdir, streamBniBytes_, "LIGHT", a.lightTag);
  a.sndWind = tagOf(bdir, "WIND");
  a.sndHitside = tagOf(bdir, "HITSIDE");
  a.sndRescue = tagOf(bdir, "RESCUE");
  a.sndApple = tagOf(bdir, "APPLE");
  for (int i = 0; i != 7; ++i) {
    char nm[8];
    std::snprintf(nm, sizeof nm, "HURT%d", i + 1);
    a.sndHurt[i] = tagOf(bdir, nm);
  }
  // 19B.3B1 — the eleven FUN_004039c8+02e2c sound registrations
  // (0x2b3d8..0x2b578): authored vol 0x7fff everywhere, the DS-loop
  // flag on WIND alone. The host's per-lifetime census and the
  // name->entry cache reset with the registration table; the BNI
  // directory stays bound for payload lookups until the next entry
  // (the teardown tail still resolves names through it).
  streamAudio_.reset();
  mdkbridge::streamAudioBindRegs(streamAudio_, bdir);
  streamBniDir_ = bdir;
  streamSndEntries_.clear();
  streamAudioDecodeMisses_ = 0;

  // Engine HUD table slots (0x49a828): 2 = SC_STAT, 7 = SNIP_TXT —
  // the OBSERVED native bindings; the tags are those slot ids.
  a.hudIconTag = 2;
  a.hudDigitTag = 7;
  bindIndexed(hdir, streamHudBytes_, "SC_STAT", a.hudIconTag,
              &a.hudIconW, &a.hudIconH);
  bindIndexed(hdir, streamHudBytes_, "SNIP_TXT", a.hudDigitTag,
              &a.hudDigitW, &a.hudDigitH);

  auto bindProto = [&](const char* name,
                       std::optional<mdk::RuntimeModel>& out) {
    const mdk::BniRecord* r = mdk::findBniRecord(bdir, name);
    if (!r) return;
    const std::byte* p =
        streamBniBytes_.data() + r->payloadFileOffset;
    const std::size_t n = static_cast<std::size_t>(
        r->payloadEnd - r->payloadFileOffset);
    // Re-head for the shared parser — the original passes the flag
    // word as FUN_00428400's EDX arg (flag=1, OBSERVED); BNI payloads
    // omit it. Same convention as --stream-init/traversalShotModel.
    std::vector<std::uint8_t> headed(4 + n);
    const std::uint8_t fl[4] = {1, 0, 0, 0};
    std::memcpy(headed.data(), fl, 4);
    std::memcpy(headed.data() + 4, p, n);
    out = mdk::parseGeometryRecord(headed.data(),
                                   headed.data() + headed.size());
  };
  bindProto("KURT", streamProtoKurt_);
  bindProto("BONES", streamProtoBones_);
  bindProto("PROFSHIP", streamProtoProf_);
  const bool isFinal = sess_.levelId >= 4;
  bindProto(isFinal ? "GUNTA" : "SWH150", streamProtoEsc_);
  a.protoKurt = streamProtoKurt_ ? &*streamProtoKurt_ : nullptr;
  a.protoBones = streamProtoBones_ ? &*streamProtoBones_ : nullptr;
  a.protoProfship = streamProtoProf_ ? &*streamProtoProf_ : nullptr;
  a.protoEscort = streamProtoEsc_ ? &*streamProtoEsc_ : nullptr;

  // 19B.2C — the model+0x10 material tables: each name-table slot
  // resolves against STREAM.MTI (bank A) — bank B is unbound for
  // stream models. streamBankA_ is fully populated by here (the
  // pointers alias its storage; deepCopyModel propagates the table
  // into spawned pool objects verbatim, matching the original's
  // shared material arena).
  {
    const std::span<const mdk::ArenaRenderMaterial> bank(
        streamBankA_.data(), streamBankA_.size());
    for (auto* mp : {&streamProtoKurt_, &streamProtoBones_,
                     &streamProtoProf_, &streamProtoEsc_})
      if (*mp) mdk::resolveModelMaterials(**mp, bank);
  }

  // 19B.3A — the BONES.WHITE stale-bank oracle. The only bank-B
  // material table that could exist at this point is the traversal
  // arena's embedded .MAT inside rt_; FUN_004371bc (the mode-3
  // dispatcher tail's traversalTeardown_) frees it before the
  // FUN_0042b270 entry runs, so rt_ must already be null here.
  // Anything else means a prior-mode bank survived the transition.
  streamBankARecs_ = static_cast<int>(streamBankA_.size());
  streamBankBBound_ = (rt_ != nullptr);
  streamWhiteSlot_ = -1;
  streamWhiteResolved_ = false;
  streamUnresolvedMats_ = 0;
  auto auditModel = [&](const std::optional<mdk::RuntimeModel>& m,
                        bool bones) {
    if (!m) return;
    for (std::size_t i = 0; i < m->names.size(); ++i) {
      if (i < m->materials.size() && m->materials[i] == nullptr)
        ++streamUnresolvedMats_;
      if (!bones) continue;
      const std::string nm(m->names[i].name.data(),
                           strnlen(m->names[i].name.data(), 12));
      if (nm == "WHITE") {
        streamWhiteSlot_ = static_cast<int>(i);
        streamWhiteResolved_ =
            i < m->materials.size() && m->materials[i] != nullptr;
      }
    }
  };
  auditModel(streamProtoKurt_, false);
  auditModel(streamProtoBones_, true);
  auditModel(streamProtoProf_, false);
  auditModel(streamProtoEsc_, false);

  auto payloadPtr = [&](const char* name) -> const std::uint8_t* {
    const std::span<const std::byte> p =
        payload(bdir, streamBniBytes_, name);
    return p.empty() ? nullptr
                     : reinterpret_cast<const std::uint8_t*>(
                         p.data());
  };
  a.animEscort = payloadPtr(isFinal ? "GUNTANIM" : "SWHANM");
  a.animBones = payloadPtr("BONESANIM");
  a.animKurt = payloadPtr("KURTANIM");
  a.animHvr = payloadPtr("FL_HVR");
  a.animWave = payloadPtr("FL_WAVE");
  // The bound the ObjectAnimView walk is checked against — the BNI
  // image end (the --stream-init convention).
  a.animLimit = reinterpret_cast<const std::uint8_t*>(
      streamBniBytes_.data() + streamBniBytes_.size());

  // TELETYPE fonts — FONTBIG (the plain + scaled renderers) and the
  // FONTSML fallback for the 600px measure overflow.
  std::string ferr;
  auto decodeFont = [&](const char* name)
      -> std::optional<mdk::FtiFont> {
    const mdk::FtiRecord* r = mdk::findFtiRecord(fdir, name);
    if (!r) return std::nullopt;
    return mdk::decodeFtiFont(
        std::span<const std::byte>(
            streamFtiBytes_.data() + r->payloadFileOffset,
            static_cast<std::size_t>(r->payloadEnd -
                                     r->payloadFileOffset)),
        &ferr);
  };
  const auto fbBig = decodeFont("FONTBIG");
  const auto fbSml = decodeFont("FONTSML");
  if (fbBig && fbSml) streamPresenter_.bindFonts(*fbBig, *fbSml);

  return true;
}

bool MdkBridge::campaignStreamEnter_(std::string& detail) {
  // The mode-5 entry — StreamAssets bind + scene init. The session
  // carries levelId/skill/rng/health (the campaign globals — no
  // fresh reset here; load_stream seeds them for the standalone
  // path).
  if (!streamLoadAssets_(detail)) return false;
  stream_ = std::make_unique<mdk::StreamScene>();
  if (!stream_->init(streamAssets_, sess_.levelId, sess_.skill,
                     sess_.rng, sess_.health)) {
    detail = "StreamScene::init failed";
    stream_.reset();
    return false;
  }
  // 19B.2B1 — kModelDraw aux is a pool index; resolve it against the
  // live scene pool (std::array storage is stable for the scene's
  // lifetime; the lambda re-checks stream_ so a stale consume after
  // teardown is a counted miss, never a deref of freed storage).
  streamPresenter_.bindModelResolver(
      [this](int i) -> const mdk::DynamicObject* {
        if (!stream_ || i < 0 || i >= mdk::kStreamPoolSize)
          return nullptr;
        return &stream_->objectAt(i);
      });
  // Synthetic 46c650 clock — the canonical harness' +33ms/frame feed
  // (host pacing is a later phase; this keeps draws deterministic).
  streamNowMs_ = 1000;
  streamFrameSeq_ = 0;
  timing_ = mdk::FrontendTimingState{};
  prevKeyLevel_ = {};
  mode_ = 5;
  ++mode5Enters_;
  hasFrame_ = false;
  return true;
}

// --- Phase 19E — mode-6 briefing host ------------------------------
// FUN_00429cb4's bind: MISC/STATS.BNI's L<levelId+1>_MAP record plus
// MDKFONT.FTI's SYS_PAL head / FONTBIG / BRIEF<levelId+1>. The buffers
// outlive the bound spans (member storage, reset on exit).
bool MdkBridge::briefingEnter_(std::string& detail) {
  std::string err;
  auto bni = root_->readFile("MISC/STATS.BNI", 1 << 28, &err);
  if (!bni) {
    detail = "MISC/STATS.BNI: " + err;
    return false;
  }
  auto fti = root_->readFile("MISC/MDKFONT.FTI", 1 << 28, &err);
  if (!fti) {
    detail = "MISC/MDKFONT.FTI: " + err;
    return false;
  }
  briefingBniBytes_ = std::move(*bni);
  briefingFtiBytes_ = std::move(*fti);

  // SYS_PAL head (192B) — the working palette's entries 0..63
  // (FUN_0040163c's resident copy; the map tail fills 64..255).
  briefingSysHead_.assign(0xc0, 0);
  const auto fdir = mdk::inspectFtiDirectory(
      std::span<const std::byte>(briefingFtiBytes_.data(),
                                 briefingFtiBytes_.size()));
  if (const mdk::FtiRecord* r = mdk::findFtiRecord(fdir, "SYS_PAL")) {
    const std::size_t n = static_cast<std::size_t>(
        r->payloadEnd - r->payloadFileOffset);
    if (n >= 0xc0) {
      std::memcpy(briefingSysHead_.data(),
                  briefingFtiBytes_.data() + r->payloadFileOffset,
                  0xc0);
    }
  }

  // FONTBIG — the briefing's typed-text font (FUN_00414c34 path).
  {
    const mdk::FtiRecord* r = mdk::findFtiRecord(fdir, "FONTBIG");
    if (!r) {
      detail = "FONTBIG not found in MDKFONT.FTI";
      return false;
    }
    std::string ferr;
    briefingFont_ = mdk::decodeFtiFont(
        std::span<const std::byte>(
            briefingFtiBytes_.data() + r->payloadFileOffset,
            static_cast<std::size_t>(r->payloadEnd -
                                     r->payloadFileOffset)),
        &ferr);
    if (!briefingFont_) {
      detail = "FONTBIG decode: " + ferr;
      return false;
    }
  }

  mdk::Mode6BriefingAssets a;
  a.font = &*briefingFont_;
  a.sysHead = std::span<const std::uint8_t>(briefingSysHead_.data(),
                                            briefingSysHead_.size());

  // L<levelId+1>_MAP — the 4B header + 768B palette + 600x360 image.
  {
    const auto bdir = mdk::inspectBniDirectory(
        std::span<const std::byte>(briefingBniBytes_.data(),
                                   briefingBniBytes_.size()));
    char name[16];
    std::snprintf(name, sizeof(name), "L%d_MAP", sess_.levelId + 1);
    const mdk::BniRecord* r = mdk::findBniRecord(bdir, name);
    if (!r) {
      detail = std::string(name) + " not found in STATS.BNI";
      return false;
    }
    const auto* bp = reinterpret_cast<const std::uint8_t*>(
        briefingBniBytes_.data());
    a.mapRecord.assign(bp + r->payloadFileOffset, bp + r->payloadEnd);
  }

  // BRIEF<levelId+1> — NUL-terminated page text via FUN_00414890.
  {
    char name[16];
    std::snprintf(name, sizeof(name), "BRIEF%d", sess_.levelId + 1);
    const mdk::FtiRecord* r = mdk::findFtiRecord(fdir, name);
    if (!r) {
      detail = std::string(name) + " not found in MDKFONT.FTI";
      return false;
    }
    const char* p = reinterpret_cast<const char*>(
        briefingFtiBytes_.data() + r->payloadFileOffset);
    const std::size_t n = static_cast<std::size_t>(
        r->payloadEnd - r->payloadFileOffset);
    a.briefText.assign(p, p + n);
    if (a.briefText.empty() || a.briefText.back() != '\0')
      a.briefText.push_back('\0');
  }

  briefing_ = std::make_unique<mdk::Mode6Briefing>();
  briefing_->enter(sess_.levelId, a);
  briefingFrameSeq_ = 0;
  return true;
}

Dictionary MdkBridge::mode6_frame() {
  Dictionary out;
  if (!briefing_ || briefingFrameSeq_ == 0) return out;
  const mdk::IndexedFramebuffer& fb = briefingFb_;
  out["w"] = fb.width();
  out["h"] = fb.height();
  PackedByteArray rgba;
  rgba.resize(static_cast<int64_t>(fb.pixelCount()) * 4);
  std::uint8_t* dst = rgba.ptrw();
  for (std::size_t i = 0; i < fb.pixelCount(); ++i) {
    const mdk::Palette::Color c = briefingPal_.get(fb.pixels()[i]);
    dst[i * 4 + 0] = c.r;
    dst[i * 4 + 1] = c.g;
    dst[i * 4 + 2] = c.b;
    dst[i * 4 + 3] = c.a;
  }
  out["rgba"] = rgba;
  out["seq"] = static_cast<int64_t>(briefingFrameSeq_);
  out["diag"] = mode6_diag();
  return out;
}

Dictionary MdkBridge::mode6_diag() {
  Dictionary out;
  if (!briefing_) return out;
  out["fade_in"] = briefing_->fadeIn();
  out["fade_out"] = briefing_->fadeOut();
  out["cursor"] = briefing_->charCursor();
  out["typing_done"] = briefing_->typingDone();
  out["exit_done"] = briefing_->exitDone();
  out["skip"] = briefing_->skipLatch();
  out["hurry"] = briefing_->hurryLatch();
  out["presented"] = int64_t(briefingFrameSeq_);
  return out;
}

// --- Freefall teletype (FALL_T1 post + FUN_0041cb44 subset) --------
// The mode-2 draw block services the engine-global queue once per
// frame; this subset covers the single pending FALL_T1 entry —
// consume -> slide-in (holdTimer 0->0.5) -> steady hold (charTimer
// rate->0) -> page-out (holdTimer 0.5->0) -> idle. Constants are the
// OBSERVED ones: dt30 = 0x3d088889 (the 0x49b6f4 step), y = 0x78
// (1-line), line-2 offsets 0x69/0x87, slide scale = hold*2, steady
// renderer 0 (FONTBIG-center / FONTSML-overflow), slide/page
// renderer 1 (scaled FONTBIG).

bool MdkBridge::ffTtEnter_(std::string& detail) {
  ffTtFontBig_.reset();
  ffTtFontSml_.reset();
  ffTtLines_ = 0;
  ffTtLine_[0].clear();
  ffTtLine_[1].clear();
  ffTtChar_ = ffTtHold_ = 0.0f;
  ffTtFlags_ = 0;
  ffTtPending_ = false;
  ffTtActive_ = false;
  ffTtFrameSeq_ = 0;
  ffTtFb_.pixels();  // keep storage alive
  std::fill(ffTtFb_.pixels(), ffTtFb_.pixels() + ffTtFb_.pixelCount(),
            0);
  for (int i = 0; i < 256; ++i)
    ffTtPal_.set(i, {0, 0, 0, 0});   // fully transparent base
  if (!ff_ || !ff_->teletypePost) return true;  // course > 0: none

  // Consume the recorded post — the mailbox clears so a later pickup's
  // post is picked up by the per-frame drain in stepFreefall_ (the
  // OBSERVED queue is a 4-entry ring; this covers the pending-post case).
  const auto post = *ff_->teletypePost;
  ff_->teletypePost.reset();

  // FUN_0041cad0's FTI resolve (FUN_00414890) + the renderer fonts —
  // MISC/MDKFONT.FTI carries FALL_T1, FONTBIG, FONTSML and SYS_PAL.
  std::string err;
  auto fti = root_->readFile("MISC/MDKFONT.FTI", 1 << 28, &err);
  if (!fti) {
    detail = "MISC/MDKFONT.FTI: " + err;
    return false;
  }
  const auto fdir = mdk::inspectFtiDirectory(
      std::span<const std::byte>(fti->data(), fti->size()));
  auto decodeFont = [&](const char* name)
      -> std::optional<mdk::FtiFont> {
    const mdk::FtiRecord* r = mdk::findFtiRecord(fdir, name);
    if (!r) return std::nullopt;
    return mdk::decodeFtiFont(
        std::span<const std::byte>(
            fti->data() + r->payloadFileOffset,
            static_cast<std::size_t>(r->payloadEnd -
                                     r->payloadFileOffset)),
        &err);
  };
  ffTtFontBig_ = decodeFont("FONTBIG");
  ffTtFontSml_ = decodeFont("FONTSML");
  if (const mdk::FtiRecord* r = mdk::findFtiRecord(fdir, "SYS_PAL")) {
    const auto n = static_cast<std::size_t>(r->payloadEnd -
                                          r->payloadFileOffset);
    std::memcpy(ffTtSysHead_.data(),
                fti->data() + r->payloadFileOffset,
                std::min<std::size_t>(n, 768));
  }
  // Palette: index 0 stays transparent (the strip clear); the text
  // indices take the resident head (glyphs index <= 62).
  for (int i = 1; i < 64; ++i)
    ffTtPal_.set(i, {ffTtSysHead_[i * 3 + 0], ffTtSysHead_[i * 3 + 1],
                     ffTtSysHead_[i * 3 + 2], 255});

  const mdk::FtiRecord* r = mdk::findFtiRecord(fdir, post.name);
  if (!r) {
    // The resolve-miss path (0x41cb25): post fails silently — no
    // queue entry ever lands.
    return true;
  }
  const char* p = reinterpret_cast<const char*>(
      fti->data() + r->payloadFileOffset);
  const std::size_t n = static_cast<std::size_t>(r->payloadEnd -
                                               r->payloadFileOffset);
  // The consume pass splits at the literal "\n" escape into up to
  // two lines (the queue's 36-col buffers — the shipped record is a
  // single short line).
  std::string text(p, strnlen(p, n));
  std::size_t nl = text.find("\\n");
  ffTtLine_[0] = nl == std::string::npos ? text : text.substr(0, nl);
  ffTtLine_[1] = nl == std::string::npos ? "" : text.substr(nl + 2);
  ffTtLines_ = ffTtLine_[1].empty() ? 1 : 2;
  ffTtFlags_ = post.flags;
  ffTtChar_ = post.rate;
  ffTtHold_ = 0.0f;
  ffTtPending_ = false;
  ffTtActive_ = true;
  return true;
}

void MdkBridge::ffTtDraw_(int renderer, int line, int y, float scale) {
  if (line < 0 || line >= ffTtLines_) return;
  const std::string& text = ffTtLine_[line];
  if (text.empty() || !ffTtFontBig_ || !ffTtFontSml_) return;
  const int fw = ffTtFb_.width();
  if (renderer == 0) {
    // FUN_00414d2c — centered FONTBIG, FONTSML when its measure
    // overflows the 600px frame (FUN_00414f1c).
    const int wBig = mdk::measureFtiText(
        *ffTtFontBig_, text, mdk::kFtiFontBigMissingAdvance);
    if (wBig >= fw) {
      const int w = mdk::measureFtiText(
          *ffTtFontSml_, text, mdk::kFtiFontSmlMissingAdvance);
      mdk::drawFtiText(*ffTtFontSml_, text, ffTtFb_, (fw - w) / 2, y,
                       mdk::kFtiFontSmlMissingAdvance);
    } else {
      mdk::drawFtiText(*ffTtFontBig_, text, ffTtFb_, (fw - wBig) / 2,
                       y, mdk::kFtiFontBigMissingAdvance);
    }
  } else {
    // FUN_0041518c — centered FONTBIG scaled.
    const int w = mdk::measureFtiText(
        *ffTtFontBig_, text, mdk::kFtiFontBigMissingAdvance);
    const int x = static_cast<int>(
        (fw - w * static_cast<double>(scale)) * 0.5);
    mdk::drawFtiTextScaled(*ffTtFontBig_, text, ffTtFb_, x, y, scale,
                           mdk::kFtiFontBigMissingAdvance);
  }
}

void MdkBridge::ffTtService_() {
  if (!ffTtActive_) return;
  constexpr float kDt30 = 0.03333553f;  // 0x3d088889 — 0x49b6f4 step
  // DrawEnable = the frame-due flag — always 1 on the presented path.
  std::fill(ffTtFb_.pixels(), ffTtFb_.pixels() + ffTtFb_.pixelCount(),
            0);
  ++ffTtFrameSeq_;
  const auto zero = [](float v) { return !(v > 0) && !(v < 0); };
  const auto eq = [](float v, float c) { return !(v > c) && !(v < c); };
  const bool oneLine = ffTtLines_ == 1;

  if (zero(ffTtChar_)) {
    if (!zero(ffTtHold_)) {
      // ---- page-out (0x41cdba) -----------------------------------
      const float s2 = ffTtHold_ * 2.0f;
      if (oneLine) {
        ffTtDraw_(1, 0, 0x78, s2);
      } else {
        const float t = s2 * 15.0f;
        ffTtDraw_(1, 0, int(std::lround(120.0f - t)), s2);
        ffTtDraw_(1, 1, int(std::lround(t + 120.0f)), s2);
      }
      float h = ffTtHold_ - kDt30;
      ffTtHold_ = h < 0.0f ? 0.0f : h;
      if (zero(ffTtHold_)) ffTtActive_ = false;   // idle
    } else {
      ffTtActive_ = false;                        // nothing pending
    }
  } else if ((ffTtFlags_ & 1u) && !eq(ffTtHold_, 0.5f)) {
    // ---- slide-in (0x41cc1a) -------------------------------------
    const float s2 = ffTtHold_ * 2.0f;
    if (oneLine) {
      ffTtDraw_(1, 0, 0x78, s2);
    } else {
      const float t = s2 * 15.0f;
      ffTtDraw_(1, 0, int(std::lround(120.0f - t)), s2);
      ffTtDraw_(1, 1, int(std::lround(t + 120.0f)), s2);
    }
    float h = kDt30 + ffTtHold_;
    ffTtHold_ = h > 0.5f ? 0.5f : h;
  } else {
    // ---- steady hold (0x41cb8e) ----------------------------------
    if (oneLine) {
      ffTtDraw_(0, 0, 0x78, 0.0f);
    } else {
      ffTtDraw_(0, 0, 0x69, 0.0f);
      ffTtDraw_(0, 1, 0x87, 0.0f);
    }
    // localRate = dt30 (queue drained — the pending-work x2 arm is
    // empty here).
    float t = ffTtChar_ - kDt30;
    ffTtChar_ = t < 0.0f ? 0.0f : t;
  }
}

Dictionary MdkBridge::ff_teletype_frame() {
  Dictionary out;
  if (!ffTtActive_ || ffTtFrameSeq_ == 0) return out;
  out["w"] = ffTtFb_.width();
  out["h"] = ffTtFb_.height();
  PackedByteArray rgba;
  rgba.resize(static_cast<int64_t>(ffTtFb_.pixelCount()) * 4);
  std::uint8_t* dst = rgba.ptrw();
  for (std::size_t i = 0; i < ffTtFb_.pixelCount(); ++i) {
    const mdk::Palette::Color c = ffTtPal_.get(ffTtFb_.pixels()[i]);
    dst[i * 4 + 0] = c.r;
    dst[i * 4 + 1] = c.g;
    dst[i * 4 + 2] = c.b;
    dst[i * 4 + 3] = c.a;
  }
  out["rgba"] = rgba;
  out["seq"] = static_cast<int64_t>(ffTtFrameSeq_);
  return out;
}

Dictionary MdkBridge::ff_teletype_diag() const {
  Dictionary out;
  out["posted"] = ff_ && ff_->teletypePost.has_value();
  out["active"] = ffTtActive_;
  out["char_timer"] = ffTtChar_;
  out["hold_timer"] = ffTtHold_;
  out["flags"] = int64_t(ffTtFlags_);
  out["lines"] = ffTtLines_;
  out["line0"] = String(ffTtLine_[0].c_str());
  out["line1"] = String(ffTtLine_[1].c_str());
  out["presented"] = int64_t(ffTtFrameSeq_);
  return out;
}

namespace {
// IEEE-754 binary16 (round-to-nearest via the standard bit trick —
// enough for row ints and z' values up to ~5300).
std::uint16_t f32ToF16(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4);
  const std::uint32_t sign = (u >> 16) & 0x8000u;
  const int exp = int((u >> 23) & 0xff) - 127 + 15;
  std::uint32_t man = u & 0x7fffffu;
  if (exp <= 0) return std::uint16_t(sign);           // underflow -> 0
  if (exp >= 31) return std::uint16_t(sign | 0x7bffu); // clamp to max
  return std::uint16_t(sign | (std::uint32_t(exp) << 10) |
                       (man >> 13));
}
} // namespace

// §4C — pack the VeilMask for the Godot upload: Image FORMAT_RGBAH,
// width 600*4, height 360; px column x carries its 8 records as 4
// texels {row0,z0, row1,z1} in R,G then B,A. Empty slots read -1.0.
Dictionary MdkBridge::ff_veil_mask() {
  Dictionary out;
  constexpr int kK = mdk::VeilMask::kK;
  constexpr int kTexPerPx = kK / 2;
  constexpr int kW = mdk::VeilMask::kW * kTexPerPx;
  constexpr int kH = mdk::VeilMask::kH;
  const int pxCount = mdk::VeilMask::kW * mdk::VeilMask::kH;
  // Never built (load_freefall ran, first step hasn't): emit an
  // all-empty mask rather than touching unallocated storage.
  if (int(ffVeilMask_.counts.size()) != pxCount) {
    ffVeilMask_.clear();
  }
  if (ffVeilMaskBytes_.size() !=
      int64_t(kW) * kH * 8) {
    ffVeilMaskBytes_.resize(int64_t(kW) * kH * 8);
  }
  std::uint16_t* dst =
      reinterpret_cast<std::uint16_t*>(ffVeilMaskBytes_.ptrw());
  for (int px = 0; px < pxCount; ++px) {
    const int n = std::min<int>(ffVeilMask_.counts[px], kK);
    const mdk::VeilMask::Rec* recs =
        ffVeilMask_.recs.data() + std::size_t(px) * kK;
    std::uint16_t* t = dst + std::size_t(px) * kK * 2;
    for (int k = 0; k < kK; ++k) {
      const float row =
          k < n ? float(recs[k].row) : -1.0f;
      const float z = k < n ? recs[k].z : 0.0f;
      t[k * 2 + 0] = f32ToF16(row);
      t[k * 2 + 1] = f32ToF16(z);
    }
  }
  out["w"] = kW;
  out["h"] = kH;
  out["data"] = ffVeilMaskBytes_;
  out["ops"] = int64_t(ffVeilMask_.ops);
  out["elems"] = int64_t(ffVeilMask_.elems);
  out["overflow"] = int64_t(ffVeilMask_.overflow);
  int covered = 0, multi = 0;
  for (int px = 0; px < pxCount; ++px) {
    if (ffVeilMask_.counts[px] >= 1) ++covered;
    if (ffVeilMask_.counts[px] >= 2) ++multi;
  }
  out["covered"] = int64_t(covered);
  out["multi"] = int64_t(multi);
  return out;
}

Dictionary MdkBridge::stepStream_(double dt_ms, int64_t action_mask,
                                  const Dictionary* input) {
  Dictionary out;
  out["ok"] = true;
  out["mode"] = mode_;
  if (!stream_) {
    setError_("load_stream() first");
    out["ok"] = false;
    return out;
  }
  (void)dt_ms;   // pinned 1/30 — the sim's per-step frame delta.
                 // Host pacing (19C.1) lives in the caller's step
                 // cadence: interactive paths run one step per
                 // ~33.3ms of wall time; deterministic harnesses
                 // call this once per 33.333ms quanta. The limiter
                 // sees in.nowMs — step-quantized +33 either way.
  // Host input fold (the FUN_00407f2c domain — digital +-180 on the
  // two steering axes; the runtime consumes the resolved axes). The
  // live "keys" list lands here through the configured binding table
  // (KeyLeft/Right steer axis0, KeyLookUp/Down steer axis1 — the
  // same slots traversal binds); the QA actions mask stays as an
  // alias for harness dicts.
  const mdk::RawGameplayInput raw = buildRawInput_(action_mask, input);
  const auto heldKey = [&](int slot) -> bool {
    const int gi = mdk::kKeyboardSlotToGlobal[slot];
    const int code = bindings_.keys[gi];
    return code > 0 && code < mdk::kGameplayKeyCount &&
           ((raw.keyLevel[code >> 5] >> (code & 31)) & 1u) != 0;
  };
  mdk::StreamInput in{};
  if ((action_mask & kActTurnLeft) || heldKey(0)) in.axis0 = -180.0f;
  if ((action_mask & kActTurnRight) || heldKey(1)) in.axis0 = 180.0f;
  if ((action_mask & kActLookUp) || heldKey(10)) in.axis1 = 180.0f;
  if ((action_mask & kActLookDown) || heldKey(11)) in.axis1 = -180.0f;
  in.nowMs = static_cast<std::uint32_t>(streamNowMs_);
  streamNowMs_ += 33;

  const int listenerBefore = stream_->seams().listener;
  const bool running = stream_->step(in, 1.0f / 30.0f);
  // Event drain — order is the core's emission order (backdrop ->
  // sprites -> teletype -> HUD -> present; kExitMode may cut the
  // frame early). The scene's paletteDac is the live DAC surface the
  // upload events apply.
  // 19B.3B1 — the audio half of the frame: the counted 0x4026f8
  // seam's host side feeds camView_ (0x540bb0) + the limiter scalar
  // (0x49b6f0 — pinned 1.0 while pacing is deferred) to the shared
  // pool's listener — gated on the core's own seam count so the
  // early-exit step, which never reaches the frame's camera arm,
  // feeds nothing. The sound events translate in emission order,
  // then the sweep ticks once at the pinned sim cadence.
  if (stream_->seams().listener != listenerBefore)
    streamAudio_.updateListener(stream_->camView(), timing_.smoothed);
  const auto streamRes = [this](const std::string& n,
                                mdk::TraversalAudioSoundDef& d) {
    return streamAudioResolve_(n, d);
  };
  for (const mdk::StreamEvent& ev : stream_->events()) {
    streamAudio_.consume(ev, audioMixer_, streamRes);
    streamPresenter_.consume(ev, stream_->paletteDac());
  }
  stream_->clearEvents();
  streamAudio_.tick(audioMixer_, 1.0 / 30.0);

  const bool frameReady = streamPresenter_.framePending();
  if (frameReady) {
    streamPresenter_.clearFramePending();
    ++streamFrameSeq_;
  }
  if (!running || stream_->finished()) streamHandoff_();

  out["mode"] = mode_;
  out["sess_mode"] = sess_.mode;
  out["level_id"] = sess_.levelId;
  out["exited"] = stream_ == nullptr;
  out["frame_ready"] = frameReady;
  out["seq"] = static_cast<int64_t>(streamFrameSeq_);
  out["diag"] = stream_diag();
  return out;
}

void MdkBridge::streamHandoff_() {
  // Dispatcher exit (0x4015c3): the scene's terminal globals write
  // back to the campaign session, the FUN_0042c824 teardown frees
  // the protos/objects, then the tally-done progression step routes
  // health<=0 -> frontend / levelId<4 -> loader / else mode 7.
  if (stream_) {
    const mdk::StreamSnapshot s = stream_->snapshot();
    sess_.health = s.health;
    sess_.rng = stream_->rng();
    // 19B.3A route audit — the terminal state handed to the tally
    // step: the presented-frame seq, the completion tag, and the
    // health writeback the <= 0 death gate reads.
    streamExitFrame_ = static_cast<int64_t>(streamFrameSeq_);
    streamExitReason_ = static_cast<int>(s.completionSrc) + 1;
    streamExitHealth_ = s.health;
    stream_->teardown();
    // 19B.3B1 — teardown emits the WIND stop (FUN_004020b4(eda84))
    // into the event vector AFTER the step drain; consume the tail
    // batch before the scene storage dies, then the bank-free arm:
    // FUN_0042c824's sound-bank death releases every still-playing
    // instance — the mixer's stops land in the same drain as the
    // explicit WIND stop.
    const auto tailRes = [this](const std::string& n,
                                  mdk::TraversalAudioSoundDef& d) {
      return streamAudioResolve_(n, d);
    };
    for (const mdk::StreamEvent& ev : stream_->events()) {
      streamAudio_.consume(ev, audioMixer_, tailRes);
      streamPresenter_.consume(ev, stream_->paletteDac());
    }
    stream_->clearEvents();
    stream_.reset();
    audioMixer_.stopAll();
    ++streamTeardowns_;
  }
  routeFrom_ = mode_;
  const mdk::ProgressionError e =
      mdk::progressionStepIntermission(sess_, true);
  if (e != mdk::ProgressionError::kOk)
    setError_(std::string("stream handoff: ") +
              std::string(mdk::progressionErrorName(e)));
  mode_ = sess_.mode;
  routeTo_ = mode_;
  if (sess_.mode == 6) ++mode6Enters_;
  if (sess_.mode == 0 && feShell_)
    // The dead-hero route lands the campaign's frontend exit — the
    // same fresh entry the progression pump's mode-0 arm runs
    // (returning=false; only the mode-8 tail passes nonzero).
    feShell_->enterFrontend(false);
  hasFrame_ = false;
}

void MdkBridge::traversalTeardown_() {
  // FUN_004371bc — the traversal teardown: the runtime and every
  // presentation structure derived from it die at the edge
  // (objects, arenas, scripts, the bank-B MTO overlay table — the
  // core free lives inside traversalRuntimeLoad's own teardown
  // path; the bridge's mirrored display state dies with them). The
  // session globals carry — 541554/ammo/inventory/rng/541498 have
  // no writer in the teardown's call graph — and the frontend
  // shell/save root are host-level and survive, matching the
  // original's process-global lifetime.
  rt_.reset();
  objIds_ = mdkfront::MdkObjectIds{};
  // The traversal sound bank dies with the runtime — every live
  // instance releases here (the bank-free teardown arm); the queued
  // stops drain into the next frame's presenter pass alongside
  // mode 5's own commands.
  audioMixer_.stopAll();
  arenaSets_.clear();
  objTexCache_.clear();
  arenaSetFailed_.clear();
  arenaName_.clear();
  displaySet_.clear();
  arenaIndex_ = -1;
  arenaLoaded_ = false;
}

void MdkBridge::traversalStreamHandoff_() {
  // Dispatcher mode-3 tail (0x401497): 49a030 consumed + cleared,
  // the fade globals arm (a presentation seam — StreamPresenter
  // applies its own fades), FUN_004371bc teardown, then
  // FUN_0042b270 — the mode-5 entry — which writes 541492 = 5.
  // The progression session stages the same sequence: 2 -> 3 on
  // the latch, teardown -> 5.
  (void)mdk::progressionAdvanceVictory(sess_);          // 2 -> 3
  const mdk::ProgressionError e =
      mdk::progressionTraversalTeardown(sess_);          // -> mode 5
  if (e != mdk::ProgressionError::kOk) {
    setError_(std::string("traversal teardown: ") +
              std::string(mdk::progressionErrorName(e)));
    return;
  }
  // The shared-globals writeback — FUN_004371bc's teardown has no
  // writer for health/ammo/rng/levelId because the original keeps
  // them in one storage; the port's runtime owns its own copy, so
  // the values the runtime mutated flow back to the session here
  // (the same carry the freefall handoff performs).
  sess_.health = rt_->fieldHealth;   // 541554 — the shared global
  sess_.rng = rt_->rngState;
  sess_.ammo = rt_->ammo;            // 54161f..33 — grants carry
  routeFrom_ = mode_;              // 3
  traversalTeardown_();
  routeTo_ = 5;
  std::string detail;
  if (!campaignStreamEnter_(detail))
    setError_("campaign stream entry: " + detail);
}

// --- Phase 19D — mode-8 ending cinematic ---------------------------

bool MdkBridge::endingEnter_() {
  std::string err;
  auto flc = root_->readFile("MISC/FLIC/MDKEND.FLC", 1 << 28, &err);
  if (!flc) {
    setError_("MISC/FLIC/MDKEND.FLC: " + err);
    return false;
  }
  endingFlicBytes_ = std::move(*flc);
  ending_ = std::make_unique<mdk::EndingCinematic>();
  if (!ending_->open(
          std::span<const std::byte>(endingFlicBytes_), &err)) {
    setError_("MISC/FLIC/MDKEND.FLC: " + err);
    ending_.reset();
    endingFlicBytes_.clear();
    return false;
  }
  // FUN_0047b0fc resolves the FINISH.BNI handles (DOGSHIP/DROP/
  // FLYBY/EXPLODE1/ENDEXP). Missing bank = silent marks — the
  // original's lookup fatals, but a host-missing bank should not
  // kill the cinematic; the miss surfaces via ending_diag.
  if (auto bni = root_->readFile("MISC/FINISH.BNI", 1 << 28, &err)) {
    endingBniBytes_ = std::move(*bni);
    endingBniDir_ = mdk::inspectBniDirectory(
        std::span<const std::byte>(endingBniBytes_));
  }
  endingSndEntries_.clear();
  endingSeq_ = 0;
  endingMveBoundary_ = false;
  mode_ = 8;
  return true;
}

void MdkBridge::endingTeardown_() {
  ending_.reset();
  endingFlicBytes_.clear();
  endingBniBytes_.clear();
  endingBniDir_ = mdk::BniDirectory{};
  endingSndEntries_.clear();
  mve_.reset();
  mveIdx_.clear();
  mvePendingIdx_.clear();
  mveStage_ = false;
  mveAudioPending_ = false;
  mveEof_ = false;
  mveFrameValid_ = false;
  mvePendingValid_ = false;
  mveClockLive_ = false;
}

const MdkBridge::AudioEntry_* MdkBridge::endingSndEntry_(
    const std::string& name) {
  if (const auto it = endingSndEntries_.find(name);
      it != endingSndEntries_.end()) {
    return &it->second;
  }
  AudioEntry_ e;
  if (const mdk::BniRecord* rec =
          mdk::findBniRecord(endingBniDir_, name)) {
    std::span<const std::byte> payload(
        endingBniBytes_.data() + rec->payloadFileOffset,
        static_cast<std::size_t>(rec->payloadEnd -
                                 rec->payloadFileOffset));
    auto riffAt = [](std::span<const std::byte> s, std::size_t i) {
      return s.size() >= i + 4 && s[i] == std::byte('R') &&
             s[i + 1] == std::byte('I') && s[i + 2] == std::byte('F') &&
             s[i + 3] == std::byte('F');
    };
    if (!riffAt(payload, 0) && riffAt(payload, 4)) {
      payload = payload.subspan(4);
    }
    mdk::SniWave wv;
    std::string derr;
    const mdk::SniWaveStatus st = mdk::decodeSniWave(payload, &wv,
                                                   &derr);
    if (st != mdk::SniWaveStatus::kOk) {
      UtilityFunctions::printerr(
          "MdkBridge: FINISH.BNI '", String(name.c_str()),
          "' decode failed: ", mdk::sniWaveStatusName(st).data(),
          " — ", derr.c_str());
    } else {
      e.def.volume = 0x7fff;
      e.def.rateHz = wv.rateHz;
      e.def.frames = static_cast<std::uint32_t>(wv.frames);
      Ref<AudioStreamWAV> wav;
      wav.instantiate();
      wav->set_format(wv.bitsPerSample == 8
                          ? AudioStreamWAV::FORMAT_8_BITS
                          : AudioStreamWAV::FORMAT_16_BITS);
      wav->set_stereo(false);
      wav->set_mix_rate(wv.rateHz);
      // RIFF PCM8 is unsigned-biased; FORMAT_8_BITS wants
      // signed. One conversion here at the host boundary
      // (pcmForGodotWav) — wv.pcm stays verbatim, PCM16
      // passes through untouched.
      const std::vector<std::uint8_t> pcm =
          mdkbridge::pcmForGodotWav(wv);
      PackedByteArray data;
      data.resize(static_cast<int64_t>(pcm.size()));
      std::memcpy(data.ptrw(), pcm.data(), pcm.size());
      wav->set_data(data);
      e.stream = wav;
      e.resolved = true;
    }
  }
  const auto [it, inserted] =
      endingSndEntries_.emplace(name, std::move(e));
  return &it->second;
}

// Phase 19E — FUN_0047b674's file-open edge: the Interplay MVE
// library is substituted by the staged FFmpeg dylibs (mve_player.*).
// Success arms the MVE stage; any failure takes the original's
// iVar1==0 route straight to the frontend (result 3 = missing).
bool MdkBridge::endingTryMve_() {
#if MDK_WITH_MVE
  std::string err;
  const auto path = root_->resolve("MISC/FLIC/MDKBZK.MVE", &err);
  if (!path) {
    setError_("MISC/FLIC/MDKBZK.MVE: " + err);
    return false;
  }
  auto p = std::make_unique<mdkbridge::MvePlayer>();
  if (!p->open(path->string().c_str(), &err)) {
    setError_("MISC/FLIC/MDKBZK.MVE: " + err);
    return false;
  }
  mve_ = std::move(p);
  mveStage_ = true;
  mveAudioPending_ = true;
  mveWallSec_ = 0.0;
  mveClockSec_ = -1.0;
  mveClockLive_ = false;
  mveEof_ = false;
  mveFrameValid_ = false;
  mvePendingValid_ = false;
  mveIdx_.clear();
  mvePal_.fill(0);
  mvePresented_ = 0;
  mveDecodedQ_ = 0;
  endingSeq_ = 0;   // MVE seq restarts — the FLIC count is its own
  return true;
#else
  setError_("MDKBZK.MVE: bridge built without MVE support "
            "(run frontend/godot/fetch_ffmpeg.sh)");
  return false;
#endif
}

// Shared end-of-cinematic tail — natural EOF and keypress abort take
// the same post-movie route (FUN_0047d30a teardown -> FUN_00413b20),
// the missing-file route is FUN_00408eb0; both converge on the
// returning frontend entry.
void MdkBridge::endingFinish_(int result, Dictionary* out) {
  endingResult_ = result;
  (void)mdk::progressionStepCinematic(sess_, true);
  endingTeardown_();
  mode_ = sess_.mode;                        // 0
  if (feShell_) feShell_->enterFrontend(true);
  routeFrom_ = 8;
  routeTo_ = 0;
  out->operator[]("mode") = mode_;
  out->operator[]("sess_mode") = sess_.mode;
  out->operator[]("ending_result") = result;
}

Dictionary MdkBridge::stepEnding_(double dt_ms, int64_t action_mask,
                                  const Dictionary* input) {
  (void)action_mask;
  Dictionary out;
  out["ok"] = true;
  out["mode"] = mode_;
  if (!ending_ && !mveStage_) {
    // The 49bd40 arm — the mode-8 frame head runs the traversal
    // teardown (session globals carry first, same writeback the
    // mode-5 handoff performs) then FUN_0047b0fc's load.
    if (rt_) {
      sess_.health = rt_->fieldHealth;
      sess_.rng = rt_->rngState;
      sess_.ammo = rt_->ammo;
      traversalTeardown_();
    }
    if (!endingEnter_()) {
      // FUN_0047b674's iVar1==0 shape — a missing/unreadable FLIC
      // skips the player straight to the FUN_0041d85c frontend
      // return (the same route the MVE boundary takes).
      (void)mdk::progressionStepCinematic(sess_, true);
      endingTeardown_();
      mode_ = sess_.mode;                // 0
      if (feShell_) feShell_->enterFrontend(true);
      out["mode"] = mode_;
      out["sess_mode"] = sess_.mode;
      out["ending_missing"] = true;
      return out;
    }
  }
  if (mveStage_) {
    // --- MVE stage (Phase 19E) -----------------------------------
    // Clock: the soundtrack WAV's playback position while it's
    // live (Godot feeds ending_set_audio_clock); accumulated step
    // dt before start and after natural audio end.
    mveWallSec_ += dt_ms / 1000.0;
    const double clockMs =
        (mveClockLive_ ? mveClockSec_ : mveWallSec_) * 1000.0;
    // Abort: any fresh key edge kills the movie (the WndProc
    // abort the FUN_0047b674 pump dispatches to). Held levels and
    // mouse motion do not abort.
    if (input) {
      bool abort = false;
      const Dictionary& d = *input;
      if (d.has("confirm") && bool(d["confirm"])) abort = true;
      if (d.has("cancel") && bool(d["cancel"])) abort = true;
      if (d.has("typed") && int64_t(d["typed"]) != 0) abort = true;
      if (d.has("raw_edges")) {
        const PackedInt32Array raw = d["raw_edges"];
        for (int i = 0; i < raw.size(); ++i)
          if (raw[i] != 0) abort = true;
      }
      if (abort) {
        endingFinish_(2, &out);
        return out;
      }
    }
    // Present loop — a decoded frame only becomes the surface when
    // its pts is due; one pending frame may wait head-of-queue.
    // Each commit updates the surface; surfaces superseded inside a
    // tick count as decoded-not-presented (catch-up skip).
    int commits = 0;
    if (mvePendingValid_ && mvePendingPtsMs_ <= clockMs) {
      mveIdx_.swap(mvePendingIdx_);
      mvePal_ = mvePendingPal_;
      mvePendingValid_ = false;
      mveFrameValid_ = true;
      ++commits;
    }
    while (!mvePendingValid_ && !mveEof_) {
      std::vector<std::uint8_t> idx;
      std::array<std::uint8_t, 768> pal = mvePal_;
      double pts = -1.0;
      std::string derr;
      bool palDirty = false;
      if (!mve_->nextVideoFrame(&idx, &pal, &palDirty, &pts, &derr)) {
        mveEof_ = true;
        if (!derr.empty()) setError_("mve decode: " + derr);
        break;
      }
      if (pts <= clockMs) {
        mveIdx_.swap(idx);
        mvePal_ = pal;
        mveFrameValid_ = true;
        ++commits;
      } else {
        mvePendingIdx_.swap(idx);
        mvePendingPal_ = pal;
        mvePendingPtsMs_ = pts;
        mvePendingValid_ = true;
      }
    }
    if (commits > 0) {
      ++mvePresented_;                    // the last commit shows
      mveDecodedQ_ += commits - 1;        // superseded inside tick
      endingSeq_ = mvePresented_ + mveDecodedQ_;
    }
    // End: video drained and the movie's own length (max of last
    // frame pts and the soundtrack duration) has played out.
    if (mveEof_ && mve_) {
      const double endMs =
          std::max(mve_->lastPtsMs(), mve_->audioDurationSec() * 1000.0);
      if (clockMs >= endMs) {
        endingFinish_(1, &out);
        return out;
      }
    }
    out["stage"] = int64_t(2);
    out["seq"] = endingSeq_;
    out["mode"] = mode_;
    out["sess_mode"] = sess_.mode;
    return out;
  }
  // One pump per caller-paced limiter tick (~33.3 ms — the file's
  // speed field). tickDelta = 1: 0x49b6e8 reads 1 per tick OBSERVED.
  const mdk::EndingStage st = ending_->step(1.0);
  endingSeq_ = ending_->decoder().decoded();
  out["stage"] = static_cast<int64_t>(st);
  out["mark"] = static_cast<int64_t>(ending_->mark());
  if (st == mdk::EndingStage::kMveBoundary) {
    endingMveBoundary_ = true;
    if (endingTryMve_()) {
      out["mve_open"] = true;
      out["mode"] = mode_;
      out["sess_mode"] = sess_.mode;
      out["seq"] = endingSeq_;
      return out;
    }
    // FUN_0047b674's missing-file edge — MDKBZK.MVE failed to open,
    // so the run continues to FUN_0041d85c exactly as the original
    // does when the player can't load the file.
    endingFinish_(3, &out);
    return out;
  }
  out["mode"] = mode_;
  out["sess_mode"] = sess_.mode;
  out["seq"] = endingSeq_;
  return out;
}

bool MdkBridge::load_ending() {
  if (!root_) {
    setError_("init() first");
    return false;
  }
  sess_.mode = 3;   // the script VM's enclosing frame
  const mdk::ProgressionError e =
      mdk::progressionEnterCinematic(sess_);
  if (e != mdk::ProgressionError::kOk) {
    setError_(std::string("ending entry: ") +
              mdk::progressionErrorName(e));
    return false;
  }
  mode_ = sess_.mode;   // 8 — stepEnding_ materializes the run
  return true;
}

Dictionary MdkBridge::ending_frame() {
  Dictionary out;
  if (mveStage_) {
    if (!mveFrameValid_) return out;
    const int w = mve_->width();
    const int h = mve_->height();
    out["w"] = w;
    out["h"] = h;
    // FUN_00489a50's 640x480 surface keeps the 432x320 movie
    // aspect-preserved — the Godot side letterboxes on this flag.
    out["keep_aspect"] = true;
    out["mve"] = true;
    PackedByteArray rgba;
    rgba.resize(static_cast<int64_t>(w) * h * 4);
    std::uint8_t* dst = rgba.ptrw();
    for (int i = 0; i < w * h; ++i) {
      const std::uint8_t c = mveIdx_[static_cast<std::size_t>(i)];
      dst[i * 4 + 0] = mvePal_[c * 3 + 0];
      dst[i * 4 + 1] = mvePal_[c * 3 + 1];
      dst[i * 4 + 2] = mvePal_[c * 3 + 2];
      dst[i * 4 + 3] = 255;
    }
    out["rgba"] = rgba;
    out["seq"] = endingSeq_;
    out["ramp"] = 0.0;
    out["mve_boundary"] = true;
    return out;
  }
  if (!ending_ || ending_->decoder().decoded() == 0) return out;
  const mdk::FlicDecoder& dec = ending_->decoder();
  const int w = dec.width();
  const int h = dec.height();
  out["w"] = w;
  out["h"] = h;
  const auto pal = ending_->effectivePalette();
  const std::span<const std::byte> px = dec.pixels();
  PackedByteArray rgba;
  rgba.resize(static_cast<int64_t>(w) * h * 4);
  std::uint8_t* dst = rgba.ptrw();
  for (int i = 0; i < w * h; ++i) {
    const std::uint8_t c = static_cast<std::uint8_t>(px[i]);
    dst[i * 4 + 0] = pal[c * 3 + 0];
    dst[i * 4 + 1] = pal[c * 3 + 1];
    dst[i * 4 + 2] = pal[c * 3 + 2];
    dst[i * 4 + 3] = 255;
  }
  out["rgba"] = rgba;
  out["seq"] = endingSeq_;
  out["mark"] = static_cast<int64_t>(ending_->mark());
  out["ramp"] = ending_->rampT();
  out["mve_boundary"] = endingMveBoundary_;
  return out;
}

Array MdkBridge::ending_drain_audio() {
  Array out;
  if (ending_) {
    for (const mdk::EndingEvent& ev : ending_->drainEvents()) {
      Dictionary e;
      switch (ev.kind) {
        case mdk::EndingEvent::kPlayOnce: e["op"] = "play"; break;
        case mdk::EndingEvent::kStop:     e["op"] = "stop"; break;
        default: continue;   // kPaletteDirty — the frame path sees it
      }
      if (ev.name != nullptr) {
        e["name"] = String(ev.name);
        const AudioEntry_* en = endingSndEntry_(ev.name);
        if (en && en->resolved && en->stream.is_valid()) {
          e["stream"] = en->stream;
          e["vol"] = double(mdk::traversalAudioVolDb(
              mdk::traversalAudioScaledVol(en->def.volume,
                                           audioSfxPct_)));
        }
      }
      out.push_back(e);
    }
  }
  // The MVE soundtrack emits once when the stage arms — one
  // AudioStreamWAV carrying the whole decoded interplay_dpcm PCM.
  if (mveStage_ && mveAudioPending_ && mve_) {
    Ref<AudioStreamWAV> wav;
    wav.instantiate();
    wav->set_format(AudioStreamWAV::FORMAT_16_BITS);
    wav->set_stereo(mve_->audioChannels() == 2);
    wav->set_mix_rate(mve_->audioRate());
    PackedByteArray data;
    const std::vector<std::uint8_t>& pcm = mve_->audioPcm();
    data.resize(static_cast<int64_t>(pcm.size()));
    if (!pcm.empty()) std::memcpy(data.ptrw(), pcm.data(), pcm.size());
    wav->set_data(data);
    Dictionary e;
    e["op"] = "mve_audio";
    e["stream"] = wav;
    e["dur"] = mve_->audioDurationSec();
    e["vol"] = double(mdk::traversalAudioVolDb(
        mdk::traversalAudioScaledVol(0x7fff, audioSfxPct_)));
    out.push_back(e);
    mveAudioPending_ = false;
  }
  // Abort mid-movie silences the soundtrack (natural EOF does not —
  // the WAV plays itself out under the post-movie route).
  if (endingResult_ == 2) {
    Dictionary e;
    e["op"] = "mve_stop";
    out.push_back(e);
    endingResult_ = -2;   // emitted once — diag keeps the record
  }
  return out;
}

void MdkBridge::ending_set_audio_clock(double sec) {
  if (sec < 0.0) {
    mveClockLive_ = false;              // soundtrack ended/not live
    return;
  }
  if (sec > mveClockSec_) mveClockSec_ = sec;   // monotonic
  mveClockLive_ = true;
}

Dictionary MdkBridge::ending_diag() {
  Dictionary out;
  out["active"] = ending_ != nullptr || mveStage_;
  out["mve_boundary"] = endingMveBoundary_;
  out["seq"] = endingSeq_;
  out["stage"] = mveStage_ ? 2 : (ending_ ? 1 : 0);
  out["result"] = endingResult_ < 0 ? -endingResult_ : endingResult_;
  if (mve_) {
    out["mve_decoded"] = mve_->videoDecoded();
    out["mve_frames"] = mve_->videoPackets();
    out["mve_presented"] = mvePresented_;
    out["mve_skipped"] = mveDecodedQ_;
    out["mve_eof"] = mveEof_;
    out["mve_w"] = mve_->width();
    out["mve_h"] = mve_->height();
    out["mve_last_pts_ms"] = mve_->lastPtsMs();
    out["mve_clock_s"] = mveClockLive_ ? mveClockSec_ : mveWallSec_;
    out["mve_clock_live"] = mveClockLive_;
    out["mve_audio_s"] = mve_->audioDurationSec();
    out["mve_audio_hz"] = mve_->audioRate();
    out["mve_verr"] = mve_->videoDecodeErrors();
    out["mve_aerr"] = mve_->audioDecodeErrors();
  }
  if (ending_) {
    out["mark"] = static_cast<int64_t>(ending_->mark());
    out["decoded"] =
        static_cast<int64_t>(ending_->decoder().decoded());
    out["frames"] =
        static_cast<int64_t>(ending_->decoder().frameCount());
    out["ramp"] = ending_->rampT();
  }
  int resolved = 0;
  for (const auto& [k, e] : endingSndEntries_)
    if (e.resolved) ++resolved;
  out["snd_resolved"] = resolved;
  out["snd_missed"] =
      static_cast<int64_t>(endingSndEntries_.size()) - resolved;
  return out;
}

Dictionary MdkBridge::stream_frame() {
  Dictionary out;
  const mdkbridge::StreamPresenterDiag& d = streamPresenter_.diag();
  if (d.presented == 0 && d.terminalFills == 0) return out;
  const mdk::IndexedFramebuffer& fb = streamPresenter_.framebuffer();
  const mdk::Palette& pal = streamPresenter_.palette();
  out["w"] = fb.width();
  out["h"] = fb.height();
  // Indexed -> RGBA expand through the applied DAC — a fresh
  // PackedByteArray per call (copy-safe; no core buffer escapes).
  PackedByteArray rgba;
  rgba.resize(static_cast<int64_t>(fb.pixelCount()) * 4);
  std::uint8_t* dst = rgba.ptrw();
  for (std::size_t i = 0; i < fb.pixelCount(); ++i) {
    const mdk::Palette::Color c = pal.get(fb.pixels()[i]);
    dst[i * 4 + 0] = c.r;
    dst[i * 4 + 1] = c.g;
    dst[i * 4 + 2] = c.b;
    dst[i * 4 + 3] = c.a;
  }
  out["rgba"] = rgba;
  out["seq"] = static_cast<int64_t>(streamFrameSeq_);
  out["diag"] = stream_diag();
  return out;
}

Dictionary MdkBridge::stream_diag() {
  Dictionary out;
  const mdkbridge::StreamPresenterDiag& d = streamPresenter_.diag();
  out["presented"] = int64_t(d.presented);
  out["backdrop_blits"] = int64_t(d.backdropBlits);
  out["sprites"] = int64_t(d.sprites);
  out["sprite_drawn"] = int64_t(d.spriteDrawn);
  out["sprite_misses"] = int64_t(d.spriteMisses);      // res + meta
  out["sprite_miss_res"] = int64_t(d.spriteMissRes);  // unbound tag
  out["sprite_miss_meta"] = int64_t(d.spriteMissMeta);// bad bound img
  out["sprite_zero_size"] = int64_t(d.spriteZeroSize);// >>8 collapse
  out["sprite_clipped"] = int64_t(d.spriteClipped);   // offscreen
  out["sprite_transparent"] = int64_t(d.spriteTransparent);
  // The deterministic miss census — every non-drawn outcome bucketed
  // by (class, tag, source dims); the record name joins from the
  // bind table where known.
  {
    Array cen;
    for (const auto& [k, r] : d.spriteCensus) {
      Dictionary e;
      const auto cls = static_cast<mdkbridge::StreamSpriteResult>(
          std::get<0>(k));
      e["cls"] = streamSpriteResultName(cls);
      e["tag"] = int64_t(std::get<1>(k));
      const auto nm = streamImageNames_.find(std::get<1>(k));
      e["name"] = nm != streamImageNames_.end()
                      ? String(nm->second.c_str()) : String();
      e["src_w"] = int64_t(std::get<2>(k));
      e["src_h"] = int64_t(std::get<3>(k));
      e["count"] = int64_t(r.count);
      e["size_min"] = int64_t(r.sizeMin);
      e["size_max"] = int64_t(r.sizeMax);
      cen.push_back(e);
    }
    out["sprite_census"] = cen;
  }
  out["hud_blits"] = int64_t(d.hudBlits);
  out["hud_misses"] = int64_t(d.hudMisses);
  out["teletype_draws"] = int64_t(d.teletypeDraws);
  out["palette_sets"] = int64_t(d.paletteSets);
  // 19B.2B1 — the kModelDraw submitter census: commands = events
  // consumed; resolved = events that bound the object's live model
  // set; lookup_miss = aux out of range / no element set / foreign
  // set; class_rec0 = the 0x4edcc0 fuse-sentinel arm (no geometry);
  // the A..G arms split the submitted tris by the 0c860 dispatch
  // class. The deferred-textured/effect counts are the A/C/F arms.
  out["model_commands"] = int64_t(d.model.commands);
  out["model_resolved"] = int64_t(d.model.resolved);
  out["model_lookup_miss"] = int64_t(d.model.lookupMiss);
  out["model_class_rec0"] = int64_t(d.model.classRec0);
  out["model_elements_walked"] = int64_t(d.model.elementsWalked);
  out["model_elements_masked"] = int64_t(d.model.elementsMasked);
  out["model_tris_walked"] = int64_t(d.model.trisWalked);
  out["model_polys_backface"] = int64_t(d.model.polysBackface);
  out["model_polys_overflow"] = int64_t(d.model.polysOverflow);
  out["model_invalid_geometry"] = int64_t(d.model.invalidGeometry);
  out["model_flushes"] = int64_t(d.model.flushes);
  out["model_pixels"] = static_cast<int64_t>(d.model.raster.pixels);
  out["model_fb_digest"] =
      static_cast<int64_t>(d.model.fbDigest);
  {
    // A..G arm census — [material, flatB, fx47a770, flatD, lut1024,
    // fx46e940, lut1029] over the submitted (winding-passed) tris.
    Array cls;
    for (int i = 0; i != 7; ++i) cls.push_back(int64_t(d.model.matCls[i]));
    out["model_mat_census"] = cls;
    Array br;
    for (const auto& [id, n] : d.model.raster.branch) {
      Dictionary e;
      e["branch"] = streamTriBranchName(
          static_cast<mdkbridge::StreamTriBranch>(id));
      e["count"] = int64_t(n);
      br.push_back(e);
    }
    out["model_branch"] = br;
  }
  out["model_clip_dropped"] = int64_t(d.model.raster.clipDropped);
  // 19B.2B2 — the material-path census: submitted A-arm tris reach
  // the dispatch kMaterial arm, then split into the perspective /
  // affine drawers or the three flat-0xff fallback classes.
  out["model_mat_persp"] = int64_t(d.model.raster.matPersp);
  out["model_mat_affine"] = int64_t(d.model.raster.matAffine);
  out["model_mat_flat"] = int64_t(d.model.raster.matFlat);
  out["model_mat_index_rec"] = int64_t(d.model.raster.matIndexRec);
  out["model_mat_lookup_miss"] =
      int64_t(d.model.raster.matLookupMiss);
  out["model_mat_invalid_rec"] =
      int64_t(d.model.raster.matInvalidRec);
  out["model_mat_clip_fan"] = int64_t(d.model.raster.matClipFan);
  out["model_mat_degenerate"] =
      int64_t(d.model.raster.matDegenerate);
  out["model_mat_zero"] = int64_t(d.model.raster.matZero);
  out["model_tex_lookup_miss"] =
      int64_t(d.model.raster.texLookupMiss);
  out["model_tex_invalid_meta"] =
      int64_t(d.model.raster.texInvalidMeta);
  out["model_mat_pixels"] =
      static_cast<int64_t>(d.model.raster.matPixels);
  out["model_tex_transparent"] =
      static_cast<int64_t>(d.model.raster.texTransparent);
  out["model_mat_rasterized"] = int64_t(d.model.raster.rasterized);
  out["model_deferred_textured"] = int64_t(d.model.matCls[0]);
  out["model_deferred_fx47a770"] = int64_t(d.model.matCls[2]);
  out["model_deferred_fx46e940"] = int64_t(d.model.matCls[5]);
  // 19B.2A — the kRibbonTri raster census: commands = events
  // consumed (== the native golden triangle census), rasterized =
  // triangles that reached a filler (fan members counted), clipped =
  // events that entered 0ca00, clip_dropped = clip-path events with
  // no surviving fan, unsupported = dispatches on an unported
  // 0c860 branch, lut_misses = LUT-row draws with no bound palette.
  out["ribbon_commands"] = int64_t(d.ribbon.commands);
  out["ribbon_rasterized"] = int64_t(d.ribbon.rasterized);
  out["ribbon_zero_pixels"] = int64_t(d.ribbon.zeroPixels);
  out["ribbon_clipped"] = int64_t(d.ribbon.clipped);
  out["ribbon_clip_dropped"] = int64_t(d.ribbon.clipDropped);
  out["ribbon_unsupported"] = int64_t(d.ribbon.unsupported);
  out["ribbon_lut_misses"] = int64_t(d.ribbon.lutMisses);
  out["ribbon_pixels"] = static_cast<int64_t>(d.ribbon.pixels);
  {
    Array br;
    for (const auto& [id, n] : d.ribbon.branch) {
      Dictionary e;
      e["branch"] = streamTriBranchName(
          static_cast<mdkbridge::StreamTriBranch>(id));
      e["count"] = int64_t(n);
      br.push_back(e);
    }
    out["ribbon_branch"] = br;
  }
  out["sound_events"] = int64_t(d.soundEvents);
  {
    // 19B.3B1 — the mode-5 audio census: events consumed by the
    // host (== sound_events once the teardown tail drains), the
    // play/stop split by call form, the listener feed count (== the
    // core's listener seam), and the miss/decode/drop counters the
    // closure gate requires at zero.
    const mdkbridge::StreamAudioDiag& ad = streamAudio_.diag();
    out["snd_events"] = int64_t(ad.events);
    out["snd_plays"] = int64_t(ad.plays);
    out["snd_ensure_plays"] = int64_t(ad.ensurePlays);
    out["snd_restart_plays"] = int64_t(ad.restartPlays);
    out["snd_loop_plays"] = int64_t(ad.loopPlays);
    out["snd_positional"] = int64_t(ad.positional);
    out["snd_stops"] = int64_t(ad.stops);
    out["snd_unknown_tags"] = int64_t(ad.unknownTags);
    out["snd_resolve_misses"] = int64_t(ad.resolveMisses);
    out["snd_decode_misses"] = int64_t(streamAudioDecodeMisses_);
    out["snd_listener_updates"] = int64_t(ad.listenerUpdates);
    out["snd_active"] = int64_t(audioMixer_.activeCount());
    out["snd_pool_exhausted"] =
        int64_t(audioMixer_.poolExhaustedCount());
    Array names;
    for (const auto& [nm, n] : ad.playNames) {
      Dictionary e;
      e["name"] = String(nm.c_str());
      e["plays"] = int64_t(n);
      const auto st = ad.stopNames.find(nm);
      e["stops"] = int64_t(st == ad.stopNames.end() ? 0 : st->second);
      names.push_back(e);
    }
    out["snd_names"] = names;
  }
  out["terminal_fills"] = int64_t(d.terminalFills);
  out["terminal_fill"] = int64_t(d.terminalFill);
  out["fb_hash"] = static_cast<int64_t>(d.fbHash);       // bit-cast
  out["palette_hash"] = static_cast<int64_t>(d.paletteHash);
  // 19B.3A — the route/lifetime audit: the last transition edge,
  // the enter/exit counts (repeat-entry proof), the terminal state
  // handed to the tally step, and the mode-5 material-bank bind —
  // bank_b_bound=false means no traversal .MAT survived into the
  // FUN_0042b270 entry (the BONES.WHITE stale-bank oracle).
  out["route_from"] = int64_t(routeFrom_);
  out["route_to"] = int64_t(routeTo_);
  out["stream_teardowns"] = int64_t(streamTeardowns_);
  out["mode5_enters"] = int64_t(mode5Enters_);
  out["mode6_enters"] = int64_t(mode6Enters_);
  out["mode6_exits"] = int64_t(mode6Exits_);
  out["stream_exit_frame"] = streamExitFrame_;
  out["stream_exit_reason"] = int64_t(streamExitReason_);
  out["stream_exit_health"] = int64_t(streamExitHealth_);
  out["bank_a_records"] = int64_t(streamBankARecs_);
  out["bank_b_bound"] = streamBankBBound_;
  out["white_slot"] = int64_t(streamWhiteSlot_);
  out["white_resolved"] = streamWhiteResolved_;
  out["unresolved_materials"] = int64_t(streamUnresolvedMats_);
  out["seq"] = static_cast<int64_t>(streamFrameSeq_);
  return out;
}

void MdkBridge::shutdown() {
  arenaLoaded_ = false;
  arenaIndex_ = -1;
  arenaName_.clear();
  arenaSets_.clear();
  objTexCache_.clear();
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
  // Phase 19B.1 — the StreamScene and its bound buffers die with the
  // session (presenter state is host-side; reset with the mode).
  stream_.reset();
  streamPresenter_.reset();
  // 19B.3B1 — the mode-5 audio registration table, resource cache,
  // and BNI directory die with the session's buffers.
  streamAudio_.reset();
  streamBniDir_ = mdk::BniDirectory{};
  streamSndEntries_.clear();
  streamAudioDecodeMisses_ = 0;
  streamBniBytes_.clear();
  streamFtiBytes_.clear();
  streamHudBytes_.clear();
  streamImageNames_.clear();
  streamAssets_ = mdk::StreamAssets{};
  streamProtoKurt_.reset();
  streamProtoBones_.reset();
  streamProtoProf_.reset();
  streamProtoEsc_.reset();
  streamMtiBytes_.clear();
  streamBankA_.clear();
  streamNowMs_ = 0;
  streamFrameSeq_ = 0;
  // 19B.3A — the route audit counters are session-scoped: they die
  // with the session they measured (the repeat-entry proof reads
  // them between entries of one session).
  routeFrom_ = -1;
  routeTo_ = -1;
  streamTeardowns_ = 0;
  mode5Enters_ = 0;
  mode6Enters_ = 0;
  mode6Exits_ = 0;
  streamExitFrame_ = -1;
  streamExitReason_ = -1;
  streamExitHealth_ = -1;
  streamBankARecs_ = 0;
  streamBankBBound_ = false;
  streamWhiteSlot_ = -1;
  streamWhiteResolved_ = false;
  streamUnresolvedMats_ = 0;
  // Phase 19D — the ending cinematic and its FINISH.BNI cache die
  // with the session (the abort-yes table's FUN_0047b0d8 slot).
  endingTeardown_();
  endingMveBoundary_ = false;
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
  // 19C.4 — the frontend-lifetime audio bank set (MAINSONG +
  // the music-class SNI records) rides the boot: the per-mode SFX
  // banks turn over on boundaries; these stay.
  loadFeAudioBanks_();
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
  // DAT_00541308/0c are process-global — the sound menu mutates
  // them in place, so the mixer/song scalars resync every frame.
  audioSfxPct_ = feShell_->flow().soundFx();
  audioMusicPct_ = feShell_->flow().soundMusic();
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

// 19C.4 — the frontend-lifetime bank set: OPTIONS.BNI (MAINSONG —
// the FUN_0041d720 ambient bed) + MDKSOUND.SNI's music-class
// records (OPTSONG/OPTBUTT). Absent records resolve as misses —
// decode/lookup diagnostics print; the host simply stays silent.
void MdkBridge::loadFeAudioBanks_() {
  feAudioBankStore_.clear();
  feAudioBanks_.clear();
  feAudioEntries_.clear();
  feOptBniBytes_.clear();
  feOptBniDir_ = mdk::BniDirectory{};
  feAudioDecodeMisses_ = 0;
  std::string err;
  if (auto bytes = root_->readFile("MISC/OPTIONS.BNI", 1 << 28, &err)) {
    feOptBniBytes_ = std::move(*bytes);
    feOptBniDir_ = mdk::inspectBniDirectory(
        std::span<const std::byte>(feOptBniBytes_));
  }
  if (auto bytes =
          root_->readFile("MISC/MDKSOUND.SNI", 1 << 28, &err)) {
    feAudioBankStore_.push_back(std::move(*bytes));
    AudioBank_ b;
    b.bytes = std::span<const std::byte>(feAudioBankStore_.back());
    b.dir = mdk::inspectSniDirectory(b.bytes);
    feAudioBanks_.push_back(b);
  }
}

// Resolve + decode a frontend song/button record (memoized).
// BNI first — MAINSONG's payload carries the OBSERVED 4-byte record
// tag (fe ff fe 00) before RIFF; the SNI walk intentionally has NO
// flags&0x2 gate (these ARE the music-class records — OPTSONG's
// field0x0c low word is 0x3: DS-loop + music).
const MdkBridge::AudioEntry_* MdkBridge::feAudioEntry_(
    const std::string& name) {
  if (const auto it = feAudioEntries_.find(name);
      it != feAudioEntries_.end()) {
    return &it->second;
  }
  AudioEntry_ e;
  std::span<const std::byte> payload;
  int flags = 0;
  std::uint32_t volume = 0x7fff;
  if (const mdk::BniRecord* rec =
          mdk::findBniRecord(feOptBniDir_, name)) {
    payload = std::span<const std::byte>(
        feOptBniBytes_.data() + rec->payloadFileOffset,
        static_cast<std::size_t>(rec->payloadEnd -
                                 rec->payloadFileOffset));
    // OBSERVED record-tag belt: payloadFileOffset already skips the
    // fe ff fe 00 tag, but tolerate a tag-inclusive span too.
    auto riffAt = [](std::span<const std::byte> s, std::size_t i) {
      return s.size() >= i + 4 && s[i] == std::byte('R') &&
             s[i + 1] == std::byte('I') && s[i + 2] == std::byte('F') &&
             s[i + 3] == std::byte('F');
    };
    if (!riffAt(payload, 0) && riffAt(payload, 4)) {
      payload = payload.subspan(4);
    }
    e.def.loop = true;   // MAINSONG is the ambient bed — FUN_0041d720
  } else {
    for (const AudioBank_& b : feAudioBanks_) {
      if (b.dir.status != mdk::SniDirectoryStatus::kOk) continue;
      const mdk::SniEntry* rec = nullptr;
      for (const mdk::SniEntry& en : b.dir.entries) {
        if (!en.isSentinel() && en.name() == name) {
          rec = &en;
          break;
        }
      }
      if (!rec) continue;
      const std::uint64_t off = rec->payloadFileOffset();
      const std::uint64_t end = rec->payloadFileEnd();
      if (off >= end || end > b.bytes.size()) break;
      payload = b.bytes.subspan(static_cast<std::size_t>(off),
                                static_cast<std::size_t>(end - off));
      const std::uint32_t fld = rec->fieldAt0x0C;
      flags = static_cast<int>(fld & 0xffffu);
      volume = (fld >> 16) & 0xffffu;
      e.def.loop = (flags & 0x1) != 0;
      break;
    }
  }
  if (!payload.empty()) {
    mdk::SniWave wv;
    std::string derr;
    const mdk::SniWaveStatus st =
        mdk::decodeSniWave(payload, &wv, &derr);
    if (st != mdk::SniWaveStatus::kOk) {
      ++feAudioDecodeMisses_;
      UtilityFunctions::printerr(
          "MdkBridge: frontend wave '", String(name.c_str()),
          "' decode failed: ", mdk::sniWaveStatusName(st).data(),
          " — ", derr.c_str());
    } else {
      e.def.volume = static_cast<int>(volume);
      e.def.rateHz = wv.rateHz;
      e.def.frames = static_cast<std::uint32_t>(wv.frames);
      Ref<AudioStreamWAV> wav;
      wav.instantiate();
      wav->set_format(wv.bitsPerSample == 8
                          ? AudioStreamWAV::FORMAT_8_BITS
                          : AudioStreamWAV::FORMAT_16_BITS);
      wav->set_stereo(false);
      wav->set_mix_rate(wv.rateHz);
      // RIFF PCM8 is unsigned-biased; FORMAT_8_BITS wants
      // signed. One conversion here at the host boundary
      // (pcmForGodotWav) — wv.pcm stays verbatim, PCM16
      // passes through untouched.
      const std::vector<std::uint8_t> pcm =
          mdkbridge::pcmForGodotWav(wv);
      PackedByteArray data;
      data.resize(static_cast<int64_t>(pcm.size()));
      std::memcpy(data.ptrw(), pcm.data(), pcm.size());
      wav->set_data(data);
      if (e.def.loop) {
        wav->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
        wav->set_loop_begin(0);
        wav->set_loop_end(static_cast<int64_t>(wv.frames));
      }
      e.stream = wav;
      e.resolved = true;
    }
  }
  const auto [it, inserted] =
      feAudioEntries_.emplace(name, std::move(e));
  return &it->second;
}

Dictionary MdkBridge::frontend_song_stream(const String& name) {
  Dictionary out;
  const AudioEntry_* e =
      feAudioEntry_(std::string(name.utf8().get_data()));
  if (e && e->resolved) {
    out["stream"] = e->stream;
    out["vol"] = int64_t(e->def.volume);
    out["loop"] = e->def.loop;
  }
  return out;
}

Array MdkBridge::frontend_drain_audio_events() {
  Array out;
  if (!feShell_) return out;
  for (const mdk::SoundAudioEvent e :
       feShell_->flow().drainAudioEvents()) {
    out.push_back(int64_t(e));
  }
  return out;
}

Dictionary MdkBridge::frontend_volumes() const {
  Dictionary out;
  out["sound_fx"] = audioSfxPct_;
  out["sound_music"] = audioMusicPct_;
  return out;
}

double MdkBridge::audio_vol_db(int64_t vol, int64_t pct) const {
  return double(mdk::traversalAudioVolDb(
      mdk::traversalAudioScaledVol(static_cast<int>(vol),
                                   static_cast<int>(pct))));
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
      objTexCache_.clear();
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
  objTexCache_.clear();
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
      // 0x401174: a clean exit deletes SAVES\LASTGAME.SAV — the
      // checkpoint's lifetime ends here, not at teardown.
      if (feHost_) (void)feHost_->saves().deleteLastgame();
      mdk::progressionDeleteCheckpoint(sess_);
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
  ffVeilMask_.clear();   // sized before any ff_veil_mask() call
  timing_ = mdk::FrontendTimingState{};
  prevKeyLevel_ = {};
  ffHandoffDone_ = false;
  ffHandoffRoute_ = -1;
  ffHandoffDetail_.clear();
  ffTex_.clear();
  mode_ = 2;
  hasFrame_ = false;
  // 19C.3 — same bank set as load_freefall (the campaign entry is
  // the mode-6 exit's FALL3D_<levelId> arm).
  loadFreefallSoundBanks_();
  {
    std::string detail2;
    if (!ffTtEnter_(detail2)) {
      detail = "freefall teletype: " + detail2;
      return false;
    }
  }
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
  objTexCache_.clear();
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
    if (stream_) {
      // A live StreamScene owns mode 5 — its own kExitMode drives
      // the transition (stepCore_ -> stepStream_ -> streamHandoff_).
      // The pump is a no-op while it runs.
      return out;
    }
    // First pump after the transition — install the scene (the
    // FUN_0042b270 bind + init carrying the campaign globals).
    {
      std::string detail;
      if (!campaignStreamEnter_(detail)) {
        setError_("campaign stream: " + detail);
        out["ok"] = false;
        out["detail"] = String(detail.c_str());
        return out;
      }
    }
    out["pumped"] = true;
    out["error"] = String(mdk::progressionErrorName(
        mdk::ProgressionError::kStageRunning));
    out["mode"] = mode_;
    out["sess_mode"] = sess_.mode;
    return out;
  case 6:
    if (sess_.loaderSub == 3) {
      // Phase 19E — FUN_00429cb4 owns the stage edge while sub-state
      // 3 runs: the briefing machine's exit (FUN_00429600's arm) is
      // the only route out; the placeholder hold does not apply.
      if (!briefing_) {
        std::string detail;
        if (!briefingEnter_(detail)) {
          setError_("mode-6 briefing: " + detail);
          out["ok"] = false;
          out["detail"] = String(detail.c_str());
          return out;
        }
      }
      // The pump's timing source — the host passes the real frame
      // delta; frontendTimingUpdate keeps 0x49b6f4/0x49b6e8 semantics
      // (the same feed the runtime modes use).
      mdk::frontendTimingUpdate(
          timing_, double(input.get("dt_ms", 1000.0 / 30.0)));
      mdk::Mode6BriefingInput bi;
      bi.dtSec = timing_.deltaSec;
      bi.frameStep = timing_.frameStep;
      // The host's "stage complete" edge maps onto the briefing's
      // own skip arm: esc latches the type-skip (instant page fill)
      // and also counts as the exit key at the hold gate — the
      // briefing's real proceed-fast path. `confirm` is a genuine
      // key press (anyKey); neither forces the machine's state.
      bi.esc = bool(input.get("esc", false)) || stageDone;
      bi.hurryA = bool(input.get("hurry", false));
      bi.hurryB = bool(input.get("hurry", false));
      bi.anyKey = bool(input.get("any_key", false)) ||
                  bool(input.get("confirm", false));
      const bool done6 = briefing_->step(bi, briefingFb_, briefingPal_);
      ++briefingFrameSeq_;
      e = mdk::progressionStepLoader(sess_, done6);
      if (sess_.mode != 6 || sess_.loaderSub != 3) briefing_.reset();
    } else {
      e = mdk::progressionStepLoader(sess_, stageDone);
    }
    break;
  case 7:
    e = mdk::progressionStepMode7(sess_);
    break;
  case 8:
    if (ending_) {
      // A live EndingCinematic owns mode 8 — its kMveBoundary edge
      // drives the transition (stepCore_ -> stepEnding_).
      return out;
    }
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
  // 19B.3A route audit — a completed pump edge (mode-6 loader exit,
  // mode-7/8 tail). The runtime-presented modes' edges are counted
  // in their own handoffs.
  routeFrom_ = prevMode;
  routeTo_ = mode_;
  if (prevMode == 6 && mode_ != 6) ++mode6Exits_;
  out["mode"] = mode_;
  out["sess_mode"] = sess_.mode;
  out["level_id"] = sess_.levelId;
  return out;
}
