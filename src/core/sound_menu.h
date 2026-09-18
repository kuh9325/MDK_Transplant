// Phase 4I — Sound options child screen (FUN_004233d8, front-end
// mode 0x02) — the second real child screen of the options sub-menu.
//
// EVIDENCE (instruction-level, original binary — see
// docs/ENGINE_RECONSTRUCTION.md Phase 4I and
// analysis-private/logs/show_4233d8.txt, show_sndhelpers.txt,
// show_sndhelpers2.txt, dump_sndstrings.txt, dump_set4i.txt):
//
//   Entry (FUN_0042322c, OBSERVED — reached from Options row 1 via
//   0x420fdd under the LEFT, RIGHT, or activate query; all three
//   dispatch tables bind row 1 to this entry):
//     DAT_00541493 = 0x02       sound mode
//     FUN_0041d774              stop+release the ambient menu song
//                               (MAINSONG handle DAT_0049aa94)
//     FUN_00428828("MISC\MDKSOUND.SNI")  load the sound module blob
//     OPTSONG -> DAT_0054bdc0   resolve via FUN_00402fe8
//     OPTBUTT -> DAT_0054bdc4   resolve via FUN_00402fe8
//     FUN_00402388(OPTSONG, 0)  start the screen's song
//                               (play-if-not-playing — EDX=0)
//     DAT_0054bdb8 = 3          item count (2 volume rows + Done)
//     DAT_0054bdbc UNTOUCHED    — the selection is a process global
//                               never reset at entry (BSS 0 on the
//                               first entry; later entries resume
//                               wherever it was left)
//     NO mouse/tick/ramp/timing reset — the shared globals carry over
//
//   Frame (FUN_004233d8, OBSERVED): the same shared query helpers as
//   the options screen, in the same order, with ONE asymmetry —
//   every FIRED query plays OPTBUTT first
//   (FUN_00402388(DAT_0054bdc4, 1) — restart), except Esc:
//     DAT_00541538 delegation flag — `cmp !=0 -> write 0` at frame
//       top; OBSERVED dead in BUILD_A (no nonzero writer), not modeled
//     prev  -> OPTBUTT, sel-1, wraps <0 -> DAT_0054bdb8-1 (=2)
//     next  -> OPTBUTT, sel+1, wraps >=3 -> 0
//     mouse gate (same three-global check, clamp (590,350) inside):
//              band = trunc((mouseY - 61) / 46); valid bands 0..2
//              assign unconditionally
//     Esc (DAT_0054b570 raw level) -> FUN_00423280 + RET — NO
//              OPTBUTT on this path (the only silent exit)
//     LEFT  -> OPTBUTT; row 0: SoundFX   -10 (clamp <0 -> 0);
//              row 1: SoundMusic -10 (clamp <0 -> 0); row 2: falls
//              through. Mutations latch DAT_00541486 then call
//              FUN_004024c4 (push scaled volumes to live voices)
//     RIGHT -> OPTBUTT; same rows +10 (clamp >100 -> 100)
//     activate -> OPTBUTT; rows 0/1 fall through to the draw
//              (no mutation — the original checks sel==0 -> draw,
//              sel!=1 -> exit); row 2 -> FUN_00423280 + RET
//     draw block (below) -> FUN_0046c86c present -> FUN_0042fe78
//              timing update.
//   OBSERVED quirk: the <0 clamp writes ESI — the Esc key state,
//   provably 0 at that point (nonzero would have exited above).
//   The <0 clamp also still latches dirty and calls FUN_004024c4 —
//   a mutation at the boundary is not suppressed.
//
//   Draw (OBSERVED): FUN_00415658 clear(0);
//   FUN_00414d2c(SND_TITL, y=31) and (SND_INFO, y=350) — centered,
//   FONTBIG when the measure < 600 else the FONTSML centered path
//   (FUN_00414f1c); FUN_004232b0 per volume row i at
//   rowY = 87 + 46*i:
//     label via FUN_00423b10 — ramp key (4, rowY), FONTBIG scaled
//       (FUN_00414f64) left-aligned at x=4 (the key x IS the pen x)
//     bar via FUN_00416aa8 inclusive rectfill —
//       x = 210 .. 210+trunc(vol*280/100), y = rowY-12 .. rowY-1,
//       color index 4 (at vol=0 a single 1px column draws)
//     "100%" (SND_100) FONTSML at x=498, "0%" (SND_0) at x=175,
//       both at y=rowY (FUN_00414dd4, missing advance 4)
//   FUN_00423384 row 2 — SND_DONE centered at y=179 via
//   FUN_00423b88 (ramp key (-1,179) — the options-row helper).
//   ARROW at the raw logical mouse (FUN_004236c0).
//
//   Exit (FUN_00423280, OBSERVED): DAT_00541493 = 0x0b (options
//   mode); FUN_0040210c(OPTSONG) stops the screen's song;
//   FUN_00428b34 releases the SNI module's records; when
//   DAT_00541492 (game-in-progress) == 0 — always true in the
//   front-end — FUN_0041d720 restarts the ambient MAINSONG. The
//   options selection _DAT_0054bd34 is untouched -> resumes 1.
//   The shared dirty flag carries back; persistence waits for the
//   eventual options exit (FUN_00420d68), never happens here.
//
//   Volume globals (OBSERVED): DAT_00541308 = SoundFX,
//   DAT_0054130c = SoundMusic — settings-table entries 8 and 9
//   ({name,type u8,value*} 9-byte records at 0x49acf0/0x49acf9 —
//   both type 0 int), factory mirrors 70 (@0x49b0fc) and 100
//   (@0x49b100). Domain [0,100], step 10, clamp (never wrap).
//   FUN_0040202c routes the scale by voice flag bit 0x2 (music
//   channel) — OPTSONG is the only music-flagged record in
//   MDKSOUND.SNI (flags 0x0003; OPTBUTT is 0x0000).
//
//   Phase 4I scope: the screen's UI, mutations, persistence, and
//   the proven audio-trigger SEMANTICS are reconstructed; actual
//   DirectSound playback is deferred — the events below are the
//   port's narrow semantic seam.
//
#ifndef MDK_CORE_SOUND_MENU_H
#define MDK_CORE_SOUND_MENU_H

