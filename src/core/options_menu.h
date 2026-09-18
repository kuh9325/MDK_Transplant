// Phase 4F — options sub-menu (FUN_00420eac, front-end mode 0x0b).
//
// EVIDENCE (instruction-level, original binary — see
// docs/ENGINE_RECONSTRUCTION.md Phase 4F and
// analysis-private/logs/decomp_20eac.txt, disasm_optitems.txt):
//
//   Transition (FUN_0041dc90 selection 3 -> FUN_00420cf0, OBSERVED):
//     DAT_00541493 = 0x0b        options sub-menu mode
//     _DAT_0054bd34 = 8          entry selection = OM_QUIT
//     FUN_0046d614(svlut)        flatten active palette -> "svlut" buf
//     FUN_0046d208(0,0x100,
//       DAT_00540820)            upload the resident system palette
//                                (SYS_PAL head + tail written at
//                                front-end entry by FUN_004346e8)
//     NO mouse/tick/ramp reset   — those globals carry over
//
//   Item table (FUN_00420eac draw block + FUN_00420df8, OBSERVED):
//     row i draws FTI record via FUN_00414890 at y = 49 + 36*i,
//     x = trunc((600 - measure*scale)/2), scale from FUN_00423a24
//     keyed (-1, y), FONTBIG path (missing-glyph advance 6).
//     DAT_005414f4 ("-mapok" dev flag) hides rows 2,3,4 without
//     compacting the index space. Row 6's record is dynamic:
//     OM_SK_0/1/2 by DAT_0054147a (skill 0..2).
//
//   Input order (OBSERVED): prev query -> next query -> mouse gate ->
//   Esc check -> LEFT query -> RIGHT query -> activate query -> draw.
//
//   This module freezes none of that into the renderers: the static
//   spec and the live controller below model it exactly.
//
#ifndef MDK_CORE_OPTIONS_MENU_H
#define MDK_CORE_OPTIONS_MENU_H

#include "core/frontend_machines.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mdk {

class IndexedFramebuffer;
class Palette;
struct FtiFont;
struct FtiSpriteFrame;
struct FrontendMenuInput;

// OBSERVED options item table (FUN_00420eac draw block).
inline constexpr int kOptionsItemCount = 9;      // OM_* rows 0..8
inline constexpr int kOptionsItemY0 = 49;        // 0x31
inline constexpr int kOptionsItemStep = 36;      // 0x24
inline constexpr int kOptionsEntrySelection = 8; // _DAT_0054bd34 = 8
// OBSERVED hit-test constants: band = trunc((mouseY - 23) / 36).
inline constexpr int kOptionsHitBandBase = 23;   // 0x17
inline constexpr int kOptionsHitBandSize = 36;   // 0x24
// OBSERVED centered-x constants (FUN_0041518c): x87 f32 constants
// 600.0 and 0.5 — x = trunc((600 - w*scale) / 2). The options path
// centers on the framebuffer width, unlike the root menu's maxW/2.
inline constexpr int kOptionsCenterWidth = 600;
// Ramp item key (FUN_00423b88 -> FUN_00423a24): EDX = 0xffffffff.
inline constexpr int kOptionsRampKeyX = -1;
// Skill row's dynamic record set (DAT_0054147a == 0/1/2).
inline constexpr int kOptionsSkillRow = 6;

// FTI record names per row (OBSERVED at 0x495ca8..0x495d04 in
// MDK95.EXE). Row 6 is a placeholder — the drawn record is one of
// kOptionsSkillRecords chosen by the skill state.
inline constexpr std::array<const char*, kOptionsItemCount>
    kOptionsRecordNames = {"OM_HELP",  "OM_SOUND", "OM_JOY",
                           "OM_MOUSE", "OM_KEY",   "OM_PERF",
                           "OM_SK_0",  "OM_DISPL", "OM_QUIT"};
inline constexpr std::array<const char*, 3> kOptionsSkillRecords = {
    "OM_SK_0", "OM_SK_1", "OM_SK_2"};

// Resolved label strings for one frame: items[i] is the text drawn on
// row i. items[6] must be the label of the CURRENT skill variant.
struct OptionsMenuLabels {
  std::string_view items[kOptionsItemCount] = {};
};

