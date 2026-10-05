// Mode-6 sub-state-3 briefing — see mode6_briefing.h for the full
// evidence header. OBSERVED addresses are MDK95.EXE.

#include "core/mode6_briefing.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mdk {
namespace {

// Original constants:
//   0x54beec / 0x54bef0 = 2.0f   (fade rates, init'd by the mode-6
//                                  dispatcher FUN_004292a4)
//   d[0x496d64] = 15.0f          (typing rate, normal)
//   d[0x496d68] = 60.0f          (typing rate, hurry 0x54c260)
//   d[0x496cfc] = 2.0            (hurry fade multiplier)
//   0x44fa0000  = 2000.0f        (skip latch forces the cursor)
//   penY starts 0x20; '\n' adds 36 (0x42adf4 path constant 0x24*? —
//   the unnumbered '\n' arm advances penY by 36; '\Nn' by N).
constexpr float kFadeRate = 2.0f;
constexpr float kTypeRate = 15.0f;
constexpr float kTypeHurry = 60.0f;
constexpr float kHurryMult = 2.0f;
constexpr float kSkipCursor = 2000.0f;
constexpr float kHoldTime = 5.0f;    // 0x40a00000 (sub-3 init arm)
constexpr int kPenY0 = 0x20;
constexpr int kLineStep = 36;
constexpr int kCenterX = 300;

inline int truncf_i(float f) { return static_cast<int>(f); }

// FUN_00416410 — blend `src` (flat 768B DAC) toward (r,g,b) by t;
// frac = trunc(t*256) with the original's endpoint clamp (t<=0 -> arg,
// t>=1 -> src).
void fadePalette(Palette& pal, const std::uint8_t* src, float t,
                 int r, int g, int b) {
  long frac = static_cast<long>(t * 256.0f);
  if (frac < 0) frac = 0;
  if (frac > 256) frac = 256;
  const long inv = 256 - frac;
  const int arg[3] = {r, g, b};
  for (int i = 0; i < 256; ++i) {
    Palette::Color c;
    c.r = (std::uint8_t)((src[i * 3 + 0] * frac + arg[0] * inv) >> 8);
    c.g = (std::uint8_t)((src[i * 3 + 1] * frac + arg[1] * inv) >> 8);
    c.b = (std::uint8_t)((src[i * 3 + 2] * frac + arg[2] * inv) >> 8);
    pal.set(i, c);
  }
}

} // namespace

void Mode6Briefing::enter(int levelId, const Mode6BriefingAssets& assets) {
  // FUN_00429cb4's init arm: palette tail copy rec[+0xc0..+0x300) into
  // working bytes [0xc0..0x300) (0x90 dwords at 0x429db7; same
  // 0x54c238 base feeds the +0x304 pixel blit, so the base INCLUDES
  // the record's 4-byte header). OBSERVED quirk: the copy lands
  // record-palette bytes [0xbc..0x2fc) — working entries 64..255 are
  // shifted 4 bytes (1⅓ entries) relative to the stored palette and
  // the top 4 palette bytes never load. Reproduced byte-exact.
  // health floor, bef4=3, bf04=5.0, BRIEF%d via FTI.
  // The head (entries 0..63) is the resident SYS_PAL, bound by the
  // caller via assets.sysHead.
  font_ = assets.font;
  const std::vector<std::uint8_t>& rec = assets.mapRecord;
  map_.assign(600 * 360, 0);
  if (rec.size() >= 0x304 + 600 * 360) {
    std::memcpy(map_.data(), rec.data() + 0x304, 600 * 360);
  }
  srcPal_.fill(0);
  if (assets.sysHead.size() >= 0xc0) {
    std::memcpy(srcPal_.data(), assets.sysHead.data(), 0xc0);
  }
  if (rec.size() >= 0x300) {
    std::memcpy(srcPal_.data() + 0xc0, rec.data() + 0xc0, 0x300 - 0xc0);
  }
  text_ = assets.briefText;

  bee4_ = 0.0f;
  bee8_ = 0.0f;
  bf04_ = kHoldTime;
  c24c_ = 1.0f;          // init arm: edx = 0x3f800000
  c254_ = 0;
  c268_ = 0;
  c25c_ = false;
  c260_ = false;
  pageDone_ = false;
  exitDone_ = false;
  palFinal_ = false;
  tickEvents_ = 0;
  initDone_ = true;      // first step stores bee4 = rate*dt, not +=
  (void)levelId;
}