#include "core/frontend_machines.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mdk {

class IndexedFramebuffer;
class Palette;
struct FtiFont;
struct FtiSpriteFrame;
struct FrontendMenuInput;

// OBSERVED sound item table (DAT_0054bdb8 = 3; row i draws at
// y = 87 + 46*i).
inline constexpr int kSoundItemCount = 3;   // rows 0..2
inline constexpr int kSoundRowY0 = 87;      // 0x57
inline constexpr int kSoundRowStep = 46;    // 0x2e
inline constexpr int kSoundDoneRow = 2;
// OBSERVED hit-test constants: band = trunc((mouseY - 61) / 46).
inline constexpr int kSoundHitBandBase = 61;  // 0x3d
inline constexpr int kSoundHitBandSize = 46;  // 0x2e
// OBSERVED volume domain: ±10 per fired query, clamps [0,100].
inline constexpr int kSoundVolumeMin = 0;
inline constexpr int kSoundVolumeMax = 100;   // 0x64
inline constexpr int kSoundVolumeStep = 10;   // 0xa
inline constexpr int kSoundFxFactory = 70;    // mirror @0x49b0fc
inline constexpr int kSoundMusicFactory = 100;// mirror @0x49b100
// OBSERVED volume-row geometry (FUN_004232b0): label pen x = 4 —
// the same value the original passes as the ramp key x; the bar is
// the FUN_00416aa8 inclusive rectfill x = 210..210+w,
// y = rowY-12..rowY-1, color index 4; w = trunc(vol*280/100).
inline constexpr int kSoundLabelX = 4;
inline constexpr int kSoundVolRowKeyX = 4;    // FUN_00423b10 EDX
inline constexpr int kSoundDoneKeyX = -1;     // FUN_00423b88 EDX=-1
inline constexpr int kSoundBarX0 = 210;       // 0xd2
inline constexpr int kSoundBarWidth = 280;    // 0x118
inline constexpr int kSoundBarTop = 12;       // rowY - 12
inline constexpr int kSoundBarBottom = 1;     // rowY - 1 (inclusive)
inline constexpr int kSoundBarColor = 4;
// OBSERVED endpoint labels (FUN_00414dd4 — FONTSML, advance 4):
// "100%" at x=498, "0%" at x=175, drawn per volume row at y=rowY.
inline constexpr int kSoundEnd100X = 498;     // 0x1f2
inline constexpr int kSoundEnd0X = 175;       // 0xaf
// OBSERVED title/info rows (FUN_00414d2c — centered on 600).
inline constexpr int kSoundCenterWidth = 600;
inline constexpr int kSoundTitleY = 31;       // 0x1f
inline constexpr int kSoundInfoY = 350;       // 0x15e