// Semantic activation outputs of FUN_00420eac — emitted only; the
// original dispatch targets are child screens/settings mutations and
// are deferred (documented per case below). The skill mutation itself
// is NOT deferred: it is applied to the controller's skill state in
// the same dispatch that emits the cycle event.
enum class OptionsAction {
  None = 0,
  Help,           // sel 0 -> FUN_0041d540 (mode 0x0a help screen)
  Sound,          // sel 1 -> FUN_0042322c (mode 2 sound screen)
  Joystick,       // sel 2 -> FUN_0041fa24 (mode 3) — f4-flag gated
  Mouse,          // sel 3 -> FUN_00421664 (mode 4) — f4-flag gated
  Keyboard,       // sel 4 -> FUN_0041f030 (mode 5) — f4-flag gated
  Performance,    // sel 5 -> FUN_00421e70 (mode 6 perf screen)
  SkillCyclePrev, // sel 6 LEFT   -> DAT_0054147a -1 (wraps 0->2) +
                  //               DAT_00541486=1 dirty (0x421085)
  SkillCycleNext, // sel 6 RIGHT/activate -> DAT_0054147a +1 (wraps 2->0)
                  //               + dirty (0x421131 / 0x4211cb)
  Display,        // sel 7 -> FUN_0041d020 (mode 7 display screen)
  Back,           // sel 8 activate OR DIK_ESCAPE -> FUN_00420d68
                  // (mode 0 -> restore saved palette, re-enter root).
                  // OBSERVED quirk: LEFT/RIGHT on row 8 do NOT reach
                  // this — both dispatch tables bound at `cmp eax,7;
                  // ja`, so the QUIT row falls through to the next
                  // query (0x420fbe/0x4210b2 vs activate's 0x421167
                  // `cmp eax,8; ja`).
};

// The frozen frame state for the static preview (OBSERVED entry
// register values): selection 8 at scale 1.0, the rest 0.65, ARROW at
// the (unchanged) logical mouse position.
struct OptionsMenuSpec {
  int selection = kOptionsEntrySelection;
  int skill = 1;          // DAT_0054147a canonical 1 — the factory
                          // default (mirror byte @0x49b26e = 1) loaded
                          // by FUN_00425de4 at startup; BUILD_A's
                          // MDK.CFG carries no Skill override
  bool devHidden = false; // DAT_005414f4 ("-mapok") canonical 0
  int brightness = 0;     // DAT_0054147e canonical 0 — the FUN_0046d208
                          // upload lift; a Display-screen mutation
                          // shows on every screen's palette (Phase 4H)
  int arrowX = 300;       // logical mouse — NOT reset on entry
  int arrowY = 180;
};

// OBSERVED skill-record pick (0x421254..0x4212bf): `test eax,eax` ->
// OM_SK_0, `cmp eax,1` -> OM_SK_1, anything else -> OM_SK_2.
inline int optionsSkillRecordIndex(int skill) {
  return skill == 0 ? 0 : skill == 1 ? 1 : 2;
}

// The reconstructed controller — FUN_00420eac input/selection block.
// Frame protocol mirrors the original main loop exactly as the root
// controller does:
//     update(input)   — FUN_004187e0 accumulate + tick advance +
//                       FUN_00420eac queries/dispatch
//     itemScale(...)  — FUN_00423a24 keyed (-1, y), called by the
//                       renderer once per drawn item in draw order
//     endFrame(dtMs)  — FUN_0042fe78/FUN_0042fcd0 timing update
class OptionsMenuController {
public:
  // Mirrors FUN_00420cf0: selection 8; everything else carries over
  // from the shared machine state `s` (mouse, tick, deadlines, latch,
  // ramp, timing). `devHidden` is DAT_005414f4 (canonical 0); `skill`
  // is DAT_0054147a (canonical 1 -> "Skill - Normal"); `settingsDirty`
  // is DAT_00541486 (0 unless an earlier settings screen latched it).
  OptionsMenuController(const FrontendMachineState& s, bool devHidden,
                        int skill, bool settingsDirty = false);

  int selection() const { return selection_; }   // _DAT_0054bd34
  int skill() const { return skill_; }           // DAT_0054147a
  bool devHidden() const { return devHidden_; }  // DAT_005414f4
  // DAT_00541486 — latched 1 by every skill mutation, consumed by
  // FUN_00420d68 on exit (gated FUN_004260ac persist, then cleared).
  bool settingsDirty() const { return settingsDirty_; }
  int mouseX() const { return m_.mouseX; }
  int mouseY() const { return m_.mouseY; }
  int tick() const { return m_.tick; }
  float rampAccumulator() const { return m_.ramp.acc; }
  float smoothedDelta() const { return m_.timing.smoothed; }

