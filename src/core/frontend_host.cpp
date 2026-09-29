// Phase 18B.1 — frontend host services. See frontend_host.h for the
// evidence notes; instruction addresses cited inline are BUILD_A.

#include "core/frontend_host.h"

#include "core/lbb_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace mdk {

namespace {

// "*.SAV" — case-folded extension match (the original ran on
// case-insensitive filesystems; FindFirstFile's pattern match folds).
bool isSavName(const std::string& name) {
  if (name.size() < 4) return false;
  const std::string ext = name.substr(name.size() - 4);
  return ext.size() == 4 && ext[0] == '.' &&
         std::tolower(static_cast<unsigned char>(ext[1])) == 's' &&
         std::tolower(static_cast<unsigned char>(ext[2])) == 'a' &&
         std::tolower(static_cast<unsigned char>(ext[3])) == 'v';
}

// Reject anything that could escape the save root — the UI charset
// (alnum + '_' + '$') can never produce these, so a hit means a
// non-frontend caller; refuse rather than normalize (no invented
// modern filename rules — the host contract is verbatim stems).
bool stemSafe(std::string_view stem) {
  if (stem.empty() || stem.size() > 64) return false;
  for (const char c : stem) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
  }
  return stem != "." && stem != "..";
}

}  // namespace

bool FrontendHostServices::lastGameExists() const {
  // FUN_00428290 -> FUN_00426618 -> fopen + FUN_004264f0: openable
  // and envelope-consistent — no packet walk.
  return saveGameEnvelopeValidFile(store_.lastgamePath());
}

