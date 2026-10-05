#ifndef MDK_CORE_MODE6_BRIEFING_H
#define MDK_CORE_MODE6_BRIEFING_H

// Mode-6 sub-state-3 briefing screen — the Earth/mission map + typed
// BRIEF%d text that runs between the frontend New Game edge (or the
// campaign advance chain) and the mode-2 freefall.
//
// OBSERVED (MDK95.EXE disasm, analysis-private/logs/p5n_29cb4.txt,
// p14_dis_296f0.txt, p19a_m6.asm):
//
//   FUN_00429cb4 (sub-3 body):
//     - init arm (54bef4 != 3): binds L<levelId+1>_MAP
//       (s__L_d_MAP_00496d0b+1 -> "L%d_MAP"), palette tail copy
//       rec[+0xc0..+0x300) -> 0x54bf34+0xc0 (0x90 dwords, byte-exact —
//       the record carries a 4-byte header then 768B palette then
//       216000B pixels at +0x304, so the copy lands record bytes
//       [0xc0..0x300) into working entries 64-255), health floor 100,
//       54bef4 = 3, 54bf04 = 5.0 (key-hold, never decremented), binds
//       BRIEF<levelId+1> via FTI -> 0x54c250.
//     - per frame: rep MOVSD rec+0x304 -> framebuffer (54000 dwords =
//       600x360 indexed); when the pump's fade-in reaches 1.0, the
//       typewriter runs.
//   FUN_004296f0 (shared mode-6 pump, runs before the body):
//     - input: 54b570 (Esc level) -> 54c25c = 1 (skip latch);
//       else 54b668|54b660 -> 54c260 = 1 (hurry latch); else both 0.
//     - fade-in: 54bee4 += 54beec(2.0)*dt*(x2.0 when hurry) until
//       ==1.0 -> FUN_00413b40 (palette upload); while <1:
//       FUN_00416410(bee4, &54bf34, sub==2 ? ff,ff,ff : 0,0,0).
//     - fade-out: at bee4 boundary, when 54bee8 is mid:
//       54bee8 += 54bef0(2.0)*dt*(x2.0 hurry);
//       FUN_00416410(1.0-bee8, &54bf34, 0,0,0).
//   FUN_0042acd4 (typed-page renderer): cursor n = trunc(54c24c);
//     54c24c += dt*(60 when 54c260 else 15); 54c25c -> 54c24c=2000.
//     Escape machine: '\' + optional [-]digits -> atoi -> number.
//     '\c' = commit line (x = number-or-300, no penY advance),
//     '\n' = commit + penY += 36, '\Nn' = commit + penY += N,
//     '\p' = n -= N (page pause), 'd'/'i' = count-flag on/off.
//     Two 80-byte line buffers: the typed prefix (drawn) and the
//     prefix+cursor+lookahead (center measure). Lines draw via
//     FUN_0042b020 -> FUN_00414c34 FONTBIG at x=300 centered,
//     penY from 0x20 (+36/line). NUL = page end -> return 1.
//   FUN_00429600 (exit query, called when the page completes):
//     bee4==1.0 && bf04==0 && bee8==1.0 -> 1; else, while bf04>0:
//     any of the 31 action-key slots (or 54c25c) zeroes bf04 and arms
//     bee8 += 54bef0*dt*(x2.0 hurry).
//   FUN_00416410(t, src, r,g,b): per-channel out =
//     (src*trunc(t*256) + arg*(256-trunc(t*256))) >> 8 — the 256-entry
//     palette blend used for both fades.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/framebuffer.h"
#include "core/fti_font.h"

namespace mdk {

// One frame of input for the briefing machine. `dtSec` is the
// smoothed frame delta (0x49b6f4 semantics — frame units / 30);
// `frameStep` is the integer timer step (0x49b6e8).
struct Mode6BriefingInput {
  float dtSec = 0.0f;
  int frameStep = 0;
  bool esc = false;     // 0x54b570 — Esc held level
  bool hurryA = false;  // 0x54b668 — bound advance slot 6
  bool hurryB = false;  // 0x54b660 — bound advance slot 4
  bool anyKey = false;  // 0x54b650[0..31] aggregate — any bound action
};

// Bound assets the briefing consumes. mapRecord/briefText are copied
// on enter; font/sysHead are views (the caller owns the font object).
struct Mode6BriefingAssets {
  // FONTBIG (0x541648) — decoded by the caller from MISC/MDKFONT.FTI.
  const FtiFont* font = nullptr;
  // The resident SYS_PAL head — working entries 0..63 (192 bytes).
  std::span<const std::uint8_t> sysHead;
  // L<levelId+1>_MAP record payload INCLUDING its 4-byte header:
  // [0..4) header, [4..0x304) palette, [0x304..) 600*360 pixels.
  std::vector<std::uint8_t> mapRecord;
  // BRIEF<levelId+1> page text (NUL-terminated).
  std::vector<char> briefText;
};

class Mode6Briefing {
public:
  // FUN_00429cb4's init arm. `levelId` is 541498 (0-based; the record
  // names take levelId+1).
  void enter(int levelId, const Mode6BriefingAssets& assets);

  // One pump+body frame. Blits the map, runs fades/typing into `fb`
  // and the faded DAC palette into `pal`. Returns true when the stage
  // is done (FUN_00429600's exit edge — fade-out complete).
  bool step(const Mode6BriefingInput& in, IndexedFramebuffer& fb,
            Palette& pal);

  // Per-frame event counters for the host (TELETYPE tick, etc).
  int takeTickEvents() { int n = tickEvents_; tickEvents_ = 0; return n; }

  // State mirrors for diagnostics/tests.
  float fadeIn() const { return bee4_; }
  float fadeOut() const { return bee8_; }
  float charCursor() const { return c24c_; }
  bool typingDone() const { return pageDone_; }
  bool exitDone() const { return exitDone_; }
  bool skipLatch() const { return c25c_; }
  bool hurryLatch() const { return c260_; }

private:
  // The FUN_0042acd4 walk/draw — returns 1 at a page NUL (page done).
  int drawPage_(IndexedFramebuffer& fb, std::uint8_t cursorChar, int n);
  // FUN_0042b020 -> FUN_00414c34: draws `draw` at (x, penY); when
  // `centered`, x is shifted left by measure(measure)/2 first.
  void drawLine_(IndexedFramebuffer& fb, std::string_view draw,
                 std::string_view measure, int x, int penY,
                 bool centered);

  const FtiFont* font_ = nullptr;
  std::vector<std::uint8_t> map_;       // 600*360 pixels (rec +0x304)
  std::array<std::uint8_t, 768> srcPal_{};  // working palette 0x54bf34
  std::vector<char> text_;              // BRIEF%d stream
  bool initDone_ = false;               // 0x54bef4 latch

  float bee4_ = 0.0f;   // fade-in 0..1   (0x54bee4)
  float bee8_ = 0.0f;   // fade-out 0..1  (0x54bee8)
  float bf04_ = 5.0f;   // key-hold       (0x54bf04)
  float c24c_ = 0.0f;   // char cursor    (0x54c24c)
  int   c254_ = 0;      // blink accum    (0x54c254)
  int   c268_ = 0;      // prev trunc     (0x54c268)
  bool  c25c_ = false;  // skip latch     (0x54c25c)
  bool  c260_ = false;  // hurry latch    (0x54c260)
  bool  pageDone_ = false;
  bool  exitDone_ = false;
  bool  palFinal_ = false;  // FUN_00413b40 once-latch
  int   tickEvents_ = 0;
};

} // namespace mdk

#endif // MDK_CORE_MODE6_BRIEFING_H