// FUN_0042b020: when `measure` is empty the original returns WITHOUT
// drawing. Otherwise flag!=0 shifts x left by measureWidth/2, then
// FUN_00414c34 (FONTBIG) draws `draw` at (x, penY).
void Mode6Briefing::drawLine_(IndexedFramebuffer& fb,
                              std::string_view draw,
                              std::string_view measure, int x, int penY,
                              bool centered) {
  if (!font_ || measure.empty()) return;
  int px = x;
  if (centered) {
    px = x - measureFtiText(*font_, measure, kFtiFontBigMissingAdvance) / 2;
  }
  drawFtiText(*font_, draw, fb, px, penY, kFtiFontBigMissingAdvance);
}

// Direct port of FUN_0042acd4's walk. Locals map:
//   [ebp-0x10] = n, [ebp-0xc] = cursorChar, edi = penY (arg = 0x20)
//   [ebp-0x20] = state s, [ebp-0x28] = countFlag, [ebp-0x1c] = flag,
//   [ebp-0x18] = x, [ebp-0x14] = hadNum, [ebp-0x2c] = escNum,
//   [ebp-0x30] = escNum + penY, ecx = write ptr into buf0[ebp-0xd0]
//   or buf1[ebp-0x80].  flag and x BOTH start at 0.
int Mode6Briefing::drawPage_(IndexedFramebuffer& fb,
                             std::uint8_t cursorChar, int n) {
  if (!font_) return 1;
  const char* src = text_.data();
  char buf0[80] = {};
  char buf1[80] = {};
  int penY = kPenY0;
  int x = 0;
  int flag = 0;
  int s = 0;
  int countFlag = 1;
  char* dst = buf0;
  // Native hardening only: the original walks the buffers with no
  // bound; a record line longer than the buffers would corrupt its
  // stack. Stop consuming rather than overrun — unreachable for the
  // shipped BRIEF%d records (longest line < 40 chars).
  const auto atEnd = [&]() {
    return dst >= (s == 0 ? buf0 : buf1) + sizeof(buf0) - 2;
  };

head:
  if (n <= 0) {
    if (s == 1) goto walk;
    goto tail;
  }
  if (atEnd()) goto tail;
walk: {
  const unsigned char ch = static_cast<unsigned char>(*src);
  ++src;
  if (ch == '\\') {
    // Optional number: digits (or a leading '-') run; the original's
    // digit-skip loop also tolerates a '-' INSIDE the run.
    bool hadNum = false;
    long num = 0;
    {
      const char* p = src;
      if (*p == '-') ++p;
      const char* q = p;
      while (*q >= '0' && *q <= '9') ++q;
      if (q != p) {
        hadNum = true;
        num = std::strtol(src, nullptr, 10);
      }
      // Skip digits and any interleaved '-' chars (0x42ad2a/0x42add5).
      while ((*src >= '0' && *src <= '9') || *src == '-') ++src;
    }
    const unsigned char esc = static_cast<unsigned char>(*src);
    if (esc != '\0') ++src;
    switch (esc) {
      case 'c':
        *dst = '\0'; ++dst;
        if (s != 0) { s = 2; break; }
        drawLine_(fb, buf0, buf0, x, penY, flag != 0);
        x = hadNum ? (int)num : kCenterX;
        flag = 1;
        dst = buf0; buf0[0] = '\0';
        break;
      case 'n':
        *dst = '\0'; ++dst;
        if (s != 0) { s = 2; break; }
        drawLine_(fb, buf0, buf0, x, penY, flag != 0);
        flag = 0; x = 0;
        penY = hadNum ? penY + (int)num : penY + kLineStep;
        dst = buf0; buf0[0] = '\0';
        break;
      case 'x':
        *dst = '\0'; ++dst;
        if (s != 0) { s = 2; break; }
        drawLine_(fb, buf0, buf0, x, penY, flag != 0);
        x = hadNum ? (int)num : 0;   // 0x42aeaa: no number -> x=0
        flag = 0;
        dst = buf0; buf0[0] = '\0';
        break;
      case 'y':
        *dst = '\0'; ++dst;
        if (s != 0) { s = 2; break; }
        drawLine_(fb, buf0, buf0, x, penY, flag != 0);
        flag = 0; x = 0;
        penY = hadNum ? penY + (int)num : penY + kLineStep;
        dst = buf0; buf0[0] = '\0';
        break;
      case 'p':
        if (hadNum && countFlag) n -= (int)num;
        break;
      case 'd':
        countFlag = 1;
        break;
      case 'i':
        countFlag = 0;
        break;
      default:
        break;
    }
  } else if (ch == '\0') {
    ++dst;
    *(dst - 1) = '\0';
    if (s != 0) goto tail;
    drawLine_(fb, buf0, buf0, x, penY, flag != 0);
    return 1;
  } else {
    ++dst;
    *(dst - 1) = static_cast<char>(ch);
    if (countFlag) --n;
  }
  // Post-unit dispatch (0x42ada8).
  if (n > 0) goto head;
  if (flag == 0) goto head;
  if (s == 0) {
    // Split (0x42afcc): buf0 = prefix + cursor + NUL — the draw
    // string stays terminated. buf1 = the prefix copied over; the
    // lookahead appends at len (the measure string has NO cursor —
    // its slot is overwritten by the first lookahead char or the
    // next commit's NUL).
    const long len = dst - buf0;
    std::memcpy(buf1, buf0, (std::size_t)len);
    buf0[len] = static_cast<char>(cursorChar);
    buf0[len + 1] = '\0';
    s = 1;
    dst = buf1 + len;
    goto head;
  }
  if (s == 1) goto head;
  goto tail;
}
tail:
  if (flag == 0) {
    // 0x42b007: append the cursor at the current write pointer.
    *dst = static_cast<char>(cursorChar);
    dst[1] = '\0';
  }
  // 0x42ad47: flag!=0 pushes 1 (centered via buf1); flag==0 pushes 0
  // (drawn flush at x — a line never opened by '\c').
  drawLine_(fb, buf0, buf1, x, penY, flag != 0);
  return 0;
}

