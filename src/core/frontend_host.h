// Phase 18B.1 — frontend HOST services: the filesystem/environment
// side of the Phase-18A shell seams (FrontendShellSeams). The shell
// owns the menu state machine; this host owns everything it was
// deliberately kept blind to: the SAVES directory, the LASTGAME
// probe, slot inspection, save writes, and the MISC\MDKS_* slide
// probe.
//
// EVIDENCE (instruction-level, BUILD_A — see frontend_shell.h and
// analysis-private/logs/decomp_28290.txt, p18_dec2.txt,
// decomp_1d85c.txt):
//
//   FUN_00428290 (Continue gate): FUN_0041d85c builds
//   "SAVES\LASTGAME.SAV" (FUN_0041ab50 + "LASTGAME"+".SAV") and calls
//   FUN_00428290 = FUN_00426618 = open + FUN_004264f0 envelope check
//   (fileSize field == length, checksum == bytes[8..) sum). No packet
//   walk — an envelope-valid file passes the gate.
//
//   FUN_004202cc (save-list arm): a "*.SAV" wildcard scan of the
//   SAVES directory via the _findfirst/_findnext wrappers
//   FUN_0047e5fa/FUN_0047e66e/FUN_0047e6b8. OBSERVED details: every
//   match is counted (unbounded), the 9-byte stem record is the
//   filename truncated at the first ' '/'.' capped at 8 (host returns
//   raw names — the shell applies saveListStem), and an EMPTY stem
//   breaks the fill loop. LASTGAME.SAV matches "*.SAV" and is
//   enumerated like any other save — the original does not filter it.
//   ORDER: the original reports raw filesystem enumeration order
//   (unsorted). NATIVE PORT DECISION: byte-sorted for determinism —
//   the same normalization mdk-inspect's Phase-18A diagnostic used.
//
//   FUN_00428144 (slot inspect): envelope + expect-reads SAVE, THMB,
//   GAME in order; the THMB payload is captured into the preview
//   buffer (0x49f010) as part of the inspect. Implemented in
//   save_game.cpp as saveGameInspectHead — the same packet/cipher
//   machinery, the inspector's shorter read.
//
//   FUN_00422d84 -> FUN_00427ed4 (name-entry write): the host
//   performs the filesystem write only; the serialized bytes come
//   from the existing writers (saveGameWriteHeaderOnly for the
//   autosave/briefing form, saveGameWriteFull for the manual F2
//   form). The write seam never receives data it can't source: the
//   embedder binds a FrontendSaveSource that snapshots the live
//   session fields at commit time.
//
//   FUN_0041ef74 (attract advance): loads "MISC\MDKS_%03d.GIF" gated
//   on a 600x360 decode. The core does not decode GIF (an externally
//   documented format — file_family.h kStandardExternalFormat); the
//   probe reads the GIF logical-screen descriptor for the 600x360
//   gate and slideData() hands the raw bytes to whatever decoder the
//   presentation host owns (the GDExtension's gif_decode.cpp covers
//   the corpus-proven GIF87a/89a single-image shape).
//
//   Transition seam: FUN_0041d85c(arg!=0) sets DAT_0049aa7c=1
//   (transition armed — emitted as FrontendFx::TransitionArmed) and
//   DAT_0054152c=1 (Esc-abort suppression); FUN_0041ebf4 clears
//   0x54152c when the entry transition finishes. The original also
//   holds the idle/attract timer while the blend buffer DAT_0049aa8c
//   is active. Presentation plays the transition; the host calls
//   frontendHostTransitionComplete() to clear both.
//
// THMB CAPTURE (thmb_capture.h, OBSERVED FUN_00427e8c/FUN_0046d614):
// the save-name dialog grabs a 64x45 nearest-sample of the live
// 600x360 indexed framebuffer (x offset 44, pitch 8) plus the staged
// 768-byte palette at arm time, staged into the record the writer
// embeds on confirm. Writers emit a zeroed THMB when the source
// thumbnail is empty (the load path only previews it). A
// presentation that can produce the exact 3648-byte record supplies
// it through FrontendSaveData::headerOnly.thumbnail / the
// full-write input.

#ifndef MDK_CORE_FRONTEND_HOST_H
#define MDK_CORE_FRONTEND_HOST_H

#include "core/data_root.h"
#include "core/frontend_shell.h"
#include "core/indexed_image.h"
#include "core/save_game.h"
#include "core/save_slot_list.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mdk {

// Copy-safe slide probe result for MISC\MDKS_<index:03d>.GIF. The
// dims come from the GIF logical-screen descriptor (header fields) —
// enough for the OBSERVED 600x360 gate without decoding.
struct FrontendSlideInfo {
  int index = 0;                 // slide number requested
  std::string relPath;           // "MISC/MDKS_001.GIF" (port-normalized)
  bool exists = false;           // resolved + readable under the root
  int width = 0;                 // GIF logical-screen width
  int height = 0;                // GIF logical-screen height
  std::uint64_t bytes = 0;       // physical file size
};

// The inspectSlot result plus the host-only extras (the THMB payload
// the original captures into the preview buffer, and the probe's
// error for diagnostics). Presentation consumes this — the shell's
// seam contract stays SaveSlotSummary.
struct FrontendSlotInspection {
  SaveSlotSummary summary;
  SaveError error = SaveError::kReadFail;
  std::vector<std::byte> thumbnail;   // the 3648-byte THMB payload
};