// Resource names (OBSERVED at 0x495f98..0x495ff8 in MDK95.EXE).
// The SNI path is the FUN_00428828 blob load; OPTSONG/OPTBUTT are
// resolved inside it via FUN_00402fe8. The SND_* names resolve
// through FUN_00414890 against the resident MDKFONT.FTI blob —
// SND_SET ("Setup Device") also exists in the FTI but is never
// resolved by the proven frame (vestigial — not loaded).
inline constexpr const char* kSoundSniFile = "MISC/MDKSOUND.SNI";
inline constexpr const char* kSoundSongRecord = "OPTSONG";
inline constexpr const char* kSoundButtonRecord = "OPTBUTT";
inline constexpr const char* kSoundTitleRecord = "SND_TITL";
inline constexpr const char* kSoundInfoRecord = "SND_INFO";
inline constexpr const char* kSoundFxRecord = "SND_FX";
inline constexpr const char* kSoundMusicRecord = "SND_MUSI";
inline constexpr const char* kSoundDoneRecord = "SND_DONE";
inline constexpr const char* kSoundEnd100Record = "SND_100";
inline constexpr const char* kSoundEnd0Record = "SND_0";

// Resolved label strings for one frame (OBSERVED payload text in
// BUILD_A: "Sound Settings", "Left/Right to Change Volumes",
// "Effects", "Music", "Done", "100%", "0%").
struct SoundMenuLabels {
  std::string_view title;    // SND_TITL
  std::string_view info;     // SND_INFO
  std::string_view effects;  // SND_FX
  std::string_view music;    // SND_MUSI
  std::string_view end100;   // SND_100
  std::string_view end0;     // SND_0
  std::string_view done;     // SND_DONE
};

// The frozen frame state for the static preview (OBSERVED entry
// register values): the first-entry selection is 0 (DAT_0054bdbc
// is BSS-zeroed and never written at entry), factory volumes
// 70/100, ARROW at the (unchanged) logical mouse position.
struct SoundMenuSpec {
  int selection = 0;          // DAT_0054bdbc first entry (BSS 0)
  int soundFx = kSoundFxFactory;
  int soundMusic = kSoundMusicFactory;
  int brightness = 0;         // DAT_0054147e — the upload lift
  int arrowX = 300;           // logical mouse — NOT reset on entry
  int arrowY = 180;
};

// Semantic audio-trigger events — the narrow seam Phase 4I emits
// instead of driving DirectSound. Order within a frame mirrors the
// original's call order exactly.
enum class SoundAudioEvent {
  AmbientSongStop,   // FUN_0041d774 at entry — stop+release MAINSONG
  SongStart,         // FUN_00402388(OPTSONG, 0) at entry —
                     // play-if-not-playing
  Button,            // FUN_00402388(OPTBUTT, 1) — restart, emitted by
                     // EVERY fired repeat/activate query (not Esc)
  VolumesApplied,    // FUN_004024c4 — volume pushed to live voices,
                     // after every LEFT/RIGHT mutation (even a
                     // boundary-clamped one)
  SongStop,          // FUN_0040210c(OPTSONG) at exit
  AmbientSongStart,  // FUN_0041d720 at exit — MAINSONG restart
                     // (DAT_00541492 == 0 in the front-end)
};

// Semantic outputs of FUN_004233d8 — the only terminal action is
// the exit (FUN_00423280 -> mode 0x0b), reached by Esc or by
// activating row 2.
enum class SoundAction {
  None = 0,
  Back,  // -> FUN_00423280: stop song, release SNI, restart ambient
};

// The reconstructed controller — FUN_004233d8 input/selection
// block plus the entry/exit audio semantics of FUN_0042322c/
// FUN_00423280. Same frame protocol as the options controller:
//     update(input)   — FUN_004187e0 accumulate + tick advance +
//                       FUN_004233d8 queries/mutations
//     itemScale(...)  — FUN_00423a24, called by the renderer once
//                       per drawn row in draw order with the row's
//                       ramp key (4,y) or (-1,y)
//     endFrame(dtMs)  — FUN_0042fe78/FUN_0042fcd0 timing update
class SoundMenuController {
public:
  // Mirrors FUN_0042322c: `selection` is DAT_0054bdbc — the process
  // global that entry does NOT reset (first entry 0, later entries
  // resume it); `soundFx`/`soundMusic` are the process globals
  // DAT_00541308/0c; `settingsDirty` is DAT_00541486 carried from
  // the parent options screen. Everything else carries over from
  // the shared machine state `s` untouched.
  SoundMenuController(const FrontendMachineState& s, int selection,
                      int soundFx, int soundMusic,
                      bool settingsDirty = false);