bool Mode6Briefing::step(const Mode6BriefingInput& in,
                         IndexedFramebuffer& fb, Palette& pal) {
  if (!initDone_) return true;
  const float dt = in.dtSec;

  // ---- FUN_004296f0 pump: input latches ---------------------------
  if (in.esc) {
    c25c_ = true;
  } else {
    c260_ = in.hurryA || in.hurryB;
    c25c_ = false;
  }

  // ---- Pump: first-frame init store (bee4 = rate*dt, not +=) ------
  if (bee4_ == 0.0f && bee8_ == 0.0f && !palFinal_) {
    bee4_ = kFadeRate * dt;
  }

  // ---- Pump: fade arms --------------------------------------------
  float frac = -1.0f;    // -1 = leave pal as the settled value
  if (bee4_ != 0.0f && bee4_ != 1.0f) {
    bee4_ += kFadeRate * dt * (c260_ ? kHurryMult : 1.0f);
    if (bee4_ >= 1.0f) {
      bee4_ = 1.0f;
      palFinal_ = true;  // FUN_00413b40 — settled-palette upload
    } else {
      frac = bee4_;
    }
  } else if (bee8_ != 0.0f && bee8_ != 1.0f) {
    bee8_ += kFadeRate * dt * (c260_ ? kHurryMult : 1.0f);
    if (bee8_ > 1.0f) bee8_ = 1.0f;
    frac = 1.0f - bee8_;
  }
  if (frac >= 0.0f) {
    fadePalette(pal, srcPal_.data(), frac, 0, 0, 0);
  } else {
    for (int i = 0; i < 256; ++i) {
      pal.set(i, {srcPal_[i * 3 + 0], srcPal_[i * 3 + 1],
                  srcPal_[i * 3 + 2], 255});
    }
    if (bee8_ == 1.0f) fadePalette(pal, srcPal_.data(), 0.0f, 0, 0, 0);
  }

  // ---- Body: map blit ---------------------------------------------
  std::memcpy(fb.pixels(), map_.data(), 600 * 360);

  // ---- Body: typing + exit (bee4==1.0 only) -----------------------
  if (bee4_ == 1.0f && !exitDone_) {
    c254_ += in.frameStep;
    const std::uint8_t cursorChar =
        (c254_ & 0x1f) <= 15 ? (std::uint8_t)'_' : (std::uint8_t)' ';
    c268_ = truncf_i(c24c_);
    c24c_ += dt * (c260_ ? kTypeHurry : kTypeRate);
    if (c25c_) c24c_ = kSkipCursor;
    const int n = truncf_i(c24c_);
    const int done = drawPage_(fb, cursorChar, n);
    if (truncf_i(c24c_) != c268_) ++tickEvents_;
    if (done) {
      pageDone_ = true;
      // ---- FUN_00429600 ------------------------------------------
      if (bf04_ != 0.0f) {
        if (c25c_ || in.anyKey) {
          bf04_ = 0.0f;
        }
        if (bf04_ == 0.0f) {
          bee8_ = kFadeRate * dt * (c260_ ? kHurryMult : 1.0f);
        }
      }
      exitDone_ = (bf04_ == 0.0f && bee8_ == 1.0f);
    }
  }
  return exitDone_;
}

} // namespace mdk