// What the write seam needs that only the live session knows
// (FUN_00427ed4's callers read these globals at write time). The
// embedder snapshots them into this record inside the callback.
struct FrontendSaveData {
  // Header-only form inputs — GAME fields are the verbatim globals
  // (0x541492/0x541498/0x541554/0x541637/0x54163b); `thumbnail` is
  // the arm-time THMB grab (empty = the writer's zeroed record).
  SaveWriteInput headerOnly;
  // Full-save bytes — saveGameWriteFull output for the manual F2
  // path. Empty when no live traversal session can serialize (the
  // write then fails, keeping the dialog open — the OBSERVED
  // FUN_00422dec failure behavior).
  std::vector<std::byte> full;
};
// Returns nullopt when no save content can be produced at all.
using FrontendSaveSource =
    std::function<std::optional<FrontendSaveData>()>;

// The host save-root environment: one explicit writable directory
// standing in for the original's "<base>\SAVES" (FUN_0041ab50), plus
// an optional read-only DataRoot for the MISC\MDKS_* slide probe.
// Production binds the native runtime's SAVES dir; tests bind temp
// dirs. This class never writes outside saveRoot().
class FrontendHostServices {
public:
  explicit FrontendHostServices(std::filesystem::path saveRoot)
      : store_(std::move(saveRoot)) {}
  // DataRoot used for the attract slide probe (optional — without it
  // every slide probe reports absent, the FUN_0041b004 open-failure
  // path).
  void setDataRoot(const DataRoot* root) { data_ = root; }
  const DataRoot* dataRoot() const { return data_; }

  const std::filesystem::path& saveRoot() const { return store_.dir(); }
  const SaveStore& saves() const { return store_; }

  // --- the Phase-18A seam implementations -------------------------
  // FUN_00428290 — SAVES\LASTGAME.SAV exists AND is envelope-valid.
  bool lastGameExists() const;
  // FUN_004202cc's "*.SAV" scan — raw file names, byte-sorted (the
  // shell applies saveListStem itself). Includes LASTGAME.SAV.
  std::vector<std::string> enumerateSaves() const;
  // FUN_00428144 — the header/thumb probe for one stem. nullopt when
  // the file cannot be opened (the inspect-failure path); a returned
  // summary carries valid=false for a readable but malformed file.
  std::optional<SaveSlotSummary> inspectSlot(
      std::string_view stem) const;
  // The same probe for the host/presentation: adds the THMB payload
  // and the probe's SaveError.
  std::optional<FrontendSlotInspection> inspectSlotDetail(
      std::string_view stem) const;
  // The filesystem half of FUN_00422d84: write `<stem>.SAV` under
  // saveRoot() (create_directories first, like the original's
  // implicit dir). The serialized bytes come from the embedder's
  // writers — this never re-serializes game state.
  bool writeSaveFile(std::string_view stem,
                     std::span<const std::byte> bytes) const;
  // The shell seam (FUN_0041b004 + FUN_00416e98 gate): the slide
  // resolves, reads, and its GIF header reports 600x360.
  bool slideExists(int slide) const;
  // Copy-safe detail for presentation — identity + header dims.
  std::optional<FrontendSlideInfo> slideInfo(int slide) const;
  // Raw GIF bytes for the presentation host's own decoder (the core
  // deliberately has no GIF decoder — kStandardExternalFormat).
  std::optional<std::vector<std::byte>> slideData(int slide) const;

  // Phase 18B.2B — the save-list detail pane's level fallback
  // image: FUN_004202cc preloads LOAD_<id>.LBB for the six level
  // ids {7,6,3,4,8,5} (rodata 0x4999e8), pixel slots indexed by the
  // save's level field. Returns the decoded image (palette
  // embedded), lazily cached — bounded at six entries. Null when
  // levelId is out of range or the file is absent/malformed.
  const IndexedImage* saveListLbbImage(int levelId) const;

  // Bind all five shell seams to this host. `saveSource` supplies the
  // serialized content at write-commit time (the SaveNameWriter seam
  // only carries name + headerOnly — the original pulls the rest
  // from live globals). With no source the write seam fails every
  // request — the OBSERVED error-dialog/stay-open path.
  //
  // The seams capture `this` — the services object must outlive any
  // shell they are handed to (the embedder owns both).
  FrontendShellSeams makeSeams(FrontendSaveSource saveSource) const;

private:
  SaveStore store_;          // saveRoot() — the <name>.SAV path model
  const DataRoot* data_ = nullptr;   // MISC\MDKS_* read-only access
  // Bounded lazy cache for saveListLbbImage (six level slots).
  mutable std::array<std::optional<IndexedImage>, 6> lbbCache_{};
};

// --- transition acknowledgement (the FUN_0041ebf4 clear point) ----
// The host mirrors DAT_0049aa8c while the entry transition plays —
// set it when FrontendFx::TransitionArmed is drained.
void frontendHostTransitionArmed(FrontendShell& sh);
// The host's transition-complete acknowledgement: clears
// suppressEscAbort (DAT_0054152c — the FUN_0041ebf4 clear point) and
// the blend gate so the idle/attract timer runs again.
void frontendHostTransitionComplete(FrontendShell& sh);

}  // namespace mdk

#endif  // MDK_CORE_FRONTEND_HOST_H