  int selection() const { return selection_; }   // DAT_0054bdbc
  int soundFx() const { return soundFx_; }       // DAT_00541308
  int soundMusic() const { return soundMusic_; } // DAT_0054130c
  bool settingsDirty() const { return settingsDirty_; }  // 0x541486
  int mouseX() const { return m_.mouseX; }
  int mouseY() const { return m_.mouseY; }
  int tick() const { return m_.tick; }
  float rampAccumulator() const { return m_.ramp.acc; }
  float smoothedDelta() const { return m_.timing.smoothed; }

  const FrontendMachineState& machineState() const { return m_; }

  // Per-frame update in the original order:
  //   prev -> next -> mouse hit-test -> Esc -> LEFT -> RIGHT ->
  //   activate. Every fired repeat/activate query emits Button
  //   BEFORE its row logic (the original plays OPTBUTT first);
  //   Esc emits none. LEFT/RIGHT mutations continue to the next
  //   query; Esc and row-2 activate end the frame before the draw.
  void update(const FrontendMenuInput& in);

  // FUN_00423a24 keyed (keyX, keyY) — the volume rows pass
  // (4, rowY), the Done row (-1, 179); called per drawn row in
  // draw order by the renderer.
  float itemScale(int keyX, int keyY, bool selFlag);

  // FUN_0042fe78 timing update — same body as the options endFrame.
  void endFrame(double dtMs);

  // Pending semantic action from the last dispatch.
  SoundAction pendingAction() const { return action_; }
  SoundAction consumeAction();

  // OBSERVED frame-termination flag: the Esc branch and row-2
  // activate end in RET before the draw block and timing update —
  // a dispatched frame draws nothing and does not advance the
  // timing machine. Mutations never set this.
  bool frameEndedEarly() const { return endedEarly_; }

  // The semantic audio events accumulated so far — entry events
  // (AmbientSongStop, SongStart) land in the constructor, per-frame
  // Button/VolumesApplied during update, exit events (SongStop,
  // AmbientSongStart) on the dispatch frame. The audio system in
  // the original is global; callers drain through the flow.
  const std::vector<SoundAudioEvent>& audioEvents() const {
    return events_;
  }
  std::vector<SoundAudioEvent> drainAudioEvents();

private:
  FrontendMachineState m_;   // the shared globals block
  int selection_;            // DAT_0054bdbc
  int soundFx_;              // DAT_00541308 — mutated by row 0
  int soundMusic_;           // DAT_0054130c — mutated by row 1
  bool settingsDirty_;       // DAT_00541486
  SoundAction action_ = SoundAction::None;
  bool endedEarly_ = false;
  std::vector<SoundAudioEvent> events_;
};

// Compose the proven static frame in the original draw order:
// palette bind (SYS_PAL head + zeroed tail + brightness lift —
// the screen performs NO palette upload of its own; it inherits
// the options screen's FUN_0046d208 upload, which the port binds
// per frame) -> clear(0) -> SND_TITL centered y=31 -> SND_INFO
// centered y=350 -> volume rows (scaled label x=4, bar, "0%"/"100%"
// FONTSML endpoints) -> SND_DONE centered y=179 -> ARROW at the
// mouse. `sysPalHead` must be the 192-byte SYS_PAL record head.
// Returns false with `err` on contract violations.
bool renderSoundMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                          const FtiFont& fontBig,
                          const FtiFont& fontSml,
                          const FtiSpriteFrame& arrow,
                          const SoundMenuLabels& labels,
                          std::span<const std::byte> sysPalHead,
                          const SoundMenuSpec& spec,
                          std::string* err);

// Same composition driven by the live controller: per-row scale
// from the FUN_00423a24 ramp (volume rows keyed (4,y), Done
// (-1,y), called in draw order), the controller's volumes/
// selection, ARROW at the controller's logical mouse. `brightness`
// is DAT_0054147e — the FUN_0046d208 upload lift applied to the
// bound palette (a process global the Display screen can mutate).
bool renderSoundMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                            const FtiFont& fontBig,
                            const FtiFont& fontSml,
                            const FtiSpriteFrame& arrow,
                            const SoundMenuLabels& labels,
                            std::span<const std::byte> sysPalHead,
                            SoundMenuController& ctl, int brightness,
                            std::string* err);

} // namespace mdk

#endif // MDK_CORE_SOUND_MENU_H