std::vector<std::string> FrontendHostServices::enumerateSaves() const {
  // FUN_004202cc's "*.SAV" scan over the SAVES directory. Every
  // regular-file match is returned (unbounded — the original counts
  // before allocating). Order is byte-sorted for determinism; the
  // original reported raw _findnext order (FS-defined).
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e :
       std::filesystem::directory_iterator(store_.dir(), ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string name = e.path().filename().string();
    if (isSavName(name)) out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::optional<SaveSlotSummary> FrontendHostServices::inspectSlot(
    std::string_view stem) const {
  // FUN_00428144 — the head inspect. Unopenable -> nullopt (the
  // invalid-entry path); a readable but malformed file still returns
  // a summary so the row can carry the stem with valid=false.
  if (!stemSafe(stem)) return std::nullopt;
  SaveGamePacket game;
  const SaveError e = saveGameInspectHeadFile(
      store_.pathFor(std::string(stem)), &game, nullptr);
  if (e == SaveError::kReadFail) return std::nullopt;
  SaveSlotSummary s;
  s.name = std::string(stem);
  s.valid = (e == SaveError::kOk);
  if (s.valid) {
    s.fullSave = game.full();
    s.levelId = game.levelId;
    s.modeField = game.modeField;
    s.health = game.health;
    s.deathCount = game.deathCount;
  }
  return s;
}

std::optional<FrontendSlotInspection>
FrontendHostServices::inspectSlotDetail(std::string_view stem) const {
  if (!stemSafe(stem)) return std::nullopt;
  FrontendSlotInspection d;
  d.summary.name = std::string(stem);
  // The head probe decodes GAME + captures THMB in one pass.
  SaveGamePacket game;
  std::vector<std::byte> thmb;
  d.error = saveGameInspectHeadFile(store_.pathFor(std::string(stem)),
                                    &game, &thmb);
  if (d.error == SaveError::kReadFail) return std::nullopt;
  d.thumbnail = std::move(thmb);
  d.summary.valid = (d.error == SaveError::kOk);
  if (d.summary.valid) {
    d.summary.fullSave = game.full();
    d.summary.levelId = game.levelId;
    d.summary.modeField = game.modeField;
    d.summary.health = game.health;
    d.summary.deathCount = game.deathCount;
  }
  return d;
}

bool FrontendHostServices::writeSaveFile(
    std::string_view stem, std::span<const std::byte> bytes) const {
  if (!stemSafe(stem) || bytes.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(store_.dir(), ec);
  if (ec) return false;
  std::ofstream f(store_.pathFor(std::string(stem)),
                  std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(bytes.data()),
          std::streamsize(bytes.size()));
  return bool(f);
}

std::optional<FrontendSlideInfo> FrontendHostServices::slideInfo(
    int slide) const {
  if (data_ == nullptr || slide < 0 || slide > 999) return std::nullopt;
  // FUN_0041ef74: sprintf "MISC\MDKS_%3.3d.GIF" then FUN_0041b004.
  char rel[24];
  std::snprintf(rel, sizeof(rel), "MISC/MDKS_%03d.GIF", slide);
  FrontendSlideInfo info;
  info.index = slide;
  info.relPath = rel;
  std::string err;
  const auto size = data_->fileSize(info.relPath, &err);
  if (!size) return info;
  // GIF header probe — signature + logical-screen dims. The observed
  // 600x360 gate runs against these fields (a real decode is the
  // presentation host's job; GIF is kStandardExternalFormat).
  const auto head = data_->readPrefix(info.relPath, 10, &err);
  if (!head || head->size() < 10) return info;
  const auto* b = head->data();
  if (!(b[0] == std::byte('G') && b[1] == std::byte('I') &&
        b[2] == std::byte('F') && b[3] == std::byte('8')))
    return info;   // not a GIF stream — the decode gate would fail
  info.exists = true;
  info.bytes = *size;
  info.width = int(std::uint8_t(b[6])) |
               (int(std::uint8_t(b[7])) << 8);
  info.height = int(std::uint8_t(b[8])) |
                (int(std::uint8_t(b[9])) << 8);
  return info;
}

bool FrontendHostServices::slideExists(int slide) const {
  const auto info = slideInfo(slide);
  // FUN_00416e98's decode gate is 600x360.
  return info && info->exists && info->width == 600 &&
         info->height == 360;
}

std::optional<std::vector<std::byte>> FrontendHostServices::slideData(
    int slide) const {
  if (data_ == nullptr || slide < 0 || slide > 999) return std::nullopt;
  char rel[24];
  std::snprintf(rel, sizeof(rel), "MISC/MDKS_%03d.GIF", slide);
  return data_->readFile(rel, 8 << 20, nullptr);
}

const IndexedImage* FrontendHostServices::saveListLbbImage(
    int levelId) const {
  if (data_ == nullptr || levelId < 0 ||
      levelId >= kSaveListLbbCount) {
    return nullptr;
  }
  auto& slot = lbbCache_[static_cast<std::size_t>(levelId)];
  if (!slot.has_value()) {
    // Engaged-but-empty sentinel caches a miss — the file set is
    // static for the session, so we do not re-probe every frame.
    slot = loadSaveListLbb(*data_, levelId).value_or(IndexedImage{});
  }
  return slot->width > 0 ? &*slot : nullptr;
}

FrontendShellSeams FrontendHostServices::makeSeams(
    FrontendSaveSource saveSource) const {
  FrontendShellSeams s;
  s.lastGameExists = [this] { return lastGameExists(); };
  s.enumerateSaves = [this] { return enumerateSaves(); };
  s.inspectSlot = [this](std::string_view stem) {
    return inspectSlot(stem);
  };
  s.slideProbe = [this](int n) { return slideExists(n); };
  // FUN_00422d84 -> FUN_00427ed4: serialize through the existing
  // writers (the embedder's live session provides the content), then
  // perform the filesystem write. Empty bytes = the write cannot be
  // produced -> the OBSERVED failure path (dialog stays open).
  s.writeSave = [this, source = std::move(saveSource)](
                    std::string_view name, bool headerOnly) {
    const auto data = source ? source() : std::nullopt;
    if (!data) return false;
    const std::vector<std::byte> bytes =
        headerOnly ? saveGameWriteHeaderOnly(data->headerOnly)
                   : data->full;
    return !bytes.empty() && writeSaveFile(name, bytes);
  };
  return s;
}

void frontendHostTransitionArmed(FrontendShell& sh) {
  // DAT_0049aa8c — the blend gate holds the attract idle timer while
  // the entry transition resource is active.
  sh.flow().root().setAttractBlendActive(true);
}

void frontendHostTransitionComplete(FrontendShell& sh) {
  // FUN_0041ebf4's clear point: DAT_0054152c (the Esc-abort
  // suppression armed by the FUN_0041d85c arg!=0 entry) clears when
  // the entry transition finishes; the blend gate releases too.
  sh.setSuppressEscAbort(false);
  sh.flow().root().setAttractBlendActive(false);
}

}  // namespace mdk