  const FrontendMachineState& machineState() const { return m_; }
  // FUN_0041d144 returns from the Display child screen with the
  // shared globals intact — the options controller resumes with the
  // child's machine state and the shared dirty flag (DAT_00541486)
  // written back (Phase 4H).
  void setMachineState(const FrontendMachineState& s) { m_ = s; }
  void setSettingsDirty(bool dirty) { settingsDirty_ = dirty; }

  // Per-frame update in the original order:
  //   prev query -> next query -> mouse hit-test -> Esc -> LEFT ->
  //   RIGHT -> activate. Terminal dispatches end the frame early (the
  //   original RETs before the draw block); row 6 mutations and row 8
  //   under LEFT/RIGHT fall through to the next query.
  void update(const FrontendMenuInput& in);

  // FUN_00423a24 keyed (-1, itemY) — called per drawn row in draw
  // order by the renderer.
  float itemScale(int itemY, bool selFlag);

  // FUN_0042fe78 timing update — same body as the root's endFrame.
  void endFrame(double dtMs);

  // Pending semantic action from the last dispatch.
  OptionsAction pendingAction() const { return action_; }
  OptionsAction consumeAction();

  // OBSERVED frame-termination flag: the Esc branch and every
  // non-skill dispatch branch of FUN_00420eac end in RET before the
  // draw block and timing update — a dispatched frame draws nothing
  // and does not advance the timing machine. Skill cycles fall
  // through to the draw, so they do NOT set this.
  bool frameEndedEarly() const { return endedEarly_; }

private:
  // Which query is dispatching — the three dispatch tables
  // (0x420e48/0x420e68/0x420e88) differ on rows 6 and 8.
  enum class Query { Left, Right, Activate };
  // The shared dispatch: returns true when the original's branch ends
  // the frame (RET before the draw block — terminal rows 0,1,5,7, the
  // hidden-row no-op, and activate-8's FUN_00420d68). Returns false
  // when control falls through: row 6 after its in-place mutation, and
  // row 8 under LEFT/RIGHT (`cmp eax,7; ja` — OBSERVED bound quirk).
  bool dispatch(Query q);

  FrontendMachineState m_;   // the shared globals block
  int selection_;            // _DAT_0054bd34
  int skill_;                // DAT_0054147a — mutated by row 6
  bool settingsDirty_;       // DAT_00541486
  bool devHidden_;           // DAT_005414f4 ("-mapok")
  OptionsAction action_ = OptionsAction::None;
  // Whether the last update() hit a dispatch-branch RET — see
  // frameEndedEarly().
  bool endedEarly_ = false;
};

// Compose the proven static frame in the original draw order:
// clear(0) -> OM_* labels (row i at y=49+36i, centered on 600,
// scale 1.0 selected / 0.65 otherwise) -> ARROW at the mouse position.
// `sysPalHead` must be the 192-byte SYS_PAL record head — the palette
// bound is SYS_PAL[0:64] + zeros[64:256] (the FUN_0046d208 upload of
// DAT_00540820; the tail's exact runtime contents are the level-file
// palette resolved by FUN_00433d40 — unresolved in BUILD_A, and no
// drawn pixel references entries >= 64).
// Returns false with `err` on contract violations (wrong fb size,
// missing/empty labels, missing arrow frame).
bool renderOptionsMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                            const FtiFont& fontBig,
                            const FtiSpriteFrame& arrow,
                            const OptionsMenuLabels& labels,
                            std::span<const std::byte> sysPalHead,
                            const OptionsMenuSpec& spec,
                            std::string* err);

// Same composition driven by the live controller: per-row scale from
// the FUN_00423a24 ramp (keyed -1,y, called in draw order), ARROW at
// the controller's logical mouse. `brightness` is DAT_0054147e — the
// FUN_0046d208 upload lift applied to the bound palette (the Display
// screen mutates it; it is a process global, not options state).
bool renderOptionsMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                              const FtiFont& fontBig,
                              const FtiSpriteFrame& arrow,
                              const OptionsMenuLabels& labels,
                              std::span<const std::byte> sysPalHead,
                              OptionsMenuController& ctl,
                              int brightness,
                              std::string* err);

} // namespace mdk

#endif // MDK_CORE_OPTIONS_MENU_H
