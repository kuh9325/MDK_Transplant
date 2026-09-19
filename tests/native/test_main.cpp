// Phase 3A unit tests for platform-neutral logic. No SDL, no Metal —
// those paths are exercised by the runtime smoke test instead.

#include "core/binary_reader.h"
#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/cmi_directory.h"
#include "core/collision_query.h"
#include "core/compat.h"
#include "core/container.h"
#include "core/data_root.h"
#include "core/display_menu.h"
#include "core/dti_structure.h"
#include "core/file_family.h"
#include "core/framebuffer.h"
#include "core/frontend_flow.h"
#include "core/frontend_menu.h"
#include "core/frontend_settings.h"
#include "core/fti_directory.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/gameplay_input.h"
#include "core/indexed_image.h"
#include "core/keyboard_menu.h"
#include "core/mode_dispatch.h"
#include "core/mti_directory.h"
#include "core/mto_directory.h"
#include "core/options_menu.h"
#include "core/player_motion.h"
#include "core/player_vertical.h"
#include "core/sni_directory.h"
#include "core/sound_menu.h"
#include "core/stream_context.h"
#include "core/viewport.h"
#include "input/input_state.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++checks;                                                              \
    if (!(cond)) {                                                         \
      ++failures;                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                      \
  } while (0)

bool near(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) < eps;
}

void test_framebuffer() {
  mdk::IndexedFramebuffer fb(8, 4);
  CHECK(fb.width() == 8 && fb.height() == 4);
  CHECK(fb.pixelCount() == 32);

  fb.clear(7);
  CHECK(fb.at(0, 0) == 7 && fb.at(7, 3) == 7);

  fb.put(3, 2, 42);
  CHECK(fb.at(3, 2) == 42);
  CHECK(fb.at(3, 1) == 7); // neighbor untouched

  // Out-of-bounds writes/reads are safe no-ops / zero.
  fb.put(-1, 0, 9);
  fb.put(0, 99, 9);
  CHECK(fb.at(-5, -5) == 0);
  CHECK(fb.at(8, 4) == 0);
}

void test_palette_expand() {
  mdk::Palette pal;
  pal.set(0, {0, 0, 0, 255});
  pal.set(1, {10, 20, 30, 255});
  pal.set(255, {200, 100, 50, 255});

  mdk::IndexedFramebuffer fb(2, 2);
  fb.clear(0);
  fb.put(0, 0, 1);
  fb.put(1, 1, 255);

  std::vector<std::uint8_t> out(fb.pixelCount() * 4);
  mdk::expandToBGRA(fb, pal, out.data());

  // BGRA byte order: B,G,R,A
  CHECK(out[0] == 30 && out[1] == 20 && out[2] == 10 && out[3] == 255);
  CHECK(out[4] == 0 && out[5] == 0 && out[6] == 0 && out[7] == 255);
  const std::size_t last = 3 * 4;
  CHECK(out[last] == 50 && out[last + 1] == 100 && out[last + 2] == 200 &&
        out[last + 3] == 255);
}

void test_viewport() {
  using mdk::aspectFit;
  using mdk::presentationRect;

  // Exact 4:3 window: canvas fills it.
  mdk::RectD r = aspectFit(640, 480, 4.0 / 3.0);
  CHECK(near(r.x, 0) && near(r.y, 0) && near(r.w, 640) && near(r.h, 480));

  // Wide window: pillarbox (full height, centered horizontally).
  r = aspectFit(800, 480, 4.0 / 3.0);
  CHECK(near(r.w, 640) && near(r.h, 480) && near(r.x, 80) && near(r.y, 0));

  // Tall window: letterbox (full width, centered vertically).
  r = aspectFit(640, 600, 4.0 / 3.0);
  CHECK(near(r.w, 640) && near(r.h, 480) && near(r.x, 0) && near(r.y, 60));

  // Degenerate input.
  r = aspectFit(0, 480, 4.0 / 3.0);
  CHECK(r.w == 0 && r.h == 0);

  // presentationRect: 600x360 centered inside the 640x480 canvas.
  // On an exact 4:3 window: x=20, y=60, w=600, h=360.
  r = presentationRect(640, 480);
  CHECK(near(r.x, 20) && near(r.y, 60) && near(r.w, 600) && near(r.h, 360));

  // On a 2x Retina drawable the rect scales linearly.
  r = presentationRect(1280, 960);
  CHECK(near(r.x, 40) && near(r.y, 120) && near(r.w, 1200) &&
        near(r.h, 720));

  // On a wide drawable: pillarboxed canvas (x=160..1440), inner rect
  // centered within it (+40 inside the canvas).
  r = presentationRect(1600, 960);
  CHECK(near(r.w, 1200) && near(r.h, 720));
  CHECK(near(r.x, 200) && near(r.y, 120));
}

void test_binary_reader() {
  const std::byte raw[] = {
      std::byte{0x34}, std::byte{0x12},                         // u16
      std::byte{0x78}, std::byte{0x56}, std::byte{0x34},
      std::byte{0x12},                                          // u32
      std::byte{'A'},  std::byte{'B'},  std::byte{'C'},
      std::byte{'D'},                                           // tag
      std::byte{0xff},
  };
  mdk::BinaryReader r(raw);
  CHECK(r.size() == 11 && r.remaining() == 11);
  CHECK(r.u16le() == 0x1234);
  CHECK(r.u32le() == 0x12345678);
  CHECK(r.position() == 6);
  const auto tag = r.bytes(4);
  CHECK(tag && tag->size() == 4 && (*tag)[0] == std::byte{'A'} &&
        (*tag)[3] == std::byte{'D'});
  CHECK(r.u8() == 0xff);
  CHECK(r.atEnd());
  CHECK(!r.u8().has_value());          // EOF
  CHECK(!r.u16le().has_value());
  CHECK(!r.peekU32le(8).has_value());  // truncated peek
  CHECK(r.peekU32le(0) == 0x56781234ull); // bytes 0..3 as u32
  CHECK(!r.seek(12));                  // past end
  CHECK(r.seek(0) && r.position() == 0);

  // Sub-reader bounds: slicing advances the parent, the slice cannot
  // read past its own bounds.
  CHECK(r.seek(6));
  auto sub = r.subReader(4);
  CHECK(sub && r.position() == 10);
  CHECK(sub->u8() == 'A');
  CHECK(!r.subReader(10).has_value()); // oversized slice fails, no move
  CHECK(r.position() == 10);

  // Oversized lengths and overflow-safe arithmetic.
  mdk::BinaryReader empty(std::span<const std::byte>{});
  CHECK(!empty.u8().has_value());
  CHECK(empty.seek(0));
  CHECK(!empty.seek(1));
  CHECK(!r.skip(100));
  CHECK(!r.bytes(std::size_t(-1)).has_value()); // would overflow n<=rem
  CHECK(!r.peekU32le(std::size_t(-3)).has_value());
}

void test_container() {
  using mdk::ContainerShape;

  // Valid synthetic tag envelope: u32 len = size-4, name "TEST.MAT".
  std::byte env[28] = {};
  env[0] = std::byte{24};
  std::memcpy(env + 4, "TEST.MAT", 8);
  env[16] = std::byte{16}; // u32@16 = size-12, as observed in BUILD_A
  const auto info = mdk::inspectContainer(env, sizeof(env));
  CHECK(info.hasDeclaredLength && info.declaredLength == 24);
  CHECK(info.lengthValid);
  CHECK(info.shape == ContainerShape::kTaggedName);
  CHECK(info.hasTag && info.tag[0] == std::byte{'T'});
  CHECK(info.logicalName == "TEST.MAT");
  CHECK(mdk::nameStemMatches(info, "TEST"));
  CHECK(mdk::nameStemMatches(info, "test"));   // ASCII case-insensitive
  CHECK(!mdk::nameStemMatches(info, "TES"));   // prefix-only rejected
  CHECK(!mdk::nameStemMatches(info, "TESTX"));
  CHECK(!mdk::nameStemMatches(info, "TESTTOOLONGNAME"));

  // Zero-length payload envelope: size 4, declared 0 — admitted at the
  // envelope level (original semantics UNKNOWN; we only check length).
  const std::byte tiny[] = {std::byte{0}, std::byte{0}, std::byte{0},
                            std::byte{0}};
  const auto tinfo = mdk::inspectContainer(tiny, sizeof(tiny));
  CHECK(tinfo.lengthValid && tinfo.shape == ContainerShape::kLengthEnvelope);

  // Truncated headers.
  const auto none0 = mdk::inspectContainer({}, 0);
  CHECK(!none0.hasDeclaredLength && none0.shape == ContainerShape::kNone);
  const std::byte three[3] = {};
  CHECK(!mdk::inspectContainer(three, 3).hasDeclaredLength);

  // Declared length beyond the file / wrong length -> not an envelope.
  std::byte bad[20] = {};
  bad[0] = std::byte{0xff};
  std::memcpy(bad + 4, "TEST.MAT", 8);
  const auto binfo = mdk::inspectContainer(bad, sizeof(bad));
  CHECK(!binfo.lengthValid && binfo.shape == ContainerShape::kNone);

  // Unknown / non-printable tag (the .FTI/.BNI shape): length valid,
  // but the name field is binary — kLengthEnvelope, never kTaggedName.
  std::byte cnt[24] = {};
  cnt[0] = std::byte{20};
  cnt[4] = std::byte{0x03}; // count-like field, non-ASCII run start
  std::memcpy(cnt + 8, "ENTRYONE", 8);
  const auto cinfo = mdk::inspectContainer(cnt, sizeof(cnt));
  CHECK(cinfo.lengthValid);
  CHECK(cinfo.shape == ContainerShape::kLengthEnvelope);
  CHECK(cinfo.hasTag && !mdk::tagIsPrintable(cinfo.tag));

  // Corrupt padding after NUL -> not a plausible name field.
  std::byte pad[24] = {};
  pad[0] = std::byte{20};
  std::memcpy(pad + 4, "AB\0X", 4);
  const auto pinfo = mdk::inspectContainer(pad, sizeof(pad));
  CHECK(pinfo.lengthValid && pinfo.shape == ContainerShape::kLengthEnvelope);

  // Full 12-char unterminated name is still tagged (MDKSOUND.SND case).
  std::byte full[24] = {};
  full[0] = std::byte{20};
  std::memcpy(full + 4, "ABCDEFGHIJKL", 12);
  const auto finfo = mdk::inspectContainer(full, sizeof(full));
  CHECK(finfo.shape == ContainerShape::kTaggedName);
  CHECK(finfo.logicalName == "ABCDEFGHIJKL");
  CHECK(mdk::nameStemMatches(finfo, "ABCDEFGHIJKL"));
  CHECK(!mdk::nameStemMatches(finfo, "ABCDEFGHIJK"));

  // Family classification by extension.
  using mdk::ParserFamily;
  CHECK(mdk::parserFamilyForPath("TRAVERSE\\LEVEL7\\LEVEL7O.MTO") ==
        ParserFamily::kTagEnvelope);
  CHECK(mdk::parserFamilyForPath("misc/mdksound.sni") ==
        ParserFamily::kTagEnvelope);
  CHECK(mdk::parserFamilyForPath("MISC/FONTF.FTI") ==
        ParserFamily::kLengthEnvelope);
  CHECK(mdk::parserFamilyForPath("MISC/OPTIONS.BNI") ==
        ParserFamily::kLengthEnvelope);
  CHECK(mdk::parserFamilyForPath("MISC/LOAD_7.LBB") ==
        ParserFamily::kOtherFormat);
  CHECK(mdk::parserFamilyForPath("MISC/FLIC/MDK12.FLC") ==
        ParserFamily::kOtherFormat);
  CHECK(mdk::parserFamilyForPath("SAVES/X.SAV") == ParserFamily::kOtherFormat);
  CHECK(mdk::parserFamilyForPath("FOO.XYZ") == ParserFamily::kUnknown);
  CHECK(mdk::parserFamilyForPath("NOEXT") == ParserFamily::kUnknown);
}

void test_file_family() {
  using mdk::MdkFileFamily;
  using mdk::FamilySupport;
  using mdk::fileFamilyForPath;
  using mdk::fileFamilySupport;

  // Extension dispatch is case-insensitive and path-position aware.
  CHECK(fileFamilyForPath("TRAVERSE\\LEVEL7\\LEVEL7O.MTO") ==
        MdkFileFamily::kMto);
  CHECK(fileFamilyForPath("misc/mdksound.sni") == MdkFileFamily::kSni);
  CHECK(fileFamilyForPath("MISC\\MDKSOUND.SNI") == MdkFileFamily::kSni);
  CHECK(fileFamilyForPath("a/b/c.mti") == MdkFileFamily::kMti);
  CHECK(fileFamilyForPath("X.CMI") == MdkFileFamily::kCmi);
  CHECK(fileFamilyForPath("X.DTI") == MdkFileFamily::kDti);
  CHECK(fileFamilyForPath("X.FTI") == MdkFileFamily::kFti);
  CHECK(fileFamilyForPath("X.BNI") == MdkFileFamily::kBni);
  CHECK(fileFamilyForPath("X.LBB") == MdkFileFamily::kLbb);
  CHECK(fileFamilyForPath("X.SAV") == MdkFileFamily::kSav);
  CHECK(fileFamilyForPath("X.FLC") == MdkFileFamily::kFlic);
  CHECK(fileFamilyForPath("X.MVE") == MdkFileFamily::kMve);
  CHECK(fileFamilyForPath("X.GIF") == MdkFileFamily::kGif);
  CHECK(fileFamilyForPath("X.FRC") == MdkFileFamily::kFrc);
  CHECK(fileFamilyForPath("MDK95.EXE") == MdkFileFamily::kOtherKnown);
  CHECK(fileFamilyForPath("README.TXT") == MdkFileFamily::kOtherKnown);
  CHECK(fileFamilyForPath("X.XYZ") == MdkFileFamily::kUnknown);
  CHECK(fileFamilyForPath("NOEXT") == MdkFileFamily::kUnknown);
  CHECK(fileFamilyForPath("dir.with.dot/noext") == MdkFileFamily::kUnknown);
  CHECK(fileFamilyForPath(".SNI") == MdkFileFamily::kSni); // ext-only name
  CHECK(fileFamilyForPath("a.SNI.bak") == MdkFileFamily::kUnknown);

  // Support levels (Phase 3C: SNI; Phase 3D: MTI; Phase 3E: MTO;
  // Phase 3F: CMI; Phase 3G: DTI; Phase 3H: FTI/BNI).
  CHECK(fileFamilySupport(MdkFileFamily::kSni) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kMti) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kMto) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kCmi) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kDti) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kFti) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kBni) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kFlic) ==
        FamilySupport::kStandardExternalFormat);
  CHECK(fileFamilySupport(MdkFileFamily::kMve) ==
        FamilySupport::kStandardExternalFormat);
  CHECK(fileFamilySupport(MdkFileFamily::kGif) ==
        FamilySupport::kStandardExternalFormat);
  CHECK(fileFamilySupport(MdkFileFamily::kFrc) ==
        FamilySupport::kStandardExternalFormat);
  CHECK(fileFamilySupport(MdkFileFamily::kLbb) == FamilySupport::kUnsupported);
  CHECK(fileFamilySupport(MdkFileFamily::kSav) == FamilySupport::kUnsupported);
  CHECK(fileFamilySupport(MdkFileFamily::kOtherKnown) ==
        FamilySupport::kUnsupported);
  CHECK(fileFamilySupport(MdkFileFamily::kUnknown) ==
        FamilySupport::kUnsupported);

  // Envelope-level grouping stays consistent with the family table.
  CHECK(mdk::parserFamilyForPath("x.sni") == mdk::ParserFamily::kTagEnvelope);
  CHECK(mdk::parserFamilyForPath("x.bni") ==
        mdk::ParserFamily::kLengthEnvelope);
  CHECK(mdk::parserFamilyForPath("x.flc") == mdk::ParserFamily::kOtherFormat);
}

// Synthetic SNI file builder (no original data). Layout mirrors the
// OBSERVED structure: [u32 size-4][name12][u32 size-12][u32 count]
// [count x 24B records][payloads][name12 trailer].
struct SyntheticSni {
  std::vector<std::byte> buf;

  void put32(std::size_t off, std::uint32_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    buf[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    buf[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  }
  void putName(std::size_t off, const char* s, std::size_t width = 12) {
    for (std::size_t i = 0; s[i] && i < width; ++i)
      buf[off + i] = static_cast<std::byte>(s[i]);
  }

  // Build: logicalName, entries as {name, field0c, payloadSize};
  // payload blobs are laid out contiguously from directory end.
  static SyntheticSni build(const char* logicalName,
                            std::initializer_list<
                                std::tuple<const char*, std::uint32_t,
                                           std::uint32_t>> entries) {
    SyntheticSni s;
    const std::uint32_t count = static_cast<std::uint32_t>(entries.size());
    const std::uint64_t dirEnd = 0x18 + std::uint64_t(count) * 24;
    std::uint64_t total = dirEnd;
    for (const auto& [n, f, sz] : entries)
      total += sz;
    total += 12; // trailer
    s.buf.assign(static_cast<std::size_t>(total), std::byte{0});

    s.put32(0x00, static_cast<std::uint32_t>(total - 4));
    s.putName(0x04, logicalName);
    s.put32(0x10, static_cast<std::uint32_t>(total - 12));
    s.put32(0x14, count);
    std::uint64_t payloadAt = dirEnd;
    std::uint32_t i = 0;
    for (const auto& [n, f, sz] : entries) {
      const std::uint64_t rec = 0x18 + std::uint64_t(i) * 24;
      s.putName(rec, n);
      s.put32(rec + 0x0c, f);
      s.put32(rec + 0x10,
              static_cast<std::uint32_t>(payloadAt - 4)); // blob-relative
      s.put32(rec + 0x14, sz);
      payloadAt += sz;
      ++i;
    }
    s.putName(static_cast<std::size_t>(total - 12), logicalName);
    return s;
  }
};

void test_sni_directory() {
  using mdk::SniDirectoryStatus;
  using mdk::inspectSniDirectory;

  // Valid two-entry directory: offsets tile from dirEnd, trailer ok.
  {
    auto s = SyntheticSni::build("TEST.SND", {{"CORRIDOR", 3, 100},
                                            {"FOOT1", 0, 200}});
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kOk);
    CHECK(d.count == 2 && d.entries.size() == 2);
    CHECK(d.entries[0].name() == "CORRIDOR");
    CHECK(d.entries[1].name() == "FOOT1");
    CHECK(d.entries[0].fieldAt0x0C == 3 && d.entries[1].fieldAt0x0C == 0);
    // dirEnd = 0x18 + 2*24 = 0x48. First payload at file offset dirEnd.
    CHECK(d.directoryEnd == 0x48);
    CHECK(d.entries[0].payloadFileOffset() == 0x48);
    CHECK(d.entries[0].payloadFileEnd() == 0x48 + 100);
    // blobOffset is stored relative to file offset 4.
    CHECK(d.entries[0].blobOffset == 0x48 - 4);
    CHECK(d.entries[1].payloadFileOffset() == 0x48 + 100);
    CHECK(d.entries[1].payloadFileEnd() == s.buf.size() - 12);
    CHECK(d.trailerPresent);
    CHECK(d.secondaryEqualsTrailerOffset);
    CHECK(d.secondaryLength == s.buf.size() - 12);
  }

  // Zero-entry directory: count=0, only the trailer after it.
  {
    auto s = SyntheticSni::build("EMPTY.SND", {});
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kOk);
    CHECK(d.count == 0 && d.entries.empty());
    CHECK(d.directoryEnd == 0x18);
  }

  // Name at exactly 12 bytes (no NUL terminator inside the field).
  {
    auto s = SyntheticSni::build("TEST.SND", {{"SNIPERSHOT12", 0, 8}});
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kOk);
    CHECK(d.entries[0].name() == "SNIPERSHOT12");
    CHECK(d.entries[0].nameField[11] == std::byte{'2'});
  }

  // Payload offset landing before directory end → rejected.
  {
    auto s = SyntheticSni::build("TEST.SND", {{"A", 0, 4}});
    s.put32(0x18 + 0x10, 0); // blobOff=0 → file offset 4 < dirEnd
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kEntryOutOfBounds);
    CHECK(d.badEntryIndex == 0);
  }

  // Payload end escaping past the trailer → rejected.
  {
    auto s = SyntheticSni::build("TEST.SND", {{"A", 0, 4}});
    s.put32(0x18 + 0x14, static_cast<std::uint32_t>(s.buf.size()));
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kEntryOutOfBounds);
  }

  // Directory table overrunning the file → rejected (count too large).
  {
    auto s = SyntheticSni::build("TEST.SND", {{"A", 0, 4}});
    s.put32(0x14, 0xffffffff); // impossible count
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kDirectoryOutOfBounds);
  }

  // Inflated count in a 1-record file (total 64 B): count=2 needs the
  // table to reach 0x48 > 0x40 → directory bound fails.
  {
    auto s = SyntheticSni::build("TEST.SND", {{"A", 0, 4}});
    s.put32(0x14, 2);
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kDirectoryOutOfBounds);
  }

  // Inflated count where the table still "fits" on disk (2-entry file
  // with 20 B payloads, 124 B; count=3 → dirEnd 0x60 < 0x7c): the
  // enlarged directory swallows entry 0's real payload start
  // (0x48 < 0x60) — caught by the entry bounds check on the first
  // invalidated entry.
  {
    auto s = SyntheticSni::build("TEST.SND", {{"A", 0, 20}, {"B", 0, 20}});
    s.put32(0x14, 3);
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kEntryOutOfBounds);
    CHECK(d.badEntryIndex == 0);
  }

  // Sentinel record class (OBSERVED: 'K_'-prefixed trailing entries
  // with field0x0c == payloadSize == 0xffffffff): the stored offset is
  // a real in-bounds position; size is never treated as a byte count.
  {
    auto s = SyntheticSni::build("TEST.SND", {{"A", 0, 4}, {"B", 0, 20}});
    // Overwrite entry 1 into a sentinel pointing at file offset 0x50
    // (inside the payload region, before the trailer at size-12).
    s.putName(0x18 + 24, "K_MARK");
    s.put32(0x18 + 24 + 0x0c, 0xffffffff);
    s.put32(0x18 + 24 + 0x10, 0x50 - 4); // blobOff → file 0x50
    s.put32(0x18 + 24 + 0x14, 0xffffffff);
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kOk);
    CHECK(d.entries.size() == 2);
    CHECK(!d.entries[0].isSentinel());
    CHECK(d.entries[1].isSentinel());
    CHECK(d.entries[1].payloadFileOffset() == 0x50);

    // Sentinel with an out-of-bounds marker position is still invalid.
    auto s2 = s;
    s2.put32(0x18 + 24 + 0x10,
             static_cast<std::uint32_t>(s2.buf.size())); // way past EOF
    const auto d2 = inspectSniDirectory(s2.buf);
    CHECK(d2.status == SniDirectoryStatus::kEntryOutOfBounds);
    CHECK(d2.badEntryIndex == 1);
  }

  // Half-sentinel (only size == 0xffffffff) is NOT the sentinel class —
  // treated as a normal entry with an impossible byte count.
  {
    auto s = SyntheticSni::build("TEST.SND", {{"A", 0, 4}, {"B", 0, 4}});
    s.put32(0x18 + 24 + 0x14, 0xffffffff); // size only
    const auto d = inspectSniDirectory(s.buf);
    CHECK(!d.entries.empty()); // entries vector may be partial on fail
    CHECK(d.status == SniDirectoryStatus::kEntryOutOfBounds);
  }

  // Truncated header: envelope is fine but file ends before count.
  {
    // 20-byte tagged envelope (name valid, length valid), < 0x18.
    std::byte t[20] = {};
    t[0] = std::byte{16};
    std::memcpy(t + 4, "T.SND", 5);
    const auto d = inspectSniDirectory(t);
    CHECK(d.status == SniDirectoryStatus::kTruncatedHeader);
  }

  // Not a tagged envelope → "not this format", not "malformed".
  {
    std::byte raw[64] = {};
    const auto d = inspectSniDirectory(raw);
    CHECK(d.status == SniDirectoryStatus::kNotTaggedEnvelope);

    // Length-valid but non-tag name field (the FTI/BNI shape).
    std::byte fti[32] = {};
    fti[0] = std::byte{28};
    fti[4] = std::byte{0x03};
    const auto d2 = inspectSniDirectory(fti);
    CHECK(d2.status == SniDirectoryStatus::kNotTaggedEnvelope);
  }

  // Missing trailer → payloads may extend to EOF; still parses.
  {
    auto s = SyntheticSni::build("TEST.SND", {{"A", 0, 4}});
    // Corrupt the trailer (all NUL now → name mismatch).
    for (std::size_t i = s.buf.size() - 12; i < s.buf.size(); ++i)
      s.buf[i] = std::byte{'X'};
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kOk);
    CHECK(!d.trailerPresent);
    // u32@0x10 still equals size-12 numerically.
    CHECK(d.secondaryEqualsTrailerOffset);
  }

  // u32@0 length mismatch → envelope invalid → not this format.
  {
    auto s = SyntheticSni::build("TEST.SND", {{"A", 0, 4}});
    s.put32(0x00, 0);
    const auto d = inspectSniDirectory(s.buf);
    CHECK(d.status == SniDirectoryStatus::kNotTaggedEnvelope);
  }
}

// Synthetic MTI file builder (no original data). Layout mirrors the
// CODE-CORROBORATED structure: [u32 size-4][name12][u32 size-12]
// [u32 count][count x 24B records {name[8],f8,fc,f10,f14}][payloads]
// [name12 trailer].
struct SyntheticMti {
  std::vector<std::byte> buf;

  void put32(std::size_t off, std::uint32_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    buf[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    buf[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  }
  void put16(std::size_t off, std::uint16_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
  }
  void putName(std::size_t off, const char* s, std::size_t width = 12) {
    for (std::size_t i = 0; s[i] && i < width; ++i)
      buf[off + i] = static_cast<std::byte>(s[i]);
  }

  // entries: {name, field0x08, field0x0c, field0x10, payloadBytes}.
  // field0x08 == 0xffffffff builds an index record (no payload);
  // otherwise a payload record whose payload of payloadBytes is laid
  // out contiguously from the directory end (zero-filled unless the
  // test pokes header bytes itself).
  static SyntheticMti build(const char* logicalName,
                            std::initializer_list<
                                std::tuple<const char*, std::uint32_t,
                                           std::uint32_t, std::uint32_t,
                                           std::uint32_t>> entries) {
    SyntheticMti s;
    const std::uint32_t count = static_cast<std::uint32_t>(entries.size());
    const std::uint64_t dirEnd = 0x18 + std::uint64_t(count) * 24;
    std::uint64_t total = dirEnd;
    for (const auto& [n, f8, fc, f10, sz] : entries)
      if (f8 != 0xffffffffu)
        total += sz;
    total += 12; // trailer
    s.buf.assign(static_cast<std::size_t>(total), std::byte{0});

    s.put32(0x00, static_cast<std::uint32_t>(total - 4));
    s.putName(0x04, logicalName);
    s.put32(0x10, static_cast<std::uint32_t>(total - 12));
    s.put32(0x14, count);
    std::uint64_t payloadAt = dirEnd;
    std::uint32_t i = 0;
    for (const auto& [n, f8, fc, f10, sz] : entries) {
      const std::uint64_t rec = 0x18 + std::uint64_t(i) * 24;
      s.putName(rec, n, 8);
      s.put32(rec + 0x08, f8);
      s.put32(rec + 0x0c, fc);
      s.put32(rec + 0x10, f10);
      if (f8 == 0xffffffffu) {
        s.put32(rec + 0x14, 0); // ignored field, observed 0
      } else {
        s.put32(rec + 0x14,
                static_cast<std::uint32_t>(payloadAt - 4)); // blob-rel
        payloadAt += sz;
      }
      ++i;
    }
    s.putName(static_cast<std::size_t>(total - 12), logicalName);
    return s;
  }
};

// Synthetic MTO builder (no original data). Layout mirrors the
// OBSERVED+CODE-CORROBORATED structure:
//   [u32 size-4][name12][u32 size-12][u32 count]
//   [count x 12B records {name[8], u32 blockFileOff}]
//   [overlay blocks, align4-chained] [name12 trailer]
// Block: {u32 len, u32 ofsA, u32 ofsB, u32 ofsC} then an embedded
// tagged ".MAT" file at +0x10, then region A {u32 size, struct
// {ca,cb,cc} + rec12[ca] + rec12[cb] + rec24[cc] + blobs}, region B
// span, region C {c1, rec10[c1], pad2|c1 odd, c2, rec44[c2], c3,
// rec36[c3], c4, rec12[c4], u32, extra}.
struct SyntheticMto {
  using InnerRec =
      std::tuple<const char*, std::uint32_t, std::uint32_t,
                 std::uint32_t, std::uint32_t>;  // name,f08,f0c,f10,sz
  struct BlockSpec {
    const char* entryName;
    const char* innerName;
    std::vector<InnerRec> innerRecs = {};
    std::uint32_t ca = 0, cb = 0, cc = 0;
    std::uint32_t c1 = 0, c2 = 0, c3 = 0, c4 = 0;
    std::uint32_t regionBSize = 0x150;   // OBSERVED constant
    std::uint32_t regionCExtra = 0;
  };

  std::vector<std::byte> buf;
  std::vector<std::size_t> blockOffs;  // file offset of each block
  std::vector<std::size_t> innerOffs;  // file offset of each .MAT image

  void put32(std::size_t off, std::uint32_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    buf[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    buf[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  }
  void put16(std::size_t off, std::uint16_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
  }
  void putName(std::size_t off, const char* s, std::size_t width = 12) {
    for (std::size_t i = 0; s[i] && i < width; ++i)
      buf[off + i] = static_cast<std::byte>(s[i]);
  }
  static std::uint64_t align4(std::uint64_t v) { return (v + 3) & ~3ull; }

  static SyntheticMto build(const char* logicalName,
                            std::initializer_list<BlockSpec> specs) {
    SyntheticMto s;
    const std::uint32_t count = static_cast<std::uint32_t>(specs.size());
    const std::uint64_t dirEnd = 0x18 + std::uint64_t(count) * 12;

    // Pass 1: compute total size (same arithmetic as pass 2).
    std::uint64_t pos = dirEnd;
    for (const auto& sp : specs) {
      const std::uint64_t innerSize = 0x24 +
          std::uint64_t(sp.innerRecs.size()) * 24 +
          [&] {
            std::uint64_t t = 0;
            for (const auto& ir : sp.innerRecs) t += std::get<4>(ir);
            return t;
          }() + 12;
      const std::uint64_t blobBase =
          12 + sp.ca * 12ull + sp.cb * 12ull + sp.cc * 24ull;
      const std::uint64_t sizeA =
          blobBase + (sp.ca + sp.cb + sp.cc) * 8ull;
      const std::uint64_t regionCSize =
          4 + sp.c1 * 10ull + (sp.c1 & 1 ? 2 : 0) + 4 + sp.c2 * 44ull +
          4 + sp.c3 * 36ull + 4 + sp.c4 * 12ull + 4 + sp.regionCExtra;
      pos = align4(pos);
      const std::uint64_t tA = pos + 0x10 + innerSize + 4;
      pos = align4(tA + sizeA) + sp.regionBSize + regionCSize;
    }
    const std::uint64_t total = pos + 12;
    s.buf.assign(static_cast<std::size_t>(total), std::byte{0});

    // Pass 2: envelope + table 1.
    s.put32(0x00, static_cast<std::uint32_t>(total - 4));
    s.putName(0x04, logicalName);
    s.put32(0x10, static_cast<std::uint32_t>(total - 12));
    s.put32(0x14, count);

    pos = dirEnd;
    std::uint32_t i = 0;
    for (const auto& sp : specs) {
      pos = align4(pos);
      const std::uint64_t off = pos;
      s.blockOffs.push_back(static_cast<std::size_t>(off));
      const std::uint64_t rec = 0x18 + std::uint64_t(i) * 12;
      s.putName(rec, sp.entryName, 8);
      s.put32(rec + 8, static_cast<std::uint32_t>(off));

      // Embedded ".MAT" file.
      const std::uint64_t inner = off + 0x10;
      s.innerOffs.push_back(static_cast<std::size_t>(inner));
      std::uint64_t payloadSum = 0;
      for (const auto& ir : sp.innerRecs) payloadSum += std::get<4>(ir);
      const std::uint64_t innerSize =
          0x24 + std::uint64_t(sp.innerRecs.size()) * 24 + payloadSum +
          12;
      const std::uint64_t innerEnd = inner + innerSize;
      s.put32(inner + 0x00, static_cast<std::uint32_t>(innerSize - 4));
      s.putName(inner + 0x04, sp.innerName);
      s.put32(inner + 0x10, static_cast<std::uint32_t>(innerSize - 12));
      s.put32(inner + 0x14,
              static_cast<std::uint32_t>(sp.innerRecs.size()));
      std::uint64_t pAt =
          inner + 0x18 + std::uint64_t(sp.innerRecs.size()) * 24;
      std::uint32_t j = 0;
      for (const auto& ir : sp.innerRecs) {
        const std::uint64_t r = inner + 0x18 + std::uint64_t(j) * 24;
        s.putName(r, std::get<0>(ir), 8);
        s.put32(r + 0x08, std::get<1>(ir));
        s.put32(r + 0x0c, std::get<2>(ir));
        s.put32(r + 0x10, std::get<3>(ir));
        if (std::get<1>(ir) == 0xffffffffu) {
          s.put32(r + 0x14, 0);
        } else {
          s.put32(r + 0x14,
                  static_cast<std::uint32_t>(pAt - (inner + 4)));
          pAt += std::get<4>(ir);
        }
        ++j;
      }
      s.putName(innerEnd - 12, sp.innerName);

      // Region A: u32 size at innerEnd, struct at innerEnd+4.
      const std::uint64_t tA = innerEnd + 4;
      s.put32(off + 0x04,
              static_cast<std::uint32_t>(tA - (off + 8)));
      const std::uint64_t blobBase =
          12 + sp.ca * 12ull + sp.cb * 12ull + sp.cc * 24ull;
      const std::uint64_t sizeA =
          blobBase + (sp.ca + sp.cb + sp.cc) * 8ull;
      s.put32(innerEnd, static_cast<std::uint32_t>(sizeA));
      s.put32(tA + 0x00, sp.ca);
      s.put32(tA + 0x04, sp.cb);
      s.put32(tA + 0x08, sp.cc);
      std::uint64_t blob = tA + blobBase;
      std::uint64_t q = tA + 12;
      char nm[9];
      for (std::uint32_t k = 0; k < sp.ca; ++k, q += 12, blob += 8) {
        std::snprintf(nm, sizeof(nm), "RA%u", k);
        s.putName(q, nm, 8);
        s.put32(q + 8, static_cast<std::uint32_t>(blob - tA));
        s.put32(blob, 1);  // count-prefixed blob shape
      }
      for (std::uint32_t k = 0; k < sp.cb; ++k, q += 12, blob += 8) {
        std::snprintf(nm, sizeof(nm), "RB%u", k);
        s.putName(q, nm, 8);
        s.put32(q + 8, static_cast<std::uint32_t>(blob - tA));
        s.put32(blob, 1);
      }
      for (std::uint32_t k = 0; k < sp.cc; ++k, q += 24, blob += 8) {
        s.put32(q + 0x10, static_cast<std::uint32_t>(blob - tA));
        s.put32(blob, 1);
      }

      // Region B (fixed-size span) then region C.
      const std::uint64_t tB = align4(tA + sizeA);
      s.put32(off + 0x08, static_cast<std::uint32_t>(tB - (off + 4)));
      const std::uint64_t tC = tB + sp.regionBSize;
      s.put32(off + 0x0c, static_cast<std::uint32_t>(tC - (off + 4)));
      q = tC;
      s.put32(q, sp.c1); q += 4;
      for (std::uint32_t k = 0; k < sp.c1; ++k, q += 10) {
        std::snprintf(nm, sizeof(nm), "RC%05u", k);
        s.putName(q, nm, 10);
      }
      if (sp.c1 & 1) q += 2;
      s.put32(q, sp.c2); q += 4 + sp.c2 * 44ull;
      s.put32(q, sp.c3); q += 4 + sp.c3 * 36ull;
      s.put32(q, sp.c4); q += 4 + sp.c4 * 12ull;
      s.put32(q, 0); q += 4;
      q += sp.regionCExtra;
      s.put32(off + 0x00, static_cast<std::uint32_t>(q - off));
      pos = q;
      ++i;
    }
    s.putName(static_cast<std::size_t>(total - 12), logicalName);
    return s;
  }
};

void test_mti_directory() {
  using mdk::MtiDirectoryStatus;
  using mdk::inspectMtiDirectory;

  // Valid mixed directory: plain-header payload, extended-header
  // payload, index record — all three proven record forms.
  {
    auto s = SyntheticMti::build("TEST.MTI",
        {{"MAT_A", 0x00000000, 0, 0x40600000, 24},
         {"MAT_B", 0x00010001, 0, 0x40c00000, 20},
         {"IDX_C", 0xffffffff, 7, 0, 0}});
    // dirEnd = 0x18 + 3*24 = 0x60; payloads [0x60,0x78) and
    // [0x78,0x8c); trailer at size-12.
    // MAT_A plain header: u16 @0x60=64, u16 @0x62=32.
    s.put16(0x60, 64);
    s.put16(0x62, 32);
    // MAT_B extended header: u16 @0x78=2, u16 @0x7c=128, u16 @0x7e=128.
    s.put16(0x78, 2);
    s.put16(0x7c, 128);
    s.put16(0x7e, 128);
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kOk);
    CHECK(d.count == 3 && d.entries.size() == 3);
    CHECK(d.directoryEnd == 0x60);
    CHECK(d.trailerPresent && d.secondaryEqualsTrailerOffset);

    CHECK(d.entries[0].name() == "MAT_A");
    CHECK(!d.entries[0].isIndexRecord());
    CHECK(!d.entries[0].hasExtendedHeader());
    CHECK(d.entries[0].fieldAt0x08 == 0x00000000u);
    CHECK(d.entries[0].fieldAt0x10 == 0x40600000u);
    CHECK(d.entries[0].fieldAt0x14 == 0x60 - 4);
    CHECK(d.entries[0].payloadFileOffset() == 0x60);
    CHECK(!d.entries[0].headerCount.has_value());
    CHECK(d.entries[0].headerFieldA == 64 && d.entries[0].headerFieldB == 32);
    CHECK(d.entries[0].payloadDataFileOffset == 0x64);

    CHECK(d.entries[1].name() == "MAT_B");
    CHECK(d.entries[1].hasExtendedHeader());
    CHECK(d.entries[1].payloadFileOffset() == 0x78);
    CHECK(d.entries[1].headerCount == 2);
    CHECK(d.entries[1].headerFieldA == 128 && d.entries[1].headerFieldB == 128);
    CHECK(d.entries[1].payloadDataFileOffset == 0x80);

    CHECK(d.entries[2].name() == "IDX_C");
    CHECK(d.entries[2].isIndexRecord());
    CHECK(d.entries[2].fieldAt0x0C == 7);
  }

  // Zero-record directory: count=0 is legal (the original branches on
  // count==0 and produces an empty table).
  {
    auto s = SyntheticMti::build("EMPTY.MTI", {});
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kOk);
    CHECK(d.count == 0 && d.entries.empty());
    CHECK(d.directoryEnd == 0x18);
  }

  // Name at exactly 8 bytes (fills the field, no NUL inside it).
  {
    auto s = SyntheticMti::build("TEST.MTI",
        {{"EXPLODEX", 0, 0, 0x40600000, 8}});
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kOk);
    CHECK(d.entries[0].name() == "EXPLODEX");
    CHECK(d.entries[0].nameField[7] == std::byte{'X'});
  }

  // Index record with nonzero +0x10/+0x14: the original ignores those
  // fields for this class — preserved raw, never validated.
  {
    auto s = SyntheticMti::build("TEST.MTI",
        {{"IDX", 0xffffffff, 0x1234, 0, 0}});
    s.put32(0x18 + 0x10, 0xdeadbeef);
    s.put32(0x18 + 0x14, 0xcafef00d); // would be out-of-bounds if read
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kOk);
    CHECK(d.entries[0].isIndexRecord());
    CHECK(d.entries[0].fieldAt0x10 == 0xdeadbeefu);
    CHECK(d.entries[0].fieldAt0x14 == 0xcafef00du);
  }

  // Payload record pointing before the directory end → rejected.
  {
    auto s = SyntheticMti::build("TEST.MTI", {{"A", 0, 0, 0, 8}});
    s.put32(0x18 + 0x14, 0); // blobOff=0 → file offset 4 < dirEnd
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kEntryOutOfBounds);
    CHECK(d.badEntryIndex == 0);
  }

  // Payload header escaping the payload region (offset lands on the
  // trailer itself) → rejected.
  {
    auto s = SyntheticMti::build("TEST.MTI", {{"A", 0, 0, 0, 8}});
    s.put32(0x18 + 0x14,
            static_cast<std::uint32_t>(s.buf.size() - 12 - 4)); // → file size-12
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kEntryOutOfBounds);
    CHECK(d.badEntryIndex == 0);
  }

  // Extended-header payload needs 8 bytes before the trailer: a 6-byte
  // tail is too small for the variant the flags select.
  {
    auto s = SyntheticMti::build("TEST.MTI",
        {{"A", 0x00010000, 0, 0, 8}, {"B", 0, 0, 0, 6}});
    // dirEnd = 0x48; A payload [0x48,0x50) ok (8B ext hdr); B payload
    // [0x50,0x56), trailer at 0x56 — B needs a 4B header, exactly fits.
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kOk);
    // Same layout but B flagged extended: needs 8B in [0x50,0x56)=6 →
    // entry bounds fail on B.
    auto s2 = SyntheticMti::build("TEST.MTI",
        {{"A", 0x00010000, 0, 0, 8}, {"B", 0x00010000, 0, 0, 6}});
    const auto d2 = inspectMtiDirectory(s2.buf);
    CHECK(d2.status == MtiDirectoryStatus::kEntryOutOfBounds);
    CHECK(d2.badEntryIndex == 1);
  }

  // Impossible count → directory bound fails.
  {
    auto s = SyntheticMti::build("TEST.MTI", {{"A", 0, 0, 0, 8}});
    s.put32(0x14, 0xffffffff);
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kDirectoryOutOfBounds);
  }

  // Inflated count that still "fits" physically but pushes dirEnd past
  // the real payloads → entry bounds catch the invalidated record.
  {
    auto s = SyntheticMti::build("TEST.MTI",
        {{"A", 0, 0, 0, 40}, {"B", 0, 0, 0, 40}});
    s.put32(0x14, 3); // dirEnd 0x60 > A payload start 0x48
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kEntryOutOfBounds);
    CHECK(d.badEntryIndex == 0);
  }

  // Truncated header: envelope fine but file ends before count.
  {
    std::byte t[20] = {};
    t[0] = std::byte{16};
    std::memcpy(t + 4, "T.MTI", 5);
    const auto d = inspectMtiDirectory(t);
    CHECK(d.status == MtiDirectoryStatus::kTruncatedHeader);
  }

  // Not a tagged envelope → "not this format", not "malformed".
  {
    std::byte raw[64] = {};
    const auto d = inspectMtiDirectory(raw);
    CHECK(d.status == MtiDirectoryStatus::kNotTaggedEnvelope);

    std::byte fti[32] = {};
    fti[0] = std::byte{28};
    fti[4] = std::byte{0x03};
    const auto d2 = inspectMtiDirectory(fti);
    CHECK(d2.status == MtiDirectoryStatus::kNotTaggedEnvelope);
  }

  // Missing trailer → payload region extends to EOF; still parses.
  {
    auto s = SyntheticMti::build("TEST.MTI", {{"A", 0, 0, 0, 8}});
    for (std::size_t i = s.buf.size() - 12; i < s.buf.size(); ++i)
      s.buf[i] = std::byte{'X'};
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kOk);
    CHECK(!d.trailerPresent);
    CHECK(d.secondaryEqualsTrailerOffset);
  }

  // Unknown +0x08 values are preserved raw and treated as payload
  // records (any value other than 0xffffffff follows that path in the
  // original); a lone 0x00000002 flag is observed in BUILD_A.
  {
    auto s = SyntheticMti::build("TEST.MTI",
        {{"A", 0x00000002, 0, 0x40600000, 8}});
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kOk);
    CHECK(d.entries[0].fieldAt0x08 == 0x00000002u);
    CHECK(!d.entries[0].isIndexRecord());
    CHECK(!d.entries[0].hasExtendedHeader());
  }

  // u32@0 length mismatch → envelope invalid → not this format.
  {
    auto s = SyntheticMti::build("TEST.MTI", {{"A", 0, 0, 0, 8}});
    s.put32(0x00, 0);
    const auto d = inspectMtiDirectory(s.buf);
    CHECK(d.status == MtiDirectoryStatus::kNotTaggedEnvelope);
  }
}

void test_mto_directory() {
  using mdk::MtoDirectoryStatus;
  using mdk::inspectMtoDirectory;

  // Valid two-block file: one populated block plus the corpus's
  // "minimal" form (inner count 0, region A size 0xc, tiny region C).
  {
    auto s = SyntheticMto::build("TEST.MTO", {
        {"OV1", "OV1.MAT",
         {{"TEX_A", 0x00000000, 0, 0x40600000, 16},
          {"TEX_B", 0x00020000, 0, 0, 8}},
         2, 1, 1, 3, 2, 1, 2, 0x150, 8},
        {"OV2", "OV2.MAT", {}, 0, 0, 0, 1, 1, 2, 4, 0x150, 0}});
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kOk);
    CHECK(d.count == 2 && d.entries.size() == 2 && d.blocks.size() == 2);
    CHECK(d.directoryEnd == 0x30);
    CHECK(d.trailerPresent && d.secondaryEqualsTrailerOffset);
    CHECK(d.entries[0].name() == "OV1");
    CHECK(d.entries[0].blockFileOffset == s.blockOffs[0]);
    CHECK(d.entries[1].name() == "OV2");
    CHECK(d.entries[1].blockFileOffset == s.blockOffs[1]);

    const auto& b0 = d.blocks[0];
    CHECK(b0.innerName() == "OV1.MAT");
    CHECK(b0.innerCount == 2 && b0.innerRecords.size() == 2);
    CHECK(b0.innerRecords[0].name() == "TEX_A");
    CHECK(!b0.innerRecords[0].isIndexRecord());
    CHECK(!b0.innerRecords[0].hasExtendedHeader());
    CHECK(b0.innerRecords[0].fieldAt0x10 == 0x40600000u);
    CHECK(b0.innerRecords[1].hasExtendedHeader());  // flags & 0x30000
    CHECK(b0.innerTrailerPresent &&
          b0.innerSecondaryEqualsTrailerOffset);
    CHECK(b0.regionACountA == 2 && b0.regionACountB == 1 &&
          b0.regionACountC == 1);
    CHECK(b0.regionAArrayA.size() == 2);
    CHECK(b0.regionAArrayA[0].name() == "RA0");
    CHECK(b0.regionAArrayA[0].fieldAt0x08 >= 12);
    CHECK(b0.regionAArrayB.size() == 1 &&
          b0.regionAArrayB[0].name() == "RB0");
    CHECK(b0.regionAArrayC.size() == 1);
    CHECK(b0.regionAArrayC[0].fieldAt0x10 >= 12);
    CHECK(b0.regionBSize == 0x150);
    CHECK(b0.regionCCount1 == 3 && b0.regionCNames.size() == 3);
    CHECK(b0.regionCNames[0].name() == "RC00000");
    CHECK(b0.regionCCount2 == 2 && b0.regionCCount3 == 1 &&
          b0.regionCCount4 == 2);
    // c1=3 is odd → the 2-byte pad path was exercised by the builder.

    const auto& b1 = d.blocks[1];
    CHECK(b1.innerCount == 0 && b1.innerRecords.empty());
    CHECK(b1.regionASize == 0x0c);
    CHECK(b1.regionACountA == 0 && b1.regionACountB == 0 &&
          b1.regionACountC == 0);
    CHECK(b1.regionAArrayA.empty() && b1.regionAArrayB.empty() &&
          b1.regionAArrayC.empty());
    CHECK(b1.regionCCount1 == 1 && b1.regionCCount4 == 4);
  }

  // Zero-entry directory: count=0 is legal.
  {
    auto s = SyntheticMto::build("EMPTY.MTO", {});
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kOk);
    CHECK(d.count == 0 && d.entries.empty() && d.blocks.empty());
    CHECK(d.directoryEnd == 0x18);
  }

  // Full-width 8-byte entry name (fills the field, no NUL inside).
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"ABCDEFGH", "FULLNAME.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kOk);
    CHECK(d.entries[0].name() == "ABCDEFGH");
    CHECK(d.entries[0].nameField[7] == std::byte{'H'});
  }

  // Block offset before the directory end → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    s.put32(0x18 + 8, 0x14);  // inside the directory itself
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kBlockOutOfBounds);
    CHECK(d.badEntryIndex == 0);
  }

  // Block offset past the file end → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    s.put32(0x18 + 8, static_cast<std::uint32_t>(s.buf.size() - 8));
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kBlockOutOfBounds);
  }

  // Block length escaping the trailer bound → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    const std::size_t off = s.blockOffs[0];
    s.put32(off, static_cast<std::uint32_t>(s.buf.size() - off + 8));
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kBlockOutOfBounds);
  }

  // Embedded ".MAT" size escaping the block → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    const std::size_t inner = s.innerOffs[0];
    s.put32(inner, 0x40000000);
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kInteriorMalformed);
    CHECK(d.badEntryIndex == 0);
  }

  // Embedded record count running past the inner trailer → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {{"T", 0, 0, 0, 8}}, 0, 0, 0, 1, 0, 0, 0}});
    const std::size_t inner = s.innerOffs[0];
    s.put32(inner + 0x14, 9);  // 9 records, only 1 fits
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kInteriorMalformed);
  }

  // Inner payload offset pointing into the inner directory → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {{"T", 0, 0, 0, 8}}, 0, 0, 0, 1, 0, 0, 0}});
    const std::size_t inner = s.innerOffs[0];
    s.put32(inner + 0x18 + 0x14, 0);  // img+0 = inside inner dir
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kInteriorMalformed);
  }

  // Inner index record (flags=0xffffffff): no such record is OBSERVED
  // in BUILD_A MTOs, but the shared MTI mechanism supports the class —
  // preserved raw, its +0x14 never bounds-checked.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {{"IDX", 0xffffffff, 7, 0, 0}}, 0, 0, 0,
          1, 0, 0, 0}});
    const std::size_t inner = s.innerOffs[0];
    s.put32(inner + 0x18 + 0x14, 0xdeadbeef);
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kOk);
    CHECK(d.blocks[0].innerRecords[0].isIndexRecord());
    CHECK(d.blocks[0].innerRecords[0].fieldAt0x0C == 7);
    CHECK(d.blocks[0].innerRecords[0].fieldAt0x14 == 0xdeadbeefu);
  }

  // Region A arrays escaping the declared size → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    // Inflate ca past what regionASize covers.
    const auto d0 = inspectMtoDirectory(s.buf);
    CHECK(d0.status == MtoDirectoryStatus::kOk);
    s.put32(d0.blocks[0].regionAOffset, 0x100);
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kInteriorMalformed);
  }

  // Region A record offset pointing before the arrays or past the
  // declared region → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 1, 0, 0, 1, 0, 0, 0}});
    const auto d0 = inspectMtoDirectory(s.buf);
    CHECK(d0.status == MtoDirectoryStatus::kOk);
    // recA[0] sits at regionAOffset+12; its +8 field is the tA-rel off.
    s.put32(d0.blocks[0].regionAOffset + 12 + 8, 4);  // < 12
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kInteriorMalformed);

    auto s2 = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 1, 0, 0, 1, 0, 0, 0}});
    const auto e0 = inspectMtoDirectory(s2.buf);
    s2.put32(e0.blocks[0].regionAOffset + 12 + 8,
             e0.blocks[0].regionASize);  // == sizeA: outside region
    const auto d2 = inspectMtoDirectory(s2.buf);
    CHECK(d2.status == MtoDirectoryStatus::kInteriorMalformed);
  }

  // Region C counts escaping the block → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    const auto d0 = inspectMtoDirectory(s.buf);
    CHECK(d0.status == MtoDirectoryStatus::kOk);
    // c1=1 → rec10[1] + 2-byte pad; c2 field sits at tC+16.
    s.put32(d0.blocks[0].regionCOffset + 16, 0x10000);
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kInteriorMalformed);
  }

  // Region targets out of order (ofsB > ofsC) → rejected.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    const auto d0 = inspectMtoDirectory(s.buf);
    CHECK(d0.status == MtoDirectoryStatus::kOk);
    const std::size_t off = s.blockOffs[0];
    s.put32(off + 0x08,
            static_cast<std::uint32_t>(d0.blocks[0].fieldAt0x0C + 4));
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kInteriorMalformed);
  }

  // Inflated outer count → directory bound fails (division-first math;
  // count*stride never overflows).
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    s.put32(0x14, 0xffffffff);
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kDirectoryOutOfBounds);
  }

  // Truncated header: envelope fine but file ends before count.
  {
    std::byte t[20] = {};
    t[0] = std::byte{16};
    std::memcpy(t + 4, "T.MTO", 5);
    const auto d = inspectMtoDirectory(t);
    CHECK(d.status == MtoDirectoryStatus::kTruncatedHeader);
  }

  // Not a tagged envelope → "not this format", not "malformed".
  {
    std::byte raw[64] = {};
    const auto d = inspectMtoDirectory(raw);
    CHECK(d.status == MtoDirectoryStatus::kNotTaggedEnvelope);

    std::byte fti[32] = {};
    fti[0] = std::byte{28};
    fti[4] = std::byte{0x03};
    const auto d2 = inspectMtoDirectory(fti);
    CHECK(d2.status == MtoDirectoryStatus::kNotTaggedEnvelope);
  }

  // Missing trailer → still parses; trailerPresent reports the fact.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    for (std::size_t i = s.buf.size() - 12; i < s.buf.size(); ++i)
      s.buf[i] = std::byte{'X'};
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kOk);
    CHECK(!d.trailerPresent);
  }

  // Slack before the trailer: OBSERVED block lengths can leave an
  // align4 gap — extend the file past the last block end. The old
  // trailer bytes stay in place and become ordinary slack; the
  // envelope fields move to the new size.
  {
    auto s = SyntheticMto::build("TEST.MTO",
        {{"A", "A.MAT", {}, 0, 0, 0, 1, 0, 0, 0}});
    const std::size_t oldSize = s.buf.size();
    s.buf.resize(oldSize + 12);
    s.put32(0x00, static_cast<std::uint32_t>(s.buf.size() - 4));
    s.put32(0x10, static_cast<std::uint32_t>(s.buf.size() - 12));
    s.putName(oldSize, "TEST.MTO");  // trailer at new size-12
    const auto d = inspectMtoDirectory(s.buf);
    CHECK(d.status == MtoDirectoryStatus::kOk);
    CHECK(d.trailerPresent);
  }
}

// Synthetic CMI file builder (no original data). Layout mirrors the
// CODE-CORROBORATED structure: [u32 size-4][name12][u32 size-12]
// [table x N: u32 count, then count x {u8 len, name[len], u32 value}]
// [data region bytes] [name12 trailer]. Stored name length INCLUDES
// the NUL terminator (the OBSERVED convention); a nullptr name builds
// a len-0 record (u8 0 + u32 value only).
struct SyntheticCmi {
  std::vector<std::byte> buf;
  std::vector<std::uint64_t> tableEnds;      // per given table
  std::vector<std::uint64_t> recStarts;      // flat, table-major
  std::vector<std::uint64_t> recValueOffs;   // u32 field offsets
  std::uint64_t dataStart = 0;

  void put32(std::size_t off, std::uint32_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    buf[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    buf[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  }
  void putName(std::size_t off, const char* s, std::size_t width = 12) {
    for (std::size_t i = 0; s[i] && i < width; ++i)
      buf[off + i] = static_cast<std::byte>(s[i]);
  }

  using Rec = std::tuple<const char*, std::uint32_t>;  // name, raw u32
  using Table = std::initializer_list<Rec>;

  static std::uint64_t tablesEnd(
      std::initializer_list<Table> tables) {
    std::uint64_t pos = 0x14;
    for (const auto& t : tables) {
      pos += 4;
      for (const auto& [n, v] : t)
        pos += (n ? std::strlen(n) + 1 : 0) + 5;
    }
    return pos;
  }

  static SyntheticCmi build(const char* logicalName,
                            std::initializer_list<Table> tables,
                            std::uint32_t dataBytes = 16) {
    SyntheticCmi s;
    const std::uint64_t dStart = tablesEnd(tables);
    const std::uint64_t total = dStart + dataBytes + 12;
    s.dataStart = dStart;
    s.buf.assign(static_cast<std::size_t>(total), std::byte{0});

    s.put32(0x00, static_cast<std::uint32_t>(total - 4));
    s.putName(0x04, logicalName);
    s.put32(0x10, static_cast<std::uint32_t>(total - 12));

    std::uint64_t pos = 0x14;
    for (const auto& t : tables) {
      s.put32(static_cast<std::size_t>(pos),
              static_cast<std::uint32_t>(t.size()));
      pos += 4;
      for (const auto& [n, v] : t) {
        const std::size_t len = n ? std::strlen(n) + 1 : 0;
        s.recStarts.push_back(pos);
        s.recValueOffs.push_back(pos + 1 + len);
        s.buf[pos] = static_cast<std::byte>(len);
        for (std::size_t i = 0; i < len; ++i)
          s.buf[pos + 1 + i] = static_cast<std::byte>(n[i]);
        s.put32(pos + 1 + len, v);
        pos += len + 5;
      }
      s.tableEnds.push_back(pos);
    }
    s.putName(static_cast<std::size_t>(total - 12), logicalName);
    return s;
  }
};

void test_cmi_directory() {
  using mdk::CmiDirectoryStatus;
  using mdk::inspectCmiDirectory;

  // Valid file: all four tables populated; every nonzero value points
  // into the data region (the OBSERVED corpus property).
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{{"OBJ$ANIM0", 0}, {"OBJ$ANIM1", 0}},
         {{"ALPHA", 0}, {"BETA", 0}},
         {{"LONGERNAME", 0}},
         {{"X", 0}, {"YY", 0}, {"ZZZ", 0}}},
        32);
    const std::uint32_t imgOff =
        static_cast<std::uint32_t>(s.dataStart - 4);  // -> file dataStart
    for (std::size_t i = 0; i < s.recValueOffs.size(); ++i)
      s.put32(static_cast<std::size_t>(s.recValueOffs[i]), imgOff);
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kOk);
    CHECK(d.tables.size() == 4);
    CHECK(d.tables[0].count == 2 && d.tables[1].count == 2 &&
          d.tables[2].count == 1 && d.tables[3].count == 3);
    CHECK(d.tables[0].countFileOffset == 0x14);
    CHECK(d.tables[0].recordsFileOffset == 0x18);
    CHECK(d.tables[0].endFileOffset == s.tableEnds[0]);
    CHECK(d.dataRegionOffset == s.dataStart);
    CHECK(d.dataRegionEnd == s.buf.size() - 12);
    CHECK(d.trailerPresent && d.secondaryEqualsTrailerOffset);

    const auto& r0 = d.tables[0].records[0];
    CHECK(r0.name() == "OBJ$ANIM0");
    CHECK(r0.nameLength == 10);          // strlen + NUL
    CHECK(r0.nameBytes.size() == 10);
    CHECK(r0.nameBytes.back() == std::byte{0});
    CHECK(r0.nameEndsWithTerminator);
    CHECK(r0.fileOffset == s.recStarts[0]);
    CHECK(r0.value == imgOff);
    CHECK(r0.valueFileOffset() ==
          std::optional<std::uint64_t>(s.dataStart));
    CHECK(r0.valueReachesDataRegion);
  }

  // Zero-count first table (OBSERVED in LEVEL4/5/7): the next count
  // immediately follows at 0x18.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{}, {{"E1", 0}}, {{"N1", 0}, {"N2", 0}}, {{"T", 0}}}, 8);
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kOk);
    CHECK(d.tables[0].count == 0 && d.tables[0].records.empty());
    CHECK(d.tables[0].endFileOffset == 0x18);
    CHECK(d.tables[1].countFileOffset == 0x18);
    CHECK(d.tables[1].records[0].name() == "E1");
    CHECK(d.dataRegionOffset == s.dataStart);
  }

  // All four tables empty: minimum valid interior, data region starts
  // right after the fourth count field (0x14 + 4*4 = 0x24).
  {
    auto s = SyntheticCmi::build("EMPTY.CMD", {{}, {}, {}, {}}, 8);
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kOk);
    CHECK(d.tables.size() == 4);
    CHECK(d.dataRegionOffset == 0x24);
  }

  // Null record value (the table-1 consumer's tested null form):
  // preserved raw; valueFileOffset() reports no target.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{}, {{"NULLABLE", 0}}, {}, {}}, 8);
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kOk);
    CHECK(d.tables[1].records[0].value == 0);
    CHECK(!d.tables[1].records[0].valueFileOffset().has_value());
    CHECK(!d.tables[1].records[0].valueReachesDataRegion);
  }

  // Value pointing into the table area instead of the data region:
  // in-bounds for the original's unconditional dereference, so the
  // file still parses; valueReachesDataRegion reports the deviation.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{}, {{"A", 0x10}} , {}, {}}, 8);   // -> file 0x14
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kOk);
    CHECK(d.tables[1].records[0].valueFileOffset() ==
          std::optional<std::uint64_t>(0x14));
    CHECK(!d.tables[1].records[0].valueReachesDataRegion);
  }

  // Record value escaping the file entirely → rejected (native
  // hardening; the original dereferences unconditionally).
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{}, {{"A", 0}}, {}, {}}, 8);
    s.put32(static_cast<std::size_t>(s.recValueOffs[0]),
            static_cast<std::uint32_t>(s.buf.size()));  // -> past EOF
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kValueOutOfBounds);
    CHECK(d.badTableIndex == 1 && d.badRecordIndex == 0);
  }

  // 0xffffffff is not a file-format sentinel here: it is an
  // image-relative offset far past EOF → rejected like any other
  // out-of-range value.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{}, {{"A", 0xffffffff}}, {}, {}}, 8);
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kValueOutOfBounds);
  }

  // len-0 record: legal stride-5 record with an empty name.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{}, {{nullptr, 0}, {"B", 0}}, {}, {}}, 8);
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kOk);
    CHECK(d.tables[1].count == 2);
    CHECK(d.tables[1].records[0].nameLength == 0);
    CHECK(d.tables[1].records[0].nameBytes.empty());
    CHECK(d.tables[1].records[0].name().empty());
    CHECK(d.tables[1].records[1].name() == "B");
    CHECK(d.tables[1].records[1].fileOffset == s.recStarts[1]);
  }

  // Name without an internal NUL: stored length does not include a
  // terminator; structurally walkable, flag reports the deviation.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{}, {{"ABC", 0}}, {}, {}}, 8);
    // "ABC\0" stored as len 4; overwrite the NUL so no terminator is
    // counted. Record still parses (the original would over-read, our
    // walk is length-driven).
    s.buf[s.recStarts[0] + 1 + 3] = std::byte{'!'};
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kOk);
    CHECK(!d.tables[1].records[0].nameEndsWithTerminator);
    CHECK(d.tables[1].records[0].name() == "ABC!");
  }

  // Non-printable byte inside a name: preserved raw, escaped for
  // display.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{}, {{"A\x80", 0}}, {}, {}}, 8);
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kOk);
    CHECK(d.tables[1].records[0].nameBytes[1] == std::byte{0x80});
    CHECK(d.tables[1].records[0].name() == "A\\x80");
  }

  // Inflated first count → records escape the interior region.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{{"A", 0}}, {}, {}, {}}, 8);
    s.put32(0x14, 0xffffffff);
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kTableOutOfBounds);
    CHECK(d.badTableIndex == 0);
  }

  // Inflated mid-chain count: the walk consumes the following tables'
  // bytes as "records"; the first such record's value field reads as
  // an out-of-file offset → value bound fires before the walk escapes.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{{"A", 0}}, {{"B", 0}}, {{"C", 0}}, {{"D", 0}}}, 8);
    s.put32(static_cast<std::size_t>(s.tableEnds[0]), 0x4000); // T1 count
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kValueOutOfBounds);
    CHECK(d.badTableIndex == 1);
  }

  // Inflated last-table count: zeroed data bytes walk as len-0/value-0
  // records until the record stride escapes the trailer bound.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{{"A", 0}}, {{"B", 0}}, {{"C", 0}}, {{"D", 0}}}, 8);
    s.put32(static_cast<std::size_t>(s.tableEnds[2]), 0x4000); // T3 count
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kTableOutOfBounds);
    CHECK(d.badTableIndex == 3);
  }

  // Inflated length byte: the record's own len+5 stride escapes.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{}, {{"A", 0}}, {}, {}}, 8);
    s.buf[s.recStarts[0]] = std::byte{0xff};
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kTableOutOfBounds);
    CHECK(d.badTableIndex == 1 && d.badRecordIndex == 0);
  }

  // File that physically ends after three tables: the fourth count
  // field lands on/past the trailer → table bound fails.
  {
    auto s = SyntheticCmi::build("TEST.CMD",
        {{{"A", 0}}, {}, {}}, 0);  // 3 tables, then trailer
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kTableOutOfBounds);
    CHECK(d.badTableIndex == 3);
    CHECK(d.badRecordIndex == static_cast<std::size_t>(-1));
  }

  // Truncated header: envelope fine but file ends before the count.
  {
    std::byte t[20] = {};
    t[0] = std::byte{16};
    std::memcpy(t + 4, "T.CMD", 5);
    const auto d = inspectCmiDirectory(t);
    CHECK(d.status == CmiDirectoryStatus::kTruncatedHeader);
  }

  // Not a tagged envelope → "not this format", not "malformed".
  {
    std::byte raw[64] = {};
    const auto d = inspectCmiDirectory(raw);
    CHECK(d.status == CmiDirectoryStatus::kNotTaggedEnvelope);

    std::byte fti[32] = {};
    fti[0] = std::byte{28};
    fti[4] = std::byte{0x03};
    const auto d2 = inspectCmiDirectory(fti);
    CHECK(d2.status == CmiDirectoryStatus::kNotTaggedEnvelope);
  }

  // u32@0 length mismatch → envelope invalid → not this format.
  {
    auto s = SyntheticCmi::build("TEST.CMD", {{}, {}, {}, {}}, 8);
    s.put32(0x00, 0);
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kNotTaggedEnvelope);
  }

  // Missing trailer → the data region extends to EOF; still parses.
  {
    auto s = SyntheticCmi::build("TEST.CMD", {{}, {}, {}, {}}, 8);
    for (std::size_t i = s.buf.size() - 12; i < s.buf.size(); ++i)
      s.buf[i] = std::byte{'X'};
    const auto d = inspectCmiDirectory(s.buf);
    CHECK(d.status == CmiDirectoryStatus::kOk);
    CHECK(!d.trailerPresent);
    CHECK(d.dataRegionEnd == s.buf.size());
    CHECK(d.secondaryEqualsTrailerOffset);
  }
}

// Synthetic DTI builder (no original data). Layout mirrors the proven
// structure: tagged envelope + five-entry image-relative TOC @0x14 +
// s0 params (29 u32s) + s1 keyed records + s2 arena table with tiled
// payloads + s3 palette + s4 grid + name trailer.
struct SyntheticDti {
  std::vector<std::byte> buf;
  std::uint64_t s0File = 0, s1File = 0, s2File = 0, s3File = 0,
                s4File = 0, trailerFile = 0;
  std::vector<std::uint64_t> arenaRecFileOffs;
  std::vector<std::uint64_t> arenaPayloadFileOffs;
  std::vector<std::uint64_t> keyedFileOffs;

  void put32(std::size_t off, std::uint32_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    buf[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    buf[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  }
  void putName(std::size_t off, const char* s, std::size_t width = 12) {
    for (std::size_t i = 0; s[i] && i < width; ++i)
      buf[off + i] = static_cast<std::byte>(s[i]);
  }
  static std::uint32_t f32bits(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
  }

  struct Sub {                       // one 36-byte payload record
    std::uint32_t type;
    std::array<std::uint32_t, 8> fields;
  };
  struct Arena {                     // one s2 record
    const char* name;                // <=8 chars, NUL-padded
    float scalar;
    std::vector<Sub> subs;
  };

  // tocOrder: section file order is always s0,s1,s2,s3,s4 — the TOC
  // stores image offsets; `gridRows`/`gridCols`/`altFillA` drive s0's
  // proven fields and the s4 plane size.
  static SyntheticDti build(
      const char* logicalName,
      std::vector<std::pair<std::uint32_t, std::uint32_t>>
          keyed,  // (word0, key) — floats zeroed
      std::vector<Arena> arenas,
      std::uint32_t paletteCount, std::uint32_t gridCols,
      std::uint32_t gridRows, std::uint32_t altFillA,
      bool dualPlane) {
    SyntheticDti s;
    // Section sizes (file bytes):
    const std::uint64_t s0Bytes = 0x74;
    const std::uint64_t s1Bytes = 4 + keyed.size() * 24;
    std::uint64_t s2Bytes = 4 + arenas.size() * 16;
    for (const auto& a : arenas)
      s2Bytes += 4 + a.subs.size() * 36;
    const std::uint64_t s3Bytes = 4 + 768;
    const std::uint64_t planeBytes =
        (static_cast<std::uint64_t>(gridCols) + 4) * gridRows;
    const std::uint64_t s4Bytes = planeBytes * (dualPlane ? 2 : 1);

    s.s0File = 0x28;
    s.s1File = s.s0File + s0Bytes;
    s.s2File = s.s1File + s1Bytes;
    s.s3File = s.s2File + s2Bytes;
    s.s4File = s.s3File + s3Bytes;
    s.trailerFile = s.s4File + s4Bytes;
    const std::uint64_t total = s.trailerFile + 12;
    s.buf.assign(static_cast<std::size_t>(total), std::byte{0});

    s.put32(0x00, static_cast<std::uint32_t>(total - 4));
    s.putName(0x04, logicalName);
    s.put32(0x10, static_cast<std::uint32_t>(total - 12));
    // TOC image offsets = file - 4.
    s.put32(0x14, static_cast<std::uint32_t>(s.s0File - 4));
    s.put32(0x18, static_cast<std::uint32_t>(s.s1File - 4));
    s.put32(0x1c, static_cast<std::uint32_t>(s.s2File - 4));
    s.put32(0x20, static_cast<std::uint32_t>(s.s3File - 4));
    s.put32(0x24, static_cast<std::uint32_t>(s.s4File - 4));

    // s0: only proven-read fields populated.
    s.put32(static_cast<std::size_t>(s.s0File + 9 * 4), gridCols);
    s.put32(static_cast<std::size_t>(s.s0File + 10 * 4), gridRows);
    s.put32(static_cast<std::size_t>(s.s0File + 0x0b * 4), altFillA);

    // s1 records: {word0, key, f32 x4 = 0}
    s.put32(static_cast<std::size_t>(s.s1File),
            static_cast<std::uint32_t>(keyed.size()));
    for (std::size_t i = 0; i < keyed.size(); ++i) {
      const std::uint64_t rp = s.s1File + 4 + i * 24;
      s.keyedFileOffs.push_back(rp);
      s.put32(static_cast<std::size_t>(rp), keyed[i].first);
      s.put32(static_cast<std::size_t>(rp + 4), keyed[i].second);
    }

    // s2: count + 16-byte records + tiled payloads.
    s.put32(static_cast<std::size_t>(s.s2File),
            static_cast<std::uint32_t>(arenas.size()));
    std::uint64_t pay = s.s2File + 4 + arenas.size() * 16;
    for (std::size_t i = 0; i < arenas.size(); ++i) {
      const std::uint64_t rp = s.s2File + 4 + i * 16;
      s.arenaRecFileOffs.push_back(rp);
      s.putName(static_cast<std::size_t>(rp), arenas[i].name, 8);
      s.put32(static_cast<std::size_t>(rp + 8),
              static_cast<std::uint32_t>(pay - 4));
      s.put32(static_cast<std::size_t>(rp + 12),
              f32bits(arenas[i].scalar));
      s.arenaPayloadFileOffs.push_back(pay);
      s.put32(static_cast<std::size_t>(pay),
              static_cast<std::uint32_t>(arenas[i].subs.size()));
      for (std::size_t j = 0; j < arenas[i].subs.size(); ++j) {
        const std::uint64_t sp = pay + 4 + j * 36;
        s.put32(static_cast<std::size_t>(sp), arenas[i].subs[j].type);
        for (std::size_t f = 0; f < 8; ++f)
          s.put32(static_cast<std::size_t>(sp + 4 + f * 4),
                  arenas[i].subs[j].fields[f]);
      }
      pay += 4 + arenas[i].subs.size() * 36;
    }

    s.put32(static_cast<std::size_t>(s.s3File), paletteCount);
    s.putName(static_cast<std::size_t>(s.trailerFile), logicalName);
    return s;
  }
};

void test_dti_structure() {
  using mdk::DtiStructureStatus;
  using mdk::inspectDtiStructure;

  // Valid file: all sections populated; arena payload tiles inside s2.
  {
    auto s = SyntheticDti::build(
        "TEST.DAT",
        {{1, 0}, {2, 1}},
        {{"ARENA_1", 4.0f,
          {{6, {1000, 1, SyntheticDti::f32bits(1.0f),
                SyntheticDti::f32bits(2.0f), SyntheticDti::f32bits(3.0f),
                SyntheticDti::f32bits(4.0f), SyntheticDti::f32bits(5.0f),
                SyntheticDti::f32bits(6.0f)}},
           {2, {9, 0, 0, 0, 0, 0x534758, 0, 0}}}},  // "XGS" @0x18
         {"CARENA_2", -8.0f, {}}},
        0x70, 8, 4, 0xffffffff, false);
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kOk);
    CHECK(d.tocImageOffsets[0] == 0x24);
    CHECK(d.sections[0].fileStart == 0x28);
    CHECK(d.sections[0].fileEnd == s.s1File);
    CHECK(d.params[9] == 8 && d.params[10] == 4);
    CHECK(d.keyedRecords.size() == 2);
    CHECK(d.keyedRecords[0].word0 == 1 && d.keyedRecords[0].key == 0);
    CHECK(d.keyedRecords[1].fileOffset == s.keyedFileOffs[1]);
    CHECK(d.arenas.size() == 2);
    CHECK(d.arenas[0].name() == "ARENA_1");
    CHECK(d.arenas[0].nameEndsWithTerminator);
    CHECK(d.arenas[0].payloadFileOffset ==
          s.arenaPayloadFileOffs[0]);
    CHECK(d.arenas[0].scalar() == 4.0f);
    CHECK(d.arenas[0].subRecords.size() == 2);
    CHECK(d.arenas[0].subRecords[0].type == 6);
    CHECK(d.arenas[0].subRecords[0].fields[0] == 1000);
    CHECK(d.arenas[0].subRecords[0].fields[1] == 1);
    CHECK(d.arenas[0].subRecords[0].fieldAsFloat(2) == 1.0f);
    CHECK(d.arenas[0].subRecords[1].type == 2);
    CHECK(d.arenas[0].subRecords[1].name18() == "XGS");
    CHECK(d.arenas[1].subRecords.empty());
    CHECK(d.arenas[1].subRecordCount == 0);
    CHECK(d.s2PayloadRegionStart == s.arenaPayloadFileOffs[0]);
    CHECK(d.paletteCount == 0x70);
    CHECK(d.paletteBytes.size() == 768);
    CHECK(d.gridPlaneSize == 48 && d.gridPlaneCount == 1);
    CHECK(d.sections[4].fileEnd == s.trailerFile);
    CHECK(d.s1TrailingBytes == 0 && d.s3TrailingBytes == 0 &&
          d.s4TrailingBytes == 0);
    CHECK(d.trailerPresent && d.secondaryEqualsTrailerOffset);
  }

  // Dual-plane variant: altFillA > 0 (signed) → s4 holds two planes.
  {
    auto s = SyntheticDti::build("TEST.DAT", {}, {}, 0x40, 10, 5, 0x30,
                                 true);
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kOk);
    CHECK(d.gridPlaneCount == 2);
    CHECK(d.gridPlaneSize == 14 * 5);
    CHECK(d.sections[4].size() == 14 * 5 * 2);
  }

  // Zero-count sections: minimal valid interior (s1/s2 empty, arenas
  // empty) — legal; counts are u32, zero is a valid count.
  {
    auto s = SyntheticDti::build("EMPTY.DAT", {}, {}, 0, 4, 2,
                                 0xffffffff, false);
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kOk);
    CHECK(d.keyedRecords.empty() && d.arenas.empty());
    CHECK(d.sections[1].size() == 4 && d.sections[2].size() == 4);
    CHECK(d.gridPlaneCount == 1 && d.gridPlaneSize == 16);
  }

  // Non-monotonic TOC: toc[2] < toc[1] → section bound fails.
  {
    auto s = SyntheticDti::build("TEST.DAT", {}, {}, 0x10, 8, 4,
                                 0xffffffff, false);
    s.put32(0x1c, 0x10);  // s2 offset before s1
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kSectionOutOfBounds);
    CHECK(d.badSection == 2);
  }

  // TOC entry past the image end → section bound fails.
  {
    auto s = SyntheticDti::build("TEST.DAT", {}, {}, 0x10, 8, 4,
                                 0xffffffff, false);
    s.put32(0x20, static_cast<std::uint32_t>(s.buf.size()));  // s3
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kSectionOutOfBounds);
    CHECK(d.badSection == 3);
  }

  // TOC entry inside the TOC itself (image < 0x24) → rejected.
  {
    auto s = SyntheticDti::build("TEST.DAT", {}, {}, 0x10, 8, 4,
                                 0xffffffff, false);
    s.put32(0x14, 0x10);  // toc[0] inside the TOC
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kSectionOutOfBounds);
    CHECK(d.badSection == 0);
  }

  // s0 span too small for the 29 proven words.
  {
    auto s = SyntheticDti::build("TEST.DAT", {}, {}, 0x10, 8, 4,
                                 0xffffffff, false);
    // Move s1's TOC entry to s0File+0x40: s0 shrinks to 0x40 < 0x74
    // while the ordering stays monotonic (0x24 < 0x64 < s2..s4).
    s.put32(0x18, static_cast<std::uint32_t>(s.s0File - 4 + 0x40));
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kSectionOutOfBounds);
    CHECK(d.badSection == 0);
  }

  // s1 count inflated past its section → record bound fails.
  {
    auto s = SyntheticDti::build("TEST.DAT", {{1, 0}}, {}, 0x10, 8, 4,
                                 0xffffffff, false);
    s.put32(static_cast<std::size_t>(s.s1File), 0x400);
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kRecordOutOfBounds);
    CHECK(d.badSection == 1);
  }

  // s2 count inflated → record bound fails.
  {
    auto s = SyntheticDti::build("TEST.DAT", {},
                                 {{"A", 1.0f, {}}},
                                 0x10, 8, 4, 0xffffffff, false);
    s.put32(static_cast<std::size_t>(s.s2File), 0x400);
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kRecordOutOfBounds);
    CHECK(d.badSection == 2);
  }

  // Arena payload offset before the payload region (into the name
  // table) → offset bound fails (native hardening).
  {
    auto s = SyntheticDti::build("TEST.DAT", {},
                                 {{"A", 1.0f, {}}},
                                 0x10, 8, 4, 0xffffffff, false);
    s.put32(static_cast<std::size_t>(s.arenaRecFileOffs[0] + 8),
            static_cast<std::uint32_t>(s.s2File - 4));  // -> s2 count
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kOffsetOutOfBounds);
    CHECK(d.badSection == 2 && d.badRecord == 0);
  }

  // Arena payload offset past s2's end → offset bound fails.
  {
    auto s = SyntheticDti::build("TEST.DAT", {},
                                 {{"A", 1.0f, {}}},
                                 0x10, 8, 4, 0xffffffff, false);
    s.put32(static_cast<std::size_t>(s.arenaRecFileOffs[0] + 8),
            static_cast<std::uint32_t>(s.s3File - 4));  // -> s3 start
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kOffsetOutOfBounds);
    CHECK(d.badSection == 2 && d.badRecord == 0);
  }

  // Payload sub-record count inflated → record bound fails inside s2.
  {
    auto s = SyntheticDti::build("TEST.DAT", {},
                                 {{"A", 1.0f,
                                   {{1, {0, 0, 0, 0, 0, 0, 0, 0}}}}},
                                 0x10, 8, 4, 0xffffffff, false);
    s.put32(static_cast<std::size_t>(s.arenaPayloadFileOffs[0]),
            0x1000);
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kRecordOutOfBounds);
    CHECK(d.badSection == 2 && d.badRecord == 0);
  }

  // s3 span too small for count + 768 palette bytes: shrink s3 by
  // moving toc[4] earlier (still monotonic, still ≥ s3 start).
  {
    auto s = SyntheticDti::build("TEST.DAT", {}, {}, 0x10, 8, 4,
                                 0xffffffff, false);
    s.put32(0x24, static_cast<std::uint32_t>(s.s3File - 4 + 0x200));
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kSectionOutOfBounds);
    CHECK(d.badSection == 3);
  }

  // s4 smaller than grid-derived plane size → section bound fails.
  {
    auto s = SyntheticDti::build("TEST.DAT", {}, {}, 0x10, 8, 4,
                                 0xffffffff, false);
    // grid 8x4 needs 48 bytes; shrink the file's s4 span to 32 by
    // truncating before the trailer (rebuild envelope for new size).
    const std::uint64_t newTotal = s.s4File + 32 + 12;
    s.buf.resize(static_cast<std::size_t>(newTotal));
    s.put32(0x00, static_cast<std::uint32_t>(newTotal - 4));
    s.put32(0x10, static_cast<std::uint32_t>(newTotal - 12));
    s.putName(static_cast<std::size_t>(newTotal - 12), "TEST.DAT");
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kSectionOutOfBounds);
    CHECK(d.badSection == 4);
  }

  // Missing trailer → interior extends to EOF; s4 must still hold the
  // proven planes (the last bytes become grid, not trailer).
  {
    auto s = SyntheticDti::build("TEST.DAT", {}, {}, 0x10, 8, 4,
                                 0xffffffff, false);
    for (std::size_t i = s.buf.size() - 12; i < s.buf.size(); ++i)
      s.buf[i] = std::byte{'X'};
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kOk);
    CHECK(!d.trailerPresent);
    CHECK(d.sections[4].fileEnd == s.buf.size());
    CHECK(d.s4TrailingBytes == 12);  // trailer bytes now inside s4
  }

  // Not a tagged envelope → "not this format", not "malformed".
  {
    std::byte raw[64] = {};
    const auto d = inspectDtiStructure(raw);
    CHECK(d.status == DtiStructureStatus::kNotTaggedEnvelope);
  }

  // u32@0 length mismatch → envelope invalid → not this format.
  {
    auto s = SyntheticDti::build("TEST.DAT", {}, {}, 0x10, 8, 4,
                                 0xffffffff, false);
    s.put32(0x00, 0);
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kNotTaggedEnvelope);
  }

  // Truncated before the TOC end (< 0x28).
  {
    std::byte t[0x24] = {};
    t[0] = std::byte{0x20};
    std::memcpy(t + 4, "T.DAT", 5);
    const auto d = inspectDtiStructure(t);
    CHECK(d.status == DtiStructureStatus::kTruncatedHeader);
  }

  // A CMI-shaped buffer fed to the DTI parser: the first CMI count at
  // 0x14 is not a plausible TOC → rejected, never kOk.
  {
    auto c = SyntheticCmi::build("TEST.CMD",
        {{{"A", 0}}, {{"B", 0}}, {{"C", 0}}, {{"D", 0}}}, 8);
    const auto d = inspectDtiStructure(c.buf);
    CHECK(d.status != DtiStructureStatus::kOk);
  }

  // Arena name without an internal NUL is preserved raw with the flag
  // reporting the deviation (corpus names are all NUL-terminated).
  {
    auto s = SyntheticDti::build("TEST.DAT", {},
                                 {{"ABCDEFGH", 1.0f, {}}},
                                 0x10, 8, 4, 0xffffffff, false);
    const auto d = inspectDtiStructure(s.buf);
    CHECK(d.status == DtiStructureStatus::kOk);
    CHECK(d.arenas[0].name() == "ABCDEFGH");
    CHECK(!d.arenas[0].nameEndsWithTerminator);
  }
}

// Synthetic FTI file builder (no original data). Layout mirrors the
// CODE-CORROBORATED structure: [u32 size-4][u32 count]
// [count x 12B {name[8], imgOff}][payloads to EOF].
struct SyntheticFti {
  std::vector<std::byte> buf;

  void put32(std::size_t off, std::uint32_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    buf[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    buf[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  }
  void putName(std::size_t off, const char* s, std::size_t width = 8) {
    for (std::size_t i = 0; s[i] && i < width; ++i)
      buf[off + i] = static_cast<std::byte>(s[i]);
  }
  void putBytes(std::size_t off, std::initializer_list<int> bytes) {
    std::size_t i = 0;
    for (int b : bytes)
      buf[off + i++] = static_cast<std::byte>(b & 0xff);
  }

  // entries: {name, payloadSize}; payloads laid out contiguously from
  // directory end (the OBSERVED corpus packing).
  static SyntheticFti
  build(std::initializer_list<
        std::pair<const char*, std::uint32_t>> entries) {
    SyntheticFti s;
    const std::uint32_t count = static_cast<std::uint32_t>(entries.size());
    const std::uint64_t dirEnd = 0x08 + std::uint64_t(count) * 12;
    std::uint64_t total = dirEnd;
    for (const auto& [n, sz] : entries)
      total += sz;
    s.buf.assign(static_cast<std::size_t>(total), std::byte{0});

    s.put32(0x00, static_cast<std::uint32_t>(total - 4));
    s.put32(0x04, count);
    std::uint64_t payloadAt = dirEnd;
    std::uint32_t i = 0;
    for (const auto& [n, sz] : entries) {
      const std::uint64_t rec = 0x08 + std::uint64_t(i) * 12;
      s.putName(rec, n);
      s.put32(rec + 0x08,
              static_cast<std::uint32_t>(payloadAt - 4)); // img-relative
      payloadAt += sz;
      ++i;
    }
    return s;
  }
};

void test_fti_directory() {
  using mdk::FtiDirectoryStatus;
  using mdk::inspectFtiDirectory;

  // Valid three-entry directory: offsets tile from dirEnd to EOF.
  {
    auto s = SyntheticFti::build({{"FIRST", 10}, {"SECOND", 6},
                                  {"EIGHTCHR", 4}});
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kOk);
    CHECK(d.count == 3);
    CHECK(d.directoryEnd == 0x2c);
    CHECK(d.records.size() == 3);
    CHECK(d.records[0].name() == "FIRST");
    CHECK(d.records[0].payloadFileOffset == 0x2c);
    CHECK(d.records[0].payloadEnd == 0x36);
    CHECK(d.records[0].payloadSize() == 10);
    CHECK(d.records[1].payloadFileOffset == 0x36);
    CHECK(d.records[1].payloadSize() == 6);
    CHECK(d.records[2].name() == "EIGHTCHR");
    CHECK(!d.records[2].nameHasTerminator);   // full 8 bytes, no NUL
    CHECK(d.records[2].payloadEnd == s.buf.size());
    CHECK(d.offsetsSortedAscending);
    CHECK(d.offsetsUnique);
    CHECK(d.firstPayloadAtDirectoryEnd);
  }

  // Zero-record directory: count=0; payload region [0x08, size).
  {
    auto s = SyntheticFti::build({});
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kOk);
    CHECK(d.count == 0);
    CHECK(d.directoryEnd == 0x08);
    CHECK(d.records.empty());
    CHECK(!d.firstPayloadAtDirectoryEnd);
  }

  // Names with no NUL in the 8-byte field are legal (exact two-u32
  // compare) — preserved raw, decoded for display.
  {
    auto s = SyntheticFti::build({{"ABCDEFGH", 4}});
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kOk);
    CHECK(d.records[0].name() == "ABCDEFGH");
    CHECK(!d.records[0].nameHasTerminator);
  }

  // Non-ASCII bytes inside the name field are preserved raw; the
  // decoded name escapes them (display only).
  {
    auto s = SyntheticFti::build({{"AB", 4}});
    s.putBytes(0x08, {0x80, 0x41, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00});
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kOk);
    CHECK(d.records[0].nameField[0] == std::byte{0x80});
    CHECK(d.records[0].name() == "\\x80" "A");
    CHECK(d.records[0].nameHasTerminator);
  }

  // Tagged-name envelope → not this family.
  {
    auto sni = SyntheticSni::build("X.SND", {});
    const auto d = inspectFtiDirectory(sni.buf);
    CHECK(d.status == FtiDirectoryStatus::kNotLengthEnvelope);
  }

  // Bad declared length → not this envelope.
  {
    auto s = SyntheticFti::build({{"A", 4}});
    s.put32(0x00, 0xdeadbeef);
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kNotLengthEnvelope);
  }

  // Truncated file (< 8 bytes) → header bound fails.
  {
    const std::array<std::byte, 6> tiny = {
        std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{1}, std::byte{0}};
    const auto d = inspectFtiDirectory(tiny);
    CHECK(d.status == FtiDirectoryStatus::kTruncatedHeader);
  }

  // Impossible count → directory bound fails (division-first math).
  {
    auto s = SyntheticFti::build({{"A", 4}});
    s.put32(0x04, 0x00ffffff);
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kDirectoryOutOfBounds);
  }

  // Stored offset pointing into the directory → rejected.
  {
    auto s = SyntheticFti::build({{"A", 8}, {"B", 4}});
    s.put32(0x08 + 0x08, 0x04);  // img 0x04 -> file 0x08 (inside dir)
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kOffsetOutOfBounds);
    CHECK(d.badRecordIndex == 0);
  }

  // Stored offset past EOF → rejected.
  {
    auto s = SyntheticFti::build({{"A", 4}});
    s.put32(0x08 + 0x08, static_cast<std::uint32_t>(s.buf.size()));
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kOffsetOutOfBounds);
  }

  // Offset exactly at EOF is a legal empty-tail form.
  {
    auto s = SyntheticFti::build({{"A", 4}, {"Z", 0}});
    s.put32(0x08 + 0x0c + 0x08,
            static_cast<std::uint32_t>(s.buf.size() - 4));
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kOk);
    CHECK(d.records[1].payloadFileOffset == s.buf.size());
    CHECK(d.records[1].payloadSize() == 0);
  }

  // Unsorted offsets: still enumerated (spans follow sorted order),
  // reported via the flag — corpus files are all sorted.
  {
    auto s = SyntheticFti::build({{"A", 8}, {"B", 4}});
    // Swap the two offsets: A -> second payload, B -> first.
    const std::uint64_t dirEnd = 0x08 + 2 * 12;
    s.put32(0x08 + 0x08,
            static_cast<std::uint32_t>(dirEnd + 8 - 4)); // A -> dirEnd+8
    s.put32(0x14 + 0x08,
            static_cast<std::uint32_t>(dirEnd - 4));     // B -> dirEnd
    const auto d = inspectFtiDirectory(s.buf);
    CHECK(d.status == FtiDirectoryStatus::kOk);
    CHECK(!d.offsetsSortedAscending);
    CHECK(d.records[0].payloadFileOffset == dirEnd + 8);
    CHECK(d.records[0].payloadEnd == s.buf.size());  // last in sort order
    CHECK(d.records[1].payloadFileOffset == dirEnd);
    CHECK(d.records[1].payloadEnd == dirEnd + 8);
  }
}

// Synthetic BNI file builder (no original data). Layout mirrors the
// CODE-CORROBORATED structure: [u32 size-4][u32 count]
// [count x 16B {name[12], imgOff}][payloads to EOF].
struct SyntheticBni {
  std::vector<std::byte> buf;

  void put32(std::size_t off, std::uint32_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    buf[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    buf[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  }
  void putName(std::size_t off, const char* s, std::size_t width = 12) {
    for (std::size_t i = 0; s[i] && i < width; ++i)
      buf[off + i] = static_cast<std::byte>(s[i]);
  }

  // entries: {name, payloadSize}; payloads laid out contiguously from
  // directory end (the OBSERVED corpus packing).
  static SyntheticBni
  build(std::initializer_list<
        std::pair<const char*, std::uint32_t>> entries) {
    SyntheticBni s;
    const std::uint32_t count = static_cast<std::uint32_t>(entries.size());
    const std::uint64_t dirEnd = 0x08 + std::uint64_t(count) * 16;
    std::uint64_t total = dirEnd;
    for (const auto& [n, sz] : entries)
      total += sz;
    s.buf.assign(static_cast<std::size_t>(total), std::byte{0});

    s.put32(0x00, static_cast<std::uint32_t>(total - 4));
    s.put32(0x04, count);
    std::uint64_t payloadAt = dirEnd;
    std::uint32_t i = 0;
    for (const auto& [n, sz] : entries) {
      const std::uint64_t rec = 0x08 + std::uint64_t(i) * 16;
      s.putName(rec, n);
      s.put32(rec + 0x0c,
              static_cast<std::uint32_t>(payloadAt - 4)); // img-relative
      payloadAt += sz;
      ++i;
    }
    return s;
  }
};

void test_bni_directory() {
  using mdk::BniDirectoryStatus;
  using mdk::inspectBniDirectory;

  // Valid three-entry directory, including a 9-char name (OBSERVED
  // "BONESANIM" — names are not limited to 8 chars).
  {
    auto s = SyntheticBni::build({{"KURT", 10}, {"BONESANIM", 6},
                                  {"RES3", 4}});
    const auto d = inspectBniDirectory(s.buf);
    CHECK(d.status == BniDirectoryStatus::kOk);
    CHECK(d.count == 3);
    CHECK(d.directoryEnd == 0x38);
    CHECK(d.records.size() == 3);
    CHECK(d.records[0].name() == "KURT");
    CHECK(d.records[0].payloadFileOffset == 0x38);
    CHECK(d.records[0].payloadSize() == 10);
    CHECK(d.records[1].name() == "BONESANIM");
    CHECK(d.records[1].nameHasTerminator);
    CHECK(d.records[1].payloadFileOffset == 0x42);
    CHECK(d.records[2].payloadEnd == s.buf.size());
    CHECK(d.offsetsSortedAscending);
    CHECK(d.offsetsUnique);
    CHECK(d.firstPayloadAtDirectoryEnd);
  }

  // Zero-record directory: count=0 is legal.
  {
    auto s = SyntheticBni::build({});
    const auto d = inspectBniDirectory(s.buf);
    CHECK(d.status == BniDirectoryStatus::kOk);
    CHECK(d.count == 0);
    CHECK(d.directoryEnd == 0x08);
    CHECK(d.records.empty());
  }

  // A name without a NUL inside the 12-byte field is an anomaly (the
  // original's unbounded compare would read into the offset word) —
  // still structurally valid; reported via the flag.
  {
    auto s = SyntheticBni::build({{"TWELVECHARS!", 4}});
    const auto d = inspectBniDirectory(s.buf);
    CHECK(d.status == BniDirectoryStatus::kOk);
    CHECK(d.records[0].name() == "TWELVECHARS!");
    CHECK(!d.records[0].nameHasTerminator);
  }

  // Tagged-name envelope → not this family.
  {
    auto sni = SyntheticSni::build("X.SND", {});
    const auto d = inspectBniDirectory(sni.buf);
    CHECK(d.status == BniDirectoryStatus::kNotLengthEnvelope);
  }

  // Bad declared length → not this envelope.
  {
    auto s = SyntheticBni::build({{"A", 4}});
    s.put32(0x00, 0xdeadbeef);
    const auto d = inspectBniDirectory(s.buf);
    CHECK(d.status == BniDirectoryStatus::kNotLengthEnvelope);
  }

  // Truncated file (< 8 bytes) → header bound fails.
  {
    const std::array<std::byte, 6> tiny = {
        std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{1}, std::byte{0}};
    const auto d = inspectBniDirectory(tiny);
    CHECK(d.status == BniDirectoryStatus::kTruncatedHeader);
  }

  // Impossible count → directory bound fails.
  {
    auto s = SyntheticBni::build({{"A", 4}});
    s.put32(0x04, 0x00ffffff);
    const auto d = inspectBniDirectory(s.buf);
    CHECK(d.status == BniDirectoryStatus::kDirectoryOutOfBounds);
  }

  // Stored offset pointing into the directory → rejected.
  {
    auto s = SyntheticBni::build({{"A", 8}, {"B", 4}});
    s.put32(0x08 + 0x0c, 0x04);  // img 0x04 -> file 0x08 (inside dir)
    const auto d = inspectBniDirectory(s.buf);
    CHECK(d.status == BniDirectoryStatus::kOffsetOutOfBounds);
    CHECK(d.badRecordIndex == 0);
  }

  // Stored offset past EOF → rejected.
  {
    auto s = SyntheticBni::build({{"A", 4}});
    s.put32(0x08 + 0x0c, static_cast<std::uint32_t>(s.buf.size()));
    const auto d = inspectBniDirectory(s.buf);
    CHECK(d.status == BniDirectoryStatus::kOffsetOutOfBounds);
  }

  // Cross-family byte check: a BNI-shaped file fed to the FTI parser
  // (and vice versa) must not be accepted — different record layouts.
  {
    auto b = SyntheticBni::build({{"KURT", 8}, {"EXPLODE", 4}});
    const auto d = mdk::inspectFtiDirectory(b.buf);
    CHECK(d.status != mdk::FtiDirectoryStatus::kOk);
    auto f = SyntheticFti::build({{"FONTSML", 8}, {"SND_PUSH", 4}});
    const auto d2 = inspectBniDirectory(f.buf);
    CHECK(d2.status != BniDirectoryStatus::kOk);
  }
}

// Synthetic palette-embedded BNI image payload (no original data):
// {u8 rgb[768], u16le w, u16le h, u8 px[w*h]} — the CODE-CORROBORATED
// MDKOPT-class layout.
std::vector<std::byte> makePalettedImage(std::uint16_t w,
                                         std::uint16_t h) {
  std::vector<std::byte> v(772 + std::size_t(w) * h, std::byte{0});
  for (int i = 0; i < 256; ++i) {  // deterministic synthetic palette
    v[i * 3 + 0] = static_cast<std::byte>(i);
    v[i * 3 + 1] = static_cast<std::byte>(255 - i);
    v[i * 3 + 2] = static_cast<std::byte>((i * 2) & 0xff);
  }
  v[768] = static_cast<std::byte>(w & 0xff);
  v[769] = static_cast<std::byte>(w >> 8);
  v[770] = static_cast<std::byte>(h & 0xff);
  v[771] = static_cast<std::byte>(h >> 8);
  for (std::size_t i = 772; i < v.size(); ++i) {
    v[i] = static_cast<std::byte>((i - 772) % 256);  // ramp 0..255
  }
  return v;
}

void test_bni_image() {
  using mdk::BniImageShape;

  // findBniRecord — bounded ASCII case-insensitive name match on the
  // NUL-terminated field content.
  {
    auto s = SyntheticBni::build({{"KURT", 10}, {"BONESANIM", 6},
                                  {"RES3", 4}});
    const auto d = mdk::inspectBniDirectory(s.buf);
    CHECK(d.status == mdk::BniDirectoryStatus::kOk);
    CHECK(mdk::findBniRecord(d, "BONESANIM") == &d.records[1]);
    CHECK(mdk::findBniRecord(d, "bonesanim") == &d.records[1]);
    CHECK(mdk::findBniRecord(d, "kurt") == &d.records[0]);
    CHECK(mdk::findBniRecord(d, "KUR") == nullptr);   // prefix only
    CHECK(mdk::findBniRecord(d, "KURTX") == nullptr); // longer
    CHECK(mdk::findBniRecord(d, "NOPE") == nullptr);
    CHECK(mdk::findBniRecord(d, "") == nullptr);
  }

  // probe shape classification.
  {
    const auto pal = makePalettedImage(4, 3);
    const auto p = mdk::probeBniImage(pal);
    CHECK(p.shape == BniImageShape::kPaletted);
    CHECK(p.width == 4 && p.height == 3);
    CHECK(p.headerBytes == 772 && p.pixelBytes == 12);
  }
  {
    // Indexed-only {u16 w, u16 h, px}: 4+6 for a 3x2.
    std::vector<std::byte> v(10, std::byte{0});
    v[0] = std::byte{3};
    v[2] = std::byte{2};
    const auto p = mdk::probeBniImage(v);
    CHECK(p.shape == BniImageShape::kIndexedOnly);
    CHECK(p.width == 3 && p.height == 2);
    CHECK(p.headerBytes == 4 && p.pixelBytes == 6);
  }
  {
    const std::array<std::byte, 20> junk = {std::byte{9}};
    CHECK(mdk::probeBniImage(junk).shape == BniImageShape::kOther);
    const std::vector<std::byte> empty;
    CHECK(mdk::probeBniImage(empty).shape == BniImageShape::kOther);
  }

  // Smallest valid paletted image: 1x1.
  {
    const auto pal = makePalettedImage(1, 1);
    CHECK(pal.size() == 773);
    const auto img = mdk::decodeBniPalettedImage(pal);
    CHECK(img.has_value());
    CHECK(img->width == 1 && img->height == 1 && img->stride == 1);
    CHECK(img->pixels.size() == 1 && img->pixels[0] == 0);
    CHECK(img->hasPalette);
    CHECK(img->palette[0].r == 0 && img->palette[0].g == 255 &&
          img->palette[0].b == 0);
    CHECK(img->palette[255].r == 255 && img->palette[255].g == 0 &&
          img->palette[255].b == 254);
  }

  // Ordinary dimensions + exact pixel/palette preservation. The pixel
  // ramp exercises all 256 indices — every one resolves through the
  // embedded 256-entry palette.
  {
    const auto pal = makePalettedImage(7, 5);
    const auto img = mdk::decodeBniPalettedImage(pal);
    CHECK(img.has_value());
    CHECK(img->width == 7 && img->height == 5);
    CHECK(img->pixels.size() == 35);
    for (std::size_t i = 0; i < 35; ++i) {
      CHECK(img->pixels[i] == static_cast<std::uint8_t>(i % 256));
    }
    CHECK(img->palette[42].r == 42 && img->palette[42].g == 213 &&
          img->palette[42].b == 84);
  }

  // Row-major top-down orientation: pixels[y*w+x] == payload
  // [772 + y*w + x]; verified again through the framebuffer blit.
  {
    auto pal = makePalettedImage(4, 2);
    pal[772 + 1 * 4 + 2] = std::byte{0xab};  // row 1, col 2
    const auto img = mdk::decodeBniPalettedImage(pal);
    CHECK(img->pixels[1 * 4 + 2] == 0xab);
    CHECK(img->pixels[2] == 2);  // row 0 col 2 = ramp value
  }

  // Truncation: any payload shorter than the exact tiling is refused.
  {
    auto pal = makePalettedImage(4, 3);
    pal.pop_back();
    CHECK(!mdk::decodeBniPalettedImage(pal).has_value());
    const std::vector<std::byte> headOnly(pal.begin(),
                                          pal.begin() + 771);
    CHECK(!mdk::decodeBniPalettedImage(headOnly).has_value());
    const std::vector<std::byte> palOnly(pal.begin(),
                                         pal.begin() + 768);
    CHECK(!mdk::decodeBniPalettedImage(palOnly).has_value());
  }

  // Trailing slack: a payload larger than the exact tiling is refused
  // — no silent truncation.
  {
    auto pal = makePalettedImage(4, 3);
    pal.push_back(std::byte{0});
    CHECK(!mdk::decodeBniPalettedImage(pal).has_value());
  }

  // Zero dimensions are malformed.
  {
    auto pal = makePalettedImage(0, 3);
    pal.resize(772);  // size must match 772+0 to isolate the w==0 rule
    CHECK(!mdk::decodeBniPalettedImage(pal).has_value());
    auto pal2 = makePalettedImage(3, 0);
    pal2.resize(772);
    CHECK(!mdk::decodeBniPalettedImage(pal2).has_value());
  }

  // Declared dimensions escaping the payload: no overflow, no read
  // outside the span.
  {
    auto pal = makePalettedImage(4, 3);
    pal[768] = std::byte{0xff};  // w = 0x03ff
    pal[769] = std::byte{0x03};
    CHECK(!mdk::decodeBniPalettedImage(pal).has_value());
    auto big = makePalettedImage(4, 3);
    big[768] = std::byte{0xff};  // w = h = 65535 -> w*h ~ 4.3e9
    big[769] = std::byte{0xff};
    big[770] = std::byte{0xff};
    big[771] = std::byte{0xff};
    CHECK(!mdk::decodeBniPalettedImage(big).has_value());
  }

  // The paletted decoder refuses the indexed-only layout (and vice
  // versa via the probe) — layout confusion must not decode.
  {
    std::vector<std::byte> v(10, std::byte{0});
    v[0] = std::byte{3};
    v[2] = std::byte{2};
    std::string err;
    CHECK(!mdk::decodeBniPalettedImage(v, &err).has_value());
    CHECK(!err.empty());
  }

  // Deterministic digest: same payload -> same digest; one changed
  // pixel or palette byte -> different digest.
  {
    const auto a = mdk::decodeBniPalettedImage(makePalettedImage(8, 4));
    const auto b = mdk::decodeBniPalettedImage(makePalettedImage(8, 4));
    CHECK(a && b);
    CHECK(mdk::imageDigest(*a) == mdk::imageDigest(*b));
    auto c = *b;
    c.pixels[0] ^= 0x01;
    CHECK(mdk::imageDigest(*a) != mdk::imageDigest(c));
    auto d = *b;
    d.palette[0].r ^= 0x01;
    CHECK(mdk::imageDigest(*a) != mdk::imageDigest(d));
  }
}

// Synthetic indexed-only image payload {u16le w, u16le h, px[w*h]}
// and 768-byte RGB palette (no original data) — the Phase 4B shape.
std::vector<std::byte> makeIndexedOnlyImage(std::uint16_t w,
                                            std::uint16_t h) {
  std::vector<std::byte> v(4 + std::size_t(w) * h, std::byte{0});
  v[0] = static_cast<std::byte>(w & 0xff);
  v[1] = static_cast<std::byte>(w >> 8);
  v[2] = static_cast<std::byte>(h & 0xff);
  v[3] = static_cast<std::byte>(h >> 8);
  for (std::size_t i = 4; i < v.size(); ++i) {
    v[i] = static_cast<std::byte>((i - 4) % 256);  // ramp 0..255
  }
  return v;
}

std::vector<std::byte> makeRgbPalette(std::size_t bytes,
                                      int seed) {
  std::vector<std::byte> v(bytes, std::byte{0});
  for (std::size_t i = 0; i < bytes; ++i) {
    v[i] = static_cast<std::byte>((i + seed) & 0xff);
  }
  return v;
}

void test_bni_indexed_image() {
  // decodeBniIndexedImage — valid image + 768-byte RGB palette.
  {
    const auto img = mdk::decodeBniIndexedImage(
        makeIndexedOnlyImage(7, 5), makeRgbPalette(768, 0));
    CHECK(img.has_value());
    CHECK(img->width == 7 && img->height == 5 && img->stride == 7);
    CHECK(img->pixels.size() == 35);
    for (std::size_t i = 0; i < 35; ++i) {
      CHECK(img->pixels[i] == static_cast<std::uint8_t>(i % 256));
    }
    CHECK(img->hasPalette);
    // palette byte i -> entry i/3, channel i%3.
    CHECK(img->palette[0].r == 0 && img->palette[0].g == 1 &&
          img->palette[0].b == 2);
    CHECK(img->palette[255].r == 253 && img->palette[255].g == 254 &&
          img->palette[255].b == 255);
  }

  // 1x1 minimum size.
  {
    const auto img = mdk::decodeBniIndexedImage(
        makeIndexedOnlyImage(1, 1), makeRgbPalette(768, 0));
    CHECK(img.has_value());
    CHECK(img->pixels.size() == 1 && img->pixels[0] == 0);
  }

  // Top-down row mapping: pixels[y*w+x] == payload[4 + y*w + x].
  {
    auto raw = makeIndexedOnlyImage(4, 2);
    raw[4 + 1 * 4 + 2] = std::byte{0xab};  // row 1, col 2
    const auto img =
        mdk::decodeBniIndexedImage(raw, makeRgbPalette(768, 0));
    CHECK(img->pixels[1 * 4 + 2] == 0xab);
    CHECK(img->pixels[2] == 2);  // row 0 col 2 = ramp value
  }

  // Truncated pixel region / truncated dims -> refused.
  {
    auto v = makeIndexedOnlyImage(4, 3);
    v.pop_back();
    CHECK(!mdk::decodeBniIndexedImage(v, makeRgbPalette(768, 0))
               .has_value());
    const std::vector<std::byte> head(v.begin(), v.begin() + 3);
    CHECK(!mdk::decodeBniIndexedImage(head, makeRgbPalette(768, 0))
               .has_value());
    const std::vector<std::byte> dims(v.begin(), v.begin() + 4);
    CHECK(!mdk::decodeBniIndexedImage(dims, makeRgbPalette(768, 0))
               .has_value());
  }

  // Trailing slack -> refused (exact tiling).
  {
    auto v = makeIndexedOnlyImage(4, 3);
    v.push_back(std::byte{0});
    CHECK(!mdk::decodeBniIndexedImage(v, makeRgbPalette(768, 0))
               .has_value());
  }

  // Zero/overflowing dimensions -> refused.
  {
    auto v = makeIndexedOnlyImage(0, 3);
    v.resize(4);  // isolate the w==0 rule (size would tile at 4)
    CHECK(!mdk::decodeBniIndexedImage(v, makeRgbPalette(768, 0))
               .has_value());
    auto big = makeIndexedOnlyImage(4, 3);
    big[0] = std::byte{0xff};  // w = 0xff04 -> w*h escapes payload
    big[1] = std::byte{0xff};
    CHECK(!mdk::decodeBniIndexedImage(big, makeRgbPalette(768, 0))
               .has_value());
  }

  // Palette size: exactly 768 required — short and long both fail.
  {
    const auto img = makeIndexedOnlyImage(2, 2);
    CHECK(!mdk::decodeBniIndexedImage(img, makeRgbPalette(767, 0))
               .has_value());
    CHECK(!mdk::decodeBniIndexedImage(img, makeRgbPalette(769, 0))
               .has_value());
    CHECK(!mdk::decodeBniIndexedImage(img, {}).has_value());
  }

  // The indexed decoder refuses the paletted layout.
  {
    std::string err;
    CHECK(!mdk::decodeBniIndexedImage(makePalettedImage(4, 3),
                                      makeRgbPalette(768, 0), &err)
               .has_value());
    CHECK(!err.empty());
  }

  // Deterministic digest across the external-palette path.
  {
    const auto a = mdk::decodeBniIndexedImage(makeIndexedOnlyImage(8, 4),
                                              makeRgbPalette(768, 0));
    const auto b = mdk::decodeBniIndexedImage(makeIndexedOnlyImage(8, 4),
                                              makeRgbPalette(768, 0));
    CHECK(a && b);
    CHECK(mdk::imageDigest(*a) == mdk::imageDigest(*b));
    const auto c = mdk::decodeBniIndexedImage(makeIndexedOnlyImage(8, 4),
                                              makeRgbPalette(768, 9));
    CHECK(c && mdk::imageDigest(*a) != mdk::imageDigest(*c));
  }
}

void test_stream_context() {
  // composeStreamPalette: entries 0-63 from SYS_PAL head, 64-255
  // from PAL bytes [0xc0, 0x300) — the FUN_0042b270 composition.
  {
    const auto sys = makeRgbPalette(192, 0x10);
    const auto pal = makeRgbPalette(768, 0x80);
    const auto out = mdk::composeStreamPalette(sys, pal);
    CHECK(out.has_value());
    CHECK(out->size() == 768);
    for (std::size_t i = 0; i < 192; ++i) {
      CHECK((*out)[i] == sys[i]);  // head from SYS_PAL
    }
    for (std::size_t i = 0; i < 576; ++i) {
      CHECK((*out)[0xc0 + i] == pal[0xc0 + i]);  // tail from PAL+0xc0
    }
    // Verify the tail really starts at 0xc0, not 0.
    CHECK((*out)[0xc0] == pal[0xc0]);
    CHECK((*out)[0xc0] != pal[0] || pal[0] == pal[0xc0]);
  }

  // Malformed inputs: wrong sizes rejected.
  {
    const auto sys = makeRgbPalette(192, 0);
    const auto pal = makeRgbPalette(768, 0);
    CHECK(!mdk::composeStreamPalette(makeRgbPalette(191, 0), pal)
               .has_value());
    CHECK(!mdk::composeStreamPalette(makeRgbPalette(193, 0), pal)
               .has_value());
    CHECK(!mdk::composeStreamPalette(sys, makeRgbPalette(767, 0))
               .has_value());
    CHECK(!mdk::composeStreamPalette(sys, makeRgbPalette(769, 0))
               .has_value());
  }

  // isStreamBackdropRequest: case-insensitive, '/' and '\'
  // equivalent — matches DOS path semantics.
  {
    CHECK(mdk::isStreamBackdropRequest("STREAM/STREAM.BNI", "BG"));
    CHECK(mdk::isStreamBackdropRequest("stream\\stream.bni", "bg"));
    CHECK(mdk::isStreamBackdropRequest("STREAM\\STREAM.BNI", "Bg"));
    CHECK(!mdk::isStreamBackdropRequest("STREAM/STREAM.BNI",
                                        "PLANET"));
    CHECK(!mdk::isStreamBackdropRequest("FALL3D/FALL3D.BNI", "BG"));
    CHECK(!mdk::isStreamBackdropRequest("STREAM/STREAM.BNI", ""));
  }

  // End-to-end on fully synthetic container bytes: a BNI with
  // {PAL 768B, BG {u16 w,h,px}} plus an FTI with {SYS_PAL 192B}.
  auto bni = SyntheticBni::build({{"PAL", 768}, {"BG", 4 + 12}});
  auto fti = SyntheticFti::build({{"SYS_PAL", 192}, {"FONTSML", 8}});
  {
    const auto bdir = mdk::inspectBniDirectory(bni.buf);
    const auto fdir = mdk::inspectFtiDirectory(fti.buf);
    CHECK(bdir.status == mdk::BniDirectoryStatus::kOk);
    CHECK(fdir.status == mdk::FtiDirectoryStatus::kOk);
    const auto* pr = mdk::findBniRecord(bdir, "PAL");
    const auto* br = mdk::findBniRecord(bdir, "BG");
    const auto* sr = mdk::findFtiRecord(fdir, "SYS_PAL");
    CHECK(pr && br && sr);
    // Fill PAL: bytes i = i&0xff (offset marker at 0xc0).
    for (std::size_t i = 0; i < 768; ++i) {
      bni.buf[pr->payloadFileOffset + i] =
          static_cast<std::byte>(i & 0xff);
    }
    // Fill BG: 4x3, pixels ramp from 0x20.
    bni.buf[br->payloadFileOffset + 0] = std::byte{4};
    bni.buf[br->payloadFileOffset + 2] = std::byte{3};
    for (std::size_t i = 0; i < 12; ++i) {
      bni.buf[br->payloadFileOffset + 4 + i] =
          static_cast<std::byte>(0x20 + i);
    }
    // Fill SYS_PAL: bytes i = 0x40 + i.
    for (std::size_t i = 0; i < 192; ++i) {
      fti.buf[sr->payloadFileOffset + i] =
          static_cast<std::byte>(0x40 + i);
    }
    const auto img = mdk::decodeStreamBackdrop(bni.buf, fti.buf);
    CHECK(img.has_value());
    CHECK(img->width == 4 && img->height == 3);
    CHECK(img->pixels.size() == 12 && img->pixels[0] == 0x20 &&
          img->pixels[11] == 0x2b);
    CHECK(img->hasPalette);
    // entries 0-63 from SYS_PAL (0x40+i), entries 64-255 from
    // PAL[0xc0+i] (value (0xc0+i)&0xff == i since pal[i]=i&0xff).
    CHECK(img->palette[0].r == 0x40 && img->palette[0].g == 0x41);
    CHECK(img->palette[63].b == 0x40 + 191);
    CHECK(img->palette[64].r == 0xc0 && img->palette[64].g == 0xc1);
    CHECK(img->palette[255].b == 0xff);
  }

  // Missing required records -> fail.
  {
    std::string err;
    auto noPal = SyntheticBni::build({{"BG", 4 + 12}});
    CHECK(!mdk::decodeStreamBackdrop(noPal.buf, fti.buf, &err)
               .has_value());
    CHECK(err.find("PAL") != std::string::npos);

    auto noBg = SyntheticBni::build({{"PAL", 768}});
    CHECK(!mdk::decodeStreamBackdrop(noBg.buf, fti.buf, &err)
               .has_value());
    CHECK(err.find("BG") != std::string::npos);

    auto noSys = SyntheticFti::build({{"FONTSML", 8}});
    CHECK(!mdk::decodeStreamBackdrop(bni.buf, noSys.buf, &err)
               .has_value());
    CHECK(err.find("SYS_PAL") != std::string::npos);
  }

  // Wrong record class: BG pointing at a non-image payload -> fail.
  {
    std::string err;
    auto bad = SyntheticBni::build({{"PAL", 768}, {"BG", 20}});
    const auto bd = mdk::inspectBniDirectory(bad.buf);
    const auto* br = mdk::findBniRecord(bd, "BG");
    bad.buf[br->payloadFileOffset] = std::byte{9};  // bogus dims
    CHECK(!mdk::decodeStreamBackdrop(bad.buf, fti.buf, &err)
               .has_value());
    CHECK(!err.empty());
  }

  // Malformed palette record (wrong size) -> fail.
  {
    std::string err;
    auto badPal = SyntheticBni::build({{"PAL", 700}, {"BG", 4 + 12}});
    const auto bd = mdk::inspectBniDirectory(badPal.buf);
    const auto* br = mdk::findBniRecord(bd, "BG");
    badPal.buf[br->payloadFileOffset + 0] = std::byte{4};
    badPal.buf[br->payloadFileOffset + 2] = std::byte{3};
    CHECK(!mdk::decodeStreamBackdrop(badPal.buf, fti.buf, &err)
               .has_value());
    CHECK(!err.empty());
  }

  // Malformed container inputs -> fail, not crash.
  {
    std::string err;
    const std::array<std::byte, 16> junk = {std::byte{7}};
    CHECK(!mdk::decodeStreamBackdrop(junk, fti.buf, &err).has_value());
    CHECK(!mdk::decodeStreamBackdrop(bni.buf, junk, &err).has_value());
  }
}

void test_indexed_image_blit() {
  // Centered 1:1 placement inside the 600x360 work surface.
  {
    const auto pal = makePalettedImage(4, 2);
    const auto img = mdk::decodeBniPalettedImage(pal);
    CHECK(img.has_value());
    mdk::IndexedFramebuffer fb(600, 360);
    fb.clear(0);
    mdk::Palette palette;
    mdk::blitIndexedImage(*img, fb, palette);
    const int ox = (600 - 4) / 2, oy = (360 - 2) / 2;
    CHECK(fb.at(ox, oy) == 0);                 // image (0,0)
    CHECK(fb.at(ox + 3, oy + 1) == 7);         // image (3,1)
    CHECK(fb.at(0, 0) == 0);                   // outside stays 0
    CHECK(fb.at(ox - 1, oy) == 0);
    const auto c = palette.get(42);
    CHECK(c.r == 42 && c.g == 213 && c.b == 84 && c.a == 255);
  }

  // Exact-fit image fills the whole surface (the MDKOPT 600x360 case
  // — no border pixels remain).
  {
    const auto pal = makePalettedImage(600, 360);
    const auto img = mdk::decodeBniPalettedImage(pal);
    CHECK(img.has_value());
    mdk::IndexedFramebuffer fb(600, 360);
    fb.clear(0xaa);
    mdk::Palette palette;
    mdk::blitIndexedImage(*img, fb, palette);
    CHECK(fb.at(0, 0) == 0);
    CHECK(fb.at(599, 359) == 191);  // (216000-1) % 256
    CHECK(fb.at(300, 180) == static_cast<std::uint8_t>(
                              (180 * 600 + 300) % 256));
  }

  // Oversized image: uniform nearest-neighbor downscale, centered.
  // 4x4 into 8x4: height-bound -> drawn 4x4 at ox=2 (identity rows).
  {
    mdk::IndexedImage img;
    img.width = 4;
    img.height = 4;
    img.stride = 4;
    img.pixels = {0, 0, 0, 0,
                  0, 9, 9, 0,
                  0, 9, 9, 0,
                  0, 0, 0, 0};
    mdk::IndexedFramebuffer fb(8, 4);
    fb.clear(0xee);
    mdk::Palette palette;
    mdk::blitIndexedImage(img, fb, palette);
    CHECK(fb.at(2, 1) == 0);
    CHECK(fb.at(3, 1) == 9);
    CHECK(fb.at(4, 1) == 9);
    CHECK(fb.at(5, 1) == 0);
    CHECK(fb.at(0, 0) == 0xee);  // untouched border
    CHECK(!img.hasPalette);      // no palette -> palette untouched
    CHECK(palette.get(0).r == 0 && palette.get(0).a == 255);
  }

  // 8x4 into 4x2: exact 2:1 nearest resample — fb(x,y) == img(2x,2y).
  {
    mdk::IndexedImage img;
    img.width = 8;
    img.height = 4;
    img.stride = 8;
    img.pixels.assign(32, 0);
    img.pixels[2 * 8 + 2] = 9;  // img(2,2) -> fb(1,1)
    img.pixels[1 * 8 + 7] = 5;  // img(7,1) — odd coords, never sampled
    mdk::IndexedFramebuffer fb(4, 2);
    fb.clear(0xee);
    mdk::Palette palette;
    mdk::blitIndexedImage(img, fb, palette);
    CHECK(fb.at(1, 1) == 9);
    CHECK(fb.at(0, 0) == 0 && fb.at(3, 0) == 0 && fb.at(3, 1) == 0);
  }
}

void test_data_root() {
  namespace fs = std::filesystem;
  const fs::path tmp =
      fs::temp_directory_path() / "mdk_native_test_dataroot";
  fs::remove_all(tmp);
  fs::create_directories(tmp / "MISC");
  fs::create_directories(tmp / "Traverse" / "LEVEL7");
  {
    std::ofstream(tmp / "MISC" / "LOAD_7.LBB", std::ios::binary)
        << "synthetic";
    std::ofstream(tmp / "Traverse" / "LEVEL7" / "level7o.mto",
                  std::ios::binary)
        << "data";
  }

  std::string err;
  auto root = mdk::DataRoot::open(tmp, &err);
  CHECK(root.has_value());

  // Exact-case resolution.
  err.clear();
  auto p = root->resolve("MISC/LOAD_7.LBB", &err);
  CHECK(p.has_value());
  CHECK(p->filename() == "LOAD_7.LBB");

  // Case-insensitive + mixed separators (the DOS-style request).
  CHECK(root->resolve("misc\\load_7.lbb").has_value());
  CHECK(root->resolve("Misc\\Load_7.LbB").has_value());
  CHECK(root->resolve("traverse/level7/LEVEL7O.MTO").has_value());
  CHECK(root->resolve("TRAVERSE\\LEVEL7/level7o.MTO").has_value());

  // '.' components normalize away.
  CHECK(root->resolve("./MISC/./LOAD_7.LBB").has_value());

  // Missing file / missing directory.
  CHECK(!root->resolve("MISC/LOAD_9.LBB", &err).has_value());
  CHECK(!err.empty());
  CHECK(!root->resolve("NOSUCH/X.BIN").has_value());

  // Escape attempts: all rejected.
  CHECK(!root->resolve("../foo").has_value());
  CHECK(!root->resolve("../../etc/passwd").has_value());
  CHECK(!root->resolve("MISC/../../etc/passwd").has_value());
  CHECK(!root->resolve("MISC/../LOAD_7.LBB").has_value()); // any '..'
  CHECK(!root->resolve("/etc/passwd").has_value());
  CHECK(!root->resolve("\\abs\\path").has_value());
  CHECK(!root->resolve("C:\\MDK.CFG").has_value());
  CHECK(!root->resolve("c:relative").has_value());
  CHECK(!root->resolve("").has_value());
  CHECK(root->resolve("MISC\\").has_value()); // trailing sep -> the dir

  // Read access: bounded + read-only.
  const auto bytes = root->readFile("misc/load_7.lbb", 64, &err);
  CHECK(bytes && bytes->size() == 9);
  CHECK((*bytes)[0] == std::byte{'s'});
  CHECK(!root->readFile("misc/load_7.lbb", 4).has_value()); // over bound
  const auto head = root->readPrefix("misc\\LOAD_7.LBB", 4);
  CHECK(head && head->size() == 4);
  const auto sz = root->fileSize("MISC/LOAD_7.LBB");
  CHECK(sz && *sz == 9);
  CHECK(!root->fileSize("MISC", &err).has_value()); // dir is not a file

  // Symlink escaping the root must be rejected even though the
  // component names resolve cleanly.
  const fs::path outside = tmp.parent_path() / "mdk_native_outside.bin";
  {
    std::ofstream(outside, std::ios::binary) << "x";
  }
  std::error_code ec;
  fs::create_symlink(outside, tmp / "MISC" / "LINK.BIN", ec);
  if (!ec) {
    CHECK(!root->resolve("misc/link.bin").has_value());
    fs::remove(tmp / "MISC" / "LINK.BIN");
  }
  fs::remove(outside);

  // Case-insensitive ambiguity: on a case-sensitive volume both files
  // can coexist; on case-insensitive volumes they cannot be created.
  {
    std::ofstream(tmp / "MISC" / "AMBIG.BIN", std::ios::binary) << "a";
  }
  {
    std::ofstream c1(tmp / "MISC" / "CASE1.BIN", std::ios::binary);
    c1 << "1";
  }
  {
    std::ofstream c2(tmp / "MISC" / "case1.bin", std::ios::binary);
    c2 << "2";
  }
  std::size_t caseEntries = 0;
  for (const auto& e : fs::directory_iterator(tmp / "MISC")) {
    const auto n = e.path().filename().string();
    if (n == "CASE1.BIN" || n == "case1.bin") {
      ++caseEntries;
    }
  }
  if (caseEntries == 2) {
    CHECK(!root->resolve("misc/case1.bin", &err).has_value());
  }

  auto missing = mdk::DataRoot::open(tmp / "no_such_dir", &err);
  CHECK(!missing.has_value());
  CHECK(!err.empty());

  const fs::path filePath = tmp / "a_file.txt";
  { FILE* f = std::fopen(filePath.c_str(), "w"); std::fclose(f); }
  auto notDir = mdk::DataRoot::open(filePath, &err);
  CHECK(!notDir.has_value());

  fs::remove_all(tmp);
}

void test_mode_dispatch() {
  mdk::ModeDispatcher d;
  CHECK(d.primary() == mdk::mode::nativeBoot);
  CHECK(!d.quitRequested());

  int bootFrames = 0, shellFrames = 0;
  d.on(mdk::mode::nativeBoot,
       [&](const mdk::FrameContext&) {
         ++bootFrames;
         d.setPrimary(mdk::mode::nativeShell);
       });
  d.on(mdk::mode::nativeShell,
       [&](const mdk::FrameContext& ctx) {
         ++shellFrames;
         CHECK(ctx.frameIndex >= 1); // shell never runs on frame 0
         if (shellFrames == 3) {
           d.requestQuit();
         }
       });

  mdk::FrameContext ctx;
  for (ctx.frameIndex = 0; ctx.frameIndex < 10 && !d.quitRequested();
       ++ctx.frameIndex) {
    d.dispatch(ctx);
  }
  CHECK(bootFrames == 1);
  CHECK(shellFrames == 3);
  CHECK(d.quitRequested());
  CHECK(d.sub() == mdk::mode::subModeNone);

  // Original observed mode ids remain untouched evidence values.
  CHECK(mdk::mode::observed::frontend == 0);
  CHECK(mdk::mode::observed::traversal == 3);
  CHECK(mdk::mode::observed::cinematic == 8);
}

// Synthetic FONTSML-layout font builder (no original data). Payload
// layout mirrors the OBSERVED/CODE-CORROBORATED structure:
// u32 offsetTable[256] then glyph records
// {s8 top, s8 bottom, u8 width, u8 px[width*(top+bottom+1)]}.
struct SyntheticFont {
  std::vector<std::byte> buf;

  void put32(std::size_t off, std::uint32_t v) {
    buf[off + 0] = static_cast<std::byte>(v & 0xff);
    buf[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    buf[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    buf[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  }
  std::uint32_t addGlyph(std::int8_t top, std::int8_t bottom,
                         std::uint8_t width,
                         const std::vector<std::uint8_t>& px) {
    const std::uint32_t off = static_cast<std::uint32_t>(buf.size());
    buf.push_back(static_cast<std::byte>(top));
    buf.push_back(static_cast<std::byte>(bottom));
    buf.push_back(static_cast<std::byte>(width));
    for (const auto v : px) {
      buf.push_back(static_cast<std::byte>(v));
    }
    return off;
  }
  static SyntheticFont make() {
    SyntheticFont f;
    f.buf.assign(0x400, std::byte{0});
    return f;
  }
};

void test_fti_font() {
  std::string err;

  // Minimal valid font: 'A' -> 2x2 glyph; ' ' and 0 unmapped.
  {
    auto f = SyntheticFont::make();
    f.put32(0x41 * 4, f.addGlyph(1, 0, 2, {9, 0, 0, 10}));
    const auto font = mdk::decodeFtiFont(f.buf, &err);
    CHECK(font && font->mappedCount == 1);
    CHECK(font->firstMapped == 0x41 && font->lastMapped == 0x41);
    const auto* g = font->glyphFor(0x41);
    CHECK(g && g->top == 1 && g->bottom == 0 && g->width == 2);
    CHECK(g->rows() == 2 && g->pixels.size() == 4);
    CHECK(g->pixels[0] == 9 && g->pixels[3] == 10);  // verbatim
    CHECK(!font->glyphFor(' ') && !font->glyphFor(0));
    CHECK(font->trailingBytes == 0);
    CHECK(font->glyphDataStart == 0x400);
  }

  // Signed header bytes: bottom = -1 keeps the whole body on/above
  // the pen row (the '!' shape in the real fonts).
  {
    auto f = SyntheticFont::make();
    f.put32('!' * 4, f.addGlyph(3, -1, 1, {7, 7, 7}));
    const auto font = mdk::decodeFtiFont(f.buf, &err);
    CHECK(font);
    const auto* g = font->glyphFor('!');
    CHECK(g && g->rows() == 3 && g->pixels.size() == 3);
  }

  // Draw rule: bitmap row 0 lands on penY - top; byte 0 skips;
  // nonzero bytes overwrite verbatim.
  {
    auto f = SyntheticFont::make();
    f.put32('A' * 4, f.addGlyph(2, 0, 2, {1, 2, 0, 3, 4, 5}));
    f.put32('x' * 4, f.addGlyph(1, 1, 1, {6, 8, 7}));  // descender
    const auto font = mdk::decodeFtiFont(f.buf, &err);
    CHECK(font);
    mdk::IndexedFramebuffer fb(32, 16);
    fb.clear(0);
    mdk::drawFtiGlyph(*font->glyphFor('A'), fb, 4, 10);
    // Rows land at penY - top .. penY + bottom = 8 .. 10.
    CHECK(fb.at(4, 8) == 1 && fb.at(5, 8) == 2);
    CHECK(fb.at(4, 9) == 0 && fb.at(5, 9) == 3);  // 0 byte = skip
    CHECK(fb.at(4, 10) == 4 && fb.at(5, 10) == 5);
    mdk::drawFtiGlyph(*font->glyphFor('x'), fb, 0, 10);
    CHECK(fb.at(0, 9) == 6 && fb.at(0, 10) == 8);
    CHECK(fb.at(0, 11) == 7);  // row below the pen
    // Out-of-range pen positions clip per-pixel, never UB.
    mdk::drawFtiGlyph(*font->glyphFor('A'), fb, -1, 0);
  }

  // String draw + measure: advance = glyph width; unmapped bytes use
  // the caller's missing-glyph advance (4 FONTSML / 6 FONTBIG paths).
  {
    auto f = SyntheticFont::make();
    f.put32('A' * 4, f.addGlyph(1, 0, 2, {1, 1, 1, 1}));
    f.put32('B' * 4, f.addGlyph(1, 0, 3, {2, 2, 2, 2, 2, 2}));
    const auto font = mdk::decodeFtiFont(f.buf, &err);
    CHECK(font);
    mdk::IndexedFramebuffer fb(32, 8);
    fb.clear(0);
    const int end = mdk::drawFtiText(*font, "A B", fb, 0, 4, 4);
    CHECK(end == 2 + 4 + 3);
    CHECK(mdk::measureFtiText(*font, "A B", 4) == 9);
    CHECK(fb.at(0, 3) == 1 && fb.at(6, 3) == 2);  // B starts at x=6
    CHECK(mdk::measureFtiText(*font, "A B", 6) == 11);
  }

  // Non-positive row count: legal advance-only glyph — the original
  // skips the draw loop but still advances the pen; consumed extent
  // is the 3-byte header only.
  {
    auto f = SyntheticFont::make();
    f.put32(0x80 * 4, f.addGlyph(0, -3, 2, {}));  // rows = -2
    const auto font = mdk::decodeFtiFont(f.buf, &err);
    CHECK(font);
    const auto* g = font->glyphFor(0x80);
    CHECK(g && g->rows() == -2 && g->pixels.empty());
    CHECK(g->consumedBytes() == 3);
  }

  // Trailing slack after the last glyph is tolerated (OBSERVED:
  // FONTBIG ends with 2 pad bytes).
  {
    auto f = SyntheticFont::make();
    f.put32('A' * 4, f.addGlyph(0, 0, 1, {7}));
    f.buf.push_back(std::byte{0xaa});
    f.buf.push_back(std::byte{0xbb});
    const auto font = mdk::decodeFtiFont(f.buf, &err);
    CHECK(font && font->trailingBytes == 2);
  }

  // Duplicate offsets: two codes may share one glyph record (the
  // original dereferences each entry independently).
  {
    auto f = SyntheticFont::make();
    const std::uint32_t off = f.addGlyph(0, 0, 1, {5});
    f.put32('A' * 4, off);
    f.put32('B' * 4, off);
    const auto font = mdk::decodeFtiFont(f.buf, &err);
    CHECK(font && font->mappedCount == 2);
    CHECK(!font->offsetsUnique);
    CHECK(font->glyphFor('A')->pixels[0] == 5);
    CHECK(font->glyphFor('B')->pixels[0] == 5);
  }

  // Non-monotonic offsets still decode but are reported unsorted.
  {
    auto f = SyntheticFont::make();
    const std::uint32_t o1 = f.addGlyph(0, 0, 1, {1});
    const std::uint32_t o2 = f.addGlyph(0, 0, 1, {2});
    f.put32('B' * 4, o1);  // 'B' -> earlier offset, 'A' -> later
    f.put32('A' * 4, o2);
    const auto font = mdk::decodeFtiFont(f.buf, &err);
    CHECK(font && !font->offsetsSortedAscending);
    CHECK(font->glyphFor('A')->pixels[0] == 2);
    CHECK(font->glyphFor('B')->pixels[0] == 1);
  }

  // Digest determinism.
  {
    auto f1 = SyntheticFont::make();
    f1.put32('A' * 4, f1.addGlyph(1, 0, 2, {1, 2, 3, 4}));
    auto f2 = SyntheticFont::make();
    f2.put32('A' * 4, f2.addGlyph(1, 0, 2, {1, 2, 3, 4}));
    auto f3 = SyntheticFont::make();
    f3.put32('A' * 4, f3.addGlyph(1, 0, 2, {1, 2, 3, 9}));
    const auto a = mdk::decodeFtiFont(f1.buf, &err);
    const auto b = mdk::decodeFtiFont(f2.buf, &err);
    const auto c = mdk::decodeFtiFont(f3.buf, &err);
    CHECK(a && b && c);
    CHECK(mdk::ftiFontDigest(*a) == mdk::ftiFontDigest(*b));
    CHECK(mdk::ftiFontDigest(*a) != mdk::ftiFontDigest(*c));
  }

  // --- malformed inputs ---

  // Truncated table.
  {
    const std::vector<std::byte> shortBuf(0x100, std::byte{0});
    CHECK(!mdk::decodeFtiFont(shortBuf, &err));
  }

  // All-zero table: no mapped glyphs -> not a font resource.
  {
    const std::vector<std::byte> zeroTable(0x400, std::byte{0});
    CHECK(!mdk::decodeFtiFont(zeroTable, &err));
  }

  // Offset pointing into the table region.
  {
    auto f = SyntheticFont::make();
    f.put32('A' * 4, 0x100);  // < 0x400
    CHECK(!mdk::decodeFtiFont(f.buf, &err));
  }

  // Offset past the payload.
  {
    auto f = SyntheticFont::make();
    f.put32('A' * 4, 0xffff);
    CHECK(!mdk::decodeFtiFont(f.buf, &err));
  }

  // Truncated glyph header (offset lands on the last 2 bytes).
  {
    auto f = SyntheticFont::make();
    f.buf.push_back(std::byte{1});
    f.buf.push_back(std::byte{2});
    f.put32('A' * 4, 0x400);
    CHECK(!mdk::decodeFtiFont(f.buf, &err));
  }

  // Bitmap overrun: header claims 4x4 (16 bytes) but only 2 remain.
  {
    auto f = SyntheticFont::make();
    f.put32('A' * 4, f.addGlyph(3, 0, 4, {1, 2}));
    CHECK(!mdk::decodeFtiFont(f.buf, &err));
  }
}

void test_input_state() {
  mdk::InputState in;
  in.beginFrame();

  in.key(41, true, false);   // down
  in.key(41, true, true);    // repeat
  CHECK(in.keyDown(41));
  CHECK(in.keyEvents().size() == 2);
  in.key(41, false, false);  // up
  CHECK(!in.keyDown(41));
  CHECK(in.keyEvents().size() == 3);

  in.mouseMotion(5.0f, -3.0f);
  in.mouseMotion(1.5f, 0.5f);
  CHECK(near(in.mouseDx(), 6.5) && near(in.mouseDy(), -2.5));

  in.mouseButton(1, true);
  CHECK(in.mouseButtonDown(1));
  in.mouseButton(1, false);
  CHECK(!in.mouseButtonDown(1));
  CHECK(in.buttonEvents().size() == 2);

  in.mouseWheel(0.25f, 1.0f, 0, 1);
  in.mouseWheel(0.0f, -0.5f, 0, -1);
  CHECK(near(in.wheelX(), 0.25) && near(in.wheelY(), 0.5));
  CHECK(in.wheelTicksX() == 0 && in.wheelTicksY() == 0);

  // beginFrame clears per-frame deltas but keeps held state.
  in.key(30, true, false);
  in.beginFrame();
  CHECK(in.keyEvents().empty());
  CHECK(in.mouseDx() == 0 && in.wheelY() == 0);
  CHECK(in.keyDown(30));
}

// Phase 4D sprite-record builder: {u32 blockBytes, u32 count,
// u32 offs[count]} then frames {u16 w, u16 h, s16 hx, s16 hy, stream}.
struct SyntheticSprite {
  std::vector<std::byte> buf;

  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      buf.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
    }
  }
  void u16(std::uint16_t v) {
    buf.push_back(static_cast<std::byte>(v & 0xff));
    buf.push_back(static_cast<std::byte>(v >> 8));
  }
  void frame(std::uint16_t w, std::uint16_t h, std::int16_t hx,
             std::int16_t hy, const std::vector<std::uint8_t>& stream) {
    u16(w);
    u16(h);
    u16(static_cast<std::uint16_t>(hx));
    u16(static_cast<std::uint16_t>(hy));
    for (const auto v : stream) {
      buf.push_back(static_cast<std::byte>(v));
    }
  }
  // One-frame record with the ARROW shape: offsets[0] = 8 (frame at
  // payload+4+8).
  static SyntheticSprite make1(std::uint16_t w, std::uint16_t h,
                               std::int16_t hx, std::int16_t hy,
                               const std::vector<std::uint8_t>& stream,
                               std::uint32_t blockBytes = 0) {
    SyntheticSprite s;
    s.u32(blockBytes);  // reported only; original never reads it
    s.u32(1);           // frameCount
    s.u32(8);           // offsets[0]
    s.frame(w, h, hx, hy, stream);
    return s;
  }
};

void test_fti_sprite() {
  std::string err;

  // Header + single frame decode; stats gathered from the stream.
  {
    // row0: literal 3px {1,0,2}; row break; row1: run x4 of 9; end.
    auto s = SyntheticSprite::make1(
        8, 4, 0, 0,
        {0x02, 1, 0, 2, 0xfe, 0x80, 9, 0xff}, 30);
    const auto sp = mdk::decodeFtiSprite(s.buf, &err);
    CHECK(sp && sp->blockBytes == 30 && sp->frames.size() == 1);
    const auto& f = sp->frames[0];
    CHECK(f.width == 8 && f.height == 4 && f.hotspotX == 0 &&
          f.hotspotY == 0);
    CHECK(f.stream.size() == 8);
    CHECK(f.literalPackets == 1 && f.runPackets == 1 &&
          f.rowBreaks == 1);
    CHECK(f.pixelAdvances == 7 && f.opaqueWrites == 6);
    CHECK(f.maxPixelIndex == 9);
    CHECK(mdk::ftiSpriteDigest(*sp) != 0);
    CHECK(mdk::ftiSpriteDigest(*sp) ==
          mdk::ftiSpriteDigest(*sp));  // deterministic
  }

  // Draw semantics: literal bytes verbatim, 0 transparent, run fills,
  // transparent run skips, 0xfe next row, 0xff stops.
  {
    auto s = SyntheticSprite::make1(
        8, 3, 0, 0,
        {0x02, 5, 0, 7,       // row0: 5, skip, 7
         0x84, 0,             // row0: transparent run x8 (cols 3..10)
         0xfe,
         0x83, 9,             // row1: run x7 of 9 (cols 0..6)
         0x01, 3, 4,          // row1: literal 2px at cols 7,8
         0xff});
    const auto sp = mdk::decodeFtiSprite(s.buf, &err);
    CHECK(sp);
    mdk::IndexedFramebuffer fb(16, 8);
    fb.clear(0x55);
    mdk::blitFtiSpriteFrame(sp->frames[0], fb, 2, 1);
    CHECK(fb.at(2, 1) == 5 && fb.at(4, 1) == 7);
    CHECK(fb.at(3, 1) == 0x55);          // literal 0 = transparent
    for (int x = 5; x <= 12; ++x) {
      CHECK(fb.at(x, 1) == 0x55);        // transparent run skips
    }
    for (int x = 2; x <= 8; ++x) {
      CHECK(fb.at(x, 2) == 9);           // run x7
    }
    CHECK(fb.at(9, 2) == 3 && fb.at(10, 2) == 4);  // literal after run
    CHECK(fb.at(2, 3) == 0x55);          // stream ended
  }

  // Hotspot: dest = (x - hx, y - hy) like FUN_00409760.
  {
    auto s = SyntheticSprite::make1(2, 1, 2, 3, {0x01, 9, 9, 0xff});
    const auto sp = mdk::decodeFtiSprite(s.buf, &err);
    CHECK(sp);
    mdk::IndexedFramebuffer fb(16, 8);
    fb.clear(0);
    mdk::blitFtiSpriteFrame(sp->frames[0], fb, 10, 6);
    CHECK(fb.at(8, 3) == 9 && fb.at(9, 3) == 9);
    CHECK(fb.at(10, 3) == 0);
  }

  // Run packet count classes: 0x80 -> 4 copies; 0xfd -> 129.
  {
    auto s = SyntheticSprite::make1(
        8, 2, 0, 0, {0x80, 7, 0xfd, 8, 0xff});
    const auto sp = mdk::decodeFtiSprite(s.buf, &err);
    CHECK(sp && sp->frames[0].pixelAdvances == 4 + 129);
    mdk::IndexedFramebuffer fb(140, 4);
    fb.clear(0);
    mdk::blitFtiSpriteFrame(sp->frames[0], fb, 0, 0);
    CHECK(fb.at(3, 0) == 7 && fb.at(4, 0) == 8 && fb.at(132, 0) == 8);
  }

  // Malformed streams rejected at decode: truncated literal, truncated
  // run value, missing 0xff, zero frames, offset out of bounds,
  // truncated header.
  {
    auto bad1 = SyntheticSprite::make1(8, 1, 0, 0,
                                       {0x05, 1, 2});  // lit wants 6
    CHECK(!mdk::decodeFtiSprite(bad1.buf, &err));
    auto bad2 = SyntheticSprite::make1(8, 1, 0, 0, {0x80});  // no value
    CHECK(!mdk::decodeFtiSprite(bad2.buf, &err));
    auto bad3 = SyntheticSprite::make1(8, 1, 0, 0,
                                       {0x00, 1, 0xfe});  // no 0xff
    CHECK(!mdk::decodeFtiSprite(bad3.buf, &err));
    SyntheticSprite bad4;
    bad4.u32(0); bad4.u32(0);  // count = 0
    CHECK(!mdk::decodeFtiSprite(bad4.buf, &err));
    SyntheticSprite bad5;
    bad5.u32(0); bad5.u32(1); bad5.u32(0x400);  // frame off OOB
    CHECK(!mdk::decodeFtiSprite(bad5.buf, &err));
    std::vector<std::byte> bad6 = {std::byte{1}, std::byte{0}};
    CHECK(!mdk::decodeFtiSprite(bad6, &err));
  }

  // Trailing bytes after 0xff are tolerated and reported (the real
  // ARROW record has one pad byte).
  {
    auto s = SyntheticSprite::make1(1, 1, 0, 0, {0x00, 9, 0xff});
    s.buf.push_back(std::byte{0xaa});
    const auto sp = mdk::decodeFtiSprite(s.buf, &err);
    CHECK(sp && sp->trailingBytes == 1);
  }

  // Clipping — mirrors FUN_00415ff0 exactly (bounds-hardened only):
  {
    auto s = SyntheticSprite::make1(4, 4, 0, 0,
                                    {0x83, 9, 0xfe, 0x83, 8, 0xfe,
                                     0x83, 7, 0xfe, 0x83, 6, 0xff});
    const auto sp = mdk::decodeFtiSprite(s.buf, &err);
    CHECK(sp);
    mdk::IndexedFramebuffer fb(16, 8);
    fb.clear(0);
    // Left edge: x=-2 -> first two pixels of each run skipped; the
    // run writes cols -2..4 so fb cols 0..4 land.
    mdk::blitFtiSpriteFrame(sp->frames[0], fb, -2, 0);
    CHECK(fb.at(0, 0) == 9 && fb.at(4, 0) == 9 && fb.at(5, 0) == 0);
    // Top edge: y=-1 -> row0 consumed silently; sprite row1 lands on
    // fb row 0.
    fb.clear(0);
    mdk::blitFtiSpriteFrame(sp->frames[0], fb, 0, -1);
    CHECK(fb.at(0, 0) == 8 && fb.at(3, 0) == 8 && fb.at(0, 1) == 7);
    // Right edge, x>=0: x+w>width -> NOTHING drawn (all-or-nothing).
    fb.clear(0);
    mdk::blitFtiSpriteFrame(sp->frames[0], fb, 14, 0);  // 14+4>16
    CHECK(fb.at(14, 0) == 0);
    // Fully outside: no writes, no crash.
    mdk::blitFtiSpriteFrame(sp->frames[0], fb, -10, 0);
    mdk::blitFtiSpriteFrame(sp->frames[0], fb, 0, 9);   // y>=height
    mdk::blitFtiSpriteFrame(sp->frames[0], fb, 16, 0);  // x>=width
    // Bottom: 0xfe past the last row returns without writing.
    fb.clear(0);
    auto deep = SyntheticSprite::make1(
        2, 2, 0, 0,
        {0x01, 9, 9, 0xfe, 0x01, 8, 8, 0xfe, 0x01, 7, 7, 0xff});
    const auto dsp = mdk::decodeFtiSprite(deep.buf, &err);
    CHECK(dsp);
    mdk::blitFtiSpriteFrame(dsp->frames[0], fb, 0, 7);  // row1 @y8>fb
    CHECK(fb.at(0, 7) == 9 && fb.at(1, 7) == 9);
    // Row spill: packets may pass the declared row width into the next
    // row (the stream is trusted — original has no per-row clip).
    fb.clear(0);
    auto wide = SyntheticSprite::make1(
        4, 2, 0, 0,
        {0xfd, 5,   // run x129 at x=0 spills to row 1
         0xff});
    const auto wsp = mdk::decodeFtiSprite(wide.buf, &err);
    CHECK(wsp);
    mdk::IndexedFramebuffer fb2(40, 4);
    fb2.clear(0);
    mdk::blitFtiSpriteFrame(wsp->frames[0], fb2, 0, 0);
    // 129 advances from flat offset 0: rows 0-2 fully, row3 cols 0-8.
    CHECK(fb2.at(39, 0) == 5 && fb2.at(0, 1) == 5 &&
          fb2.at(0, 3) == 5 && fb2.at(8, 3) == 5 && fb2.at(9, 3) == 0);
  }
}

void test_frontend_menu() {
  std::string err;

  // Synthetic resources: 600x360 backdrop of index 0x55 with a
  // ramp palette; a FONTBIG-shaped font mapping every needed byte to a
  // uniform 2x2 glyph; a 2x2 arrow sprite of index 77.
  mdk::IndexedImage backdrop;
  backdrop.width = 600;
  backdrop.height = 360;
  backdrop.stride = 600;
  backdrop.pixels.assign(600 * 360, 0x55);
  backdrop.hasPalette = true;
  for (int i = 0; i < 256; ++i) {
    backdrop.palette[i] = {std::uint8_t(i), std::uint8_t(255 - i),
                           std::uint8_t(i)};
  }

  auto f = SyntheticFont::make();
  const char* needed = "ContinueNew GamSavdpQitlOs";  // OPT0..4 chars
  for (const char* c = needed; *c; ++c) {
    f.put32(static_cast<std::uint8_t>(*c) * 4,
            f.addGlyph(1, 0, 2, {9, 9, 9, 9}));
  }
  const auto font = mdk::decodeFtiFont(f.buf, &err);
  CHECK(font);

  auto arrowS = SyntheticSprite::make1(
      2, 2, 0, 0, {0x01, 77, 77, 0xfe, 0x01, 77, 77, 0xff});
  const auto arrow = mdk::decodeFtiSprite(arrowS.buf, &err);
  CHECK(arrow && arrow->frame(0));

  const std::string_view opts[5] = {"Continue", "New Game",
                                    "Saved Game", "Options", "Quit"};

  // Spec shape: saves -> 5 items y=31+36i, sel 0; no saves -> 4 items
  // y=31+36(i-1), sel 1; arrow at the reset mouse position.
  {
    const auto spec = mdk::frontendMenuSpec(true);
    CHECK(spec.items.size() == 5 && spec.arrowX == 300 &&
          spec.arrowY == 180);
    for (int i = 0; i < 5; ++i) {
      CHECK(spec.items[i].optIndex == i &&
            spec.items[i].y == 31 + 36 * i &&
            spec.items[i].selected == (i == 0));
    }
    const auto spec2 = mdk::frontendMenuSpec(false);
    CHECK(spec2.items.size() == 4);
    for (int i = 0; i < 4; ++i) {
      CHECK(spec2.items[i].optIndex == i + 1 &&
            spec2.items[i].y == 31 + 36 * i &&
            spec2.items[i].selected == (i + 1 == 1));
    }
  }

  // Composition: backdrop first, text over it, arrow last.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    fb.clear(0);
    auto spec = mdk::frontendMenuSpec(true);
    // Park the arrow ON the selected label's first glyph to prove
    // draw order (arrow last). With every OPT char mapped (including
    // ' ') all five measure 2px/char: "Saved Game" is widest at 20
    // -> maxW=20 -> xArg=10; "Continue" w=16 at scale 1.0
    // -> x = trunc(10 - 8) = 2; glyph top=1 -> rows 30..31, cols 2,3.
    spec.arrowX = 2;
    spec.arrowY = 30;
    CHECK(mdk::renderFrontendMenuFrame(fb, palette, backdrop, *font,
                                       *arrow->frame(0), opts, spec,
                                       &err));
    CHECK(fb.at(0, 0) == 0x55);              // backdrop landed
    CHECK(palette.get(3).r == 3);            // embedded palette bound
    CHECK(fb.at(2, 30) == 77);   // arrow overwrote 'C' pixels
    CHECK(fb.at(3, 31) == 77);
    CHECK(fb.at(4, 30) == 9);    // 'o' (penX 4) intact
    // "New Game" unselected -> scale 0.65, y=67.
    // measure = 16; x = trunc(10 - 16*0.65*0.5) = trunc(4.8) = 4;
    // glyphTopY = trunc(67 - 0.65) = 66.
    CHECK(fb.at(4, 66) == 9 && fb.at(4, 67) == 9);
    // Arrow elsewhere must not have painted at the reset position.
    CHECK(fb.at(300, 180) == 0x55);
  }

  // Arrow last at its own position when not overlapping text.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    const auto spec = mdk::frontendMenuSpec(true);
    CHECK(mdk::renderFrontendMenuFrame(fb, palette, backdrop, *font,
                                       *arrow->frame(0), opts, spec,
                                       &err));
    CHECK(fb.at(300, 180) == 77 && fb.at(301, 180) == 77 &&
          fb.at(300, 181) == 77);
    CHECK(fb.at(299, 180) == 0x55);
    // Selected "Continue" at scale 1.0, x=2, rows 30-31, cols 2..17.
    CHECK(fb.at(2, 30) == 9 && fb.at(17, 30) == 9);
    // Unselected rows keep backdrop where no glyph lands.
    CHECK(fb.at(2, 67) == 0x55);
  }

  // Contract failures.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::IndexedImage small;
    small.width = small.height = small.stride = 4;
    small.pixels.assign(16, 0);
    const auto spec = mdk::frontendMenuSpec(true);
    CHECK(!mdk::renderFrontendMenuFrame(fb, palette, small, *font,
                                        *arrow->frame(0), opts, spec,
                                        &err));
    const std::string_view shortOpts[2] = {"A", "B"};
    CHECK(!mdk::renderFrontendMenuFrame(fb, palette, backdrop, *font,
                                        *arrow->frame(0), shortOpts,
                                        spec, &err));
  }

  // Scaled font edge cases: scale<=0.05 draws nothing; scale==1.0 is
  // the 1:1 path; trunc-toward-zero centering is honored.
  {
    mdk::IndexedFramebuffer fb(64, 32);
    fb.clear(0);
    auto f2 = SyntheticFont::make();
    f2.put32('A' * 4, f2.addGlyph(1, 0, 4, {9, 9, 9, 9, 9, 9, 9, 9}));
    const auto font2 = mdk::decodeFtiFont(f2.buf, &err);
    CHECK(font2);
    CHECK(mdk::drawFtiTextScaled(*font2, "AA", fb, 0, 10, 0.05f, 6) ==
          0);
    CHECK(fb.at(0, 9) == 0);                 // nothing drawn
    const int end1 = mdk::drawFtiTextScaled(*font2, "AA", fb, 0, 10,
                                            1.0f, 6);
    CHECK(end1 == 8 && fb.at(0, 9) == 9);    // 1:1 path
    fb.clear(0);
    // scale 0.5: srcStep = trunc(65536/0.5) = 131072 = 2.0 in 16.16;
    // srcColEnd = 4<<16 = 262144 -> output pixels = 2 per row.
    const int end2 = mdk::drawFtiTextScaled(*font2, "A", fb, 0, 10,
                                            0.5f, 6);
    // pen advance = trunc(0 + 4*0.5) = 2.
    CHECK(end2 == 2);
    // glyphTopY = trunc(10 - 1*0.5) = 9; rows land at 9,10.
    CHECK(fb.at(0, 9) == 9 && fb.at(1, 9) == 9 && fb.at(2, 9) == 0);
  }
}

// Phase 4E — FUN_0041dc90 interactive root-menu controller.
void test_frontend_controller() {
  // Entry state (FUN_0041d85c + FUN_00418798): saves -> sel 0,
  // no saves -> sel 1; mouse resets to (300,180); no pending action.
  {
    mdk::FrontendMenuController ctl(true);
    CHECK(ctl.savesExist() && ctl.selection() == 0);
    CHECK(ctl.mouseX() == 300 && ctl.mouseY() == 180);
    CHECK(ctl.pendingAction() == mdk::FrontendAction::None);
    CHECK(ctl.consumeAction() == mdk::FrontendAction::None);
    mdk::FrontendMenuController c2(false);
    CHECK(!c2.savesExist() && c2.selection() == 1);
  }

  // Keyboard walk + wrap, saves branch. DOWN: 0->1->2->3->4->0->1.
  {
    mdk::FrontendMenuController ctl(true);
    mdk::FrontendMenuInput in;
    const int want[6] = {1, 2, 3, 4, 0, 1};
    for (int i = 0; i < 6; ++i) {
      in = {};
      in.nextHeld = true;
      ctl.update(in);           // press fires immediately
      in = {};
      ctl.update(in);           // release resets the deadline
      ctl.endFrame(34.0);
      CHECK(ctl.selection() == want[i]);
    }
    // UP wraps 0 -> 4.
    mdk::FrontendMenuController c2(true);
    mdk::FrontendMenuInput in2;
    in2.prevHeld = true;
    c2.update(in2);
    CHECK(c2.selection() == 4);
    // UP+DOWN in one frame: prev runs first (0->4), next second
    // (4->5 -> wrap 0) — observed order in FUN_0041dc90.
    mdk::FrontendMenuController c3(true);
    mdk::FrontendMenuInput in3;
    in3.prevHeld = true;
    in3.nextHeld = true;
    c3.update(in3);
    CHECK(c3.selection() == 0);
  }

  // No-saves keyboard: index 0 unreachable. UP 1->4; DOWN 4->1.
  {
    mdk::FrontendMenuController ctl(false);
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 4);
    in = {};
    in.nextHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 1);
    // DOWN walk: 1->2->3->4->1->2 (index 0 skipped).
    in = {};
    ctl.update(in);  // release
    const int want[5] = {2, 3, 4, 1, 2};
    for (int i = 0; i < 5; ++i) {
      in = {};
      in.nextHeld = true;
      ctl.update(in);
      in = {};
      ctl.update(in);
      CHECK(ctl.selection() == want[i]);
    }
  }

  // Key repeat (FUN_004237b4): at dtMs=100/3 (~30fps, the original's
  // paced regime) rawDelta settles at 4 -> step=1 -> tick advances 1
  // per update. Press fires at tick 1 (deadline tick+30), then fires
  // at tick 32,36,40,44 (deadline tick+3).
  {
    mdk::FrontendMenuController ctl(true);
    const double kDt = 100.0 / 3.0;
    ctl.endFrame(kDt);  // seeds the virtual clock; no delta yet
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    // fires at updates {1,32,36,40,44}: sel 0->4->3->2->1->0
    const int fireAt[5] = {1, 32, 36, 40, 44};
    int exp = 0, fi = 0;
    for (int f = 1; f <= 44; ++f) {
      if (fi < 5 && f == fireAt[fi]) {
        exp -= 1;
        if (exp < 0) {
          exp = 4;
        }
        ++fi;
      }
      ctl.update(in);
      ctl.endFrame(kDt);
      CHECK(ctl.selection() == exp);
    }
    // Release resets the deadline: re-press fires immediately again.
    in = {};
    for (int f = 0; f < 3; ++f) {
      ctl.update(in);
    }
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 4);  // 0->4 wrap on immediate press fire
  }

  // Mouse accumulate + clamps (FUN_004187e0) and the tighter gate
  // clamps (FUN_0041dc90: 590/350).
  {
    mdk::FrontendMenuController ctl(true);
    mdk::FrontendMenuInput in;
    in.mouseDx = 400;
    in.mouseDy = 400;
    ctl.update(in);
    // accumulate clamps to 599/359, then the gate clamps to 590/350.
    CHECK(ctl.mouseX() == 590 && ctl.mouseY() == 350);
    CHECK(ctl.selection() == 0);  // y=350 -> band 9 -> invalid
    in = {};
    in.mouseDx = -1000;
    in.mouseDy = -1000;
    ctl.update(in);
    CHECK(ctl.mouseX() == 0 && ctl.mouseY() == 0);
    CHECK(ctl.selection() == 0);
  }

  // Mouse hit-test: gate + bands + boundaries + x-irrelevance.
  {
    mdk::FrontendMenuController ctl(true);
    mdk::FrontendMenuInput in;
    // Zero mouse input: gate closed — resting y=180 is band 4, yet
    // selection stays 0.
    ctl.update(in);
    CHECK(ctl.selection() == 0);
    in.mouseDy = 1;  // gate opens; y=181 -> band trunc(176/36)=4
    ctl.update(in);
    CHECK(ctl.mouseY() == 181 && ctl.selection() == 4);
    // Band boundaries (saves): [0,40]->0 [41,76]->1 [77,112]->2
    // [113,148]->3 [149,184]->4, >=185 invalid.
    const int ys[9] = {-141, 1, 35, 1, 35, 1, 35, 1, 35};
    const int wantY[9] = {40, 41, 76, 77, 112, 113, 148, 149, 184};
    const int wantSel[9] = {0, 1, 1, 2, 2, 3, 3, 4, 4};
    for (int i = 0; i < 9; ++i) {
      in = {};
      in.mouseDy = ys[i];
      ctl.update(in);
      CHECK(ctl.mouseY() == wantY[i] && ctl.selection() == wantSel[i]);
    }
    // y=185: band 5 -> invalid, selection holds.
    in = {};
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.mouseY() == 185 && ctl.selection() == 4);
    // y=0: trunc(-5/36)=0 -> band 0; x position never consulted.
    in = {};
    in.mouseDx = -590;
    in.mouseDy = -185;
    ctl.update(in);
    CHECK(ctl.mouseX() == 0 && ctl.mouseY() == 0 &&
          ctl.selection() == 0);
    // band == selection -> no change (y=31 is band 0, sel already 0).
    in = {};
    in.mouseDy = 31;
    ctl.update(in);
    CHECK(ctl.selection() == 0);
  }

  // No-saves hit-test: computed band +1, valid range 1..4. Last valid
  // y is 148 (band 3 -> index 4); y>=149 computes index 5 -> invalid.
  {
    mdk::FrontendMenuController ctl(false);
    mdk::FrontendMenuInput in;
    in.mouseDy = -160;  // 180 -> 20: band 0 -> index 1
    ctl.update(in);
    CHECK(ctl.mouseY() == 20 && ctl.selection() == 1);
    in = {};
    in.mouseDy = 21;    // 41: band 1 -> index 2
    ctl.update(in);
    CHECK(ctl.selection() == 2);
    in = {};
    in.mouseDy = 107;   // 148: band 3 -> index 4
    ctl.update(in);
    CHECK(ctl.selection() == 4);
    in = {};
    in.mouseDy = 1;     // 149: band 4 -> index 5 -> invalid
    ctl.update(in);
    CHECK(ctl.selection() == 4);
  }

  // Activation — Enter edge uses current selection; no mouse input
  // means no hit-test that frame.
  {
    mdk::FrontendMenuController ctl(true);
    mdk::FrontendMenuInput in;
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::FrontendAction::ContinueGame);
    CHECK(ctl.consumeAction() == mdk::FrontendAction::None);
  }

  // Activation — mouse button down-edge: same-frame hit-test first,
  // then dispatch. Latch: hold does not refire; release re-arms.
  {
    mdk::FrontendMenuController ctl(true);
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;  // 180 -> 139: band 3 -> Options
    ctl.update(in);
    CHECK(ctl.selection() == 3);
    in = {};
    in.mouseButtons = 0x1;
    ctl.update(in);  // gate: band 3 -> sel stays; latch fires
    CHECK(ctl.consumeAction() == mdk::FrontendAction::OpenOptions);
    // held: no refire
    ctl.update(in);
    CHECK(ctl.pendingAction() == mdk::FrontendAction::None);
    // release re-arms; press fires again
    in.mouseButtons = 0;
    ctl.update(in);
    in.mouseButtons = 0x1;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::FrontendAction::OpenOptions);
    // Any of the 4 nibble bits activates (bit3 = button 4).
    in.mouseButtons = 0;
    ctl.update(in);
    in.mouseButtons = 0x8;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::FrontendAction::OpenOptions);
  }

  // Per-item action mapping, saves branch. Nominal item rows sit
  // inside their bands; clicking selects then activates same frame.
  {
    const int bandY[5] = {31, 67, 103, 139, 175};
    const mdk::FrontendAction want[5] = {
        mdk::FrontendAction::ContinueGame, mdk::FrontendAction::NewGame,
        mdk::FrontendAction::SavedGame, mdk::FrontendAction::OpenOptions,
        mdk::FrontendAction::Quit};
    mdk::FrontendMenuController ctl(true);
    mdk::FrontendMenuInput in;
    int curY = 180;
    for (int i = 0; i < 5; ++i) {
      in = {};
      in.mouseDy = bandY[i] - curY;
      ctl.update(in);
      curY = bandY[i];
      CHECK(ctl.selection() == i);
      in = {};
      in.mouseButtons = 0x1;
      ctl.update(in);
      in = {};
      in.mouseButtons = 0;
      ctl.update(in);
      CHECK(ctl.consumeAction() == want[i]);
    }
  }

  // Same mapping, no-saves branch — only indices 1..4 reachable.
  {
    const int bandY[4] = {31, 67, 103, 139};
    const mdk::FrontendAction want[4] = {
        mdk::FrontendAction::NewGame, mdk::FrontendAction::SavedGame,
        mdk::FrontendAction::OpenOptions, mdk::FrontendAction::Quit};
    mdk::FrontendMenuController ctl(false);
    mdk::FrontendMenuInput in;
    int curY = 180;
    for (int i = 0; i < 4; ++i) {
      in = {};
      in.mouseDy = bandY[i] - curY;
      ctl.update(in);
      curY = bandY[i];
      CHECK(ctl.selection() == i + 1);
      in = {};
      in.mouseButtons = 0x1;
      ctl.update(in);
      in = {};
      in.mouseButtons = 0;
      ctl.update(in);
      CHECK(ctl.consumeAction() == want[i]);
    }
  }

  // Enter edge fires regardless of button state and activation beats
  // the attract edge in one frame.
  {
    mdk::FrontendMenuController ctl(true);
    mdk::FrontendMenuInput in;
    in.confirmEdge = true;
    in.attractEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::FrontendAction::ContinueGame);
    in = {};
    in.attractEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::FrontendAction::EnterAttract);
  }

  // Idle timer (DAT_0049aaa4): += deltaSec every frame, reset to 0
  // (then += same frame) on selection change.
  {
    mdk::FrontendMenuController ctl(true);
    mdk::FrontendMenuInput in;
    ctl.update(in);
    CHECK(near(ctl.idleSeconds(), 1.0 / 30.0, 1e-5));
    ctl.update(in);
    CHECK(near(ctl.idleSeconds(), 2.0 / 30.0, 1e-5));
    in.nextHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 1 &&
          near(ctl.idleSeconds(), 1.0 / 30.0, 1e-5));
  }

  // Tick advance: init step 1; dtMs=34 -> rawDelta 4 -> step 1;
  // dtMs=100 -> rawDelta 12 -> step 3; dtMs=200 -> rawDelta 24 ->
  // step 6 clamped to 4 (accum reset, smoothed pinned to 4.0).
  {
    mdk::FrontendMenuController ctl(true);
    mdk::FrontendMenuInput in;
    ctl.update(in);
    CHECK(ctl.tick() == 1);
    ctl.endFrame(34.0);  // virtual-clock seed only
    ctl.update(in);
    CHECK(ctl.tick() == 2);
    ctl.endFrame(34.0);  // first real delta
    ctl.update(in);
    CHECK(ctl.tick() == 3);
    ctl.endFrame(100.0);
    ctl.update(in);
    CHECK(ctl.tick() == 6);
    ctl.endFrame(100.0);
    ctl.update(in);
    CHECK(ctl.tick() == 9);
    ctl.endFrame(200.0);
    ctl.update(in);
    CHECK(ctl.tick() == 13);  // step clamped to 4
  }

  // Scale ramp (FUN_00423a24): cold-boot transition, growth to 1.0,
  // previous-item decay, and the mid-ramp reversal quirk. The ramp
  // adds the smoothed frame-unit value once per selected-item draw;
  // without endFrame calls it stays at the FUN_0042fb30 power-on
  // value 1.0, so acc advances exactly 1.0 per draw.
  {
    mdk::FrontendMenuController ctl(true);
    const int cx = 100;  // shared x_arg stand-in (same for all items)
    // cold boot: first selected draw transitions (0,0)->(cx,31)
    CHECK(near(ctl.itemScale(cx, 31, true), 0.65));
    CHECK(ctl.rampAccumulator() == 0.0f);
    // acc -> 1,2,3,4,5 => scales 0.72,0.79,0.86,0.93,1.0
    const float grow[5] = {0.72f, 0.79f, 0.86f, 0.93f, 1.0f};
    for (int f = 0; f < 5; ++f) {
      CHECK(near(ctl.itemScale(cx, 31, true), grow[f], 1e-5));
    }
    // Non-selected items: 0.65 — including the (0,0) non-key.
    CHECK(near(ctl.itemScale(cx, 67, false), 0.65));
    CHECK(near(ctl.itemScale(999, 200, false), 0.65));
    // Selection moves to item 1: on this pass item0 (still cur key)
    // returns 1.0, then item1's selFlag triggers the transition.
    CHECK(near(ctl.itemScale(cx, 31, false), 1.0));
    CHECK(near(ctl.itemScale(cx, 67, true), 0.65));
    // Next pass: item0==prev returns 1.0-0*0.07 = 1.0 (one extra
    // frame at full scale); item1==cur: acc 0->1 -> 0.72.
    CHECK(near(ctl.itemScale(cx, 31, false), 1.0));
    CHECK(near(ctl.itemScale(cx, 67, true), 0.72f, 1e-5));
    // Then prev decays 0.93->0.65 while cur grows to 1.0.
    const float dec[5] = {0.93f, 0.86f, 0.79f, 0.72f, 0.65f};
    const float inc[5] = {0.79f, 0.86f, 0.93f, 1.0f, 1.0f};
    for (int f = 0; f < 5; ++f) {
      CHECK(near(ctl.itemScale(cx, 31, false), dec[f], 1e-5));
      CHECK(near(ctl.itemScale(cx, 67, true), inc[f], 1e-5));
    }
    // Mid-ramp reversal: switch back to item0. item1 becomes prev;
    // its formula assumes a completed 1.0 state, so it snaps UP to
    // 1.0 (observed quirk) instead of freezing mid-ramp.
    CHECK(near(ctl.itemScale(cx, 31, true), 0.65));
    CHECK(near(ctl.itemScale(cx, 67, false), 1.0));
  }

  // Dynamic render uses the same composition with live scales and
  // the controller's logical mouse position for the arrow.
  {
    std::string err;
    mdk::IndexedImage backdrop;
    backdrop.width = 600;
    backdrop.height = 360;
    backdrop.stride = 600;
    backdrop.pixels.assign(600 * 360, 0x55);
    backdrop.hasPalette = true;
    for (int i = 0; i < 256; ++i) {
      backdrop.palette[i] = {std::uint8_t(i), std::uint8_t(255 - i),
                             std::uint8_t(i)};
    }
    auto f = SyntheticFont::make();
    const char* needed = "ContinueNew GamSavdpQitlOs";
    for (const char* c = needed; *c; ++c) {
      f.put32(static_cast<std::uint8_t>(*c) * 4,
              f.addGlyph(1, 0, 2, {9, 9, 9, 9}));
    }
    const auto font = mdk::decodeFtiFont(f.buf, &err);
    CHECK(font);
    auto arrowS = SyntheticSprite::make1(
        2, 2, 0, 0, {0x01, 77, 77, 0xfe, 0x01, 77, 77, 0xff});
    const auto arrow = mdk::decodeFtiSprite(arrowS.buf, &err);
    CHECK(arrow && arrow->frame(0));
    const std::string_view opts[5] = {"Continue", "New Game",
                                      "Saved Game", "Options", "Quit"};

    mdk::FrontendMenuController ctl(true);
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    CHECK(mdk::renderFrontendMenuDynamic(fb, palette, backdrop, *font,
                                         *arrow->frame(0), opts, ctl,
                                         0, &err));
    // First frame: the selected label draws through the live ramp
    // (acc=0 -> 0.65), so glyph pixels land in its label band.
    bool found9 = false;
    for (int y = 28; y < 34 && !found9; ++y) {
      for (int x = 0; x < 24 && !found9; ++x) {
        found9 = fb.at(x, y) == 9;
      }
    }
    CHECK(found9);
    // Arrow at the reset mouse position.
    CHECK(fb.at(300, 180) == 77 && fb.at(301, 181) == 77);
    // Contract checks reuse the static path's validation.
    mdk::IndexedImage small;
    small.width = small.height = small.stride = 4;
    small.pixels.assign(16, 0);
    CHECK(!mdk::renderFrontendMenuDynamic(fb, palette, small, *font,
                                          *arrow->frame(0), opts, ctl,
                                          0, &err));
  }
}

// Phase 4F — options sub-menu controller (FUN_00420eac).
void test_options_controller() {
  // Entry state (FUN_00420cf0): selection 8 (OM_QUIT); the shared
  // machine state carries over untouched — mouse, tick, deadlines,
  // latch, ramp, timing.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 123;
    s.mouseY = 45;
    s.tick = 77;
    mdk::OptionsMenuController ctl(s, false, 0);
    CHECK(ctl.selection() == 8);
    CHECK(ctl.mouseX() == 123 && ctl.mouseY() == 45);
    CHECK(ctl.tick() == 77);
    CHECK(!ctl.devHidden() && ctl.skill() == 0);
    CHECK(ctl.pendingAction() == mdk::OptionsAction::None);
  }

  // Keyboard vertical walk: prev 8->7, next wraps 8->0, prev wraps
  // 0->8. Same repeat machine as the root (tick+30 then tick+3).
  {
    mdk::FrontendMachineState s;
    mdk::OptionsMenuController ctl(s, false, 0);
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 7);
    mdk::OptionsMenuController c2(s, false, 0);
    in = {};
    in.nextHeld = true;
    c2.update(in);
    CHECK(c2.selection() == 0);   // 8->9 wraps to 0
    in = {};
    c2.update(in);
    in.prevHeld = true;
    c2.update(in);
    CHECK(c2.selection() == 8);   // 0->-1 wraps to 8
  }

  // Hidden-row skipping (DAT_005414f4): DOWN 1->5, UP 5->1; the index
  // space is not compacted (rows 2,3,4 unreachable).
  {
    mdk::FrontendMachineState s;
    mdk::OptionsMenuController ctl(s, true, 0);
    mdk::FrontendMenuInput in;
    // Walk DOWN from entry: 8->0->1->5 (2-4 skipped).
    in = {};
    in.nextHeld = true;
    ctl.update(in);       // 8 -> 0
    CHECK(ctl.selection() == 0);
    in = {};
    ctl.update(in);
    in.nextHeld = true;
    ctl.update(in);       // 0 -> 1
    CHECK(ctl.selection() == 1);
    in = {};
    ctl.update(in);
    in.nextHeld = true;
    ctl.update(in);       // 1 -> 5 (hidden 2-4 skipped)
    CHECK(ctl.selection() == 5);
    in = {};
    ctl.update(in);
    in.prevHeld = true;
    ctl.update(in);       // 5 -> 1 (hidden 4-2 skipped upward)
    CHECK(ctl.selection() == 1);
  }

  // Mouse hit-test: band = trunc((y - 23) / 36), x never consulted.
  // Band boundaries: row i covers [23+36i, 58+36i]; y>=347 is band 9
  // (invalid, selection holds). The trunc-toward-zero quirk maps
  // y in [0,22] to band 0 (Help) — observed IDIV semantics.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 200;
    mdk::OptionsMenuController ctl(s, false, 0);
    mdk::FrontendMenuInput in;
    // Gate closed with no mouse input.
    ctl.update(in);
    CHECK(ctl.selection() == 8);
    // y=200 -> band trunc(177/36)=4 -> Keyboard.
    in.mouseDy = 1;   // 200 -> 201: band trunc(178/36)=4 still
    in.mouseDx = -300;
    ctl.update(in);
    CHECK(ctl.mouseX() == 0 && ctl.mouseY() == 201 &&
          ctl.selection() == 4);
    // Exact band boundaries, walking upward.
    const int wantY[10] = {346, 311, 275, 239, 203, 167, 131, 95, 59, 23};
    const int wantSel[10] = {8, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    for (int i = 0; i < 10; ++i) {
      in = {};
      in.mouseDy = wantY[i] - ctl.mouseY();
      ctl.update(in);
      CHECK(ctl.mouseY() == wantY[i] && ctl.selection() == wantSel[i]);
    }
    // y=22 -> trunc(-1/36)=0 -> Help (quirk); y=0 -> band 0 too.
    in = {};
    in.mouseDy = 22 - ctl.mouseY();
    ctl.update(in);
    CHECK(ctl.mouseY() == 22 && ctl.selection() == 0);
    in = {};
    in.mouseDy = -22;
    ctl.update(in);
    CHECK(ctl.mouseY() == 0 && ctl.selection() == 0);
    // y=347 -> band 9 invalid; selection holds at 0.
    in = {};
    in.mouseDy = 347;
    ctl.update(in);
    CHECK(ctl.mouseY() == 347 && ctl.selection() == 0);
    // y=350 (gate clamp ceiling) -> band 9 invalid as well.
    in = {};
    in.mouseDy = 400;
    ctl.update(in);
    CHECK(ctl.mouseY() == 350 && ctl.selection() == 0);
  }

  // Hidden-mode hit-test: bands 2,3,4 are guarded — the selection
  // keeps its current value (the "1 < band < 5" clause reverts).
  {
    mdk::FrontendMachineState s;
    s.mouseY = 250;   // band trunc(227/36)=6 -> Skill
    mdk::OptionsMenuController ctl(s, true, 0);
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;   // gate opens at y=251 -> band 6
    ctl.update(in);
    CHECK(ctl.selection() == 6);
    in = {};
    in.mouseDy = -100;  // 151 -> band trunc(128/36)=3 -> hidden: hold
    ctl.update(in);
    CHECK(ctl.mouseY() == 151 && ctl.selection() == 6);
    in = {};
    in.mouseDy = -30;   // 121 -> band trunc(98/36)=2 -> hidden: hold
    ctl.update(in);
    CHECK(ctl.selection() == 6);
    in = {};
    in.mouseDy = 100;   // 221 -> band trunc(198/36)=5 -> Performance
    ctl.update(in);
    CHECK(ctl.selection() == 5);
  }

  // Activation dispatch — every row maps to its semantic action.
  {
    const mdk::OptionsAction want[9] = {
        mdk::OptionsAction::Help,        mdk::OptionsAction::Sound,
        mdk::OptionsAction::Joystick,    mdk::OptionsAction::Mouse,
        mdk::OptionsAction::Keyboard,    mdk::OptionsAction::Performance,
        mdk::OptionsAction::SkillCycleNext, mdk::OptionsAction::Display,
        mdk::OptionsAction::Back};
    for (int sel = 0; sel < 9; ++sel) {
      mdk::FrontendMachineState s;
      // Park the mouse inside row `sel`'s band, then click: the same-
      // frame hit-test selects, the latch fires the dispatch. One
      // button-free update arms the latch first (DAT_0049ac80).
      s.mouseY = 40 + 36 * sel;   // inside band sel (23+36i..58+36i)
      mdk::OptionsMenuController ctl(s, false, 0);
      mdk::FrontendMenuInput in;
      ctl.update(in);
      in.mouseButtons = 0x1;
      ctl.update(in);
      CHECK(ctl.selection() == sel);
      CHECK(ctl.consumeAction() == want[sel]);
      CHECK(ctl.consumeAction() == mdk::OptionsAction::None);
    }
  }

  // confirmEdge (Enter) activates the current selection without any
  // mouse input.
  {
    mdk::FrontendMachineState s;
    mdk::OptionsMenuController ctl(s, false, 0);
    mdk::FrontendMenuInput in;
    in.confirmEdge = true;   // sel 8 -> Back
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::OptionsAction::Back);
  }

  // Esc edge -> Back regardless of selection (FUN_00420d68 path).
  {
    mdk::FrontendMachineState s;
    s.mouseY = 40;   // band 0
    mdk::OptionsMenuController ctl(s, false, 0);
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.selection() == 0);
    in = {};
    in.cancelEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::OptionsAction::Back);
  }

  // LEFT/RIGHT/Enter on the skill row mutate DAT_0054147a in place —
  // LEFT -1 wraps <0 -> 2 (0x421085), RIGHT/activate +1 wraps >2 -> 0
  // (0x421131/0x4211cb) — latch DAT_00541486, emit the cycle event,
  // and fall through: the selection never moves and the frame still
  // reaches the draw block.
  {
    mdk::FrontendMachineState s;
    s.mouseY = 256;   // band trunc(233/36)=6 -> Skill
    mdk::OptionsMenuController ctl(s, false, 0);
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.selection() == 6 && ctl.skill() == 0);
    CHECK(!ctl.settingsDirty());
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::OptionsAction::SkillCyclePrev);
    CHECK(ctl.selection() == 6 && ctl.skill() == 2);   // 0-1 wraps
    CHECK(ctl.settingsDirty() && !ctl.frameEndedEarly());
    in = {};
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::OptionsAction::SkillCycleNext);
    CHECK(ctl.selection() == 6 && ctl.skill() == 0);   // 2+1 wraps
    // Enter on skill row is a forward cycle.
    in = {};
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::OptionsAction::SkillCycleNext);
    CHECK(ctl.skill() == 1 && !ctl.frameEndedEarly());
  }

  // OBSERVED bound quirk: the LEFT/RIGHT dispatch tables gate at
  // `cmp eax,7; ja` — on row 8 (OM_QUIT) both are silent fall-throughs
  // to the next query. Only the activate query (bound 8) or Esc
  // reaches FUN_00420d68.
  {
    mdk::FrontendMachineState s;
    mdk::OptionsMenuController ctl(s, false, 1);  // entry sel = 8
    mdk::FrontendMenuInput in;
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.pendingAction() == mdk::OptionsAction::None);
    CHECK(ctl.selection() == 8 && !ctl.frameEndedEarly());
    in = {};
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.pendingAction() == mdk::OptionsAction::None);
    CHECK(ctl.selection() == 8 && !ctl.frameEndedEarly());
    in = {};
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::OptionsAction::Back);
    CHECK(ctl.frameEndedEarly());
  }

  // LEFT/RIGHT on a non-skill row dispatch that row's action (the
  // original ends the frame right after the branch call).
  {
    mdk::FrontendMachineState s;
    s.mouseY = 40;    // band 0 -> Help
    mdk::OptionsMenuController ctl(s, false, 0);
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;
    ctl.update(in);
    in = {};
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::OptionsAction::Help);
  }

  // Input order (OBSERVED): prev query runs before next in one frame.
  {
    mdk::FrontendMachineState s;
    mdk::OptionsMenuController ctl(s, false, 0);
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    in.nextHeld = true;
    ctl.update(in);
    // prev: 8->7, next: 7->8.
    CHECK(ctl.selection() == 8);
  }

  // Button latch: held buttons don't refire; release re-arms.
  {
    mdk::FrontendMachineState s;
    s.mouseY = 40;   // Help band
    mdk::OptionsMenuController ctl(s, false, 0);
    mdk::FrontendMenuInput in;
    ctl.update(in);   // buttons released -> latch arms
    in.mouseButtons = 0x1;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::OptionsAction::Help);
    ctl.update(in);   // still held -> no refire
    CHECK(ctl.pendingAction() == mdk::OptionsAction::None);
    in.mouseButtons = 0;
    ctl.update(in);   // re-arm
    in.mouseButtons = 0x1;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::OptionsAction::Help);
  }

  // Scale ramp: keyed (-1, y). Cold boot -> first selected draw
  // 0.65, then acc advances once per selected draw at the FUN_0042fb30
  // power-on smoothed of 1.0: 0.72..1.0; prev-key decay and the
  // mid-ramp snap-up quirk behave exactly like the root's.
  {
    mdk::FrontendMachineState s;
    mdk::OptionsMenuController ctl(s, false, 0);
    const int y8 = 49 + 36 * 8;   // OM_QUIT row (entry selection)
    CHECK(near(ctl.itemScale(y8, true), 0.65));
    const float grow[5] = {0.72f, 0.79f, 0.86f, 0.93f, 1.0f};
    for (int f = 0; f < 5; ++f) {
      CHECK(near(ctl.itemScale(y8, true), grow[f], 1e-5));
    }
    CHECK(near(ctl.itemScale(49, false), 0.65));
    // Move selection to row 0: row 8 holds 1.0 one extra frame, then
    // decays; row 0 grows.
    const int y0 = 49;
    CHECK(near(ctl.itemScale(y8, false), 1.0));
    CHECK(near(ctl.itemScale(y0, true), 0.65));
    CHECK(near(ctl.itemScale(y8, false), 1.0));
    CHECK(near(ctl.itemScale(y0, true), 0.72f, 1e-5));
    const float dec[5] = {0.93f, 0.86f, 0.79f, 0.72f, 0.65f};
    const float inc[5] = {0.79f, 0.86f, 0.93f, 1.0f, 1.0f};
    for (int f = 0; f < 5; ++f) {
      CHECK(near(ctl.itemScale(y8, false), dec[f], 1e-5));
      CHECK(near(ctl.itemScale(y0, true), inc[f], 1e-5));
    }
  }

  // Phase 4G label-width consequence: mutating skill on row 6 swaps
  // the drawn record ("Skill - Easy/Normal/Hard" — different
  // measured widths, so the centered x shifts) but the FUN_00423a24
  // item key is positional — (-1, y) — and never sees the label.
  // OBSERVED structure: no cur/prev key swap, no accumulator reset,
  // no snap — the selected scale keeps ramping from its acc.
  {
    mdk::FrontendMachineState s;
    s.mouseY = 256;   // band trunc(233/36)=6 -> Skill
    mdk::OptionsMenuController ctl(s, false, 1);
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.selection() == 6);
    const int y6 = 49 + 36 * 6;
    CHECK(near(ctl.itemScale(y6, true), 0.65));  // first draw: acc 0
    CHECK(near(ctl.itemScale(y6, true), 0.72f, 1e-5));  // acc 1
    // RIGHT mutates the label (Normal -> Hard, narrower text).
    in = {};
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.skill() == 2);
    const auto& ramp = ctl.machineState().ramp;
    CHECK(ramp.curX == -1 && ramp.curY == y6);   // keys untouched
    // The next selected draw continues the ramp (acc 2 -> 0.79) —
    // NOT a 0.65 restart and no 1.0-acc decay of a "previous" key.
    CHECK(near(ctl.itemScale(y6, true), 0.79f, 1e-5));
    // LEFT mutates again (Hard -> Normal): still the same key state.
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.skill() == 1);
    CHECK(ramp.curX == -1 && ramp.curY == y6);
    CHECK(near(ctl.itemScale(y6, true), 0.86f, 1e-5));  // acc 3
  }
}

// Phase 4H — display child controller (FUN_0041d1e0).
void test_display_controller() {
  // Entry state (FUN_0041d020): selection 2 (DSP_QUIT); the shared
  // machine state carries over untouched — mouse, tick, deadlines,
  // latch, ramp, timing. The settings globals seed from the parent.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 123;
    s.mouseY = 45;
    s.tick = 77;
    mdk::DisplayMenuController ctl(s, 3, true, true);
    CHECK(ctl.selection() == 2);
    CHECK(ctl.mouseX() == 123 && ctl.mouseY() == 45);
    CHECK(ctl.tick() == 77);
    CHECK(ctl.brightness() == 3 && ctl.forcePCorrect());
    CHECK(ctl.settingsDirty());
    CHECK(ctl.pendingAction() == mdk::DisplayAction::None);
  }

  // Keyboard vertical walk: prev 2->1->0 wraps ->2; next wraps 2->0.
  // Same repeat machine as the other screens (release resets the
  // deadline between taps).
  {
    mdk::FrontendMachineState s;
    mdk::DisplayMenuController ctl(s, 0, false);
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 1);
    in = {};
    ctl.update(in);             // release — deadline resets
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 0);
    in = {};
    ctl.update(in);
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 2);   // 0-1 wraps to 2 (0x41d43b)
    mdk::DisplayMenuController c2(s, 0, false);
    in = {};
    in.nextHeld = true;
    c2.update(in);
    CHECK(c2.selection() == 0);   // 2+1 wraps to 0 (0x41d224)
  }

  // Mouse hit-test: band = trunc((y - 5) / 36), x never consulted.
  // Row i covers [5+36i, 40+36i]; y>=113 is band >=3 (invalid,
  // selection holds). The trunc-toward-zero quirk maps y in [0,4]
  // to band 0 — observed IDIV semantics.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 200;
    mdk::DisplayMenuController ctl(s, 0, false);
    mdk::FrontendMenuInput in;
    // Gate closed with no mouse input.
    ctl.update(in);
    CHECK(ctl.selection() == 2);
    // Band boundaries: y=76 -> band 1; y=77 -> band 2; y=112 -> 2;
    // y=113 -> band 3 invalid (holds).
    in.mouseDy = 76 - 200;
    ctl.update(in);
    CHECK(ctl.mouseY() == 76 && ctl.selection() == 1);
    in = {};
    in.mouseDy = 1;   // 77 -> band trunc(72/36)=2
    ctl.update(in);
    CHECK(ctl.selection() == 2);
    in = {};
    in.mouseDy = 35;  // 112 -> band trunc(107/36)=2 still
    ctl.update(in);
    CHECK(ctl.mouseY() == 112 && ctl.selection() == 2);
    in = {};
    in.mouseDy = 1;   // 113 -> band 3 invalid -> holds
    ctl.update(in);
    CHECK(ctl.mouseY() == 113 && ctl.selection() == 2);
    // y=4 -> trunc(-1/36)=0 -> row 0 (quirk); y=5 -> band 0.
    in = {};
    in.mouseDy = 4 - 113;
    ctl.update(in);
    CHECK(ctl.mouseY() == 4 && ctl.selection() == 0);
    in = {};
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.mouseY() == 5 && ctl.selection() == 0);
    // Gate clamp: y > 350 clamps to 350 inside the gate -> band 9
    // invalid, selection holds.
    in = {};
    in.mouseDy = 400;
    ctl.update(in);
    CHECK(ctl.mouseY() == 350 && ctl.selection() == 0);
  }

  // Row 0 brightness mutations (DAT_0054147e): LEFT -1 wraps <0 -> 7
  // (0x41d478); RIGHT/activate +1 wraps >=8 -> 0 (0x41d306/0x41d33f).
  // Every mutation latches DAT_00541486; LEFT/RIGHT never end the
  // frame (they fall through to the next query).
  {
    mdk::FrontendMachineState s;
    s.mouseY = 29;   // band trunc(24/36)=0 -> Brightness row
    mdk::DisplayMenuController ctl(s, 0, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.selection() == 0 && ctl.brightness() == 0);
    CHECK(!ctl.settingsDirty());
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.brightness() == 7 && ctl.settingsDirty());  // 0-1 wraps
    CHECK(!ctl.frameEndedEarly());
    in = {};
    ctl.update(in);             // release — deadline resets
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.brightness() == 0 && !ctl.frameEndedEarly()); // 7+1 wraps
    in = {};
    ctl.update(in);             // release — deadline resets
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.brightness() == 1);   // 0+1
    // Activate on row 0 increments too.
    in = {};
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.brightness() == 2 && !ctl.frameEndedEarly());
    CHECK(ctl.pendingAction() == mdk::DisplayAction::None);
  }

  // Row 1 ForcePCorrect toggles (DAT_00541482): LEFT, RIGHT, and
  // activate all toggle + dirty; nothing ends the frame.
  {
    mdk::FrontendMachineState s;
    s.mouseY = 49;   // band 1
    mdk::DisplayMenuController ctl(s, 0, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.selection() == 1 && !ctl.forcePCorrect());
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.forcePCorrect() && ctl.settingsDirty() &&
          !ctl.frameEndedEarly());
    in = {};
    ctl.update(in);             // release
    in.rightHeld = true;
    ctl.update(in);
    CHECK(!ctl.forcePCorrect() && !ctl.frameEndedEarly());
    in = {};
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.forcePCorrect());
    CHECK(ctl.pendingAction() == mdk::DisplayAction::None);
  }

  // Row 2 (DSP_QUIT): LEFT/RIGHT are no-ops that fall through to the
  // next query (OBSERVED — the row-2 `jnz` skips both mutation
  // blocks); activate -> FUN_0041d144 + RET (frame ends early).
  {
    mdk::FrontendMachineState s;   // entry sel = 2
    mdk::DisplayMenuController ctl(s, 0, false);
    mdk::FrontendMenuInput in;
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.pendingAction() == mdk::DisplayAction::None);
    CHECK(ctl.selection() == 2 && !ctl.frameEndedEarly());
    in = {};
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.pendingAction() == mdk::DisplayAction::None);
    CHECK(ctl.selection() == 2 && !ctl.frameEndedEarly());
    in = {};
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::DisplayAction::Back);
    CHECK(ctl.frameEndedEarly());
  }

  // Esc edge -> FUN_0041d144 regardless of selection (0x41d469).
  {
    mdk::FrontendMachineState s;
    s.mouseY = 29;
    mdk::DisplayMenuController ctl(s, 0, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.selection() == 0);
    in = {};
    in.cancelEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::DisplayAction::Back);
    CHECK(ctl.frameEndedEarly());
  }

  // Input order (OBSERVED): prev query runs before next in one frame.
  {
    mdk::FrontendMachineState s;
    mdk::DisplayMenuController ctl(s, 0, false);
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    in.nextHeld = true;
    ctl.update(in);
    // prev: 2->1, next: 1->2.
    CHECK(ctl.selection() == 2);
  }

  // Button latch: held buttons don't refire; release re-arms. A
  // click on the Quit row exits like Enter.
  {
    mdk::FrontendMachineState s;
    s.mouseY = 89;   // band 2 -> Quit
    mdk::DisplayMenuController ctl(s, 0, false);
    mdk::FrontendMenuInput in;
    ctl.update(in);   // buttons released -> latch arms
    in.mouseButtons = 0x1;
    in.mouseDy = 1;   // gate opens; band still 2
    ctl.update(in);
    CHECK(ctl.selection() == 2);
    CHECK(ctl.consumeAction() == mdk::DisplayAction::Back);
    CHECK(ctl.frameEndedEarly());
  }

  // A click on the Brightness row fires the row-0 activate (+1) —
  // the same-frame hit-test selects, the latch fires the mutation.
  {
    mdk::FrontendMachineState s;
    s.mouseY = 29;
    mdk::DisplayMenuController ctl(s, 0, false);
    mdk::FrontendMenuInput in;
    ctl.update(in);   // arm
    in.mouseButtons = 0x1;
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.selection() == 0 && ctl.brightness() == 1);
    CHECK(ctl.settingsDirty() && !ctl.frameEndedEarly());
    // Still held -> no refire.
    in = {};
    in.mouseButtons = 0x1;
    ctl.update(in);
    CHECK(ctl.brightness() == 1);
  }

  // Scale ramp: keyed (-1, y) exactly like the options rows — cold
  // boot first selected draw 0.65, then acc advances once per pass.
  {
    mdk::FrontendMachineState s;
    mdk::DisplayMenuController ctl(s, 0, false);
    const int y2 = 31 + 36 * 2;   // DSP_QUIT row (entry selection)
    CHECK(near(ctl.itemScale(y2, true), 0.65));
    const float grow[5] = {0.72f, 0.79f, 0.86f, 0.93f, 1.0f};
    for (int f = 0; f < 5; ++f) {
      CHECK(near(ctl.itemScale(y2, true), grow[f], 1e-5));
    }
    CHECK(near(ctl.itemScale(31, false), 0.65));
  }
}

// Phase 4I — sound child controller (FUN_004233d8): the same shared
// query helpers as the options screen in the same order, with one
// asymmetry — every FIRED repeat/activate query plays OPTBUTT first
// except Esc; LEFT/RIGHT mutate SoundFX (row 0) / SoundMusic (row 1)
// by 10 clamped [0,100] then call FUN_004024c4; row-2 activate and
// Esc dispatch FUN_00423280 + RET (frame ends early, no draw).
void test_sound_controller() {
  // Entry (FUN_0042322c): the constructor carries the process
  // globals — selection (DAT_0054bdbc, caller-supplied; BSS 0 on the
  // first entry, retained later), volumes, dirty — and queues the
  // entry audio events (ambient stop, OPTSONG start).
  {
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 90;
    mdk::SoundMenuController ctl(s, 0, 70, 100);
    CHECK(ctl.selection() == 0);
    CHECK(ctl.soundFx() == 70 && ctl.soundMusic() == 100);
    CHECK(!ctl.settingsDirty());
    CHECK(ctl.mouseX() == 300 && ctl.mouseY() == 90);
    const auto ev = ctl.drainAudioEvents();
    CHECK(ev.size() == 2 &&
          ev[0] == mdk::SoundAudioEvent::AmbientSongStop &&
          ev[1] == mdk::SoundAudioEvent::SongStart);
    CHECK(ctl.drainAudioEvents().empty());   // drained
  }

  // Same repeat machine as the other screens: UP/DOWN wrap among
  // the three rows (sel-1 <0 -> 2; sel+1 >=3 -> 0), each fired
  // query emitting OPTBUTT before the row logic.
  {
    mdk::FrontendMachineState s;
    mdk::SoundMenuController ctl(s, 0, 70, 100);
    ctl.drainAudioEvents();
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 2);   // 0-1 wraps
    in = {};
    ctl.update(in);                // release — deadline resets
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 1);
    in = {};
    ctl.update(in);
    in.nextHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 2);
    in = {};
    ctl.update(in);
    in.nextHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 0);   // 2+1 wraps
    const auto ev = ctl.drainAudioEvents();
    CHECK(ev.size() == 4);
    for (const auto e : ev) {
      CHECK(e == mdk::SoundAudioEvent::Button);
    }
  }

  // Mouse hit-test (OBSERVED): band = trunc((y - 61) / 46) inside
  // the gate; valid bands 0..2 assign unconditionally, invalid
  // bands hold. Row i covers [61+46i, 106+46i]; y>=199 is band>=3
  // (invalid). The trunc-toward-zero quirk maps y in [15,60] to
  // band 0 (negative/positive fractions both truncate to 0).
  {
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 200;
    mdk::SoundMenuController ctl(s, 0, 70, 100);
    mdk::FrontendMenuInput in;
    ctl.update(in);   // gate closed — no mouse input
    CHECK(ctl.selection() == 0);
    // y=107 -> band 1; y=153 -> band 2; y=198 -> band 2 still;
    // y=199 -> band 3 invalid (holds).
    in.mouseDy = 107 - 200;
    ctl.update(in);
    CHECK(ctl.mouseY() == 107 && ctl.selection() == 1);
    in = {};
    in.mouseDy = 153 - 107;
    ctl.update(in);
    CHECK(ctl.selection() == 2);
    in = {};
    in.mouseDy = 198 - 153;
    ctl.update(in);
    CHECK(ctl.mouseY() == 198 && ctl.selection() == 2);
    in = {};
    in.mouseDy = 1;   // 199 -> band 3 -> holds
    ctl.update(in);
    CHECK(ctl.mouseY() == 199 && ctl.selection() == 2);
    // y=60 -> trunc(-1/46)=0 -> row 0 (quirk); y=14 -> trunc(-47/46)
    // = -1 invalid -> holds.
    in = {};
    in.mouseDy = 60 - 199;
    ctl.update(in);
    CHECK(ctl.mouseY() == 60 && ctl.selection() == 0);
    in = {};
    in.mouseDy = 14 - 60;
    ctl.update(in);
    CHECK(ctl.mouseY() == 14 && ctl.selection() == 0);
    // Gate clamp: y>350 clamps to 350 inside the gate -> band 6
    // invalid, selection holds.
    in = {};
    in.mouseDy = 400;
    ctl.update(in);
    CHECK(ctl.mouseY() == 350 && ctl.selection() == 0);
  }

  // Row 0 SoundFX (DAT_00541308): RIGHT +10 / LEFT -10, clamped
  // [0,100] never wrapping; every mutation latches DAT_00541486
  // and emits OPTBUTT + VolumesApplied (FUN_004024c4) — even at
  // the clamp boundary (OBSERVED: the <0 clamp still latches and
  // calls the apply). Mutations never end the frame early.
  {
    mdk::FrontendMachineState s;
    mdk::SoundMenuController ctl(s, 0, 70, 100);
    ctl.drainAudioEvents();
    mdk::FrontendMenuInput in;
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.soundFx() == 80 && ctl.settingsDirty());
    CHECK(!ctl.frameEndedEarly());
    in = {};
    ctl.update(in);                // release
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.soundFx() == 90);
    // Drive to the boundary: 90 -> 100 -> clamp 100.
    for (int i = 0; i < 2; ++i) {
      in = {};
      ctl.update(in);
      in.rightHeld = true;
      ctl.update(in);
    }
    CHECK(ctl.soundFx() == 100);
    // LEFT from 0 clamps at 0 with the full side-effect set.
    mdk::SoundMenuController c2(s, 0, 0, 100);
    c2.drainAudioEvents();
    in = {};
    in.leftHeld = true;
    c2.update(in);
    CHECK(c2.soundFx() == 0 && c2.settingsDirty());
    const auto ev = c2.drainAudioEvents();
    CHECK(ev.size() == 2 &&
          ev[0] == mdk::SoundAudioEvent::Button &&
          ev[1] == mdk::SoundAudioEvent::VolumesApplied);
    CHECK(!c2.frameEndedEarly());
  }

  // Row 1 SoundMusic (DAT_0054130c): independent domain — same
  // ±10 clamp; its own mutations do not touch SoundFX.
  {
    mdk::FrontendMachineState s;
    s.mouseY = 107;   // band 1
    mdk::SoundMenuController ctl(s, 0, 70, 100);
    ctl.drainAudioEvents();
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.selection() == 1);
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.soundMusic() == 90 && ctl.soundFx() == 70);
    CHECK(ctl.settingsDirty() && !ctl.frameEndedEarly());
    // Drive to the floor: 90 -> ... -> 0 -> clamp 0.
    for (int i = 0; i < 10; ++i) {
      in = {};
      ctl.update(in);
      in.leftHeld = true;
      ctl.update(in);
    }
    CHECK(ctl.soundMusic() == 0);
  }

  // Row 2 (SND_DONE): LEFT/RIGHT fall through with no mutation and
  // no VolumesApplied — but the fired query still plays OPTBUTT
  // (OBSERVED: the sound plays at query-fire time, before the
  // row's `jnz` skips both mutation blocks).
  {
    mdk::FrontendMachineState s;
    mdk::SoundMenuController ctl(s, 2, 70, 100);
    ctl.drainAudioEvents();
    mdk::FrontendMenuInput in;
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.soundFx() == 70 && ctl.soundMusic() == 100);
    CHECK(!ctl.settingsDirty() && !ctl.frameEndedEarly());
    in = {};
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.soundFx() == 70 && ctl.soundMusic() == 100);
    CHECK(!ctl.settingsDirty());
    const auto ev = ctl.drainAudioEvents();
    CHECK(ev.size() == 2 &&
          ev[0] == mdk::SoundAudioEvent::Button &&
          ev[1] == mdk::SoundAudioEvent::Button);
  }

  // Activate on rows 0/1: OPTBUTT plays, the frame falls through
  // to the draw — no mutation, no exit (OBSERVED: sel==0 -> draw,
  // sel!=1 -> exit; rows 0 and 1 both survive).
  {
    mdk::FrontendMachineState s;
    mdk::SoundMenuController ctl(s, 0, 70, 100);
    ctl.drainAudioEvents();
    mdk::FrontendMenuInput in;
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.soundFx() == 70 && !ctl.settingsDirty());
    CHECK(ctl.pendingAction() == mdk::SoundAction::None);
    CHECK(!ctl.frameEndedEarly());
    const auto ev = ctl.drainAudioEvents();
    CHECK(ev.size() == 1 && ev[0] == mdk::SoundAudioEvent::Button);
    mdk::SoundMenuController c2(s, 1, 70, 100);
    c2.drainAudioEvents();
    c2.update(in);
    CHECK(c2.soundMusic() == 100 &&
          c2.pendingAction() == mdk::SoundAction::None);
  }

  // Activate on row 2: OPTBUTT first, then FUN_00423280 — SongStop
  // + AmbientSongStart queue in order, Back dispatches, and the
  // frame ends early (RET before the draw).
  {
    mdk::FrontendMachineState s;
    mdk::SoundMenuController ctl(s, 2, 70, 100);
    ctl.drainAudioEvents();
    mdk::FrontendMenuInput in;
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::SoundAction::Back);
    CHECK(ctl.frameEndedEarly());
    const auto ev = ctl.drainAudioEvents();
    CHECK(ev.size() == 3 &&
          ev[0] == mdk::SoundAudioEvent::Button &&
          ev[1] == mdk::SoundAudioEvent::SongStop &&
          ev[2] == mdk::SoundAudioEvent::AmbientSongStart);
  }

  // Esc -> FUN_00423280 regardless of selection — the ONLY exit
  // path with no OPTBUTT (OBSERVED: the raw key check jumps
  // straight to the exit; no FUN_00402388 runs).
  {
    mdk::FrontendMachineState s;
    mdk::SoundMenuController ctl(s, 1, 70, 100);
    ctl.drainAudioEvents();
    mdk::FrontendMenuInput in;
    in.cancelEdge = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::SoundAction::Back);
    CHECK(ctl.frameEndedEarly());
    const auto ev = ctl.drainAudioEvents();
    CHECK(ev.size() == 2 &&
          ev[0] == mdk::SoundAudioEvent::SongStop &&
          ev[1] == mdk::SoundAudioEvent::AmbientSongStart);
  }

  // Input order (OBSERVED): prev runs before next in one frame —
  // the original queries them sequentially.
  {
    mdk::FrontendMachineState s;
    mdk::SoundMenuController ctl(s, 1, 70, 100);
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    in.nextHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 1);   // 1-1=0 then 0+1=1
  }

  // Mutate-back-to-default: the dirty latch STAYS set (OBSERVED
  // latch semantics — same as Skill/Brightness); the serializer
  // decides what to emit, not the latch.
  {
    mdk::FrontendMachineState s;
    mdk::SoundMenuController ctl(s, 0, 70, 100);
    mdk::FrontendMenuInput in;
    in.rightHeld = true;
    ctl.update(in);                // 70 -> 80
    in = {};
    ctl.update(in);
    in.leftHeld = true;
    ctl.update(in);                // 80 -> 70
    CHECK(ctl.soundFx() == 70 && ctl.settingsDirty());
  }

  // Scale ramp: volume rows key (4, rowY), Done keys (-1,179) —
  // cold boot first selected draw 0.65, then acc advances.
  {
    mdk::FrontendMachineState s;
    mdk::SoundMenuController ctl(s, 0, 70, 100);
    CHECK(near(ctl.itemScale(4, 87, true), 0.65));
    const float grow[5] = {0.72f, 0.79f, 0.86f, 0.93f, 1.0f};
    for (int f = 0; f < 5; ++f) {
      CHECK(near(ctl.itemScale(4, 87, true), grow[f], 1e-5));
    }
    CHECK(near(ctl.itemScale(4, 133, false), 0.65));
  }
}

// Phase 4J — Mouse options child (FUN_004217e8, mode 0x04).
void test_mouse_controller() {
  // Entry (FUN_00421664, OBSERVED): DAT_0054bd40 selection and
  // DAT_0054bd38 column reset to 0; DAT_0054bd3c=3 axes /
  // DAT_0054bd44=4 buttons are constants; the settings globals and
  // the shared machine state (mouse, tick, deadlines, marker acc)
  // carry over untouched.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 45;
    s.tick = 77;
    s.markerAcc = 9;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16.0f, 16.0f, 50.0f});
    CHECK(ctl.selection() == 0 && ctl.column() == 0);
    CHECK(ctl.mouseOn() && ctl.mouseYReversedBits() == 0);
    CHECK(ctl.axesMap() == "ABG");
    CHECK(ctl.buttMask(0) == 1 && ctl.buttMask(1) == 4 &&
          ctl.buttMask(2) == 2 && ctl.buttMask(3) == 0);
    CHECK(near(ctl.scale(0), 16.0) && near(ctl.scale(2), 50.0));
    CHECK(!ctl.settingsDirty());
    CHECK(ctl.mouseX() == 300 && ctl.mouseY() == 45 &&
          ctl.tick() == 77 && ctl.markerAccumulator() == 9);
    CHECK(ctl.pendingAction() == mdk::MouseAction::None);
  }

  // prev/next wrap through all 23 rows (sel-1 <0 -> 22; sel+1
  // >=DAT_0054bd3c+0x13 -> 0) — the same repeat machine as the
  // other screens.
  {
    mdk::FrontendMachineState s;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 22);   // 0-1 wraps
    in = {};
    ctl.update(in);                 // release — deadline resets
    in.nextHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 0);    // 22+1 wraps
    // Drive next through the whole range to prove 0..22 all reach.
    for (int i = 1; i <= 22; ++i) {
      in = {};
      ctl.update(in);
      in.nextHeld = true;
      ctl.update(in);
      CHECK(ctl.selection() == i);
    }
    in = {};
    ctl.update(in);
    in.nextHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 0);    // 22+1 -> 0
  }

  // Esc is checked FIRST — before prev (OBSERVED 0x4217f3): a
  // frame holding both Esc and prev exits without the selection
  // moving; the exit ends the frame before the draw.
  {
    mdk::FrontendMachineState s;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.cancelEdge = true;
    in.prevHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 0);    // prev never ran
    CHECK(ctl.consumeAction() == mdk::MouseAction::Back);
    CHECK(ctl.frameEndedEarly());
    CHECK(ctl.consumeAction() == mdk::MouseAction::None);
  }

  // Mouse hit-test left column (OBSERVED): x<250 — band =
  // trunc((y-2)/16) valid 0..3 selects rows 0-3; else band2 =
  // trunc((y-259)/16) valid 0..2 selects rows 4-6. Trunc-toward-
  // zero maps y in [2-15,1] to band 0 (the quirk); y<2-15 is band
  // -1 -> falls to the axis band check.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 100;
    s.mouseY = 350;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    ctl.update(in);   // gate closed — no mouse input
    CHECK(ctl.selection() == 0);
    // y=18 -> band trunc(16/16)=1 -> row 1.
    in = {};
    in.mouseDy = 18 - 350;
    ctl.update(in);
    CHECK(ctl.mouseY() == 18 && ctl.selection() == 1);
    // y=49 -> band trunc(47/16)=2 -> row 2; y=65 -> band 3.
    in = {};
    in.mouseDy = 49 - 18;
    ctl.update(in);
    CHECK(ctl.selection() == 2);
    in = {};
    in.mouseDy = 65 - 49;
    ctl.update(in);
    CHECK(ctl.selection() == 3);
    // y=66 -> band trunc(64/16)=4 -> falls to axis band:
    // trunc((66-259)/16) = -12 invalid -> selection holds at 3.
    in = {};
    in.mouseDy = 1;
    ctl.update(in);
    CHECK(ctl.mouseY() == 66 && ctl.selection() == 3);
    // y=275 -> axis band trunc(16/16)=1 -> row 5; y=259 -> band 0
    // -> row 4; y=302 -> band 2 -> row 6.
    in = {};
    in.mouseDy = 275 - 66;
    ctl.update(in);
    CHECK(ctl.selection() == 5);
    in = {};
    in.mouseDy = 259 - 275;
    ctl.update(in);
    CHECK(ctl.selection() == 4);
    in = {};
    in.mouseDy = 302 - 259;
    ctl.update(in);
    CHECK(ctl.selection() == 6);
    // y=318 -> band trunc(59/16)=3 invalid -> holds at 6.
    in = {};
    in.mouseDy = 318 - 302;
    ctl.update(in);
    CHECK(ctl.selection() == 6);
    // y=1 -> band trunc(-1/16)=0 (trunc quirk) -> row 0.
    in = {};
    in.mouseDy = 1 - 318;
    ctl.update(in);
    CHECK(ctl.mouseY() == 1 && ctl.selection() == 0);
  }

  // Mouse hit-test right column (OBSERVED): x>=250 — band =
  // trunc((y-33)/16) valid 0..15 -> sel = band+7 AND col =
  // clamp(trunc((x-396)/16), 0, 3); the column assignment runs
  // ONLY when the band is valid.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 40;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    // y=33 -> band 0 -> row 7; x=300 -> trunc(-96/16)=-6 -> col 0.
    in.mouseDy = -7;   // 40 -> 33
    ctl.update(in);
    CHECK(ctl.selection() == 7 && ctl.column() == 0);
    // x=412 -> trunc(16/16)=1 -> col 1 (same band, y stays 33).
    in = {};
    in.mouseDx = 412 - 300;
    ctl.update(in);
    CHECK(ctl.column() == 1 && ctl.selection() == 7);
    // x=444 -> trunc(48/16)=3 -> col 3; y=49 -> band 1 -> row 8.
    in = {};
    in.mouseDx = 444 - 412;
    in.mouseDy = 49 - 33;
    ctl.update(in);
    CHECK(ctl.column() == 3 && ctl.selection() == 8);
    // x=460 -> trunc(64/16)=4 -> clamps to 3; y=288 -> band 15
    // -> row 22.
    in = {};
    in.mouseDx = 460 - 444;
    in.mouseDy = 288 - 49;
    ctl.update(in);
    CHECK(ctl.column() == 3 && ctl.selection() == 22);
    // x=380 -> trunc(-16/16) = -1 -> clamps to 0 (same band).
    in = {};
    in.mouseDx = 380 - 460;
    ctl.update(in);
    CHECK(ctl.column() == 0 && ctl.selection() == 22);
    // y=290 -> band trunc(257/16)=16 -> invalid: sel AND col hold.
    in = {};
    in.mouseDy = 290 - 288;
    ctl.update(in);
    CHECK(ctl.selection() == 22 && ctl.column() == 0);
    // The gate clamp: accumulated 359 -> clamped 350 inside the
    // gate -> band 19 invalid (selection and column hold).
    in = {};
    in.mouseDy = 500;
    ctl.update(in);
    CHECK(ctl.mouseY() == 350 && ctl.selection() == 22 &&
          ctl.column() == 0);
  }

  // LEFT/RIGHT on rows 1/2 (OBSERVED): BOTH directions toggle —
  // MouseOn 0<->1 + dirty (0x421ceb/0x421d65), MouseYReversed raw
  // bits 0<->1 + dirty (0x421d17/0x421d91). Rows 0/3 fall through.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 100;
    s.mouseY = 17;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;   // -> 18: gate opens, band 1 -> MouseOn row
    ctl.update(in);
    CHECK(ctl.selection() == 1);
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(!ctl.mouseOn() && ctl.settingsDirty());
    in = {};
    ctl.update(in);
    in.rightHeld = true;   // RIGHT toggles too — OBSERVED
    ctl.update(in);
    CHECK(ctl.mouseOn());
    // Row 2: MouseYReversed — raw int bits into the float slot.
    in = {};
    in.mouseDy = 49 - 18;
    ctl.update(in);
    CHECK(ctl.selection() == 2);
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.mouseYReversedBits() == 1 && ctl.mouseYReversed());
    in = {};
    ctl.update(in);
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.mouseYReversedBits() == 0);
    // Rows 0/3 take no LEFT/RIGHT mutation.
    in = {};
    in.mouseDy = 16 - 49;   // band 0
    ctl.update(in);
    CHECK(ctl.selection() == 0);
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.mouseOn() && ctl.mouseYReversedBits() == 0 &&
          ctl.column() == 0);
    in = {};
    in.mouseDy = 65 - 16;   // band 3 -> Quit row
    ctl.update(in);
    in = {};
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.consumeAction() == mdk::MouseAction::None &&
          !ctl.frameEndedEarly());
  }

  // LEFT/RIGHT on the axis rows 4-6 (OBSERVED): cycle the map
  // letter -1/+1 inside '0'+'A'..'H' (FUN_004216a0) — '0'->'H' on
  // LEFT, '0'->'A' on RIGHT, 'H'+1 -> '0', 'A'-1 -> '0', each
  // mutation latching dirty.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 100;
    s.mouseY = 258;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;   // -> 259: axis band 0 -> row 4
    ctl.update(in);
    CHECK(ctl.selection() == 4 && ctl.axisMapChar(0) == 'A');
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.axesMap() == "0BG" && ctl.settingsDirty());  // A-1->'0'
    in = {};
    ctl.update(in);
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.axesMap() == "HBG");   // '0'-1 -> 'H'
    in = {};
    ctl.update(in);
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.axesMap() == "0BG");   // 'H'+1 -> '0'
    in = {};
    ctl.update(in);
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.axesMap() == "ABG");   // '0'+1 -> 'A'
    // 'G' on axis 2: RIGHT -> 'H' -> '0' -> 'A'.
    in = {};
    in.mouseDy = 302 - 259;   // axis band 2 -> row 6
    ctl.update(in);
    CHECK(ctl.selection() == 6);
    in = {};
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.axesMap() == "ABH");
    in = {};
    ctl.update(in);
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.axesMap() == "AB0");
    in = {};
    ctl.update(in);
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.axesMap() == "ABA");
  }

  // LEFT/RIGHT on grid rows 7-22 (OBSERVED): the column wraps
  // bidirectionally — LEFT <0 -> cols-1, RIGHT >=cols -> 0. The
  // dirty flag is NOT set by column movement.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 32;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;   // -> 33: grid band 0 -> row 7, col 0
    ctl.update(in);
    CHECK(ctl.selection() == 7 && ctl.column() == 0);
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(ctl.column() == 3);      // 0-1 wraps to 3
    in = {};
    ctl.update(in);
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.column() == 0);      // 3+1 wraps to 0
    in = {};
    ctl.update(in);
    in.rightHeld = true;
    ctl.update(in);
    CHECK(ctl.column() == 1);
    CHECK(!ctl.settingsDirty());   // column moves never latch
  }

  // Activate (OBSERVED jump table 0x4217d8): row 0 -> no-op fall
  // through to the draw; row 1 -> MouseOn toggle; row 2 ->
  // MouseYReversed toggle; row 3 -> mode 0x0b + RET (exit);
  // rows 4-6 -> letter +1; rows 7-22 -> FUN_00421774.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 100;
    s.mouseY = 9;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;   // -> 10: band 0 -> row 0 (Test — no-op)
    ctl.update(in);
    CHECK(ctl.selection() == 0);
    in = {};
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.pendingAction() == mdk::MouseAction::None &&
          !ctl.frameEndedEarly() && !ctl.settingsDirty());
    // Row 1 activate toggles MouseOn.
    in = {};
    in.mouseDy = 18 - 10;
    ctl.update(in);
    in = {};
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(!ctl.mouseOn() && ctl.settingsDirty());
    // Row 3 activate -> Back + frame ends early.
    mdk::FrontendMachineState s2;
    s2.mouseX = 100;
    s2.mouseY = 64;
    mdk::MouseMenuController c2(s2, true, 0, "ABG", {1, 4, 2, 0},
                                {16, 16, 50});
    in = {};
    in.mouseDy = 1;   // -> 65: band 3 -> Quit row
    c2.update(in);
    in = {};
    in.confirmEdge = true;
    c2.update(in);
    CHECK(c2.consumeAction() == mdk::MouseAction::Back);
    CHECK(c2.frameEndedEarly());
    // Row 4 activate cycles the axis letter +1 (same as RIGHT).
    mdk::FrontendMachineState s3;
    s3.mouseX = 100;
    s3.mouseY = 258;
    mdk::MouseMenuController c3(s3, true, 0, "ABG", {1, 4, 2, 0},
                                {16, 16, 50});
    in = {};
    in.mouseDy = 1;   // -> 259: axis band 0 -> row 4
    c3.update(in);
    in = {};
    in.confirmEdge = true;
    c3.update(in);
    CHECK(c3.axesMap() == "BBG" && c3.settingsDirty());
  }

  // Button-bit exclusivity (FUN_00421774 + table @0x499f0c,
  // OBSERVED): bit set -> clear only it; bit clear -> clear the
  // row's exclusive group then set the bit. Factory masks
  // {1,4,2,0}: activating row 7 col 0 clears bit 0 of mask A.
  {
    mdk::FrontendMachineState s;
    s.mouseX = 396;
    s.mouseY = 32;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;   // -> 33: row 7, col trunc(0/16) = 0
    ctl.update(in);
    CHECK(ctl.selection() == 7 && ctl.column() == 0);
    in = {};
    in.confirmEdge = true;
    ctl.update(in);
    CHECK(ctl.buttMask(0) == 0 && ctl.settingsDirty());
    // Group exclusivity: row 7's group table[0] = 0x2 — setting
    // bit 0 of mask C clears bit 1 (group) then sets bit 0.
    // Factory mask C = 2 (bit 1). Select row 7 col 2.
    mdk::FrontendMachineState s2;
    s2.mouseX = 428;
    s2.mouseY = 32;
    mdk::MouseMenuController c2(s2, true, 0, "ABG", {1, 4, 2, 0},
                                {16, 16, 50});
    in = {};
    in.mouseDy = 1;   // -> 33: row 7, col trunc(32/16) = 2
    c2.update(in);
    CHECK(c2.selection() == 7 && c2.column() == 2);
    in = {};
    in.confirmEdge = true;
    c2.update(in);
    CHECK(c2.buttMask(2) == 1);   // (2 & ~0x2) | 1 = 1
    // Setting a different row's bit in the same column clears the
    // old exclusive partner: mask A row 8 (bit 1, group 0x1) —
    // factory A=1; activate row 8 col 0 -> (1 & ~0x1) | 2 = 2.
    mdk::FrontendMachineState s3;
    s3.mouseX = 396;
    s3.mouseY = 48;
    mdk::MouseMenuController c3(s3, true, 0, "ABG", {1, 4, 2, 0},
                                {16, 16, 50});
    in = {};
    in.mouseDy = 1;   // -> 49: band 1 -> row 8, col 0
    c3.update(in);
    CHECK(c3.selection() == 8 && c3.column() == 0);
    in = {};
    in.confirmEdge = true;
    c3.update(in);
    CHECK(c3.buttMask(0) == 2);
  }

  // Axis value / test marker (OBSERVED FLD/FDIV + FCOMP/JNC):
  // delta/scale clamped [-1,1]; a zero scale makes the quotient
  // NaN -> -1.0. Test marker = clamp(FISTP(50*delta/scale),-50,50)
  // — out-of-range yields INT_MIN -> -50.
  {
    mdk::FrontendMachineState s;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.mouseDx = 32;    // 32/16 = 2 -> clamps +1
    in.mouseDy = -8;    // -8/16 = -0.5
    in.mouseDz = 100;   // 100/50 = 2 -> clamps +1
    ctl.update(in);
    CHECK(near(ctl.axisValue(0), 1.0));
    CHECK(near(ctl.axisValue(1), -0.5));
    CHECK(near(ctl.axisValue(2), 1.0));
    CHECK(ctl.lastDelta(0) == 32 && ctl.lastDelta(1) == -8 &&
          ctl.lastDelta(2) == 100);
    // FISTP(50*32/16)=100 -> +50; FISTP(50*-8/16)=-25 -> -25.
    CHECK(ctl.testOffsetX() == 50 && ctl.testOffsetY() == -25);
    // Zero scale: +inf quotient -> axisValue +1 (FCOMP/JNC);
    // -inf -> -1; FISTP of either -> INT_MIN -> test offset -50.
    mdk::MouseMenuController c2(s, true, 0, "ABG", {1, 4, 2, 0},
                                {0, 0, 50});
    c2.update(in);
    CHECK(near(c2.axisValue(0), 1.0));
    CHECK(near(c2.axisValue(1), -1.0));
    CHECK(c2.testOffsetX() == -50 && c2.testOffsetY() == -50);
    // 0/0 = NaN -> axisValue -1.0 (the JNC fallthrough), FISTP ->
    // INT_MIN -> -50.
    mdk::MouseMenuController c3(s, true, 0, "ABG", {1, 4, 2, 0},
                                {0, 0, 50});
    in = {};
    c3.update(in);   // zero deltas
    CHECK(near(c3.axisValue(0), -1.0));
    CHECK(c3.testOffsetX() == -50 && c3.testOffsetY() == -50);
  }

  // The blink accumulator (DAT_0049a770) advances
  // floor(acc+DAT_0049b6f0) per flagged draw; bit 3 is the phase.
  {
    mdk::FrontendMachineState s;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    CHECK(ctl.markerAccumulator() == 0);
    CHECK(!ctl.advanceBlink());   // 0+1.0 -> 1, bit3 clear
    CHECK(ctl.markerAccumulator() == 1);
    for (int i = 0; i < 7; ++i) ctl.advanceBlink();
    CHECK(ctl.markerAccumulator() == 8);
    CHECK(ctl.advanceBlink());    // 9 — bit 3 set
  }

  // Mutate-back-to-default keeps the dirty latch (same OBSERVED
  // latch semantics as the other screens).
  {
    mdk::FrontendMachineState s;
    s.mouseX = 100;
    s.mouseY = 17;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.mouseDy = 1;   // -> 18: row 1
    ctl.update(in);
    in = {};
    in.leftHeld = true;
    ctl.update(in);               // MouseOn off
    in = {};
    ctl.update(in);
    in.leftHeld = true;
    ctl.update(in);               // MouseOn back on
    CHECK(ctl.mouseOn() && ctl.settingsDirty());
  }

  // Input order (OBSERVED): Esc first, then prev before next.
  {
    mdk::FrontendMachineState s;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    in.nextHeld = true;
    ctl.update(in);
    CHECK(ctl.selection() == 0);   // 0-1=22 then 22+1=0
  }
}

// Phase 4K — Keyboard child screen (FUN_0041f030 entry /
// FUN_0041f18c frame) plus the internal key-code domain helpers.
void test_keyboard_controller() {
  // FUN_0046b688's DIK -> internal rule: offsets <= 0x7f are the
  // code itself; > 0x7f translate through table_49bbf0[DIK & 0x7f],
  // unmapped entries yielding 0x7f — itself a capturable code.
  {
    CHECK(mdk::internalKeyFromDik(0x01) == 0x01);   // base domain
    CHECK(mdk::internalKeyFromDik(0x1c) == 0x1c);   // RETURN
    CHECK(mdk::internalKeyFromDik(0x2d) == 0x2d);   // 'X'
    CHECK(mdk::internalKeyFromDik(0x7f) == 0x7f);
    // The proven extended mappings.
    CHECK(mdk::internalKeyFromDik(0x9c) == 96);     // KP_ENTER
    CHECK(mdk::internalKeyFromDik(0x9d) == 97);     // RCTRL
    CHECK(mdk::internalKeyFromDik(0xb5) == 99);     // KP_/
    CHECK(mdk::internalKeyFromDik(0xb7) == 100);    // SYSRQ
    CHECK(mdk::internalKeyFromDik(0xb8) == 101);    // RALT
    CHECK(mdk::internalKeyFromDik(0xc7) == 102);    // HOME
    CHECK(mdk::internalKeyFromDik(0xc8) == 103);    // UP
    CHECK(mdk::internalKeyFromDik(0xc9) == 104);    // PGUP
    CHECK(mdk::internalKeyFromDik(0xcb) == 105);    // LEFT
    CHECK(mdk::internalKeyFromDik(0xcd) == 106);    // RIGHT
    CHECK(mdk::internalKeyFromDik(0xcf) == 107);    // END
    CHECK(mdk::internalKeyFromDik(0xd0) == 108);    // DOWN
    CHECK(mdk::internalKeyFromDik(0xd1) == 109);    // PGDN
    CHECK(mdk::internalKeyFromDik(0xd2) == 110);    // INS
    CHECK(mdk::internalKeyFromDik(0xd3) == 111);    // DEL
    CHECK(mdk::internalKeyFromDik(0xef) == 112);    // last mapped
    // Unmapped extended entries resolve to 0x7f.
    CHECK(mdk::internalKeyFromDik(0x80) == 0x7f);
    CHECK(mdk::internalKeyFromDik(0xc5) == 0x7f);   // PAUSE
    CHECK(mdk::internalKeyFromDik(0xdb) == 0x7f);   // LWIN
    // The mask is applied BEFORE the table index — 0x1ff & 0x7f.
    CHECK(mdk::internalKeyFromDik(0x1ff) == 0x7f);
    // NATIVE hardening: a negative offset is rejected outright.
    CHECK(mdk::internalKeyFromDik(-1) == -1);
  }

  // FUN_00419168 — the lowest set bit across the four 32-bit edge
  // dwords; an empty bitmap returns the 0 sentinel.
  {
    mdk::KeyboardEdgeBitmap e{};
    CHECK(mdk::keyboardFirstEdgeBit(e) == 0);
    e[0] = 0x8;
    CHECK(mdk::keyboardFirstEdgeBit(e) == 3);
    e = {};
    e[0] = 0x80000000u;
    CHECK(mdk::keyboardFirstEdgeBit(e) == 31);
    e = {};
    e[1] = 0x1;
    CHECK(mdk::keyboardFirstEdgeBit(e) == 32);
    e = {};
    e[2] = 0x10000u;
    CHECK(mdk::keyboardFirstEdgeBit(e) == 80);
    e = {};
    e[3] = 0x80000000u;
    CHECK(mdk::keyboardFirstEdgeBit(e) == 127);
    // Word order wins over bit position in a later word.
    e = {};
    e[0] = 0x80000000u;
    e[3] = 0x1;
    CHECK(mdk::keyboardFirstEdgeBit(e) == 31);
    e = {};
    e[0] = 0x10;
    e[3] = 0x1;
    CHECK(mdk::keyboardFirstEdgeBit(e) == 4);
  }

  // FUN_0041925c — lowest bit first, THEN the right-modifier folds:
  // 0x36->0x2a (RSHIFT->LSHIFT), 0x61->0x1d (RCTRL->LCTRL),
  // 0x65->0x38 (RALT->LALT). The fold applies to the winner only —
  // a lower un-folded bit still wins over a right-modifier bit.
  {
    mdk::KeyboardEdgeBitmap e{};
    CHECK(mdk::keyboardPollCapture(e) == 0);
    e[1] = 1u << (0x36 - 32);                       // code 54 RSHIFT
    CHECK(mdk::keyboardPollCapture(e) == 0x2a);   // RSHIFT -> LSHIFT
    e = {};
    e[3] = 1u << (0x61 - 96);                       // code 97 RCTRL
    CHECK(mdk::keyboardPollCapture(e) == 0x1d);   // RCTRL -> LCTRL
    e = {};
    e[3] = 1u << (0x65 - 96);                       // code 101 RALT
    CHECK(mdk::keyboardPollCapture(e) == 0x38);   // RALT -> LALT
    e = {};
    e[1] = 1u << (0x2d - 32);                       // code 45 'X'
    CHECK(mdk::keyboardPollCapture(e) == 45);     // 'X' untouched
    // Selection before fold: bit 20 < 0x36 wins unfolded.
    e = {};
    e[0] = 1u << 20;
    e[1] = 1u << (0x36 - 32);
    CHECK(mdk::keyboardPollCapture(e) == 20);
    // Bit 0x36 alone still folds even with a LATER bit present.
    e = {};
    e[1] = 1u << (0x36 - 32);
    e[3] = 0x1;
    CHECK(mdk::keyboardPollCapture(e) == 0x2a);
  }

  // The three 128-byte glyph tables + the LANG selector — EN is
  // QWERTY, FR is AZERTY (Q->A, W->Z, M->',' positions), DE is
  // QWERTZ (Y<->Z). Anything but 'F'/'G' — including the soft
  // resolver's 0 — selects English.
  {
    const auto& en = mdk::keyboardGlyphTableEn();
    const auto& fr = mdk::keyboardGlyphTableFr();
    const auto& de = mdk::keyboardGlyphTableDe();
    CHECK(&mdk::keyboardGlyphTable('E') == &en);
    CHECK(&mdk::keyboardGlyphTable('F') == &fr);
    CHECK(&mdk::keyboardGlyphTable('G') == &de);
    CHECK(&mdk::keyboardGlyphTable(0) == &en);    // missing LANG
    CHECK(&mdk::keyboardGlyphTable('x') == &en);
    // English: letters/digits verbatim, icon bytes for controls.
    CHECK(en[0x1e] == 'A' && en[0x2d] == 'X' && en[0x02] == '1');
    CHECK(en[0x1c] == 0x04);    // RETURN icon
    CHECK(en[0x39] == 0x08);    // SPACE icon
    CHECK(en[0x2a] == 0x06);    // LSHIFT icon
    CHECK(en[0x1d] == 0x05);    // LCTRL icon
    CHECK(en[0x38] == 0x07);    // LALT icon
    CHECK(en[0x3a] == 0x09);    // CAPS icon
    CHECK(en[0x33] == ',' && en[0x34] == '.');
    CHECK(en[0x1a] == '[' && en[0x1b] == ']');
    // The extended nav cluster -> icon glyphs 0x88..0x92.
    CHECK(en[103] == 0x89);     // UP arrow
    CHECK(en[105] == 0x8b);     // LEFT arrow
    CHECK(en[106] == 0x8c);     // RIGHT arrow
    CHECK(en[108] == 0x8e);     // DOWN arrow
    CHECK(en[0x7f] == 0x00);    // internal 127 draws blank
    // AZERTY differences (OBSERVED bytes).
    CHECK(fr[0x10] == 'A');     // QWERTY Q position -> A
    CHECK(fr[0x11] == 'Z');     // W position -> Z
    CHECK(fr[0x1e] == 'Q');     // A position -> Q
    CHECK(fr[0x32] == ',');     // M position -> ','
    CHECK(fr[0x27] == 'M');     // ';' position -> M
    // QWERTZ differences.
    CHECK(de[0x15] == 'Z');     // Y position -> Z
    CHECK(de[0x2c] == 'Y');     // Z position -> Y
    // All three share the icon rows and the extended cluster.
    CHECK(fr[103] == 0x89 && de[103] == 0x89);
    CHECK(fr[0x39] == 0x08 && de[0x39] == 0x08);
  }

  // Settings <-> 29-dword block mapping: the 19 persisted slots
  // overlay the factory block; the ten hidden hotkey slots
  // (g14..g23) always boot at factory 2..11 and never serialize.
  {
    mdk::FrontendSettings s;
    const auto g = mdk::keyboardGlobalsFromSettings(s);
    CHECK(g == mdk::kKeyboardDefaults);
    s.keyLeft = 30;      // slot 69 -> g0
    s.keySniper = 45;    // slot 76 -> g7
    s.keySideL = 20;     // slot 86 -> g27
    s.keySideR = 21;     // slot 87 -> g28
    const auto g2 = mdk::keyboardGlobalsFromSettings(s);
    CHECK(g2[0] == 30 && g2[7] == 45 && g2[27] == 20 &&
          g2[28] == 21);
    CHECK(g2[14] == 2 && g2[23] == 11);   // hidden slots factory
    mdk::FrontendSettings back;
    mdk::keyboardSettingsFromGlobals(back, g2);
    CHECK(back.keyLeft == 30 && back.keySniper == 45 &&
          back.keySideL == 20 && back.keySideR == 21);
    CHECK(back.keyFire == 29);            // untouched round-trips
    // The proven row->global draw-order map (NOT settings order).
    CHECK(mdk::kKeyboardRowToGlobal[0] == 0);
    CHECK(mdk::kKeyboardRowToGlobal[5] == 27);   // KM_SIDEL
    CHECK(mdk::kKeyboardRowToGlobal[6] == 5);    // KM_SIDE
    CHECK(mdk::kKeyboardRowToGlobal[7] == 28);   // KM_SIDER
    CHECK(mdk::kKeyboardRowToGlobal[8] == 7);    // KM_SNIPE
    CHECK(mdk::kKeyboardRowToGlobal[9] == 6);    // KM_FIRE
    CHECK(mdk::kKeyboardRowToGlobal[16] == 24);  // KM_INEXT
    CHECK(mdk::kKeyboardRowToGlobal[17] == 25);  // KM_IPREV
    CHECK(mdk::kKeyboardRowToGlobal[18] == 26);  // KM_IUSE
  }

  // Entry (FUN_0041f030): capture clear, selection = 0x14 — the
  // KM_QUIT row, NOT 0 — the 29-dword block borrowed verbatim, and
  // the shared machine state carried over untouched.
  {
    mdk::FrontendMachineState ms;
    ms.mouseX = 123;
    ms.mouseY = 45;
    ms.tick = 9;
    ms.markerAcc = 7;
    mdk::KeyboardMenuController kb(ms, mdk::kKeyboardDefaults, true);
    CHECK(kb.selection() == 20);
    CHECK(!kb.capture());
    CHECK(kb.keyGlobals() == mdk::kKeyboardDefaults);
    CHECK(kb.settingsDirty());            // the carried flag
    CHECK(kb.mouseX() == 123 && kb.mouseY() == 45);
    CHECK(kb.tick() == 9 && kb.markerAccumulator() == 7);
    CHECK(kb.pendingAction() == mdk::KeyboardAction::None);
  }

  // Normal navigation: UP|LEFT -> sel-1 (wrap <0 -> 20);
  // DOWN|RIGHT -> sel+1 (wrap >=21 -> 0). LEFT/RIGHT are the second
  // query in each pair — they only run when UP/DOWN didn't fire.
  // A held-key tap is press + release-update (the repeat deadline
  // only resets on a released frame).
  {
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   mdk::kKeyboardDefaults, false);
    mdk::FrontendMenuInput in;
    in.prevHeld = true;
    kb.update(in);
    in = {};
    kb.update(in);
    CHECK(kb.selection() == 19);          // 20 -> 19
    in.nextHeld = true;
    kb.update(in);
    in = {};
    kb.update(in);
    CHECK(kb.selection() == 20);          // 19 -> 20
    in.nextHeld = true;
    kb.update(in);
    in = {};
    kb.update(in);
    CHECK(kb.selection() == 0);           // wraps >=21 -> 0
    in.prevHeld = true;
    kb.update(in);
    in = {};
    kb.update(in);
    CHECK(kb.selection() == 20);          // wraps <0 -> 20
    in.leftHeld = true;
    kb.update(in);
    in = {};
    kb.update(in);
    CHECK(kb.selection() == 19);          // LEFT = prev
    in.rightHeld = true;
    kb.update(in);
    in = {};
    kb.update(in);
    CHECK(kb.selection() == 20);          // RIGHT = next
  }

  // Mouse hit-test (OBSERVED, inside the dx||dy||buttons gate):
  // y>=64 -> band trunc((y-50)/30), x>=320 adds 10, 0<=band<19;
  // y<64 -> [2,18) = row 19, [18,34) = row 20; the other bands
  // select nothing. Selection only — never activates.
  {
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   mdk::kKeyboardDefaults, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 64 - 180;                // -> (300,64): band 0
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 0);
    // y=350 x<320 -> band trunc(300/30)=10 <=10 -> row 10 — the
    // OBSERVED quirk: the left column's band domain reaches into
    // the right column's row range.
    in.mouseDy = 350 - 64;
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 10);
    // Same y at x>=320 -> band 10+10=20 -> >=19 -> no select.
    in.mouseDx = 400 - 300;
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 10);
    // Right column mid-band: y=94 -> band trunc(44/30)=1 -> row 11.
    in.mouseDy = 94 - 350;
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 11);
    // Reset band [2,18) -> 19; Quit band [18,34) -> 20.
    in.mouseDy = 8 - 94;
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 19);
    in.mouseDy = 20 - 8;
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 20);
    // y<2 selects nothing (stays 20); [34,64) selects nothing.
    in.mouseDy = 1 - 20;
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 20);
    in.mouseDy = 40 - 1;
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 20);
    // (A buttons!=0 frame also opens the gate — but it fires the
    // activate query in the same frame; see the click-activate
    // test below.)
  }

  // Capture entry: activate on a binding row sets DAT_0054bca8 and
  // the frame still draws (no early end).
  {
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   mdk::kKeyboardDefaults, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 180;               // -> band 8 (KM_SNIPE)
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 8);
    in.confirmEdge = true;
    kb.update(in);
    in = {};
    CHECK(kb.capture());
    CHECK(!kb.frameEndedEarly());
    CHECK(kb.pendingAction() == mdk::KeyboardAction::None);
    // A raw-key edge commits through row 8 -> g7 (KeySniper).
    in.rawKeyEdge[1] = 1u << (45 - 32);   // internal 45 = 'X'
    kb.update(in);
    in = {};
    CHECK(!kb.capture());
    CHECK(kb.keyAt(7) == 45);
    CHECK(kb.settingsDirty());
    CHECK(!kb.frameEndedEarly());         // commit still draws
  }

  // Capture Esc-cancel: the Esc edge is checked BEFORE the raw-key
  // poll, so a simultaneous key edge never commits; Escape can
  // never become a binding and the cancel is NOT an exit.
  {
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   mdk::kKeyboardDefaults, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 180;
    kb.update(in);
    in = {};
    in.confirmEdge = true;
    kb.update(in);
    in = {};
    CHECK(kb.capture());
    // The physical ESC event sets both the semantic flag and the
    // raw bit — the cancel path wins, the poll never runs.
    in.cancelEdge = true;
    in.rawKeyEdge[0] = 0x2;               // internal 1 = ESCAPE
    kb.update(in);
    in = {};
    CHECK(!kb.capture());
    CHECK(kb.keyAt(7) == 57);             // binding untouched
    CHECK(!kb.settingsDirty());
    CHECK(kb.pendingAction() == mdk::KeyboardAction::None);
    CHECK(!kb.frameEndedEarly());         // cancel still draws
  }

  // A nav key captured as a binding: the physical UP event feeds
  // both prevHeld and the raw edge — capture owns the input, the
  // row does not move, and the extended code stores verbatim.
  {
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   mdk::kKeyboardDefaults, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 180;
    kb.update(in);
    in = {};
    in.confirmEdge = true;
    kb.update(in);
    in = {};
    CHECK(kb.capture());
    in.prevHeld = true;                   // the physical UP held
    in.rawKeyEdge[3] = 1u << (103 - 96);  // ext UP -> internal 103
    kb.update(in);
    in = {};
    CHECK(!kb.capture());
    CHECK(kb.keyAt(7) == 103);
    CHECK(kb.selection() == 8);           // no nav happened
    // RETURN captured likewise — internal 28 binds, it does NOT
    // re-activate (capture returns before the activate query).
    in.confirmEdge = true;
    kb.update(in);
    in = {};
    CHECK(kb.capture());
    in.confirmEdge = true;                // the physical RETURN edge
    in.rawKeyEdge[0] = 1u << 28;
    kb.update(in);
    in = {};
    CHECK(!kb.capture());
    CHECK(kb.keyAt(7) == 28);
    CHECK(kb.pendingAction() == mdk::KeyboardAction::None);
  }

  // Same-value rebind: the commit is skipped entirely — no write,
  // no new dirty latch (OBSERVED FUN_0041f18c).
  {
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   mdk::kKeyboardDefaults, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 180;
    kb.update(in);
    in = {};
    in.confirmEdge = true;
    kb.update(in);
    in = {};
    CHECK(kb.capture());
    in.rawKeyEdge[1] = 1u << (57 - 32);   // SPACE = current g7
    kb.update(in);
    in = {};
    CHECK(!kb.capture());
    CHECK(kb.keyAt(7) == 57);
    CHECK(!kb.settingsDirty());
  }

  // Duplicate binding: the captured code stores verbatim even when
  // already bound elsewhere — no swap, no reject, no unbind (the
  // factory defaults themselves share 'A' and 'Z' pairs).
  {
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   mdk::kKeyboardDefaults, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 180;               // row 8 KeySniper (g7)
    kb.update(in);
    in = {};
    in.confirmEdge = true;
    kb.update(in);
    in = {};
    in.rawKeyEdge[1] = 1u << (45 - 32);   // 'X' — already g5/g6? no:
    kb.update(in);                        // g5 KeySide = 45
    in = {};
    CHECK(kb.keyAt(7) == 45);             // Sniper = 'X'
    CHECK(kb.keyAt(5) == 45);             // Side keeps 'X' — both bind
    CHECK(kb.settingsDirty());
  }

  // Right-modifier capture normalizes to the left/base code —
  // through the edge bitmap (the physical RSHIFT event sets bit
  // 0x36; the poll folds to 0x2a before the store).
  {
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   mdk::kKeyboardDefaults, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 180;
    kb.update(in);
    in = {};
    in.confirmEdge = true;
    kb.update(in);
    in = {};
    in.rawKeyEdge[1] = 1u << (0x36 - 32); // RSHIFT bit
    kb.update(in);
    in = {};
    CHECK(kb.keyAt(7) == 0x2a);           // stored as LSHIFT
  }

  // KM_RESET (row 19): FUN_00425db0's 29-dword mirror copy restores
  // ALL slots — the 19 visible bindings AND the ten hidden hotkey
  // slots — and ECX=1 is preserved so dirty latches even when the
  // visible values were already factory.
  {
    auto keys = mdk::kKeyboardDefaults;
    keys[7] = 45;                          // a mutated visible slot
    keys[14] = 99;                         // a mutated hidden hotkey
    keys[23] = 0;                          // ...and the last hidden
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   keys, false);
    mdk::FrontendMenuInput in;
    in.mouseDy = 8 - 180;                  // [2,18) band -> row 19
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 19);
    in.confirmEdge = true;
    kb.update(in);
    in = {};
    CHECK(kb.selection() == 19);           // reset keeps the row
    CHECK(!kb.capture());
    CHECK(kb.keyGlobals() == mdk::kKeyboardDefaults);   // ALL 29
    CHECK(kb.keyAt(7) == 57 && kb.keyAt(14) == 2 &&
          kb.keyAt(23) == 11);
    CHECK(kb.settingsDirty());             // ECX=1 preserved
    CHECK(!kb.frameEndedEarly());          // reset still draws
    // Dirty latches even when nothing visible changed.
    mdk::KeyboardMenuController kb2(mdk::FrontendMachineState{},
                                    mdk::kKeyboardDefaults, false);
    in = {};
    in.mouseDy = 8 - 180;
    kb2.update(in);
    in = {};
    in.confirmEdge = true;
    kb2.update(in);
    CHECK(kb2.settingsDirty());
  }

  // KM_QUIT (row 20 — the entry row): activate -> Back + endedEarly
  // (the original RETs before the draw). Esc in NORMAL state takes
  // the same exit — distinct from the capture-mode cancel.
  {
    mdk::KeyboardMenuController kb(mdk::FrontendMachineState{},
                                   mdk::kKeyboardDefaults, false);
    mdk::FrontendMenuInput in;
    CHECK(kb.selection() == 20);           // entry IS the quit row
    in.confirmEdge = true;
    kb.update(in);
    CHECK(kb.pendingAction() == mdk::KeyboardAction::Back);
    CHECK(kb.frameEndedEarly());
    CHECK(kb.consumeAction() == mdk::KeyboardAction::Back);
    CHECK(kb.consumeAction() == mdk::KeyboardAction::None);
    // Esc in normal mode: same exit, same early end.
    mdk::KeyboardMenuController kb2(mdk::FrontendMachineState{},
                                    mdk::kKeyboardDefaults, false);
    in = {};
    in.cancelEdge = true;
    kb2.update(in);
    CHECK(kb2.pendingAction() == mdk::KeyboardAction::Back);
    CHECK(kb2.frameEndedEarly());
    // A mouse-click activate works too (button-latch path): the
    // same event opens the hit-test gate — a click in the [18,34)
    // band selects the Quit row and fires it in one frame.
    mdk::FrontendMachineState ms3;
    ms3.mouseX = 300;
    ms3.mouseY = 20;
    mdk::KeyboardMenuController kb3(ms3, mdk::kKeyboardDefaults,
                                    false);
    in = {};
    kb3.update(in);                        // buttons=0 arms the latch
    in.mouseButtons = 0x1;
    kb3.update(in);                        // click: gate + activate
    CHECK(kb3.pendingAction() == mdk::KeyboardAction::Back);
    CHECK(kb3.frameEndedEarly());
  }

  // FUN_00414b28 — markerAcc advances by floor(acc + smoothed) per
  // flagged draw; the return is the post-advance bit-3 blink phase.
  {
    mdk::FrontendMachineState ms;
    ms.markerAcc = 0;
    ms.timing.smoothed = 4.0f;
    mdk::KeyboardMenuController kb(ms, mdk::kKeyboardDefaults, false);
    CHECK(kb.advanceBlink() == false);     // 0 -> 4: bit3 clear
    CHECK(kb.markerAccumulator() == 4);
    CHECK(kb.advanceBlink() == true);      // 4 -> 8: bit3 set
    CHECK(kb.advanceBlink() == true);      // 8 -> 12: bit3 set
    CHECK(kb.advanceBlink() == false);     // 12 -> 16: bit3 clear
    CHECK(kb.markerAccumulator() == 16);
  }
}

// Phase 4G/4H/4I — native-owned frontend settings persistence
// (Skill, Brightness, ForcePCorrect, SoundFX, SoundMusic): the
// FUN_004260ac/FUN_00425de4 contract reimplemented against
// caller-supplied paths outside the read-only DataRoot.
void test_frontend_settings() {
  // Factory defaults — the FUN_00425de4 defaults-copy result for
  // BUILD_A (none of the five proven keys appear in its MDK.CFG):
  // skill 1 (Normal), brightness 0, ForcePCorrect FALSE, SoundFX
  // 70, SoundMusic 100.
  {
    const mdk::FrontendSettings s;
    CHECK(s.skill == 1 && s.brightness == 0 && !s.forcePCorrect);
    CHECK(s.soundFx == 70 && s.soundMusic == 100);
  }

  // Delta serialization (FUN_004260ac, OBSERVED): header + blank
  // line always; `Skill = %d` iff != factory 1. CRLF lines like
  // BUILD_A's on-disk MDK.CFG.
  {
    const std::string easy =
        mdk::serializeFrontendSettings(mdk::FrontendSettings{0});
    const std::string normal =
        mdk::serializeFrontendSettings(mdk::FrontendSettings{1});
    const std::string hard =
        mdk::serializeFrontendSettings(mdk::FrontendSettings{2});
    CHECK(easy.find("; MDK Configuration file automatically "
                    "generated by MDK") == 0);
    CHECK(easy.find("Skill = 0\r\n") != std::string::npos);
    CHECK(normal.find("; MDK Configuration file") == 0);
    CHECK(normal.find("Skill") == std::string::npos);
    CHECK(hard.find("Skill = 2\r\n") != std::string::npos);
  }

  // Phase 4H entries (OBSERVED): `Brightness = %d` iff != factory 0;
  // `ForcePCorrect = TRUE` iff != factory FALSE (the writer emits a
  // bool only when the live dword differs from the mirror — FALSE
  // is never emitted). Emission order is the settings-table order:
  // Skill (88) -> Brightness (89) -> ForcePCorrect (90).
  {
    mdk::FrontendSettings s;
    s.skill = 2;
    s.brightness = 5;
    s.forcePCorrect = true;
    const std::string out = mdk::serializeFrontendSettings(s);
    const auto pSkill = out.find("Skill = 2\r\n");
    const auto pBright = out.find("Brightness = 5\r\n");
    const auto pFpc = out.find("ForcePCorrect = TRUE\r\n");
    CHECK(pSkill != std::string::npos &&
          pBright != std::string::npos &&
          pFpc != std::string::npos);
    CHECK(pSkill < pBright && pBright < pFpc);
    // Defaults emit nothing for the new keys.
    const std::string def = mdk::serializeFrontendSettings(
        mdk::FrontendSettings{});
    CHECK(def.find("Brightness") == std::string::npos &&
          def.find("ForcePCorrect") == std::string::npos);
    // A non-default ForcePCorrect alone emits just its own line.
    mdk::FrontendSettings only;
    only.forcePCorrect = true;
    const std::string onlyOut = mdk::serializeFrontendSettings(only);
    CHECK(onlyOut.find("ForcePCorrect = TRUE\r\n") !=
          std::string::npos);
    CHECK(onlyOut.find("Skill") == std::string::npos &&
          onlyOut.find("Brightness") == std::string::npos);
  }

  // Phase 4I entries (OBSERVED): `SoundFX = %d` iff != factory 70,
  // `SoundMusic = %d` iff != factory 100 — settings-table entries
  // 8/9, so the full five-entry file emits them BEFORE Skill.
  {
    mdk::FrontendSettings s;
    s.soundFx = 80;
    s.soundMusic = 90;
    s.skill = 2;
    s.brightness = 5;
    s.forcePCorrect = true;
    const std::string out = mdk::serializeFrontendSettings(s);
    const auto pFx = out.find("SoundFX = 80\r\n");
    const auto pMus = out.find("SoundMusic = 90\r\n");
    const auto pSkill = out.find("Skill = 2\r\n");
    const auto pBright = out.find("Brightness = 5\r\n");
    const auto pFpc = out.find("ForcePCorrect = TRUE\r\n");
    CHECK(pFx != std::string::npos && pMus != std::string::npos &&
          pSkill != std::string::npos &&
          pBright != std::string::npos && pFpc != std::string::npos);
    // Table order: 8 -> 9 -> 88 -> 89 -> 90.
    CHECK(pFx < pMus && pMus < pSkill && pSkill < pBright &&
          pBright < pFpc);
    // Defaults emit nothing for the new keys.
    const std::string def = mdk::serializeFrontendSettings(
        mdk::FrontendSettings{});
    CHECK(def.find("SoundFX") == std::string::npos &&
          def.find("SoundMusic") == std::string::npos);
    // A non-default SoundFX alone emits just its own line.
    mdk::FrontendSettings only;
    only.soundFx = 0;
    const std::string onlyOut = mdk::serializeFrontendSettings(only);
    CHECK(onlyOut.find("SoundFX = 0\r\n") != std::string::npos);
    CHECK(onlyOut.find("SoundMusic") == std::string::npos &&
          onlyOut.find("Skill") == std::string::npos);
  }

  // Parser (FUN_00425de4 apply loop, OBSERVED shape): defaults
  // first, `name = value` lines applied in order — the last valid
  // Skill line wins; keys match case-insensitively (FUN_0042fab4's
  // `and 0xdf` fold).
  {
    CHECK(mdk::parseFrontendSettings("").settings.skill == 1);
    CHECK(mdk::parseFrontendSettings("Skill = 0").settings.skill == 0);
    CHECK(mdk::parseFrontendSettings("Skill = 1").settings.skill == 1);
    CHECK(mdk::parseFrontendSettings("Skill = 2").settings.skill == 2);
    CHECK(mdk::parseFrontendSettings("SKILL = 2").settings.skill == 2);
    CHECK(mdk::parseFrontendSettings("skill=0").settings.skill == 0);
    // Comments, blank lines, and unknown keys are skipped — the
    // original's apply loop only touches table entries it knows.
    const auto p = mdk::parseFrontendSettings(
        "; MDK Configuration file automatically generated by MDK\r\n"
        "\r\nSoundIDX = 7\r\nSkill = 0 ; easy\r\n");
    CHECK(p.settings.skill == 0 && p.ignoredSkillLines == 0);
    // Last valid line wins (sequential apply, like the original).
    CHECK(mdk::parseFrontendSettings("Skill = 0\nSkill = 2\n")
              .settings.skill == 2);
    // A serialized file round-trips through the parser.
    CHECK(mdk::parseFrontendSettings(
              mdk::serializeFrontendSettings(mdk::FrontendSettings{0}))
              .settings.skill == 0);
    CHECK(mdk::parseFrontendSettings(
              mdk::serializeFrontendSettings(mdk::FrontendSettings{2}))
              .settings.skill == 2);
  }

  // Phase 4H parser entries: `Brightness` is an int (same
  // strtol-style leading parse + the native [0,7] hardening);
  // `ForcePCorrect` is a type-2 bool — toupper(first non-space
  // value char) == 'T' (FUN_0047d1a5 + `cmp 0x54`), applied
  // unconditionally like the original.
  {
    CHECK(mdk::parseFrontendSettings("Brightness = 0")
              .settings.brightness == 0);
    CHECK(mdk::parseFrontendSettings("Brightness = 7")
              .settings.brightness == 7);
    CHECK(mdk::parseFrontendSettings("BRIGHTNESS = 3")
              .settings.brightness == 3);
    // Leading-integer semantics like the original's strtol family.
    CHECK(mdk::parseFrontendSettings("Brightness = 5px")
              .settings.brightness == 5);
    // Type-2: 'T'/'t' true, anything else false — never ignored.
    auto p = mdk::parseFrontendSettings("ForcePCorrect = TRUE");
    CHECK(p.settings.forcePCorrect && p.ignoredSkillLines == 0 &&
          p.ignoredBrightnessLines == 0);
    CHECK(mdk::parseFrontendSettings("ForcePCorrect = true")
              .settings.forcePCorrect);
    CHECK(mdk::parseFrontendSettings("ForcePCorrect = FALSE")
              .settings.forcePCorrect == false);
    CHECK(mdk::parseFrontendSettings("ForcePCorrect = xyz")
              .settings.forcePCorrect == false);
    CHECK(mdk::parseFrontendSettings("ForcePCorrect = T")
              .settings.forcePCorrect);
    CHECK(mdk::parseFrontendSettings("ForcePCorrect = 1")
              .settings.forcePCorrect == false);   // '1' != 'T'
    CHECK(mdk::parseFrontendSettings("ForcePCorrect = ")
              .settings.forcePCorrect == false);   // no value char
    // A serialized triple round-trips through the parser.
    mdk::FrontendSettings triple;
    triple.skill = 0;
    triple.brightness = 4;
    triple.forcePCorrect = true;
    const auto rt = mdk::parseFrontendSettings(
        mdk::serializeFrontendSettings(triple));
    CHECK(rt.settings.skill == 0 && rt.settings.brightness == 4 &&
          rt.settings.forcePCorrect);
  }

  // Phase 4I parser entries: SoundFX/SoundMusic are type-0 ints —
  // the same strtol-family leading parse, case-insensitive key,
  // last valid line wins; [0,100] domain under the NATIVE
  // hardening (each key counts its own ignored lines).
  {
    CHECK(mdk::parseFrontendSettings("SoundFX = 0")
              .settings.soundFx == 0);
    CHECK(mdk::parseFrontendSettings("SoundFX = 100")
              .settings.soundFx == 100);
    CHECK(mdk::parseFrontendSettings("SOUNDFX = 55")
              .settings.soundFx == 55);
    CHECK(mdk::parseFrontendSettings("soundmusic=30")
              .settings.soundMusic == 30);
    CHECK(mdk::parseFrontendSettings("SoundMusic = 99")
              .settings.soundMusic == 99);
    // Leading-integer semantics like the original's strtol family.
    CHECK(mdk::parseFrontendSettings("SoundFX = 42px")
              .settings.soundFx == 42);
    // Last valid line wins (sequential apply).
    CHECK(mdk::parseFrontendSettings("SoundFX = 10\nSoundFX = 60\n")
              .settings.soundFx == 60);
    // A serialized five-tuple round-trips through the parser.
    mdk::FrontendSettings five;
    five.soundFx = 10;
    five.soundMusic = 20;
    five.skill = 0;
    five.brightness = 4;
    five.forcePCorrect = true;
    const auto rt = mdk::parseFrontendSettings(
        mdk::serializeFrontendSettings(five));
    CHECK(rt.settings.soundFx == 10 && rt.settings.soundMusic == 20 &&
          rt.settings.skill == 0 && rt.settings.brightness == 4 &&
          rt.settings.forcePCorrect);
  }

  // Phase 4J entries (OBSERVED — settings table 49-68): the mouse
  // block sits between SoundMusic (9) and Skill (88) in table
  // order. Type-3 strings (axes/butt maps) compare case-insensibly
  // like the original's fold; type-0 masks emit %d; type-1 floats
  // emit %g; type-2 bool emits TRUE only; MouseYReversed's float
  // slot emits the denormal its raw bits represent.
  {
    mdk::FrontendSettings s;
    CHECK(s.mouseWAxesMap == "ABG" && s.mouseDAxesMap == "ABG");
    CHECK(s.mouseWButtMap == "ACB" && s.mouseDButtMap == "ACB");
    CHECK(s.mouseWButtMapA == 1 && s.mouseWButtMapB == 4 &&
          s.mouseWButtMapC == 2 && s.mouseWButtMapD == 0);
    CHECK(s.mouseDButtMapA == 1 && s.mouseDButtMapD == 0);
    CHECK(near(s.mouseWXScale, 16.0) && near(s.mouseWZScale, 50.0));
    CHECK(near(s.mouseDXScale, 16.0) && near(s.mouseDZScale, 50.0));
    CHECK(s.mouseOn && s.mouseYReversed == 0);
    // Defaults emit none of the mouse keys.
    const std::string def = mdk::serializeFrontendSettings(s);
    CHECK(def.find("Mouse") == std::string::npos);
    // Every dirty mouse entry emits, in table order 49..68, all
    // before Skill (88).
    s.mouseWAxesMap = "BCD";       // 49
    s.mouseDAxesMap = "BCD";       // 50
    s.mouseWButtMap = "BCA";       // 51
    s.mouseDButtMap = "BCA";       // 52
    s.mouseWButtMapA = 3;          // 53
    s.mouseWButtMapB = 9;          // 54
    s.mouseWButtMapC = 5;          // 55
    s.mouseWButtMapD = 32768;      // 56 — BUILD_A's real dword
    s.mouseDButtMapA = 3;          // 57
    s.mouseDButtMapB = 9;          // 58
    s.mouseDButtMapC = 5;          // 59
    s.mouseDButtMapD = 32768;      // 60
    s.mouseWXScale = 20.0f;        // 61
    s.mouseWYScale = 20.0f;        // 62
    s.mouseWZScale = 60.0f;        // 63
    s.mouseDXScale = 20.0f;        // 64
    s.mouseDYScale = 20.0f;        // 65
    s.mouseDZScale = 60.0f;        // 66
    s.mouseOn = false;             // 67
    s.mouseYReversed = 1;          // 68 — raw bits 1 -> denormal
    s.skill = 2;
    const std::string out = mdk::serializeFrontendSettings(s);
    const char* keys[] = {
        "MouseWAxesMap = BCD",   "MouseDAxesMap = BCD",
        "MouseWButtMap = BCA",   "MouseDButtMap = BCA",
        "MouseWButtMapA = 3",    "MouseWButtMapB = 9",
        "MouseWButtMapC = 5",    "MouseWButtMapD = 32768",
        "MouseDButtMapA = 3",    "MouseDButtMapB = 9",
        "MouseDButtMapC = 5",    "MouseDButtMapD = 32768",
        "MouseWXScale = 20",     "MouseWYScale = 20",
        "MouseWZScale = 60",     "MouseDXScale = 20",
        "MouseDYScale = 20",     "MouseDZScale = 60",
        "MouseOn = FALSE",       "MouseYReversed = 1.4013e-45"};
    std::size_t prev = 0;
    for (const char* k : keys) {
      const auto p = out.find(k);
      CHECK(p != std::string::npos);
      CHECK(p > prev || p == 0);
      prev = p;
    }
    CHECK(out.find("Skill = 2") > out.find("MouseYReversed"));
    // The denormal quirk (OBSERVED): the screen writes int bits
    // 0/1 into a type-1 float slot — bits 1 serialize as the
    // smallest denormal, exactly like BUILD_A's own write would.
    mdk::FrontendSettings q;
    q.mouseYReversed = 1;
    CHECK(mdk::serializeFrontendSettings(q).find(
              "MouseYReversed = 1.4013e-45\r\n") != std::string::npos);
    // Type-3 fold comparison (OBSERVED FUN_0042fab4): a case
    // variant compares equal to the mirror and is NOT emitted.
    mdk::FrontendSettings fold;
    fold.mouseWAxesMap = "abg";
    CHECK(mdk::serializeFrontendSettings(fold).find("MouseWAxesMap")
          == std::string::npos);
  }

  // Phase 4J parser entries: type-3 strings take the value
  // verbatim (the original's unbounded copy); type-0 masks take
  // the leading integer; type-1 floats take the leading float;
  // type-2 bool folds the first value char to 'T'. Keys match
  // case-insensitively.
  {
    const auto p = mdk::parseFrontendSettings(
        "MouseWAxesMap = A0G\r\n"     // BUILD_A's real syntax
        "mousewbuttmapa = 32768\r\n"
        "MouseWXScale = 24.5\r\n"
        "MouseOn = FALSE\r\n"
        "MouseYReversed = 1.4013e-45\r\n");
    CHECK(p.settings.mouseWAxesMap == "A0G");
    CHECK(p.settings.mouseWButtMapA == 32768);
    CHECK(near(p.settings.mouseWXScale, 24.5));
    CHECK(!p.settings.mouseOn);
    CHECK(p.settings.mouseYReversed == 1);   // denormal -> bits 1
    // The bool/type-2 fold like ForcePCorrect.
    CHECK(mdk::parseFrontendSettings("MouseOn = true")
              .settings.mouseOn);
    CHECK(mdk::parseFrontendSettings("MouseOn = xyz")
              .settings.mouseOn == false);
    // A serialized mouse block round-trips — incl. the denormal.
    mdk::FrontendSettings m;
    m.mouseWAxesMap = "B0H";
    m.mouseWButtMapD = 32768;
    m.mouseWXScale = 24.0f;
    m.mouseOn = false;
    m.mouseYReversed = 1;
    const auto rt = mdk::parseFrontendSettings(
        mdk::serializeFrontendSettings(m));
    CHECK(rt.settings.mouseWAxesMap == "B0H");
    CHECK(rt.settings.mouseWButtMapD == 32768);
    CHECK(near(rt.settings.mouseWXScale, 24.0));
    CHECK(!rt.settings.mouseOn && rt.settings.mouseYReversed == 1);
    // Untouched D-set fields survive a round-trip.
    CHECK(rt.settings.mouseDButtMapA == 1 &&
          rt.settings.mouseDAxesMap == "ABG");
  }

  // Phase 4K keyboard entries (OBSERVED — settings table 69-87):
  // 19 type-0 int slots in the internal 0..127 key-code domain,
  // serialized between MouseYReversed (68) and Skill (88) — the
  // table order, not the screen's draw order.
  {
    mdk::FrontendSettings s;
    CHECK(s.keyLeft == 105 && s.keyRight == 106 &&
          s.keyUp == 103 && s.keyDown == 108);
    CHECK(s.keyJump == 56 && s.keySide == 45 && s.keyFire == 29 &&
          s.keySniper == 57);
    CHECK(s.keyTurbo == 42 && s.keySturbo == 58);
    CHECK(s.keyLookUp == 30 && s.keyLookDown == 44 &&
          s.keyZoomIn == 30 && s.keyZoomOut == 44);   // factory dups
    CHECK(s.keyItemNext == 27 && s.keyItemPrev == 26 &&
          s.keyItemUse == 28);
    CHECK(s.keySideL == 51 && s.keySideR == 52);
    // Defaults emit none of the Key* lines.
    const std::string def = mdk::serializeFrontendSettings(s);
    CHECK(def.find("Key") == std::string::npos);
    // Every dirty Key* entry emits in table order 69..87 — all
    // after the mouse block, all before Skill.
    s.keyLeft = 0;        // 69
    s.keyRight = 1;       // 70
    s.keyUp = 2;          // 71
    s.keyDown = 3;        // 72
    s.keyJump = 4;        // 73
    s.keySide = 5;        // 74
    s.keyFire = 6;        // 75
    s.keySniper = 7;      // 76
    s.keyTurbo = 8;       // 77
    s.keySturbo = 9;      // 78
    s.keyLookUp = 10;     // 79
    s.keyLookDown = 11;   // 80
    s.keyZoomIn = 12;     // 81
    s.keyZoomOut = 13;    // 82
    s.keyItemNext = 14;   // 83
    s.keyItemPrev = 15;   // 84
    s.keyItemUse = 16;    // 85
    s.keySideL = 17;      // 86
    s.keySideR = 18;      // 87
    s.skill = 2;
    const std::string out = mdk::serializeFrontendSettings(s);
    const char* keys[] = {
        "KeyLeft = 0",     "KeyRight = 1",   "KeyUp = 2",
        "KeyDown = 3",     "KeyJump = 4",    "KeySide = 5",
        "KeyFire = 6",     "KeySniper = 7",  "KeyTurbo = 8",
        "KeySturbo = 9",   "KeyLookUp = 10", "KeyLookDown = 11",
        "KeyZoomIn = 12",  "KeyZoomOut = 13","KeyItemNext = 14",
        "KeyItemPrev = 15","KeyItemUse = 16","KeySideL = 17",
        "KeySideR = 18"};
    std::size_t prev = 0;
    for (const char* k : keys) {
      const auto p = out.find(k);
      CHECK(p != std::string::npos);
      CHECK(p > prev || p == 0);
      prev = p;
    }
    CHECK(out.find("Skill = 2") > out.find("KeySideR"));
    // A lone rebind emits just its own line.
    mdk::FrontendSettings lone;
    lone.keySniper = 45;
    const std::string only = mdk::serializeFrontendSettings(lone);
    CHECK(only.find("KeySniper = 45\r\n") != std::string::npos);
    CHECK(only.find("KeyLeft") == std::string::npos &&
          only.find("KeyFire") == std::string::npos);
  }

  // Phase 4K parser entries: Key* take the leading integer like
  // the other type-0 slots, match case-insensitively, and the
  // NATIVE [0,127] hardening rejects out-of-domain lines into
  // their own ignored counter (never an original-behavior claim).
  {
    CHECK(mdk::parseFrontendSettings("KeySniper = 45")
              .settings.keySniper == 45);
    CHECK(mdk::parseFrontendSettings("KEYLEFT = 0")
              .settings.keyLeft == 0);
    CHECK(mdk::parseFrontendSettings("KeyItemUse = 127")
              .settings.keyItemUse == 127);
    auto p = mdk::parseFrontendSettings("KeySniper = 128");
    CHECK(p.settings.keySniper == 57 && p.ignoredKeyLines == 1);
    p = mdk::parseFrontendSettings("KeySniper = -1");
    CHECK(p.settings.keySniper == 57 && p.ignoredKeyLines == 1);
    p = mdk::parseFrontendSettings("KeySniper = abc");
    CHECK(p.settings.keySniper == 57 && p.ignoredKeyLines == 1);
    p = mdk::parseFrontendSettings("KeySniper = ");
    CHECK(p.settings.keySniper == 57 && p.ignoredKeyLines == 1);
    // A bad line never clobbers a good one; the counter is its own.
    p = mdk::parseFrontendSettings("KeyLeft = 30\nKeyLeft = 999\n");
    CHECK(p.settings.keyLeft == 30 && p.ignoredKeyLines == 1 &&
          p.ignoredSkillLines == 0);
    // A serialized keyboard block round-trips.
    mdk::FrontendSettings k;
    k.keySniper = 45;
    k.keyLeft = 30;
    k.keyItemUse = 127;
    const auto rt = mdk::parseFrontendSettings(
        mdk::serializeFrontendSettings(k));
    CHECK(rt.settings.keySniper == 45 && rt.settings.keyLeft == 30 &&
          rt.settings.keyItemUse == 127);
    // A full real-file layout keeps the block's position: mouse
    // lines, then Key* lines, then Skill.
    const auto disk = mdk::parseFrontendSettings(
        "MouseOn = FALSE\r\n"
        "KeySniper = 45\r\n"
        "KeyFire = 97\r\n"
        "Skill = 2\r\n"
        "Brightness = 2\r\n");
    CHECK(!disk.settings.mouseOn && disk.settings.keySniper == 45 &&
          disk.settings.keyFire == 97 && disk.settings.skill == 2 &&
          disk.settings.brightness == 2);
  }

  // NATIVE hardening (not an original-behavior claim): malformed
  // or out-of-range Skill lines are ignored and counted — the
  // running value is kept.
  {
    auto p = mdk::parseFrontendSettings("Skill = abc");
    CHECK(p.settings.skill == 1 && p.ignoredSkillLines == 1);
    p = mdk::parseFrontendSettings("Skill = 7");
    CHECK(p.settings.skill == 1 && p.ignoredSkillLines == 1);
    p = mdk::parseFrontendSettings("Skill = -1");
    CHECK(p.settings.skill == 1 && p.ignoredSkillLines == 1);
    p = mdk::parseFrontendSettings("Skill = ");
    CHECK(p.settings.skill == 1 && p.ignoredSkillLines == 1);
    // A bad line does not clobber an earlier good one.
    p = mdk::parseFrontendSettings("Skill = 0\nSkill = 9\n");
    CHECK(p.settings.skill == 0 && p.ignoredSkillLines == 1);
    // Brightness carries its own ignored counter ([0,7] domain).
    p = mdk::parseFrontendSettings("Brightness = abc");
    CHECK(p.settings.brightness == 0 &&
          p.ignoredBrightnessLines == 1);
    p = mdk::parseFrontendSettings("Brightness = 8");
    CHECK(p.settings.brightness == 0 &&
          p.ignoredBrightnessLines == 1);
    p = mdk::parseFrontendSettings("Brightness = -2");
    CHECK(p.settings.brightness == 0 &&
          p.ignoredBrightnessLines == 1);
    p = mdk::parseFrontendSettings("Brightness = 3\nBrightness = 9\n");
    CHECK(p.settings.brightness == 3 &&
          p.ignoredBrightnessLines == 1);
    // SoundFX / SoundMusic carry their own ignored counters
    // ([0,100] domain each — a bad line never clobbers a good one
    // and never spills into a sibling counter).
    p = mdk::parseFrontendSettings("SoundFX = abc");
    CHECK(p.settings.soundFx == 70 && p.ignoredSoundFxLines == 1);
    p = mdk::parseFrontendSettings("SoundFX = 101");
    CHECK(p.settings.soundFx == 70 && p.ignoredSoundFxLines == 1);
    p = mdk::parseFrontendSettings("SoundFX = -1");
    CHECK(p.settings.soundFx == 70 && p.ignoredSoundFxLines == 1);
    p = mdk::parseFrontendSettings("SoundFX = ");
    CHECK(p.settings.soundFx == 70 && p.ignoredSoundFxLines == 1);
    p = mdk::parseFrontendSettings("SoundFX = 30\nSoundFX = 999\n");
    CHECK(p.settings.soundFx == 30 && p.ignoredSoundFxLines == 1);
    p = mdk::parseFrontendSettings("SoundMusic = xyz");
    CHECK(p.settings.soundMusic == 100 &&
          p.ignoredSoundMusicLines == 1 && p.ignoredSoundFxLines == 0);
    p = mdk::parseFrontendSettings("SoundMusic = 200");
    CHECK(p.settings.soundMusic == 100 &&
          p.ignoredSoundMusicLines == 1);
    p = mdk::parseFrontendSettings("SoundMusic = -5");
    CHECK(p.settings.soundMusic == 100 &&
          p.ignoredSoundMusicLines == 1);
    p = mdk::parseFrontendSettings("SoundMusic = 40\nSoundMusic = -9\n");
    CHECK(p.settings.soundMusic == 40 &&
          p.ignoredSoundMusicLines == 1);
  }

  // Temp-file round trips — Easy / Normal / Hard through the real
  // native-owned file seam (never near the data root).
  {
    std::string err;
    const auto dir = std::filesystem::temp_directory_path();
    const auto cfg = dir / "mdk_p4g_settings_test.cfg";
    for (const int skill : {0, 1, 2}) {
      CHECK(mdk::saveFrontendSettingsFile(
          cfg, mdk::FrontendSettings{skill}, &err));
      auto disk = mdk::loadFrontendSettingsFile(cfg, &err);
      CHECK(disk && disk->settings.skill == skill &&
            disk->ignoredSkillLines == 0);
      // The on-disk text matches the serializer exactly.
      std::ifstream in(cfg, std::ios::binary);
      std::ostringstream ss;
      ss << in.rdbuf();
      CHECK(ss.str() ==
            mdk::serializeFrontendSettings(
                mdk::FrontendSettings{skill}));
    }
    std::filesystem::remove(cfg);
  }

  // Fresh-boot: an absent file loads nothing (defaults stand) and
  // is not an error.
  {
    std::string err;
    const auto cfg = std::filesystem::temp_directory_path() /
                     "mdk_p4g_absent_test.cfg";
    std::filesystem::remove(cfg);
    const auto disk = mdk::loadFrontendSettingsFile(cfg, &err);
    CHECK(!disk && err.empty());
  }

  // Write failure is reported, not fatal — the caller decides the
  // policy (the flow mirrors the original's unconditional clear).
  {
    std::string err;
    const auto bad = std::filesystem::temp_directory_path() /
                     "mdk_p4g_no_such_dir_xyz" / "cfg";
    CHECK(!mdk::saveFrontendSettingsFile(
        bad, mdk::FrontendSettings{0}, &err));
    CHECK(!err.empty());
  }
}

// Phase 4F — root <-> options flow (FUN_00420cf0 / FUN_00420d68).
void test_frontend_flow() {
  // Root -> Options: OpenOptions is consumed internally; the options
  // controller starts at selection 8 with the root's machine state.
  {
    mdk::FrontendFlowController flow(true);
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;   // 180 -> 139: root band 3 -> Options
    flow.update(in);
    in = {};
    in.mouseButtons = 0x1;
    flow.update(in);
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(flow.consumeRootAction() == mdk::FrontendAction::None);
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(flow.options().selection() == 8);
    // Shared state carried over: logical mouse at the clicked point.
    CHECK(flow.options().mouseX() == 300 &&
          flow.options().mouseY() == 139);
  }

  // Options -> Root: activating row 8 (or Esc) returns to the root
  // screen; the machine state (incl. logical mouse) carries back and
  // the root selection is where it was left.
  {
    mdk::FrontendFlowController flow(true);
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;
    flow.update(in);
    in = {};
    in.mouseButtons = 0x1;
    flow.update(in);
    flow.consumeRootAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    // Move the mouse into the Display band and release the button.
    in = {};
    in.mouseButtons = 0;
    in.mouseDy = 162;   // 139 -> 301: band trunc(278/36)=7 -> Display
    flow.update(in);
    CHECK(flow.options().selection() == 7);
    // Esc -> Back -> flow returns to root.
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    CHECK(flow.consumeOptionsAction() == mdk::OptionsAction::None);
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(flow.root().selection() == 3);   // root selection untouched
    CHECK(flow.root().mouseX() == 300 && flow.root().mouseY() == 301);
  }

  // Options -> Display (Phase 4H): activating row 7 is consumed by
  // the transition — FUN_0041d020 enters the child with selection 2
  // and the shared machine state intact; the options controller
  // stays alive underneath.
  {
    mdk::FrontendFlowController flow(true);
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;
    flow.update(in);
    in = {};
    in.mouseButtons = 0x1;
    flow.update(in);
    flow.consumeRootAction();
    in = {};
    in.mouseButtons = 0;
    in.mouseDy = 162;   // 139 -> 301: Display band
    flow.update(in);
    in = {};
    in.mouseButtons = 0x1;
    flow.update(in);    // click -> activate Display
    CHECK(flow.consumeOptionsAction() == mdk::OptionsAction::None);
    CHECK(flow.screen() == mdk::FrontendScreen::Display);
    CHECK(flow.display().selection() == 2);   // DAT_0054b834 entry
    CHECK(flow.display().mouseX() == 300 &&
          flow.display().mouseY() == 301);
  }

  // Display -> Options (Phase 4H): FUN_0041d144 restores mode 0x0b
  // with _DAT_0054bd34 still 7 — the options controller was never
  // re-initialized. Esc and Quit-activate both take this exit; the
  // machine state and the shared dirty flag carry back.
  auto enterDisplay = [](mdk::FrontendFlowController& f) {
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;            // 180 -> 139: root band 3
    f.update(in);
    in = {};
    in.mouseButtons = 0x1;
    f.update(in);
    f.consumeRootAction();
    in = {};
    in.mouseButtons = 0;
    in.mouseDy = 162;            // 139 -> 301: options band 7
    f.update(in);
    in = {};
    in.mouseButtons = 0x1;
    f.update(in);                // click -> FUN_0041d020
    f.consumeOptionsAction();
  };

  {
    mdk::FrontendFlowController flow(true);
    enterDisplay(flow);
    CHECK(flow.screen() == mdk::FrontendScreen::Display);
    // Navigate to row 1 and toggle ForcePCorrect.
    mdk::FrontendMenuInput in;
    in.mouseButtons = 0;
    in.mouseDy = 49 - 301;       // -> display band 1
    flow.update(in);
    CHECK(flow.display().selection() == 1);
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    CHECK(flow.display().forcePCorrect());
    // Esc -> Back -> FUN_0041d144: options resumes at selection 7.
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    CHECK(flow.consumeDisplayAction() == mdk::DisplayAction::None);
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(flow.options().selection() == 7);
    CHECK(flow.options().mouseX() == 300 &&
          flow.options().mouseY() == 49);
    // The child's mutations live in the flow globals now.
    CHECK(flow.forcePCorrect());
    CHECK(flow.options().settingsDirty());
  }

  // Display exit does NOT persist — FUN_00420d68 owns the write.
  // The shared dirty flag the child latched reaches the options
  // exit and persists the full triple.
  {
    int calls = 0;
    mdk::FrontendSettings persisted;
    mdk::FrontendFlowController flow(
        true, {}, [&](const mdk::FrontendSettings& s) {
          ++calls;
          persisted = s;
          return true;
        });
    enterDisplay(flow);
    // Brightness row: RIGHT twice -> 0 -> 2.
    mdk::FrontendMenuInput in;
    in.mouseButtons = 0;
    in.mouseDy = 29 - 301;       // -> display band 0
    flow.update(in);
    CHECK(flow.display().selection() == 0);
    for (int i = 0; i < 2; ++i) {
      in = {};
      in.rightHeld = true;
      flow.update(in);
      in = {};
      flow.update(in);           // release — deadline resets
    }
    CHECK(flow.display().brightness() == 2);
    CHECK(flow.display().settingsDirty());
    // Quit-activate -> options: no persist call yet.
    in = {};
    in.mouseDy = 89 - 29;        // -> display band 2
    flow.update(in);
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    flow.consumeDisplayAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(calls == 0);
    CHECK(flow.options().selection() == 7);
    CHECK(flow.brightness() == 2 && flow.options().settingsDirty());
    // Options exit -> FUN_00420d68 -> persist the triple.
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(calls == 1);
    CHECK(persisted.skill == 1 && persisted.brightness == 2 &&
          persisted.forcePCorrect == false);
    CHECK(!flow.settingsDirty());
    const std::string ser = mdk::serializeFrontendSettings(persisted);
    CHECK(ser.find("Brightness = 2\r\n") != std::string::npos);
    CHECK(ser.find("ForcePCorrect") == std::string::npos);
    CHECK(ser.find("Skill") == std::string::npos);   // default
  }

  // Re-entering Display re-runs FUN_0041d020: the child selection
  // resets to 2 but the process globals keep their mutated values.
  {
    mdk::FrontendFlowController flow(true);
    enterDisplay(flow);
    mdk::FrontendMenuInput in;
    in.mouseButtons = 0;
    in.mouseDy = 29 - 301;
    flow.update(in);
    in = {};
    in.rightHeld = true;
    flow.update(in);             // brightness 0 -> 1
    CHECK(flow.display().brightness() == 1);
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeDisplayAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(flow.brightness() == 1);
    // Back into Display (options sel still 7 -> activate).
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Display);
    CHECK(flow.display().selection() == 2);      // entry reset
    CHECK(flow.display().brightness() == 1);     // process global
    CHECK(flow.display().settingsDirty());       // flag carried
  }

  // Options -> Sound (Phase 4I): activating row 1 is consumed by
  // the transition — FUN_0042322c enters the child with the
  // process-global selection DAT_0054bdbc (BSS 0 on the first
  // entry) and the shared machine state intact; the options
  // controller stays alive underneath.
  auto enterSound = [](mdk::FrontendFlowController& f) {
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;            // 180 -> 139: root band 3
    f.update(in);
    in = {};
    in.mouseButtons = 0x1;
    f.update(in);
    f.consumeRootAction();
    in = {};
    in.mouseButtons = 0;
    in.mouseDy = 90 - 139;       // 139 -> 90: options band 1
    f.update(in);
    in = {};
    in.mouseButtons = 0x1;
    f.update(in);                // click -> FUN_0042322c
    f.consumeOptionsAction();
  };

  {
    mdk::FrontendFlowController flow(true);
    enterSound(flow);
    CHECK(flow.screen() == mdk::FrontendScreen::Sound);
    CHECK(flow.sound().selection() == 0);   // DAT_0054bdbc BSS 0
    CHECK(flow.sound().mouseX() == 300 &&
          flow.sound().mouseY() == 90);
    CHECK(flow.sound().soundFx() == 70 &&
          flow.sound().soundMusic() == 100);
  }

  // Sound -> Options (Phase 4I): FUN_00423280 restores mode 0x0b
  // with _DAT_0054bd34 still 1 — options resumes the Sound row;
  // machine state, volumes, and the shared dirty flag carry back.
  {
    mdk::FrontendFlowController flow(true);
    enterSound(flow);
    // RIGHT on row 0: SoundFX 70 -> 80, dirty latches.
    mdk::FrontendMenuInput in;
    in.mouseButtons = 0;
    in.rightHeld = true;
    flow.update(in);
    CHECK(flow.sound().soundFx() == 80 &&
          flow.sound().settingsDirty());
    // Esc -> Back -> options resumes at selection 1.
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    CHECK(flow.consumeSoundAction() == mdk::SoundAction::None);
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(flow.options().selection() == 1);
    CHECK(flow.soundFx() == 80 &&
          flow.options().settingsDirty());
  }

  // Sound exit does NOT persist — FUN_00420d68 owns the write;
  // the dirty the child latched persists the full five-tuple on
  // the eventual options exit.
  {
    int calls = 0;
    mdk::FrontendSettings persisted;
    mdk::FrontendFlowController flow(
        true, {}, [&](const mdk::FrontendSettings& s) {
          ++calls;
          persisted = s;
          return true;
        });
    enterSound(flow);
    mdk::FrontendMenuInput in;
    in.mouseButtons = 0;
    in.rightHeld = true;
    flow.update(in);             // SoundFX 70 -> 80
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeSoundAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(calls == 0);           // no persist on the child exit
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(calls == 1);
    CHECK(persisted.soundFx == 80 && persisted.soundMusic == 100 &&
          persisted.skill == 1 && persisted.brightness == 0 &&
          !persisted.forcePCorrect);
    CHECK(!flow.settingsDirty());
    const std::string ser = mdk::serializeFrontendSettings(persisted);
    CHECK(ser.find("SoundFX = 80\r\n") != std::string::npos);
    CHECK(ser.find("SoundMusic") == std::string::npos);
    CHECK(ser.find("Skill") == std::string::npos);
  }

  // DAT_0054bdbc is a process global: FUN_0042322c does not reset
  // it — a later Sound entry resumes wherever the frame handler
  // left it (unlike the Display child's fixed entry selection).
  {
    mdk::FrontendFlowController flow(true);
    enterSound(flow);
    mdk::FrontendMenuInput in;
    in.mouseButtons = 0;
    in.nextHeld = true;
    flow.update(in);             // sel 0 -> 1
    in = {};
    flow.update(in);             // release — deadline resets
    in.nextHeld = true;
    flow.update(in);             // sel 1 -> 2
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeSoundAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    // Re-enter (options sel still 1 -> activate).
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Sound);
    CHECK(flow.sound().selection() == 2);   // retained, not reset
    CHECK(flow.sound().soundFx() == 70);    // volumes are globals
  }

  // The semantic audio events survive the controller's
  // destruction: the flow-level queue holds the whole proven
  // sequence — entry (ambient stop + OPTSONG start), the exit
  // frame's OPTSONG stop + ambient restart — for the caller to
  // drain once the child is gone.
  {
    mdk::FrontendFlowController flow(true);
    enterSound(flow);
    mdk::FrontendMenuInput in;
    in.mouseButtons = 0;
    in.cancelEdge = true;
    flow.update(in);             // Esc -> FUN_00423280
    flow.consumeSoundAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    const auto ev = flow.drainAudioEvents();
    CHECK(ev.size() == 4 &&
          ev[0] == mdk::SoundAudioEvent::AmbientSongStop &&
          ev[1] == mdk::SoundAudioEvent::SongStart &&
          ev[2] == mdk::SoundAudioEvent::SongStop &&
          ev[3] == mdk::SoundAudioEvent::AmbientSongStart);
    CHECK(flow.drainAudioEvents().empty());
  }

  // Options -> Mouse (Phase 4J): activating row 3 is consumed by
  // the transition — FUN_00421664 enters the child with selection
  // AND column reset to 0 and the shared machine state intact; the
  // options controller stays alive underneath.
  auto enterMouse = [](mdk::FrontendFlowController& f) {
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;            // 180 -> 139: root band 3
    f.update(in);
    in = {};
    in.mouseButtons = 0x1;
    f.update(in);
    f.consumeRootAction();
    in = {};
    in.mouseButtons = 0;
    in.mouseDy = 140 - 139;      // 139 -> 140: options band 3
    f.update(in);
    in = {};
    in.mouseButtons = 0x1;
    f.update(in);                // click -> FUN_00421664
    f.consumeOptionsAction();
  };

  {
    mdk::FrontendFlowController flow(true);
    enterMouse(flow);
    CHECK(flow.screen() == mdk::FrontendScreen::Mouse);
    CHECK(flow.mouse().selection() == 0 &&   // DAT_0054bd40 reset
          flow.mouse().column() == 0);       // DAT_0054bd38 reset
    CHECK(flow.mouse().mouseX() == 300 &&
          flow.mouse().mouseY() == 140);
    CHECK(flow.mouse().mouseOn() &&
          flow.mouse().mouseYReversedBits() == 0);
    CHECK(flow.mouse().axesMap() == "ABG");
  }

  // Mouse -> Options (Phase 4J): the inline exit writes mode 0x0b
  // with _DAT_0054bd34 still 3 — options resumes the Mouse row;
  // machine state, mutations, and the shared dirty flag carry.
  {
    mdk::FrontendFlowController flow(true);
    enterMouse(flow);
    mdk::FrontendMenuInput in;
    // Move into the MouseOn band (x<250, y=18) and LEFT-toggle.
    in.mouseDx = 100 - 300;
    in.mouseDy = 18 - 140;
    flow.update(in);
    CHECK(flow.mouse().selection() == 1);
    in = {};
    in.leftHeld = true;
    flow.update(in);
    CHECK(!flow.mouse().mouseOn() && flow.mouse().settingsDirty());
    // Esc -> Back -> options resumes at selection 3.
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    CHECK(flow.consumeMouseAction() == mdk::MouseAction::None);
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(flow.options().selection() == 3);
    CHECK(!flow.mouseOn() && flow.options().settingsDirty());
    CHECK(flow.options().mouseX() == 100 &&
          flow.options().mouseY() == 18);
  }

  // Mouse exit does NOT persist — FUN_00420d68 owns the write.
  // The overlay emits the W-set mutations while untouched fields
  // (the D set, scales, map strings at factory) round-trip from
  // the loaded base instead of reverting to factory defaults.
  {
    int calls = 0;
    mdk::FrontendSettings persisted;
    mdk::FrontendSettings initial;
    initial.mouseDButtMapD = 32768;   // BUILD_A's real D-set dword
    initial.mouseDAxesMap = "A0G";
    mdk::FrontendFlowController flow(
        true, initial, [&](const mdk::FrontendSettings& s) {
          ++calls;
          persisted = s;
          return true;
        });
    enterMouse(flow);
    mdk::FrontendMenuInput in;
    // Toggle MouseOn (row 1) and MouseYReversed (row 2).
    in.mouseDx = 100 - 300;
    in.mouseDy = 18 - 140;
    flow.update(in);
    in = {};
    in.leftHeld = true;
    flow.update(in);             // MouseOn -> off
    in = {};
    in.mouseDy = 49 - 18;        // -> row 2
    flow.update(in);
    in = {};
    in.leftHeld = true;
    flow.update(in);             // yrev bits -> 1
    // Toggle grid row 7 col 0: factory buttA 1 -> 0.
    in = {};
    in.mouseDx = 396 - 100;
    in.mouseDy = 33 - 49;
    flow.update(in);
    CHECK(flow.mouse().selection() == 7 && flow.mouse().column() == 0);
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    CHECK(flow.mouse().buttMask(0) == 0);
    // Esc -> options: no persist yet.
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeMouseAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(calls == 0);
    CHECK(flow.mouseYReversedBits() == 1 && !flow.mouseOn());
    CHECK(flow.mouseButtMap()[0] == 0);
    // Options exit -> FUN_00420d68 -> persist with the overlays.
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(calls == 1);
    CHECK(!persisted.mouseOn && persisted.mouseYReversed == 1);
    CHECK(persisted.mouseWButtMapA == 0);
    CHECK(persisted.mouseWAxesMap == "ABG");   // untouched
    // The D-set fields from `initial` round-trip — not factory.
    CHECK(persisted.mouseDButtMapD == 32768);
    CHECK(persisted.mouseDAxesMap == "A0G");
    CHECK(!flow.settingsDirty());
    const std::string ser = mdk::serializeFrontendSettings(persisted);
    CHECK(ser.find("MouseWButtMapA = 0\r\n") != std::string::npos);
    CHECK(ser.find("MouseOn = FALSE\r\n") != std::string::npos);
    CHECK(ser.find("MouseYReversed = 1.4013e-45\r\n") !=
          std::string::npos);
    CHECK(ser.find("MouseDButtMapD = 32768\r\n") != std::string::npos);
    CHECK(ser.find("MouseDAxesMap = A0G\r\n") != std::string::npos);
    CHECK(ser.find("MouseWAxesMap") == std::string::npos);
  }

  // Re-entering Mouse re-runs FUN_00421664: selection and column
  // reset to 0 but the mutated settings globals are retained
  // (they are process globals, borrowed — never re-initialized).
  {
    mdk::FrontendFlowController flow(true);
    enterMouse(flow);
    mdk::FrontendMenuInput in;
    in.mouseDx = 100 - 300;
    in.mouseDy = 18 - 140;
    flow.update(in);
    in = {};
    in.leftHeld = true;
    flow.update(in);             // MouseOn -> off
    // Move the column and selection away from the entry values.
    in = {};
    in.mouseDx = 396 - 100;
    in.mouseDy = 33 - 18;
    flow.update(in);
    CHECK(flow.mouse().selection() == 7 && flow.mouse().column() == 0);
    in = {};
    in.rightHeld = true;
    flow.update(in);             // col 0 -> 1
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeMouseAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    // Re-enter (options sel still 3 -> activate).
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Mouse);
    CHECK(flow.mouse().selection() == 0 &&   // reset at entry
          flow.mouse().column() == 0);       // reset at entry
    CHECK(!flow.mouse().mouseOn());          // global retained
    CHECK(flow.mouse().settingsDirty());     // flag carried back
  }

  // Options -> Keyboard (Phase 4K): activating row 4 runs
  // FUN_0041f030 — mode 0x05, capture clear, selection = 0x14 (the
  // KM_QUIT row), the 29-dword key block borrowed from the loaded
  // settings, machine state carried over.
  auto enterKeyboard = [](mdk::FrontendFlowController& f) {
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;            // 180 -> 139: root band 3
    f.update(in);
    in = {};
    in.mouseButtons = 0x1;
    f.update(in);
    f.consumeRootAction();
    in = {};
    in.mouseButtons = 0;
    in.mouseDy = 180 - 139;      // 139 -> 180: options band 4
    f.update(in);
    in = {};
    in.mouseButtons = 0x1;
    f.update(in);                // click -> FUN_0041f030
    f.consumeOptionsAction();
  };

  {
    mdk::FrontendFlowController flow(true);
    enterKeyboard(flow);
    CHECK(flow.screen() == mdk::FrontendScreen::Keyboard);
    CHECK(flow.keyboard().selection() == 20);   // DAT_0054bcac=0x14
    CHECK(!flow.keyboard().capture());          // DAT_0054bca8 = 0
    CHECK(flow.keyboard().mouseX() == 300 &&
          flow.keyboard().mouseY() == 180);
    CHECK(flow.keyGlobals() == mdk::kKeyboardDefaults);
  }

  // Keyboard -> Options (Phase 4K): the inline exit writes mode
  // 0x0b with _DAT_0054bd34 still 4 — options resumes the Keyboard
  // row; the rebound binding, machine state, and the shared dirty
  // flag carry back.
  {
    mdk::FrontendFlowController flow(true);
    enterKeyboard(flow);
    mdk::FrontendMenuInput in;
    // Hit-test to row 8 (KM_SNIPE), capture, bind internal 45.
    in.mouseDy = 304 - 180;
    flow.update(in);
    in = {};
    CHECK(flow.keyboard().selection() == 8);
    in.confirmEdge = true;
    flow.update(in);
    in = {};
    CHECK(flow.keyboard().capture());
    in.rawKeyEdge[1] = 1u << (45 - 32);   // 'X'
    flow.update(in);
    in = {};
    CHECK(!flow.keyboard().capture());
    CHECK(flow.keyboard().keyAt(7) == 45);
    // Esc (normal mode) -> Back -> options resumes sel 4.
    in.cancelEdge = true;
    flow.update(in);
    CHECK(flow.consumeKeyboardAction() == mdk::KeyboardAction::None);
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(flow.options().selection() == 4);
    CHECK(flow.keyGlobals()[7] == 45);    // the block carried back
    CHECK(flow.options().settingsDirty());
    CHECK(flow.options().mouseX() == 300 &&
          flow.options().mouseY() == 304);
  }

  // Re-entering Keyboard re-runs FUN_0041f030: selection resets to
  // 0x14 and capture clears, but the 29-dword block is a process
  // global — the rebound key survives (unlike the Mouse child's
  // sel-0 reset, Keyboard entry always lands on KM_QUIT).
  {
    mdk::FrontendFlowController flow(true);
    enterKeyboard(flow);
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 180;
    flow.update(in);
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    in = {};
    in.rawKeyEdge[1] = 1u << (45 - 32);
    flow.update(in);
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeKeyboardAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    // Re-enter (options sel still 4 -> activate).
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Keyboard);
    CHECK(flow.keyboard().selection() == 20);   // reset to QUIT
    CHECK(!flow.keyboard().capture());
    CHECK(flow.keyboard().keyAt(7) == 45);      // block retained
    CHECK(flow.keyboard().settingsDirty());     // flag carried back
  }

  // Keyboard exit does NOT persist — FUN_00420d68 owns the write.
  // The overlay emits the rebound Key* entries while untouched
  // fields round-trip from the loaded base; the hidden hotkey
  // slots never serialize.
  {
    int calls = 0;
    mdk::FrontendSettings persisted;
    mdk::FrontendSettings initial;
    initial.mouseDButtMapD = 32768;   // BUILD_A's real D-set dword
    initial.keyItemUse = 99;          // a loaded non-factory key
    mdk::FrontendFlowController flow(
        true, initial, [&](const mdk::FrontendSettings& s) {
          ++calls;
          persisted = s;
          return true;
        });
    CHECK(flow.keyGlobals()[26] == 99);   // loaded overlay at boot
    enterKeyboard(flow);
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 180;
    flow.update(in);
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    in = {};
    in.rawKeyEdge[1] = 1u << (45 - 32);   // KeySniper -> 'X'
    flow.update(in);
    in = {};
    // Esc -> options: no persist call yet.
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeKeyboardAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(calls == 0);
    // Options exit -> FUN_00420d68 -> persist with the overlays.
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(calls == 1);
    CHECK(persisted.keySniper == 45 && persisted.keyItemUse == 99);
    CHECK(persisted.mouseDButtMapD == 32768);   // untouched D set
    CHECK(!flow.settingsDirty());
    const std::string ser = mdk::serializeFrontendSettings(persisted);
    CHECK(ser.find("KeySniper = 45\r\n") != std::string::npos);
    CHECK(ser.find("KeyItemUse = 99\r\n") != std::string::npos);
    CHECK(ser.find("KeyLeft") == std::string::npos);
    // The Key* block sits between the mouse entries and Skill.
    CHECK(ser.find("MouseDButtMapD = 32768") <
          ser.find("KeySniper = 45"));
  }

  // The reset row inside the flow: mutating a binding plus
  // activating KM_RESET restores all 29 globals — including the
  // hidden hotkey slots — and latches the shared dirty flag that
  // reaches the options persist gate (even though the visible
  // values serialize as factory).
  {
    int calls = 0;
    mdk::FrontendSettings persisted;
    mdk::FrontendFlowController flow(
        true, {}, [&](const mdk::FrontendSettings& s) {
          ++calls;
          persisted = s;
          return true;
        });
    enterKeyboard(flow);
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 180;
    flow.update(in);
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    in = {};
    in.rawKeyEdge[1] = 1u << (45 - 32);   // KeySniper -> 'X'
    flow.update(in);
    in = {};
    // Navigate to row 19 (KM_RESET) via the hit band, activate.
    in.mouseDy = 8 - 304;
    flow.update(in);
    in = {};
    CHECK(flow.keyboard().selection() == 19);
    in.confirmEdge = true;
    flow.update(in);
    in = {};
    CHECK(flow.keyGlobals() == mdk::kKeyboardDefaults);   // ALL 29
    CHECK(flow.keyboard().settingsDirty());
    // Quit -> options -> exit -> persist fires (dirty latched)
    // even though the emitted file has no Key* lines.
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeKeyboardAction();
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(calls == 1);
    CHECK(persisted.keySniper == 57);      // factory again
    CHECK(mdk::serializeFrontendSettings(persisted)
              .find("Key") == std::string::npos);
  }

  // Non-transition root actions pass through unchanged.
  {
    mdk::FrontendFlowController flow(true);
    mdk::FrontendMenuInput in;
    in.confirmEdge = true;   // sel 0 -> ContinueGame
    flow.update(in);
    CHECK(flow.consumeRootAction() == mdk::FrontendAction::ContinueGame);
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
  }

  // DAT_0054147a is a process global: a skill mutation inside the
  // options screen persists across the return to root and the next
  // entry; DAT_00541486 is consumed (cleared) by FUN_00420d68.
  {
    mdk::FrontendFlowController flow(true);
    CHECK(flow.skill() == 1);   // canonical factory default (Normal)
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;           // 180 -> 139: root band 3 -> Options
    flow.update(in);
    in = {};
    in.mouseButtons = 0x1;
    flow.update(in);
    flow.consumeRootAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    // Release the button and move into the skill band (y 239..274).
    in = {};
    in.mouseDy = 120;   // 139 -> 259 -> band trunc(236/36)=6
    flow.update(in);
    CHECK(flow.options().selection() == 6 &&
          flow.options().skill() == 1);
    // RIGHT on the skill row: 1 -> 2, dirty latches.
    in = {};
    in.rightHeld = true;
    flow.update(in);
    CHECK(flow.options().skill() == 2 &&
          flow.options().settingsDirty());
    // Esc -> FUN_00420d68 -> root: skill keeps its value, the dirty
    // flag is consumed.
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(flow.skill() == 2 && !flow.settingsDirty());
    // Re-enter: the mutated skill persists.
    in = {};
    in.confirmEdge = true;   // root sel still 3 -> OpenOptions
    flow.update(in);
    flow.consumeRootAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Options);
    CHECK(flow.options().skill() == 2 &&
          !flow.options().settingsDirty());
  }

  // Phase 4G persistence seam — the FUN_004260ac sink fires only
  // behind the dirty gate, then the flag clears unconditionally.
  auto enterAndSelectSkill = [](mdk::FrontendFlowController& f) {
    mdk::FrontendMenuInput in;
    in.mouseDy = -41;            // 180 -> 139: root band 3
    f.update(in);
    in = {};
    in.mouseButtons = 0x1;
    f.update(in);
    f.consumeRootAction();
    in = {};
    in.mouseDy = 120;            // 139 -> 259: options band 6
    f.update(in);
  };

  // No mutation -> clean exit -> the sink is never invoked (the
  // original's TEST DAT_00541486 skips the writer entirely).
  {
    int calls = 0;
    mdk::FrontendFlowController flow(
        true, {}, [&](const mdk::FrontendSettings&) {
          ++calls;
          return true;
        });
    enterAndSelectSkill(flow);
    CHECK(flow.options().selection() == 6 &&
          !flow.options().settingsDirty());
    mdk::FrontendMenuInput in;
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(calls == 0 && !flow.settingsDirty());
  }

  // Mutation -> exit -> sink fires once with the mutated settings;
  // the flag clears after the attempt. Re-entry keeps the process
  // global — Normal -> Hard retained without any reload.
  {
    int calls = 0;
    mdk::FrontendSettings persisted;
    mdk::FrontendFlowController flow(
        true, {}, [&](const mdk::FrontendSettings& s) {
          ++calls;
          persisted = s;
          return true;
        });
    enterAndSelectSkill(flow);
    mdk::FrontendMenuInput in;
    in.rightHeld = true;         // Normal -> Hard
    flow.update(in);
    CHECK(flow.options().skill() == 2 &&
          flow.options().settingsDirty());
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(calls == 1 && persisted.skill == 2);
    CHECK(flow.skill() == 2 && !flow.settingsDirty());
    // Process-lifetime: re-entry sees Hard, no persist machinery.
    in = {};
    in.confirmEdge = true;
    flow.update(in);
    flow.consumeRootAction();
    CHECK(flow.options().skill() == 2 &&
          !flow.options().settingsDirty());
  }

  // Normal -> Hard -> Normal: the dirty latch stays set through the
  // return to the default — the sink STILL fires at exit (dirty
  // gate), but the serialized settings carry no Skill line
  // (default-delta). The two mechanisms are independent.
  {
    int calls = 0;
    mdk::FrontendSettings persisted;
    mdk::FrontendFlowController flow(
        true, {}, [&](const mdk::FrontendSettings& s) {
          ++calls;
          persisted = s;
          return true;
        });
    enterAndSelectSkill(flow);
    mdk::FrontendMenuInput in;
    in.rightHeld = true;
    flow.update(in);             // 1 -> 2
    in = {};
    in.leftHeld = true;
    flow.update(in);             // 2 -> 1
    CHECK(flow.options().skill() == 1 &&
          flow.options().settingsDirty());
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(calls == 1 && persisted.skill == 1);
    CHECK(mdk::serializeFrontendSettings(persisted).find("Skill") ==
          std::string::npos);
    CHECK(!flow.settingsDirty());
  }

  // Write failure: the sink reports failure, the flag still clears
  // (0x420dbc — the original clears unconditionally after the
  // FUN_004260ac call, whose fopen-failure path returns silently),
  // and nothing crashes. The mutation remains process-lifetime.
  {
    int calls = 0;
    mdk::FrontendFlowController flow(
        true, {}, [&](const mdk::FrontendSettings&) {
          ++calls;
          return false;   // simulated write failure
        });
    enterAndSelectSkill(flow);
    mdk::FrontendMenuInput in;
    in.rightHeld = true;
    flow.update(in);
    in = {};
    in.cancelEdge = true;
    flow.update(in);
    flow.consumeOptionsAction();
    CHECK(calls == 1);
    CHECK(flow.screen() == mdk::FrontendScreen::Root);
    CHECK(flow.skill() == 2 && !flow.settingsDirty());
  }

  // Process-restart round trip: instance A mutates and persists
  // through the real file seam; instance B loads the same file at
  // construction — Easy and Hard both restore.
  {
    std::string err;
    const auto cfg = std::filesystem::temp_directory_path() /
                     "mdk_p4g_flow_restart.cfg";
    for (const int cycles : {1, 2}) {   // RIGHT x1 -> Hard, x2 -> Easy
      int calls = 0;
      {
        mdk::FrontendFlowController flowA(
            true, {}, [&](const mdk::FrontendSettings& s) {
              ++calls;
              std::string e;
              return mdk::saveFrontendSettingsFile(cfg, s, &e);
            });
        enterAndSelectSkill(flowA);
        mdk::FrontendMenuInput in;
        for (int i = 0; i < cycles; ++i) {
          in = {};
          in.rightHeld = true;
          flowA.update(in);
          in = {};   // release — the repeat deadline resets
          flowA.update(in);
        }
        in = {};
        in.cancelEdge = true;
        flowA.update(in);
        flowA.consumeOptionsAction();
      }
      CHECK(calls == 1);
      const auto disk = mdk::loadFrontendSettingsFile(cfg, &err);
      const int want = cycles == 1 ? 2 : 0;
      CHECK(disk && disk->settings.skill == want);
      // Instance B: fresh controller seeded from the loaded file.
      mdk::FrontendFlowController flowB(true, disk->settings, {});
      CHECK(flowB.skill() == want);
      enterAndSelectSkill(flowB);
      CHECK(flowB.options().skill() == want);
    }
    std::filesystem::remove(cfg);
  }
}

// Phase 4F — options sub-menu renderers.
void test_options_render() {
  std::string err;
  auto f = SyntheticFont::make();
  const char* labels[9] = {"Help",      "Sound",    "Joystick",
                           "Mouse",     "Keyboard", "Performance",
                           "Skill - Normal", "Display", "Quit"};
  for (const char* l : labels) {
    for (const char* c = l; *c; ++c) {
      f.put32(static_cast<std::uint8_t>(*c) * 4,
              f.addGlyph(1, 0, 2, {9, 9, 9, 9}));
    }
  }
  const auto font = mdk::decodeFtiFont(f.buf, &err);
  CHECK(font);
  auto arrowS = SyntheticSprite::make1(
      2, 2, 0, 0, {0x01, 77, 77, 0xfe, 0x01, 77, 77, 0xff});
  const auto arrow = mdk::decodeFtiSprite(arrowS.buf, &err);
  CHECK(arrow && arrow->frame(0));

  mdk::OptionsMenuLabels lbl;
  for (int i = 0; i < 9; ++i) {
    lbl.items[i] = labels[i];
  }
  std::array<std::byte, 192> sysPal{};
  for (int i = 0; i < 64; ++i) {
    sysPal[i * 3 + 0] = std::byte(i);
    sysPal[i * 3 + 1] = std::byte(200 - i);
    sysPal[i * 3 + 2] = std::byte(i);
  }

  // Static spec frame: clear(0), 9 centered rows (sel 8 at 1.0, the
  // rest 0.65), ARROW at the carried mouse, SYS_PAL head bound.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::OptionsMenuSpec spec;
    spec.arrowX = 300;
    spec.arrowY = 139;
    CHECK(mdk::renderOptionsMenuFrame(fb, palette, *font,
                                      *arrow->frame(0), lbl, sysPal,
                                      spec, &err));
    // Arrow at the spec position.
    CHECK(fb.at(300, 139) == 77 && fb.at(301, 140) == 77);
    // Glyph pixels land inside the Quit band (row 8, y 337..~360 clip).
    bool found9 = false;
    for (int y = 337; y < 342 && !found9; ++y) {
      for (int x = 280; x < 320 && !found9; ++x) {
        found9 = fb.at(x, y) == 9;
      }
    }
    CHECK(found9);
    // Corners stay cleared (no backdrop).
    CHECK(fb.at(0, 0) == 0 && fb.at(599, 0) == 0 &&
          fb.at(0, 359) == 0);
    // Palette: SYS_PAL head bound, tail zeroed.
    CHECK(palette.get(1).r == 1 && palette.get(1).g == 199);
    CHECK(palette.get(200).r == 0 && palette.get(200).a == 255);
    // Contracts: wrong fb size, empty label, short palette head.
    mdk::IndexedFramebuffer small(64, 64);
    CHECK(!mdk::renderOptionsMenuFrame(small, palette, *font,
                                     *arrow->frame(0), lbl, sysPal,
                                     spec, &err));
    mdk::OptionsMenuLabels bad;
    CHECK(!mdk::renderOptionsMenuFrame(fb, palette, *font,
                                     *arrow->frame(0), bad, sysPal,
                                     spec, &err));
    std::array<std::byte, 64> shortPal{};
    CHECK(!mdk::renderOptionsMenuFrame(fb, palette, *font,
                                     *arrow->frame(0), lbl, shortPal,
                                     spec, &err));
  }

  // Hidden mode: rows 2,3,4 skipped; indices not compacted.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::OptionsMenuSpec spec;
    spec.devHidden = true;
    CHECK(mdk::renderOptionsMenuFrame(fb, palette, *font,
                                      *arrow->frame(0), lbl, sysPal,
                                      spec, &err));
    // Rows 2,3,4 (y 121..202) must have no glyph pixels.
    bool any9 = false;
    for (int y = 121; y < 203 && !any9; ++y) {
      for (int x = 0; x < 600 && !any9; ++x) {
        any9 = fb.at(x, y) == 9;
      }
    }
    CHECK(!any9);
  }

  // Dynamic frame: live ramp drives row scales; ARROW follows the
  // controller's logical mouse.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 139;
    mdk::OptionsMenuController ctl(s, false, 0);
    CHECK(mdk::renderOptionsMenuDynamic(fb, palette, *font,
                                        *arrow->frame(0), lbl, sysPal,
                                        ctl, 0, &err));
    CHECK(fb.at(300, 139) == 77);
    // First draw: the ramp keyed (-1, y) performs the transition —
    // the accumulator restarts at 0 and advances from the next pass.
    CHECK(ctl.rampAccumulator() == 0.0f);
    CHECK(mdk::renderOptionsMenuDynamic(fb, palette, *font,
                                        *arrow->frame(0), lbl, sysPal,
                                        ctl, 0, &err));
    CHECK(ctl.rampAccumulator() > 0.0f);
  }

  // Brightness lift (Phase 4H, FUN_0046d208 staging semantics):
  // level L adds 16*L per channel clamped to 255 — applied to the
  // SYS_PAL head AND the zeroed tail, matching the original's
  // uniform staging-buffer lift.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::OptionsMenuSpec spec;
    spec.brightness = 2;
    CHECK(mdk::renderOptionsMenuFrame(fb, palette, *font,
                                      *arrow->frame(0), lbl, sysPal,
                                      spec, &err));
    // Entry 1: base (1,199,1) + lift 32.
    CHECK(palette.get(1).r == 33 && palette.get(1).g == 231 &&
          palette.get(1).b == 33);
    // Zeroed tail lifts too: index 200 -> (32,32,32).
    CHECK(palette.get(200).r == 32 && palette.get(200).g == 32 &&
          palette.get(200).b == 32);
    // Clamp: entry 63 base g = 137 -> 169 unclamped; r = 63 -> 95.
    CHECK(palette.get(63).r == 95 && palette.get(63).g == 169);
  }
}

// Phase 4H — display child renderers (FUN_0041d1e0 draw block +
// FUN_0041cf80 swatch grid).
void test_display_render() {
  std::string err;
  auto f = SyntheticFont::make();
  const char* labels[3] = {"Brightness %d", "Detail is High",
                           "Quit"};
  for (const char* l : labels) {
    for (const char* c = l; *c; ++c) {
      f.put32(static_cast<std::uint8_t>(*c) * 4,
              f.addGlyph(1, 0, 2, {9, 9, 9, 9}));
    }
  }
  const auto font = mdk::decodeFtiFont(f.buf, &err);
  CHECK(font);
  auto arrowS = SyntheticSprite::make1(
      2, 2, 0, 0, {0x01, 77, 77, 0xfe, 0x01, 77, 77, 0xff});
  const auto arrow = mdk::decodeFtiSprite(arrowS.buf, &err);
  CHECK(arrow && arrow->frame(0));

  const mdk::DisplayMenuLabels lbl{"Brightness %d", "Detail is High",
                                   "Detail is Low", "Quit"};
  std::array<std::byte, 192> sysPal{};
  for (int i = 0; i < 64; ++i) {
    sysPal[i * 3 + 0] = std::byte(i);
    sysPal[i * 3 + 1] = std::byte(200 - i);
    sysPal[i * 3 + 2] = std::byte(i);
  }

  // Static spec frame: clear(0), 3 centered rows (sel 2 at 1.0, the
  // rest 0.65), the 4x48 swatch grid, ARROW at the carried mouse,
  // SYS_PAL head + ramps bound.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::DisplayMenuSpec spec;
    spec.arrowX = 300;
    spec.arrowY = 139;
    CHECK(mdk::renderDisplayMenuFrame(fb, palette, *font,
                                      *arrow->frame(0), lbl, sysPal,
                                      spec, &err));
    // Arrow at the spec position.
    CHECK(fb.at(300, 139) == 77 && fb.at(301, 140) == 77);
    // Corners stay cleared (no backdrop).
    CHECK(fb.at(0, 0) == 0 && fb.at(599, 0) == 0 &&
          fb.at(0, 359) == 0);
    // Glyph pixels land inside the Quit row (y 103+).
    bool found9 = false;
    for (int y = 103; y < 108 && !found9; ++y) {
      for (int x = 270; x < 330 && !found9; ++x) {
        found9 = fb.at(x, y) == 9;
      }
    }
    CHECK(found9);
    // Swatch grid (FUN_0041cf80): band b rows y=200+32b..231+32b,
    // cells x=60+10i..69+10i, color index 64+48b+i.
    CHECK(fb.at(60, 200) == 64 && fb.at(69, 231) == 64);
    CHECK(fb.at(70, 200) == 65 && fb.at(60, 232) == 112);
    CHECK(fb.at(60 + 10 * 47, 200 + 32 * 3) == 64 + 48 * 3 + 47);
    CHECK(fb.at(69 + 10 * 47, 231 + 32 * 3) == 255);
    // Between bands stays cleared (y 232..231 gap is inside band —
    // check a column gap instead: x 70..69+10 = none; x=59 outside).
    CHECK(fb.at(59, 200) == 0);
    // Palette: SYS_PAL head bound for 0..63.
    CHECK(palette.get(1).r == 1 && palette.get(1).g == 199);
    // Ramps: gray ramp entry 64+i = (v,v,v) with v=i*255/47.
    CHECK(palette.get(64).r == 0 && palette.get(64 + 47).r == 255 &&
          palette.get(64 + 47).g == 255 &&
          palette.get(64 + 47).b == 255);
    // Red band: entry 112+47 = (255,0,0); green 160+47 = (0,255,0);
    // blue 208+47 = (0,0,255).
    CHECK(palette.get(112 + 47).r == 255 &&
          palette.get(112 + 47).g == 0);
    CHECK(palette.get(160 + 47).g == 255 &&
          palette.get(160 + 47).b == 0);
    CHECK(palette.get(208 + 47).b == 255 &&
          palette.get(208 + 47).r == 0);
    // Contracts: wrong fb size, empty label, short palette head.
    mdk::IndexedFramebuffer small(64, 64);
    CHECK(!mdk::renderDisplayMenuFrame(small, palette, *font,
                                       *arrow->frame(0), lbl, sysPal,
                                       spec, &err));
    mdk::DisplayMenuLabels bad;
    CHECK(!mdk::renderDisplayMenuFrame(fb, palette, *font,
                                       *arrow->frame(0), bad, sysPal,
                                       spec, &err));
    std::array<std::byte, 64> shortPal{};
    CHECK(!mdk::renderDisplayMenuFrame(fb, palette, *font,
                                       *arrow->frame(0), lbl, shortPal,
                                       spec, &err));
  }

  // The DSP_BRGT record is the row-0 printf format: a nonzero
  // brightness renders "Brightness 3" and the lift applies to the
  // whole bound palette (head AND ramps).
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::DisplayMenuSpec spec;
    spec.brightness = 3;         // lift 48
    spec.forcePCorrect = true;   // draws DSP_DETH
    CHECK(mdk::renderDisplayMenuFrame(fb, palette, *font,
                                      *arrow->frame(0), lbl, sysPal,
                                      spec, &err));
    // SYS_PAL head lifted: entry 1 (1,199,1) -> (49,247,49).
    CHECK(palette.get(1).r == 49 && palette.get(1).g == 247);
    // Gray ramp head entry 64 (0,0,0) -> (48,48,48).
    CHECK(palette.get(64).r == 48 && palette.get(64).b == 48);
    // Blue ramp end 255 (0,0,255) -> (48,48,255) clamped.
    CHECK(palette.get(255).r == 48 && palette.get(255).b == 255);
  }

  // Dynamic frame: live ramp drives row scales; ARROW follows the
  // controller's logical mouse.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 139;
    mdk::DisplayMenuController ctl(s, 0, false);
    CHECK(mdk::renderDisplayMenuDynamic(fb, palette, *font,
                                        *arrow->frame(0), lbl, sysPal,
                                        ctl, &err));
    CHECK(fb.at(300, 139) == 77);
    CHECK(ctl.rampAccumulator() == 0.0f);
    CHECK(mdk::renderDisplayMenuDynamic(fb, palette, *font,
                                        *arrow->frame(0), lbl, sysPal,
                                        ctl, &err));
    CHECK(ctl.rampAccumulator() > 0.0f);
    // Controller state drives the palette: mutate brightness and
    // the bound palette lifts on the next frame.
    mdk::FrontendMenuInput in;
    in.mouseDy = 29 - 139;   // -> band 0
    ctl.update(in);
    in = {};
    in.rightHeld = true;
    ctl.update(in);             // brightness 0 -> 1
    CHECK(ctl.brightness() == 1);
    CHECK(mdk::renderDisplayMenuDynamic(fb, palette, *font,
                                        *arrow->frame(0), lbl, sysPal,
                                        ctl, &err));
    CHECK(palette.get(1).r == 17);   // base 1 + lift 16
  }
}

// Phase 4I — sound child renderers (FUN_004233d8 draw block +
// FUN_004232b0 volume rows + FUN_00423384 Done row).
void test_sound_render() {
  std::string err;
  auto fBig = SyntheticFont::make();
  auto fSml = SyntheticFont::make();
  const char* labels[7] = {"Sound Settings",
                           "Left/Right to Change Volumes",
                           "Effects", "Music", "100%", "0%", "Done"};
  for (const char* l : labels) {
    for (const char* c = l; *c; ++c) {
      const std::uint8_t ch = static_cast<std::uint8_t>(*c);
      fBig.put32(ch * 4, fBig.addGlyph(1, 0, 2, {9, 9, 9, 9}));
      fSml.put32(ch * 4, fSml.addGlyph(1, 0, 1, {7, 7}));
    }
  }
  const auto fontBig = mdk::decodeFtiFont(fBig.buf, &err);
  const auto fontSml = mdk::decodeFtiFont(fSml.buf, &err);
  CHECK(fontBig && fontSml);
  auto arrowS = SyntheticSprite::make1(
      2, 2, 0, 0, {0x01, 77, 77, 0xfe, 0x01, 77, 77, 0xff});
  const auto arrow = mdk::decodeFtiSprite(arrowS.buf, &err);
  CHECK(arrow && arrow->frame(0));

  const mdk::SoundMenuLabels lbl{"Sound Settings",
                                 "Left/Right to Change Volumes",
                                 "Effects", "Music",
                                 "100%", "0%", "Done"};
  std::array<std::byte, 192> sysPal{};
  for (int i = 0; i < 64; ++i) {
    sysPal[i * 3 + 0] = std::byte(i);
    sysPal[i * 3 + 1] = std::byte(200 - i);
    sysPal[i * 3 + 2] = std::byte(i);
  }

  // Static spec frame (OBSERVED entry state): clear(0), centered
  // title y=31 / info y=350, two volume rows at y=87/133 (FONTBIG
  // label x=4 at the selection scale, inclusive bar
  // x=210..210+trunc(vol*280/100), y=rowY-12..rowY-1, color 4,
  // FONTSML "0%" @175 / "100%" @498), Done centered y=179, ARROW
  // at the carried mouse, SYS_PAL head bound.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::SoundMenuSpec spec;
    spec.arrowX = 300;
    spec.arrowY = 90;
    CHECK(mdk::renderSoundMenuFrame(fb, palette, *fontBig, *fontSml,
                                    *arrow->frame(0), lbl, sysPal,
                                    spec, &err));
    // Arrow at the spec position.
    CHECK(fb.at(300, 90) == 77 && fb.at(301, 91) == 77);
    // Corners stay cleared (no backdrop).
    CHECK(fb.at(0, 0) == 0 && fb.at(599, 0) == 0 &&
          fb.at(0, 359) == 0);
    // SoundFX bar: vol 70 -> w = trunc(70*280/100) = 196 —
    // x 210..406 inclusive, y 75..86, color 4.
    CHECK(fb.at(210, 80) == 4 && fb.at(406, 80) == 4);
    CHECK(fb.at(407, 80) == 0);           // just past the fill
    CHECK(fb.at(209, 80) == 0);           // left of the fill
    CHECK(fb.at(210, 74) == 0);           // above rowY-12
    CHECK(fb.at(210, 87) == 0);           // below rowY-1
    // SoundMusic bar: vol 100 -> w = 280 — x 210..490.
    CHECK(fb.at(210, 128) == 4 && fb.at(490, 128) == 4);
    CHECK(fb.at(491, 128) == 0);
    // Palette: SYS_PAL head bound, tail zeroed.
    CHECK(palette.get(1).r == 1 && palette.get(1).g == 199);
    CHECK(palette.get(200).r == 0 && palette.get(200).a == 255);
    // Contracts: wrong fb size, empty label, short palette head.
    mdk::IndexedFramebuffer small(64, 64);
    CHECK(!mdk::renderSoundMenuFrame(small, palette, *fontBig,
                                     *fontSml, *arrow->frame(0), lbl,
                                     sysPal, spec, &err));
    mdk::SoundMenuLabels bad;
    CHECK(!mdk::renderSoundMenuFrame(fb, palette, *fontBig, *fontSml,
                                     *arrow->frame(0), bad, sysPal,
                                     spec, &err));
    std::array<std::byte, 64> shortPal{};
    CHECK(!mdk::renderSoundMenuFrame(fb, palette, *fontBig, *fontSml,
                                     *arrow->frame(0), lbl, shortPal,
                                     spec, &err));
  }

  // The vol=0 edge (OBSERVED): the inclusive rectfill still draws
  // its single 1px column at x=210 — w truncates to 0, x0==x1.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::SoundMenuSpec spec;
    spec.soundFx = 0;
    CHECK(mdk::renderSoundMenuFrame(fb, palette, *fontBig, *fontSml,
                                    *arrow->frame(0), lbl, sysPal,
                                    spec, &err));
    CHECK(fb.at(210, 80) == 4 && fb.at(211, 80) == 0);
  }

  // Brightness lift (Phase 4H staging semantics): the bound
  // options palette lifts with DAT_0054147e — head AND zeroed
  // tail, exactly like the other screens (the Sound screen
  // performs no palette upload of its own).
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::SoundMenuSpec spec;
    spec.brightness = 2;
    CHECK(mdk::renderSoundMenuFrame(fb, palette, *fontBig, *fontSml,
                                    *arrow->frame(0), lbl, sysPal,
                                    spec, &err));
    CHECK(palette.get(1).r == 33 && palette.get(1).g == 231);
    CHECK(palette.get(200).r == 32 && palette.get(200).b == 32);
  }

  // Dynamic frame: the live ramp drives row scales (volume rows
  // keyed (4,y), Done (-1,179)); the controller's volumes and
  // logical mouse drive the bars and ARROW.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 90;
    mdk::SoundMenuController ctl(s, 0, 70, 100);
    CHECK(mdk::renderSoundMenuDynamic(fb, palette, *fontBig,
                                      *fontSml, *arrow->frame(0), lbl,
                                      sysPal, ctl, 0, &err));
    CHECK(fb.at(300, 90) == 77);
    CHECK(ctl.rampAccumulator() == 0.0f);
    CHECK(mdk::renderSoundMenuDynamic(fb, palette, *fontBig,
                                      *fontSml, *arrow->frame(0), lbl,
                                      sysPal, ctl, 0, &err));
    CHECK(ctl.rampAccumulator() > 0.0f);
    // Controller state drives the bar: mutate SoundFX and the fill
    // shrinks on the next frame.
    mdk::FrontendMenuInput in;
    in.leftHeld = true;
    ctl.update(in);             // 70 -> 60 -> w = 168
    CHECK(ctl.soundFx() == 60);
    CHECK(mdk::renderSoundMenuDynamic(fb, palette, *fontBig,
                                      *fontSml, *arrow->frame(0), lbl,
                                      sysPal, ctl, 0, &err));
    CHECK(fb.at(210 + 168, 80) == 4 && fb.at(210 + 169, 80) == 0);
  }
}

// Phase 4J — Mouse child rendering (FUN_004217e8 draw block +
// FUN_004213e8/21504/21448/212d0 + FUN_00414dd4/14b28 marker).
void test_mouse_render() {
  std::string err;
  auto fSml = SyntheticFont::make();
  const char* baseLabels[7] = {"Test", "Enabled", "Disabled",
                               "Reversed", "Normal", "Quit",
                               "Buttons"};
  const char* actions[16] = {"Fire",      "Sniper",    "Jump",
                             "Next Weap", "Prev Weap", "Strafe L",
                             "Strafe R",  "Accel",     "Decel",
                             "Look Up",   "Look Down", "Center",
                             "Action13",  "Action14",  "Action15",
                             "Action16"};
  const char* axisNames[9] = {"Off",   "Turn",  "Move",  "Fire",
                              "Jump",  "Snipe", "Look",  "Strafe",
                              "Zoom"};
  const char* axisCaptions[3] = {"X-Axis", "Y-Axis", "Z-Axis"};
  for (const char* l : baseLabels) {
    for (const char* c = l; *c; ++c) {
      const std::uint8_t ch = static_cast<std::uint8_t>(*c);
      fSml.put32(ch * 4, fSml.addGlyph(1, 0, 1, {7, 7}));
    }
  }
  for (const char* l : actions) {
    for (const char* c = l; *c; ++c) {
      const std::uint8_t ch = static_cast<std::uint8_t>(*c);
      fSml.put32(ch * 4, fSml.addGlyph(1, 0, 1, {7, 7}));
    }
  }
  for (const char* l : axisNames) {
    for (const char* c = l; *c; ++c) {
      const std::uint8_t ch = static_cast<std::uint8_t>(*c);
      fSml.put32(ch * 4, fSml.addGlyph(1, 0, 1, {7, 7}));
    }
  }
  for (const char* l : axisCaptions) {
    for (const char* c = l; *c; ++c) {
      const std::uint8_t ch = static_cast<std::uint8_t>(*c);
      fSml.put32(ch * 4, fSml.addGlyph(1, 0, 1, {7, 7}));
    }
  }
  const auto fontSml = mdk::decodeFtiFont(fSml.buf, &err);
  CHECK(fontSml);
  auto arrowS = SyntheticSprite::make1(
      2, 2, 0, 0, {0x01, 77, 77, 0xfe, 0x01, 77, 77, 0xff});
  const auto arrow = mdk::decodeFtiSprite(arrowS.buf, &err);
  CHECK(arrow && arrow->frame(0));

  mdk::MouseMenuLabels lbl{};
  lbl.test = baseLabels[0];
  lbl.enabled = baseLabels[1];
  lbl.disabled = baseLabels[2];
  lbl.reversed = baseLabels[3];
  lbl.normal = baseLabels[4];
  lbl.quit = baseLabels[5];
  lbl.buttons = baseLabels[6];
  for (int i = 0; i < 16; ++i) lbl.actions[i] = actions[i];
  for (int i = 0; i < 9; ++i) lbl.axisNames[i] = axisNames[i];
  for (int i = 0; i < 3; ++i) lbl.axisCaptions[i] = axisCaptions[i];

  std::array<std::byte, 192> sysPal{};
  for (int i = 0; i < 64; ++i) {
    sysPal[i * 3 + 0] = std::byte(i);
    sysPal[i * 3 + 1] = std::byte(200 - i);
    sysPal[i * 3 + 2] = std::byte(i);
  }

  // Static spec frame (OBSERVED entry state): clear(0), left rows
  // at y=row*16+16 centered x=(300-w)>>1, grid header row -1 at
  // y=30 + 16 rows at y=row*16+46, axis rows at y=270/286/302,
  // test frame (50,110)-(150,210), centered marker, ARROW at the
  // carried mouse, SYS_PAL head bound.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::MouseMenuSpec spec;
    spec.arrowX = 300;
    spec.arrowY = 45;
    CHECK(mdk::renderMouseMenuFrame(fb, palette, *fontSml,
                                    *arrow->frame(0), lbl, sysPal,
                                    spec, &err));
    // Arrow at the spec position.
    CHECK(fb.at(300, 45) == 77 && fb.at(301, 46) == 77);
    // Corners stay cleared (no backdrop).
    CHECK(fb.at(0, 0) == 0 && fb.at(599, 0) == 0 &&
          fb.at(0, 359) == 0);
    // Row 0 "Test" selected: the blink bracket — the glyph strip
    // spans (x, 15)-(x+3, 16) at y=16 pen; the bracket draws its
    // outer outline (x-2, 2)-(x+2+3, 20)-ish around it. Prove the
    // bracket exists by checking a pixel left of the text.
    // "Test" = 4 glyphs * 1px = w 4 -> x = (300-4)>>1 = 148.
    CHECK(fb.at(148, 15) == 7);   // the text itself (row0 pen 16,
                                  // glyph top 1 -> y 15)
    // Bracket outer: (x-2, penY-top+? ) — verify a bracket pixel
    // exists below the text line where nothing else draws.
    bool bracketFound = false;
    for (int x = 140; x < 160 && !bracketFound; ++x) {
      for (int y = 17; y < 24 && !bracketFound; ++y) {
        if (fb.at(x, y) == 1 || fb.at(x, y) == 2) bracketFound = true;
      }
    }
    CHECK(bracketFound);
    // Grid header row -1: "Buttons" right-aligned before x=396-8;
    // header cells show the live buttons nibble (0 -> hollow 14).
    CHECK(fb.at(397, 30 - 13) == 14);   // hollow outline color
    // Factory buttMap {1,4,2,0}: A->action0, C->action1, B->action2.
    // Row 0 (y=46): col0 filled -> solid 6 at (396,33)-(409,45).
    // Row 1 (y=62): col2 filled (mask C bit 1) -> (428,49) solid;
    //   col0 hollow -> outline (397,49)-(409,61) color 14.
    // Row 2 (y=78): col1 filled (mask B bit 2) -> (412,65) solid.
    CHECK(fb.at(396, 33) == 6);    // row0 col0 fill
    CHECK(fb.at(397, 49) == 14);   // row1 col0 hollow outline edge
    CHECK(fb.at(428, 49) == 6);    // row1 col2 fill
    CHECK(fb.at(412, 65) == 6);    // row2 col1 fill
    // Axis bars: hollow outline color 14 at (10, y-11)-(50, y-3);
    // marker centered at floor(0*20+30)=30 -> fill 6 x 28..32 —
    // it overwrites the bar's top/bottom edges where they cross.
    CHECK(fb.at(10, 259) == 14 && fb.at(50, 259) == 14);
    CHECK(fb.at(30, 263) == 6 && fb.at(28, 263) == 6 &&
          fb.at(32, 263) == 6);
    CHECK(fb.at(27, 263) == 0 && fb.at(33, 263) == 0);
    // Test indicator frame color 2 at (50,110)-(150,210); boxes
    // centered at (100,160) with delta 0.
    CHECK(fb.at(50, 110) == 2 && fb.at(150, 210) == 2);
    CHECK(fb.at(97, 157) == 3 && fb.at(103, 163) == 3);
    // Palette: SYS_PAL head bound, tail zeroed.
    CHECK(palette.get(1).r == 1 && palette.get(1).g == 199);
    CHECK(palette.get(200).r == 0 && palette.get(200).a == 255);
    // Contracts: wrong fb size, empty label, short palette head.
    mdk::IndexedFramebuffer small(64, 64);
    CHECK(!mdk::renderMouseMenuFrame(small, palette, *fontSml,
                                     *arrow->frame(0), lbl, sysPal,
                                     spec, &err));
    mdk::MouseMenuLabels bad{};
    CHECK(!mdk::renderMouseMenuFrame(fb, palette, *fontSml,
                                     *arrow->frame(0), bad, sysPal,
                                     spec, &err));
    std::array<std::byte, 64> shortPal{};
    CHECK(!mdk::renderMouseMenuFrame(fb, palette, *fontSml,
                                     *arrow->frame(0), lbl, shortPal,
                                     spec, &err));
  }

  // MouseOn=false / yrev bits=1 swap the row-1/2 texts; nonzero
  // deltas move the axis marker and the test box.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::MouseMenuSpec spec;
    spec.mouseOn = false;
    spec.mouseYReversedBits = 1;
    spec.deltas = {16, -32, 25};
    CHECK(mdk::renderMouseMenuFrame(fb, palette, *fontSml,
                                    *arrow->frame(0), lbl, sysPal,
                                    spec, &err));
    // Axis 0: delta/scale = 1 -> marker at floor(1*20+30)=50 ->
    // fill x 48..52; axis 1: -32/16 = -2 -> clamp -1 -> pos 10.
    CHECK(fb.at(50, 259) == 6 && fb.at(48, 259) == 6);
    CHECK(fb.at(10, 275) == 6);   // pos-2..pos+2 = 8..12 on row 1
    // Test box: mx = clamp(FISTP(50*16/16)=50) = 50 -> outer box
    // (147,107)-(153,113); its side edges land x=147/153.
    CHECK(fb.at(147, 110) == 3 && fb.at(153, 110) == 3);
  }

  // Dynamic frame: the controller drives the draw; the blink
  // accumulator advances once per flagged draw (rows 0-3 +
  // selected axis) — 2 flagged draws when an axis row is selected.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 45;
    mdk::MouseMenuController ctl(s, true, 0, "ABG", {1, 4, 2, 0},
                                 {16, 16, 50});
    CHECK(mdk::renderMouseMenuDynamic(fb, palette, *fontSml,
                                      *arrow->frame(0), lbl, sysPal,
                                      ctl, 0, &err));
    CHECK(fb.at(300, 45) == 77);
    // Entry selection 0 -> one flagged draw (row 0) -> acc += 1.
    CHECK(ctl.markerAccumulator() == 1);
    // Selection on an axis row flags BOTH its left-row-less slot
    // and the axis action — only the axis action draws flagged
    // here (rows 0-3 flag only when selected) -> still 1 draw.
    mdk::FrontendMenuInput in;
    in.mouseDy = 259 - 45;
    in.mouseDx = 100 - 300;
    ctl.update(in);
    CHECK(ctl.selection() == 4);
    const int accBefore = ctl.markerAccumulator();
    CHECK(mdk::renderMouseMenuDynamic(fb, palette, *fontSml,
                                      *arrow->frame(0), lbl, sysPal,
                                      ctl, 0, &err));
    CHECK(ctl.markerAccumulator() == accBefore + 1);
    // Mutate MouseOn off: next frame draws "Disabled" on row 1.
    in = {};
    in.mouseDy = 18 - 259;
    ctl.update(in);
    CHECK(ctl.selection() == 1);
    in = {};
    in.leftHeld = true;
    ctl.update(in);
    CHECK(!ctl.mouseOn());
    CHECK(mdk::renderMouseMenuDynamic(fb, palette, *fontSml,
                                      *arrow->frame(0), lbl, sysPal,
                                      ctl, 0, &err));
  }
}

// Phase 4K — the keyboard frame (FUN_0041f068 rows + frame tail).
void test_keyboard_render() {
  std::string err;
  // Every byte 1..255 maps to a 1x1 pixel glyph — labels and all
  // key-glyph table bytes draw deterministically; byte 0 stays
  // unmapped (the blank/unbound glyph case).
  auto fSml = SyntheticFont::make();
  for (int c = 1; c < 256; ++c) {
    fSml.put32(c * 4, fSml.addGlyph(0, 0, 1, {7}));
  }
  const auto fontSml = mdk::decodeFtiFont(fSml.buf, &err);
  CHECK(fontSml);
  auto arrowS = SyntheticSprite::make1(
      2, 2, 0, 0, {0x01, 77, 77, 0xfe, 0x01, 77, 77, 0xff});
  const auto arrow = mdk::decodeFtiSprite(arrowS.buf, &err);
  CHECK(arrow && arrow->frame(0));

  static const char* rowText[19] = {
      "Move Left",  "Move Right", "Move Up",    "Move Down",
      "Jump",       "Step Left",  "Side Step",  "Step Right",
      "Sniper",     "Fire",       "Turbo",      "Super Turbo",
      "Look Up",    "Look Down",  "Zoom In",    "Zoom Out",
      "Item Next",  "Item Prev",  "Item Use"};
  mdk::KeyboardMenuLabels lbl{};
  for (int i = 0; i < 19; ++i) lbl.rows[i] = rowText[i];
  lbl.reset = "Set Defaults";
  lbl.quit = "Quit";
  lbl.doit = "Press A Key";
  lbl.langTag = 'E';

  std::array<std::byte, 192> sysPal{};
  for (int i = 0; i < 64; ++i) {
    sysPal[i * 3 + 0] = std::byte(i);
    sysPal[i * 3 + 1] = std::byte(200 - i);
    sysPal[i * 3 + 2] = std::byte(i);
  }

  // Static spec frame (OBSERVED entry state): clear(0), 19 rows in
  // two columns (label x = 310*col+10, glyph x = 310*col+210,
  // y = 30*(row%10)+64), KM_RESET at y=16, KM_QUIT at y=32 with the
  // entry selection's blink bracket, no KM_DOIT, ARROW at the
  // carried mouse, SYS_PAL head bound + tail zeroed.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::KeyboardMenuSpec spec;
    spec.arrowX = 300;
    spec.arrowY = 45;
    CHECK(mdk::renderKeyboardMenuFrame(fb, palette, *fontSml,
                                       *arrow->frame(0), lbl, sysPal,
                                       spec, &err));
    // Row 0: label at (10,64), key glyph (en[105]=0x8b) at (210,64).
    CHECK(fb.at(10, 64) == 7 && fb.at(210, 64) == 7);
    // Row 10 — the right column: label (320,64), glyph (520,64).
    CHECK(fb.at(320, 64) == 7 && fb.at(520, 64) == 7);
    // Row 18 — last binding, right column y=304.
    CHECK(fb.at(320, 304) == 7 && fb.at(520, 304) == 7);
    // "Set Defaults" (12 px) centered: x = (600-12)/2 = 294 @y16.
    CHECK(fb.at(294, 16) == 7);
    // "Quit" (4 px) centered: x = (600-4)/2 = 298 @y32 — selected
    // at entry, so the blink bracket rings it. Outer top edge at
    // y = 32-14 = 18 (multi-char default top=14), inner at 19.
    CHECK(fb.at(298, 32) == 7);
    bool bracket = false;
    for (int x = 295; x <= 305 && !bracket; ++x) {
      if (fb.at(x, 18) == 1 || fb.at(x, 18) == 2 ||
          fb.at(x, 19) == 1 || fb.at(x, 19) == 2) {
        bracket = true;
      }
    }
    CHECK(bracket);
    // No capture prompt at entry.
    CHECK(fb.at(294, 354) == 0 && fb.at(300, 354) == 0);
    // ARROW at the spec position; corners stay cleared.
    CHECK(fb.at(300, 45) == 77 && fb.at(301, 46) == 77);
    CHECK(fb.at(0, 0) == 0 && fb.at(599, 0) == 0 &&
          fb.at(0, 359) == 0);
    // Palette: SYS_PAL head bound, tail zeroed.
    CHECK(palette.get(1).r == 1 && palette.get(1).g == 199);
    CHECK(palette.get(200).r == 0 && palette.get(200).a == 255);
    // Contracts: wrong fb size, short palette head, empty labels.
    mdk::IndexedFramebuffer small(64, 64);
    CHECK(!mdk::renderKeyboardMenuFrame(small, palette, *fontSml,
                                        *arrow->frame(0), lbl, sysPal,
                                        spec, &err));
    std::array<std::byte, 64> shortPal{};
    CHECK(!mdk::renderKeyboardMenuFrame(fb, palette, *fontSml,
                                        *arrow->frame(0), lbl,
                                        shortPal, spec, &err));
    mdk::KeyboardMenuLabels bad = lbl;
    bad.rows[3] = "";
    CHECK(!mdk::renderKeyboardMenuFrame(fb, palette, *fontSml,
                                        *arrow->frame(0), bad, sysPal,
                                        spec, &err));
    bad = lbl;
    bad.doit = "";
    CHECK(!mdk::renderKeyboardMenuFrame(fb, palette, *fontSml,
                                        *arrow->frame(0), bad, sysPal,
                                        spec, &err));
  }

  // A binding whose glyph byte is 0 (the unmapped/blank case —
  // internal 127's EN entry) draws nothing at the glyph position.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::KeyboardMenuSpec spec;
    spec.keys[0] = 127;   // en[127] = 0 -> blank glyph
    spec.arrowX = 300;
    spec.arrowY = 45;
    CHECK(mdk::renderKeyboardMenuFrame(fb, palette, *fontSml,
                                       *arrow->frame(0), lbl, sysPal,
                                       spec, &err));
    CHECK(fb.at(210, 64) == 0);      // blank glyph
    CHECK(fb.at(10, 64) == 7);       // label still draws
  }

  // The FR/DE tables change the drawn glyph bytes — AZERTY's 'M'
  // position (0x32) shows ',', QWERTZ's 'Z' position (0x2c) shows
  // 'Y'. A rebound value selects a different glyph byte.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::KeyboardMenuSpec spec;
    spec.langTag = 'F';
    spec.keys[0] = 0x32;             // en 'M' vs fr ','
    CHECK(mdk::renderKeyboardMenuFrame(fb, palette, *fontSml,
                                       *arrow->frame(0), lbl, sysPal,
                                       spec, &err));
    CHECK(fb.at(210, 64) == 7);      // FR ',' glyph drawn
    spec.langTag = 'x';              // unknown tag -> English
    spec.keys[0] = 0x7f;             // en[127] = 0 -> blank
    CHECK(mdk::renderKeyboardMenuFrame(fb, palette, *fontSml,
                                       *arrow->frame(0), lbl, sysPal,
                                       spec, &err));
    CHECK(fb.at(210, 64) == 0);
  }

  // Dynamic frame: the controller drives the draw — the blink
  // accumulator advances once per FLAGGED draw. Entry selection 20
  // flags KM_QUIT only: +1 per frame.
  {
    mdk::IndexedFramebuffer fb(600, 360);
    mdk::Palette palette;
    mdk::FrontendMachineState s;
    s.mouseX = 300;
    s.mouseY = 45;
    mdk::KeyboardMenuController ctl(s, mdk::kKeyboardDefaults, false);
    CHECK(mdk::renderKeyboardMenuDynamic(fb, palette, *fontSml,
                                         *arrow->frame(0), lbl,
                                         sysPal, ctl, 0, &err));
    CHECK(ctl.markerAccumulator() == 1);   // QUIT flagged once
    CHECK(fb.at(300, 45) == 77);
    // Navigate to row 8: normal mode flags the LABEL — still one
    // flagged draw.
    mdk::FrontendMenuInput in;
    in.mouseDy = 304 - 45;
    ctl.update(in);
    in = {};
    CHECK(ctl.selection() == 8);
    int acc = ctl.markerAccumulator();
    CHECK(mdk::renderKeyboardMenuDynamic(fb, palette, *fontSml,
                                         *arrow->frame(0), lbl,
                                         sysPal, ctl, 0, &err));
    CHECK(ctl.markerAccumulator() == acc + 1);
    // Capture mode flags the selected row's GLYPH instead and draws
    // KM_DOIT unflagged — still exactly one flagged draw.
    in.confirmEdge = true;
    ctl.update(in);
    in = {};
    CHECK(ctl.capture());
    acc = ctl.markerAccumulator();
    fb.clear(0);
    CHECK(mdk::renderKeyboardMenuDynamic(fb, palette, *fontSml,
                                         *arrow->frame(0), lbl,
                                         sysPal, ctl, 0, &err));
    CHECK(ctl.markerAccumulator() == acc + 1);
    // "Press A Key" (11 px) centered: x = (600-11)/2 = 294 @y354.
    CHECK(fb.at(294, 354) == 7);
    CHECK(fb.at(10, 304) == 7);      // the unflagged row-8 label
    // ARROW still draws in capture mode.
    CHECK(fb.at(300, 304) == 77);
    // After the commit the new glyph byte renders on the row.
    in.rawKeyEdge[1] = 1u << (45 - 32);   // 'X'
    ctl.update(in);
    in = {};
    CHECK(!ctl.capture() && ctl.keyAt(7) == 45);
    fb.clear(0);
    CHECK(mdk::renderKeyboardMenuDynamic(fb, palette, *fontSml,
                                         *arrow->frame(0), lbl,
                                         sysPal, ctl, 0, &err));
    CHECK(fb.at(210, 304) == 7);     // en[45]='X' glyph on row 8
    CHECK(fb.at(294, 354) == 0);     // capture prompt gone
  }
}

// Phase 5A — gameplay input consumption. Values below are
// hand-computed from the reconstructed FUN_00419370 + FUN_00406f14
// semantics (OBSERVED constants; smoothedDelta = 1.0, frameStep = 1
// unless the test overrides them).
void test_gameplay_input() {
  auto setBit = [](std::array<std::uint32_t, 4>& bm, int code) {
    bm[code >> 5] |= 1u << (code & 31);
  };
  auto press = [&](mdk::RawGameplayInput& r, int code) {
    setBit(r.keyLevel, code);
    setBit(r.keyEdge, code);
  };
  auto hold = [&](mdk::RawGameplayInput& r, int code) {
    setBit(r.keyLevel, code);
  };
  mdk::GameplayInputBindings bind;      // factory block + W set
  mdk::GameplayInputEnvironment env;    // smoothed 1.0, step 1
  mdk::GameplayInputState st;

  // ---- key level/edge queries + right-modifier fold ---------------
  {
    mdk::RawGameplayInput r;
    press(r, 103);
    CHECK(mdk::gameplayKeyLevel(r, 103) != 0);
    CHECK(mdk::gameplayKeyEdge(r, 103) != 0);
    mdk::RawGameplayInput r2;
    hold(r2, 103);   // level only — no fresh edge
    CHECK(mdk::gameplayKeyLevel(r2, 103) != 0);
    CHECK(mdk::gameplayKeyEdge(r2, 103) == 0);
    // Fold: query 0x2a (LSHIFT) sees key 0x36 (RSHIFT) — raw mask.
    mdk::RawGameplayInput r3;
    hold(r3, 0x36);
    CHECK(mdk::gameplayKeyLevel(r3, 0x2a) == (1u << 22));
    // 0x1d (LCTRL) fold sees 0x61 (RCTRL) on the edge bitmap.
    setBit(r3.keyEdge, 0x61);
    CHECK(mdk::gameplayKeyEdge(r3, 0x1d) == (1u << 1));
    // 0x38 (LALT) fold sees 0x65 (RALT).
    hold(r3, 0x65);
    CHECK(mdk::gameplayKeyLevel(r3, 0x38) == (1u << 5));
    // A non-modifier code is a plain bit test.
    CHECK(mdk::gameplayKeyLevel(r3, 0x36) == (1u << 22));
  }

  // ---- 19 visible action semantics --------------------------------
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 105);  // KeyLeft — level
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.turnAxis == -1.0f && f.yawAxis == -1.0f);
    CHECK(near(f.turnNorm, -0.9) && near(f.turnFast, -4.0));
    CHECK(near(f.turnNorm75, -0.675) && near(f.turnFast75, -3.0));
    CHECK(near(f.yawNorm, -0.4) && near(f.yawFast, -4.0));
    CHECK(near(f.yawNeg45, 45.0) && near(f.yaw4, -4.0));
    CHECK(near(f.yawThird, -1.0 / 3.0) && near(f.yaw10, -10.0));
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 106);  // KeyRight
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.turnAxis == 1.0f && near(f.turnNorm, 0.9));
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 103);  // KeyUp — move forward = -1 axis
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.moveAxis == -1.0f && f.moveDigital == -1.0f);
    CHECK(near(f.moveNorm, -0.4) && near(f.moveFast, -4.0));
    CHECK(near(f.moveVel, 1.0 / 22.5));
    CHECK(near(f.moveVelBoosted, 2.0 / 3.0));
    CHECK(near(f.moveHalfSlow, 0.5 / 22.5));
    CHECK(near(f.moveHalfFast, 1.0 / 3.0));
    CHECK(near(f.move5pct, 0.05));
    CHECK(near(f.moveThird, -1.0 / 3.0) && near(f.move10, -10.0));
    // Forward (axis < 0) selects the 15.0 rate constant.
    CHECK(near(f.moveSpeed, 15.0));
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 108);  // KeyDown — backward selects the 35.0 constant.
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.moveAxis == 1.0f && near(f.moveSpeed, -35.0));
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 56);   // KeyJump — level flag, raw bit value kept.
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.jump == (1u << 24));
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 45);   // KeySide — the strafe MODIFIER.
    hold(r, 105);  // + KeyLeft -> strafe, not turn.
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.sideStepHeld);
    CHECK(f.turnAxis == 0.0f && f.strafeAxis == -1.0f);
    CHECK(f.yawAxis == -1.0f);   // strafe feeds the yaw combined axis
    CHECK(near(f.strafeNorm, -1.0 / 22.5));
    CHECK(near(f.strafeFast, -2.0 / 3.0));
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 29);   // KeyFire — level (LCTRL).
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.fire == (1u << 29));
    mdk::RawGameplayInput r2;
    hold(r2, 97);  // RCTRL also fires via the query fold.
    st = {};
    f = mdk::consumeGameplayInput(r2, bind, env, st);
    CHECK(f.fire == (1u << 1));
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    press(r, 57);  // KeySniper — EDGE queried.
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.sniperPulse == (1u << 25));
    mdk::RawGameplayInput r2;
    hold(r2, 57);  // held — the edge does not repeat
    f = mdk::consumeGameplayInput(r2, bind, env, st);
    CHECK(f.sniperPulse == 0);
    press(r2, 57); // a fresh edge pulses again
    f = mdk::consumeGameplayInput(r2, bind, env, st);
    CHECK(f.sniperPulse != 0);
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 42);   // KeyTurbo — level; modifies rate constants.
    hold(r, 105);
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(near(f.turnNorm, -1.3) && near(f.turnFast, -6.0));
    // OBSERVED: the 0.75-scaled pair keeps the non-turbo constants.
    CHECK(near(f.turnNorm75, -0.675) && near(f.turnFast75, -3.0));
    CHECK(near(f.yawNorm, -0.6) && near(f.yawFast, -6.0));
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    press(r, 58);  // KeySturbo — edge toggles the latch.
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.turboLatched && st.setTurboLatch == 1);
    // While latched the turbo rates apply without the key held.
    mdk::RawGameplayInput r2;
    hold(r2, 105);
    f = mdk::consumeGameplayInput(r2, bind, env, st);
    CHECK(near(f.turnNorm, -1.3));
    press(r2, 58);  // second edge unlatches
    f = mdk::consumeGameplayInput(r2, bind, env, st);
    CHECK(!f.turboLatched && st.setTurboLatch == 0);
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 51);   // KeySideL — level strafe.
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.strafeAxis == -1.0f && !f.sideStepHeld);
    mdk::RawGameplayInput r2;
    hold(r2, 52);  // KeySideR
    st = {};
    f = mdk::consumeGameplayInput(r2, bind, env, st);
    CHECK(f.strafeAxis == 1.0f);
  }
  st = {};
  {
    // INEXT/IPREV/IUSE — edge-queried item actions.
    mdk::RawGameplayInput r;
    press(r, 27); press(r, 26); press(r, 28);
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.itemNext == (1u << 27));
    CHECK(f.itemPrev == (1u << 26));
    CHECK(f.itemUse == (1u << 28));
    mdk::RawGameplayInput r2;
    hold(r2, 27); hold(r2, 26); hold(r2, 28);
    f = mdk::consumeGameplayInput(r2, bind, env, st);
    CHECK(f.itemNext == 0 && f.itemPrev == 0 && f.itemUse == 0);
  }

  // ---- hidden 10 weapon hotkeys (slots 14..23, edge) ---------------
  st = {};
  {
    mdk::RawGameplayInput r;
    press(r, 2);   // '1' -> weapon slot 0
    press(r, 11);  // '0' -> weapon slot 9
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.weaponSelect[0] == (1u << 2));
    CHECK(f.weaponSelect[9] == (1u << 11));
    // Multiple simultaneous hotkey edges all land — no flattening.
    mdk::RawGameplayInput r2;
    press(r2, 3); press(r2, 5); press(r2, 7);
    f = mdk::consumeGameplayInput(r2, bind, env, st);
    CHECK(f.weaponSelect[1] != 0 && f.weaponSelect[3] != 0 &&
          f.weaponSelect[5] != 0);
    // Held without a fresh edge produces nothing.
    mdk::RawGameplayInput r3;
    hold(r3, 4);
    f = mdk::consumeGameplayInput(r3, bind, env, st);
    CHECK(f.weaponSelect[2] == 0);
  }

  // ---- duplicate bindings (factory LKUP + ZOOMI share 'A'=30) ------
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 30);   // one physical key drives BOTH actions
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.lookUp != 0);
    // ZoomIn level charged the accumulator (+1, then the tail emits
    // the zoom velocities and the dt decay drains the charge).
    CHECK(near(f.zoomVel, -0.01) && near(f.zoomVelFast, -0.15));
    CHECK(f.zoomAccumulator == 0);  // 1 - 1 -> 0
  }

  // ---- mouse axis letters A..F --------------------------------------
  st = {};
  {
    mdk::RawGameplayInput r;
    r.mouseDx = 320;   // 'A' on axis 0 -> turn
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    // 320/16 = 20; /1.0 -> clamp(20,+-4) = 4
    CHECK(near(f.turnFast, 4.0 * 6.0 * 0.5));
    CHECK(near(f.turnNorm, 4.0 * 6.0 * 0.5 / 1.0));
    CHECK(near(f.yaw4, 4.0 * 4.0) && near(f.yawNeg45, -180.0, 1e-4));
    CHECK(f.mouseTurnActive == 1);
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    r.mouseDy = 160;   // 'B' on axis 1 -> move
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    // 160/16 = 10; /1.0 -> clamp(10) = 4
    CHECK(near(f.moveVelBoosted, -4.0 * (4.0 / 3.0) * 0.5));
    CHECK(near(f.moveVel, -4.0 * (4.0 / 3.0) * 0.5 / 1.0));
    CHECK(near(f.moveHalfFast, -4.0 * (2.0 / 3.0) * 0.25));
    CHECK(near(f.move5pct, -4.0 * 0.05 * 0.5));
    CHECK(f.moveSpeed == 0.0f);   // digital move is 0 (mouse-only)
  }
  st = {};
  {
    mdk::RawGameplayInput r;
    r.mouseDz = 120;   // 'G' on axis 2 -> sniper zoom charge
    env.frameStep = 0;  // isolate the accumulator from decay
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    // rint(120/50 + 1) = rint(3.4) = 3 ticks, sign + for 'G'.
    CHECK(f.zoomAccumulator == 3);
    CHECK(near(f.zoomVel, -0.01));
    env.frameStep = 1;
  }
  st = {};
  {
    // 'D' negates the turn axis.
    mdk::GameplayInputBindings b2 = bind;
    b2.mouseAxesMap = "DBG";
    mdk::RawGameplayInput r;
    r.mouseDx = 320;
    auto f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(near(f.turnFast, -12.0));
    // 'E' negates move — the letter position selects the axis, so
    // 'E' on axis 0 consumes mouseDx (not dy).
    b2.mouseAxesMap = "EBG";
    r.mouseDx = 160;
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(near(f.moveVelBoosted, 4.0 * (4.0 / 3.0) * 0.5));
    // 'C' routes to strafe; 'F' negates it.
    b2.mouseAxesMap = "C0G";
    r = {};
    r.mouseDx = 160;
    st = {};
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(near(f.strafeFast, 4.0 * (4.0 / 3.0) * 0.5));
    b2.mouseAxesMap = "F0G";
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(near(f.strafeFast, -4.0 * (4.0 / 3.0) * 0.5));
    // 'H' is negative sniper zoom.
    b2.mouseAxesMap = "ABH";
    r = {};
    r.mouseDz = 120;
    env.frameStep = 0;
    st = {};
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.zoomAccumulator == -3);
    env.frameStep = 1;
    // '0' is inert.
    b2.mouseAxesMap = "0BG";
    r = {};
    r.mouseDx = 320;
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.turnFast == 0.0f && f.mouseTurnActive == 0);
  }
  st = {};
  {
    // SideStep modifier reroutes mouse 'A'/'D' to strafe.
    mdk::RawGameplayInput r;
    hold(r, 45);
    r.mouseDx = 160;
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.mouseTurnActive == 0 && f.strafeFast != 0.0f);
  }
  st = {};
  {
    // First-G/H-wins early-out: 'G' on axis 0 consumes dx and ends
    // the scan — the 'H' on axis 2 never processes dz.
    mdk::GameplayInputBindings b2 = bind;
    b2.mouseAxesMap = "G0H";
    env.frameStep = 0;
    mdk::RawGameplayInput r;
    r.mouseDx = 50;    // rint(50/16+1) = 4
    r.mouseDz = 50;    // would be -4 if 'H' were reached
    auto f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.zoomAccumulator == 4);
    env.frameStep = 1;
  }

  // ---- scale math / deadzone / clamp --------------------------------
  st = {};
  {
    mdk::RawGameplayInput r;
    r.mouseDx = 3;     // 3/16 = 0.1875 < 0.2 deadzone -> 0
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.turnFast == 0.0f && f.mouseTurnActive == 0);
    r.mouseDx = 4;     // 4/16 = 0.25 -> passes
    f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.mouseTurnActive == 1);
    mdk::GameplayInputBindings b2 = bind;
    b2.mouseScale = {8.0f, 16.0f, 50.0f};
    r.mouseDx = 80;    // 80/8 = 10 -> clamp(10/1.0) = 4 -> *3 = 12
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(near(f.turnFast, 12.0));
    // smoothed-delta clamp: 16000/8 = 2000 -> /1.0 = 2000 -> clamp 4
    r.mouseDx = 16000;
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(near(f.turnFast, 4.0 * 6.0 * 0.5));
    b2.mouseScale = {0.0f, 16.0f, 50.0f};   // zero scale -> inf -> clamp
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(near(f.turnFast, 12.0));
  }

  // ---- MouseOn gate (axes only — buttons still decode) --------------
  st = {};
  {
    mdk::GameplayInputBindings b2 = bind;
    b2.mouseOn = false;
    mdk::RawGameplayInput r;
    r.mouseDx = 320;
    r.mouseDz = 120;
    r.mouseButtons = 0x1;    // button A mask = bit0 Fire
    auto f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.turnFast == 0.0f && f.zoomAccumulator == 0);
    CHECK(f.mouseTurnActive == 0);
    CHECK(f.fire == 1);      // OBSERVED: MouseOn does NOT gate buttons
  }

  // ---- mouse button masks (A..D physical order) ----------------------
  st = {};
  {
    // Factory {1,4,2,0}: btn0 Fire, btn1 Jump, btn2 Sniper, btn3 off.
    mdk::RawGameplayInput r;
    r.mouseButtons = 0x1;
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.fire == 1 && f.jump == 0);
    r.mouseButtons = 0x2;
    f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.jump == 1);
    // Sniper is a button LEVEL -> synthetic edge: pulse once, then
    // silence while held, re-arms on release.
    r.mouseButtons = 0x4;
    st = {};
    f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.sniperPulse == 1);
    f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.sniperPulse == 0);
    r.mouseButtons = 0;
    f = mdk::consumeGameplayInput(r, bind, env, st);
    r.mouseButtons = 0x4;
    f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(f.sniperPulse == 1);
    // A multi-bit mask produces several actions from one button.
    mdk::GameplayInputBindings b2 = bind;
    b2.mouseButtMask = {mdk::kBtnFire | mdk::kBtnJump, 0, 0, 0};
    r.mouseButtons = 0x1;
    st = {};
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.fire == 1 && f.jump == 1);
    // The SideStep button bit reroutes the turn axis like the key.
    b2.mouseButtMask = {mdk::kBtnSideStep, 0, 0, 0};
    r.mouseButtons = 0x1;
    hold(r, 105);
    st = {};
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.sideStepHeld && f.strafeAxis == -1.0f);
    // Move/strafe button bits pre-seed the digital axes.
    b2.mouseButtMask = {mdk::kBtnMoveFwd, 0, 0, 0};
    r = {};
    r.mouseButtons = 0x1;
    st = {};
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.moveDigital == -1.0f);
    b2.mouseButtMask = {0, mdk::kBtnStrafeRight | mdk::kBtnTurbo, 0, 0};
    r.mouseButtons = 0x2;
    st = {};
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(near(f.strafeFast, 4.0 / 3.0));   // strafe + turbo mask bit
  }

  // ---- combined keyboard + mouse --------------------------------------
  st = {};
  {
    mdk::RawGameplayInput r;
    hold(r, 105);          // kbd LEFT: turn -1
    r.mouseDx = 320;       // mouse 'A': clamp(20/1.0) = 4
    auto f = mdk::consumeGameplayInput(r, bind, env, st);
    // OBSERVED priority: a nonzero mouse axis OVERWRITES the shared
    // rate fields (4*3 = 12, not the keyboard -4).
    CHECK(near(f.turnFast, 12.0));
    CHECK(f.mouseTurnActive == 1);
    // Keyboard-only fields (item/action flags) are unaffected.
    r.mouseDx = 0;
    f = mdk::consumeGameplayInput(r, bind, env, st);
    CHECK(near(f.turnFast, -4.0));   // falls back to the kbd rate
  }

  // ---- settings-derived bindings ---------------------------------------
  st = {};
  {
    mdk::FrontendSettings s;
    s.keySniper = 45;               // X instead of Space
    s.mouseWAxesMap = "HBG";        // dz -> NegSniperZoom, dy -> Move
    s.mouseWButtMapA = 0x8001;      // btn A = Fire | Turbo
    s.mouseWXScale = 8.0f;
    s.mouseYReversed = 1;           // raw bits -> semantic flag
    auto b2 = mdk::gameplayBindingsFromSettings(s);
    CHECK(b2.keys[7] == 45);
    // Hidden hotkeys come from the factory block, not settings.
    CHECK(b2.keys[14] == 2 && b2.keys[23] == 11);
    CHECK(b2.mouseOn && b2.mouseYReversedBits == 1);
    mdk::RawGameplayInput r;
    press(r, 45);
    auto f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.sniperPulse != 0);
    r = {};
    r.mouseDx = 8;               // 'H' sits on axis 0 -> consumes dx;
    env.frameStep = 0;           // 8/8=1 -> +1 bias -> 2 ticks, '-'
    st = {};
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.zoomAccumulator == -2);        // 'H' route honored
    env.frameStep = 1;
    CHECK(f.mouseYReversed);               // pass-through flag set
    r = {};
    r.mouseButtons = 0x1;
    hold(r, 105);
    st = {};
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.fire == 1 && near(f.turnNorm, -1.3));  // turbo mask bit
  }

  // ---- hidden-hotkey reset round-trip ----------------------------------
  {
    // A modified hidden slot drives weaponSelect; the factory block
    // (what Keyboard Reset restores) puts back '1'..'0'.
    mdk::GameplayInputBindings b2 = bind;
    b2.keys[14] = 50;                // rebind weapon 1 to 'M'
    mdk::RawGameplayInput r;
    press(r, 50);
    st = {};
    auto f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.weaponSelect[0] != 0);
    b2.keys = mdk::kKeyboardDefaults;      // the Reset mirror copy
    r = {};
    press(r, 2);
    f = mdk::consumeGameplayInput(r, b2, env, st);
    CHECK(f.weaponSelect[0] != 0 && f.weaponSelect[9] == 0);
  }
}

// Phase 5B — FUN_00465228 horizontal movement integration. Golden
// values are hand-computed from the disassembly: f0-scaled accel
// (move/strafe), raw-add accel (kbd turn), decel rates 4/45|8/45
// (bound 2/3) and 0.55|1.6 (bound 4), airborne scales 0.75/0.75,
// disp = move*(cos,sin) + strafe*(sin,-cos), yaw -= turnVel*f0.
void test_player_motion() {
  auto setBit = [](std::array<std::uint32_t, 4>& bm, int code) {
    bm[code >> 5] |= 1u << (code & 31);
  };
  auto hold = [&](mdk::RawGameplayInput& r, int code) {
    setBit(r.keyLevel, code);
  };
  mdk::GameplayInputBindings bind;
  mdk::GameplayInputEnvironment genv;
  mdk::PlayerMotionEnvironment env;   // smoothed 1.0, airborne
  const double kMoveRate = 1.0 / 22.5;   // 0.04444 kbd fwd accel
  const double kMoveCap = 2.0 / 3.0;     // 0.66667 kbd fwd cap

  // ---- idle --------------------------------------------------------
  {
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    const mdk::GameplayInputFrame f =
        mdk::consumeGameplayInput(mdk::RawGameplayInput{}, bind, genv, gst);
    const mdk::PlayerMotionOutput out =
        mdk::integratePlayerMotion(f, env, s);
    CHECK(out.ran && out.dispX == 0.0f && out.dispY == 0.0f &&
          out.dispZ == 0.0f);
    CHECK(out.eventType == 0 && out.eventMag == 0);
    CHECK(s.moveVel == 0.0f && s.strafeVel == 0.0f && s.turnVel == 0.0f);
    mdk::PlayerMotionState s2 = s;
    mdk::PlayerMotionOutput o2 = out;
    mdk::playerMotionPostStep(env, true, s2, o2);
    CHECK(o2.eventType == 0 && s2.yawDeg == 0.0f);
  }

  // ---- turn left (kbd — raw-add channel, no f0 scaling) ------------
  {
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::RawGameplayInput r;
    hold(r, 105);                    // KeyLeft -> turnNorm -0.9
    auto step = [&](const mdk::RawGameplayInput& rr) {
      const mdk::GameplayInputFrame f =
          mdk::consumeGameplayInput(rr, bind, genv, gst);
      mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
      mdk::playerMotionPostStep(env, true, s, o);
      return o;
    };
    mdk::PlayerMotionOutput o = step(r);
    CHECK(near(s.turnVel, -0.9));             // raw add, not x f0
    CHECK(near(s.yawDeg, 0.9));               // yaw -= turnVel
    CHECK(o.eventType == 4 && o.eventMag == 400);
    o = step(r);
    CHECK(near(s.turnVel, -1.8) && near(s.yawDeg, 2.7));
    o = step(r);
    CHECK(near(s.turnVel, -2.7) && near(s.yawDeg, 5.4));
    o = step(r);
    CHECK(near(s.turnVel, -3.6) && near(s.yawDeg, 9.0));
    o = step(r);                              // -4.5 clamps to -4
    CHECK(near(s.turnVel, -4.0) && near(s.yawDeg, 13.0));
    mdk::RawGameplayInput rel;                // release -> decay 0.55
    o = step(rel);
    CHECK(near(s.turnVel, -3.45) && near(s.yawDeg, 16.45, 1e-4));
    CHECK(o.eventType == 4);                  // residual still emits
    for (int i = 0; i < 7; ++i) o = step(rel);
    CHECK(s.turnVel == 0.0f);                 // snapped through zero
    CHECK(near(s.yawDeg, 25.6, 1e-4));
    CHECK(o.eventType == 0);
  }

  // ---- turn right + negative yaw wrap ------------------------------
  {
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::RawGameplayInput r;
    hold(r, 106);                    // KeyRight -> turnNorm +0.9
    const mdk::GameplayInputFrame f =
        mdk::consumeGameplayInput(r, bind, genv, gst);
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
    CHECK(near(s.turnVel, 0.9));
    CHECK(near(s.yawDeg, 359.1, 1e-4));       // 0 - 0.9 -> +360 wrap
    CHECK(o.eventType == 4 && o.eventMag == 400);
  }

  // ---- forward/back (f0-scaled channel) ----------------------------
  {
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::RawGameplayInput r;
    hold(r, 103);                    // KeyUp -> moveVel +1/22.5
    auto step = [&](const mdk::RawGameplayInput& rr) {
      const mdk::GameplayInputFrame f =
          mdk::consumeGameplayInput(rr, bind, genv, gst);
      mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
      mdk::playerMotionPostStep(env, true, s, o);
      return o;
    };
    mdk::PlayerMotionOutput o = step(r);
    CHECK(near(s.moveVel, kMoveRate));
    CHECK(near(o.dispX, kMoveRate) && o.dispY == 0.0f);
    CHECK(o.eventType == 6 && o.eventMag == 600);
    CHECK(o.moveConsumed && o.forwardIntent && s.moveDirLatch == 1);
    for (int i = 0; i < 14; ++i) o = step(r); // 15 * 1/22.5 = 2/3 cap
    CHECK(near(s.moveVel, kMoveCap, 1e-4));
    CHECK(near(o.dispX, kMoveCap, 1e-4));
    mdk::RawGameplayInput rel;                // release -> decel 0.0667
    o = step(rel);
    CHECK(near(s.moveVel, kMoveCap - 0.75 * (4.0 / 45.0), 1e-4));
    for (int i = 0; i < 10; ++i) o = step(rel);
    CHECK(s.moveVel == 0.0f);
    // Backward: negative channel + latch -1.
    mdk::RawGameplayInput rb;
    hold(rb, 108);                            // KeyDown
    gst = {};
    s = {};
    o = step(rb);
    CHECK(near(s.moveVel, -kMoveRate));
    CHECK(near(o.dispX, -kMoveRate) && s.moveDirLatch == -1);
    CHECK(o.eventMag == 600 && !o.forwardIntent);
  }

  // ---- strafe left/right + basis sign ------------------------------
  {
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::RawGameplayInput r;
    hold(r, 51);                     // KeySideL -> strafe -1/22.5
    const mdk::GameplayInputFrame f =
        mdk::consumeGameplayInput(r, bind, genv, gst);
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
    // Airborne accel scale 0.75: rate = -1/22.5 * 0.75 = -1/30.
    CHECK(near(s.strafeVel, -kMoveRate * 0.75));
    CHECK(o.dispX == 0.0f);                   // sin(0) term
    CHECK(near(o.dispY, kMoveRate * 0.75));   // -(-1/30)*cos(0)
    CHECK(o.eventType == 5 && o.eventMag == 500);
    // At yaw 90 the same left strafe goes -X.
    s = {};
    s.yawDeg = 90.0f;
    o = mdk::integratePlayerMotion(f, env, s);
    CHECK(near(o.dispX, -kMoveRate * 0.75, 1e-4));
    CHECK(std::fabs(o.dispY) < 1e-5f);
    // KeySideR -> +strafe -> -Y at yaw 0.
    mdk::GameplayInputState gst2;
    mdk::PlayerMotionState s2;
    mdk::RawGameplayInput r2;
    hold(r2, 52);
    const mdk::GameplayInputFrame f2 =
        mdk::consumeGameplayInput(r2, bind, genv, gst2);
    o = mdk::integratePlayerMotion(f2, env, s2);
    CHECK(near(s2.strafeVel, kMoveRate * 0.75));
    CHECK(near(o.dispY, -kMoveRate * 0.75));
  }

  // ---- diagonal: additive, not normalized ---------------------------
  {
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::RawGameplayInput r;
    hold(r, 103);                    // fwd + strafe-left
    hold(r, 51);
    const mdk::GameplayInputFrame f =
        mdk::consumeGameplayInput(r, bind, genv, gst);
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
    CHECK(near(o.dispX, kMoveRate));
    CHECK(near(o.dispY, kMoveRate * 0.75));
    // |d| > |move| alone — OBSERVED additive compose, no normalize.
    CHECK(std::sqrt(o.dispX * o.dispX + o.dispY * o.dispY) >
          (float)kMoveRate);
    CHECK(o.eventType == 6 && o.eventMag == 600);  // move priority
  }

  // ---- forward + turn: bank drive + move event wins -----------------
  {
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::RawGameplayInput r;
    hold(r, 103);
    hold(r, 105);                    // turnNorm -0.9 * moveVel +1/22.5
    const mdk::GameplayInputFrame f =
        mdk::consumeGameplayInput(r, bind, genv, gst);
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
    CHECK(near(s.bank, -0.25));               // drive < 0 -> bank down
    CHECK(o.bankEvent);
    CHECK(o.eventType == 6 && o.eventMag == 600);
    mdk::playerMotionPostStep(env, true, s, o);
    CHECK(near(s.bank, -0.25));               // no decay while driven
    mdk::PlayerMotionState s2 = s;
    mdk::PlayerMotionOutput o2 = o;
    o2.bankEvent = false;                     // decay engages
    mdk::playerMotionPostStep(env, true, s2, o2);
    // rate = |bank|*0.35 = 0.0875 (inside [0.05,2.5]).
    CHECK(near(s2.bank, -0.25 + 0.0875, 1e-5));
  }

  // ---- turbo forward -------------------------------------------------
  {
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::RawGameplayInput r;
    hold(r, 42);                     // KeyTurbo
    hold(r, 103);
    const mdk::GameplayInputFrame f =
        mdk::consumeGameplayInput(r, bind, genv, gst);
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
    // Turbo rates feed the same channels — no extra scaling here.
    CHECK(near(s.moveVel, 4.0 / 45.0));
    CHECK(near(o.dispX, 4.0 / 45.0));
    for (int i = 0; i < 14; ++i)
      o = mdk::integratePlayerMotion(f, env, s);
    CHECK(near(s.moveVel, 4.0 / 3.0, 1e-4));  // the x4/3-lifted cap
    CHECK(near(o.dispX, 4.0 / 3.0, 1e-4));
    // Beyond-bound decay: 4/3 > 2/3 -> rOut 0.75*8/45.
    o = mdk::integratePlayerMotion(mdk::GameplayInputFrame{}, env, s);
    CHECK(near(s.moveVel, 4.0 / 3.0 - 0.75 * (8.0 / 45.0), 1e-4));
    CHECK(o.eventMag == 600);   // residual still emits the move event
  }

  // ---- mouse-derived turn (f0-scaled helper, instant impulse) -------
  {
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::RawGameplayInput r;
    r.mouseDx = 320;                 // 'A': clamp(20) = 4 -> rates 12
    const mdk::GameplayInputFrame f =
        mdk::consumeGameplayInput(r, bind, genv, gst);
    CHECK(f.mouseTurnActive == 1);
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
    CHECK(near(s.turnVel, 12.0));             // f0-scaled: 12 in one hit
    CHECK(near(s.yawDeg, 348.0));
    mdk::RawGameplayInput idle;
    const mdk::GameplayInputFrame f2 =
        mdk::consumeGameplayInput(idle, bind, genv, gst);
    o = mdk::integratePlayerMotion(f2, env, s);
    CHECK(near(s.turnVel, 10.4));             // beyond bound: -1.6
    CHECK(near(s.yawDeg, 337.6, 1e-4));
  }

  // ---- one-frame input ordering --------------------------------------
  {
    // The integrator consumes the PREVIOUS frame's control block —
    // the original runs FUN_00465228 before FUN_00406f14.
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::GameplayInputFrame prev{};            // zero block pre-merge
    mdk::RawGameplayInput r;
    hold(r, 103);
    // Frame 0: input arrives but motion still sees the zero block.
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(prev, env, s);
    CHECK(s.moveVel == 0.0f && o.dispX == 0.0f);
    prev = mdk::consumeGameplayInput(r, bind, genv, gst);
    // Frame 1: now the held key drives the channel.
    o = mdk::integratePlayerMotion(prev, env, s);
    CHECK(near(s.moveVel, kMoveRate) && near(o.dispX, kMoveRate));
  }

  // ---- event quirk: residual strafe + fresh turn -> turn wins -------
  {
    mdk::PlayerMotionState s;
    mdk::GameplayInputFrame fs;
    fs.strafeNorm = -4.0f / 45.0f;             // turbo strafe input
    fs.strafeFast = -4.0f / 3.0f;
    mdk::PlayerMotionOutput o;
    for (int i = 0; i < 20; ++i)               // charge to the -4/3 cap
      o = mdk::integratePlayerMotion(fs, env, s);
    CHECK(near(s.strafeVel, -4.0 / 3.0, 1e-4));
    // Strafe input gone: channel residual emits 5/500, then the fresh
    // turn overwrites (no strafe INPUT this frame -> bit1 clear).
    mdk::GameplayInputFrame ft;
    ft.turnNorm = -0.9f;
    ft.turnFast = -4.0f;
    o = mdk::integratePlayerMotion(ft, env, s);
    CHECK(near(s.strafeVel, -4.0 / 3.0 + 0.75 * (8.0 / 45.0), 1e-4));
    CHECK(o.eventType == 4 && o.eventMag == 400);  // overwritten
    // With strafe input live the event stays strafe (bit1 suppresses).
    s = {};
    for (int i = 0; i < 20; ++i)
      o = mdk::integratePlayerMotion(fs, env, s);
    mdk::GameplayInputFrame both = fs;
    both.turnNorm = -0.9f;
    both.turnFast = -4.0f;
    o = mdk::integratePlayerMotion(both, env, s);
    CHECK(o.eventType == 5 && o.eventMag == 500);
  }

  // ---- gates: turnLock / moveBlocked / masterGate / conveyor --------
  {
    mdk::PlayerMotionState s;
    s.turnLock = 1;
    mdk::GameplayInputFrame f;
    f.turnNorm = -0.9f;
    f.turnFast = -4.0f;
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
    CHECK(s.turnVel == 0.0f && s.turnLock == 1);  // persists on < 0
    f.turnNorm = 0.9f;
    f.turnFast = 4.0f;
    o = mdk::integratePlayerMotion(f, env, s);
    CHECK(s.turnVel == 0.0f && s.turnLock == 0);  // releases, still no
    o = mdk::integratePlayerMotion(f, env, s);
    CHECK(near(s.turnVel, 0.9));                  // now applies
    // Lock 2 mirrors for positive input.
    s = {};
    s.turnLock = 2;
    o = mdk::integratePlayerMotion(f, env, s);
    CHECK(s.turnVel == 0.0f && s.turnLock == 2);
    s.turnLock = 3;                               // out-of-range resets
    o = mdk::integratePlayerMotion(f, env, s);
    CHECK(s.turnLock == 0);
    // moveBlocked skips the move input (decay still runs).
    s = {};
    mdk::PlayerMotionEnvironment be = env;
    be.moveBlocked = true;
    mdk::GameplayInputFrame fm;
    fm.moveVel = 1.0f / 22.5f;
    fm.moveVelBoosted = 2.0f / 3.0f;
    o = mdk::integratePlayerMotion(fm, be, s);
    CHECK(s.moveVel == 0.0f && !o.moveConsumed && !o.forwardIntent);
    // masterGate: immediate RET — nothing runs.
    be = env;
    be.masterGate = true;
    s = {};
    s.bank = 1.0f;
    o = mdk::integratePlayerMotion(fm, be, s);
    CHECK(!o.ran && s.moveVel == 0.0f && o.eventType == 0);
    mdk::playerMotionPostStep(be, true, s, o);   // tail decay still runs
    CHECK(near(s.bank, 1.0 - 0.35));
    // conveyor applies only on ground contact.
    s = {};
    mdk::PlayerMotionEnvironment ce = env;
    ce.groundContact = true;
    ce.conveyorX = 0.5f;
    ce.conveyorZ = 0.25f;
    o = mdk::integratePlayerMotion(fm, ce, s);
    CHECK(near(o.dispX, 0.5 + kMoveRate) && near(o.dispZ, 0.25));
    s = {};
    ce.groundContact = false;
    o = mdk::integratePlayerMotion(fm, ce, s);
    CHECK(near(o.dispX, kMoveRate) && o.dispZ == 0.0f);
    // Ground-contact accel scales: contact 1.0, low-friction 0.5.
    s = {};
    mdk::GameplayInputFrame fst;
    fst.strafeNorm = -1.0f / 22.5f;
    fst.strafeFast = -2.0f / 3.0f;
    mdk::PlayerMotionEnvironment ge = env;
    ge.groundContact = true;
    o = mdk::integratePlayerMotion(fst, ge, s);
    CHECK(near(s.strafeVel, -kMoveRate));
    s = {};
    ge.lowFriction = true;
    o = mdk::integratePlayerMotion(fst, ge, s);
    CHECK(near(s.strafeVel, -kMoveRate * 0.5));
  }

  // ---- post rules: event cancel + air-charge drain ------------------
  {
    mdk::PlayerMotionState s;
    mdk::GameplayInputFrame f;
    f.moveVel = 1.0f / 22.5f;
    f.moveVelBoosted = 2.0f / 3.0f;
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
    CHECK(o.eventMag == 600);
    mdk::playerMotionPostStep(env, false, s, o);  // position unchanged
    CHECK(o.eventMag == 0 && o.eventType == 0);
    o = mdk::integratePlayerMotion(f, env, s);
    mdk::playerMotionPostStep(env, true, s, o);
    CHECK(o.eventMag == 600);
    // air-charge drain: only while moving forward, floor 20, cap 60.
    s = {};
    s.airCharge = 30.0f;
    o = mdk::integratePlayerMotion(f, env, s);
    mdk::playerMotionPostStep(env, true, s, o);
    CHECK(near(s.airCharge, 28.25));
    s.airCharge = 100.0f;
    o = mdk::integratePlayerMotion(f, env, s);
    mdk::playerMotionPostStep(env, true, s, o);
    CHECK(near(s.airCharge, 58.25));              // 100->60->58.25
    s.airCharge = 20.0f;
    o = mdk::integratePlayerMotion(f, env, s);
    mdk::playerMotionPostStep(env, true, s, o);
    CHECK(s.airCharge == 20.0f);                  // floor: no drain
    // Backward motion does not drain.
    mdk::GameplayInputFrame fb;
    fb.moveVel = -1.0f / 22.5f;
    fb.moveVelBoosted = -2.0f / 3.0f;
    s = {};
    s.airCharge = 40.0f;
    o = mdk::integratePlayerMotion(fb, env, s);
    mdk::playerMotionPostStep(env, true, s, o);
    CHECK(s.airCharge == 40.0f);
  }

  // ---- settings-derived bindings -> motion ---------------------------
  {
    // Custom key + custom mouse map reach the channels through the
    // real settings->bindings->consume seam (no bypass).
    mdk::FrontendSettings fs;
    fs.keyUp = 17;                               // 'W' drives KeyUp
    fs.mouseWAxesMap = "C0G";                    // dx -> strafe
    fs.mouseWXScale = 8.0f;
    const mdk::GameplayInputBindings b2 =
        mdk::gameplayBindingsFromSettings(fs);
    mdk::GameplayInputState gst;
    mdk::PlayerMotionState s;
    mdk::RawGameplayInput r;
    hold(r, 17);                                 // 'W'
    const mdk::GameplayInputFrame f =
        mdk::consumeGameplayInput(r, b2, genv, gst);
    CHECK(near(f.moveVel, kMoveRate));
    mdk::PlayerMotionOutput o = mdk::integratePlayerMotion(f, env, s);
    CHECK(near(s.moveVel, kMoveRate) && near(o.dispX, kMoveRate));
    mdk::RawGameplayInput rm;
    rm.mouseDx = 160;                            // 160/8 = 20 -> clamp 4
    const mdk::GameplayInputFrame fm =
        mdk::consumeGameplayInput(rm, b2, genv, gst);
    CHECK(near(fm.strafeFast, 4.0 * (4.0 / 3.0) * 0.5));
    s = {};
    o = mdk::integratePlayerMotion(fm, env, s);
    // Airborne accel scale 0.75: vel += strafeNorm*0.75*f0.
    CHECK(near(s.strafeVel, fm.strafeNorm * 0.75f, 1e-3));
  }
}

// Phase 5C — FUN_00466740 jump-state machine + FUN_00467180
// gravity/vertical integration, bounded by the semantic
// FUN_004630d4 collision seam. The scripted world is a flat floor
// at z=10 (contact iff appliedZ reaches floorZ+0.05).
void test_player_vertical() {
  constexpr float kF4 = 1.0f / 30.0f;          // 0.033333335
  const double kGrav = 2.133333333333333;      // rise-loop step
  const double kGravS = 0.7111111111111111;    // sustain step
  const double kReb = 8.533333333333333;       // sustain rebound

  mdk::PlayerVerticalEnvironment env;
  env.deepFloorZ = -1000.0f;
  auto idleState = [&](mdk::PlayerVerticalState& vs) {
    vs.contactFlags = 0x3;         // grounded + floor probe valid
    vs.posZ = 10.05f;
    vs.floorZ = 10.0f;
    vs.contactObj = 1;
  };
  // Flat-floor collision stub (deterministic seam).
  auto collideFloor = [](mdk::PlayerVerticalState& vs, float dispZ,
                         mdk::VerticalCollisionResult& res) {
    res.posX = vs.posX;
    res.posY = vs.posY;
    res.posZ = vs.posZ + dispZ;
    res.hasFloor = true;
    res.floorZ = 10.0f;
    res.contactObj = res.posZ <= 10.05f ? 1u : 0u;
    res.normalZ = res.contactObj != 0 ? 1.0f : 0.0f;
  };
  auto frame = [&](mdk::PlayerVerticalEnvironment& e,
                   mdk::PlayerMotionState& ms,
                   mdk::PlayerVerticalState& vs,
                   const mdk::VerticalCollisionResult& res) {
    mdk::PlayerVerticalFrame f =
        mdk::integratePlayerVertical(e, ms, vs);
    if (f.collisionIssued)
      mdk::applyPlayerVerticalCollision(e, ms, vs, res, f);
    mdk::playerVerticalPostStep(e, vs);
    return f;
  };
  auto floorFrame = [&](mdk::PlayerVerticalEnvironment& e,
                        mdk::PlayerMotionState& ms,
                        mdk::PlayerVerticalState& vs) {
    mdk::PlayerVerticalFrame f =
        mdk::integratePlayerVertical(e, ms, vs);
    if (f.collisionIssued) {
      mdk::VerticalCollisionResult res;
      collideFloor(vs, f.dispZ, res);
      mdk::applyPlayerVerticalCollision(e, ms, vs, res, f);
    }
    mdk::playerVerticalPostStep(e, vs);
    return f;
  };

  // ---- grounded idle: gravity runs, pre-land clamps, lands --------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(f.collisionIssued && f.preLand);
    CHECK(near(f.dispZ, 0.0, 1e-4));   // clamped to the epsilon hover
    CHECK(f.landed && !f.ceilingHit && !f.hardLanding);
    CHECK(vs.vertVel == 0.0f);
    CHECK((vs.contactFlags & 0x1) != 0);          // grounded kept
    CHECK(near(vs.posZ, 10.05));
    CHECK(ms.airCharge == 0.0f && vs.jumpSustain == 0);
    // Stable across frames.
    f = floorFrame(env, ms, vs);
    CHECK(f.landed && vs.vertVel == 0.0f && near(vs.posZ, 10.05));
  }

  // ---- jump rejected gates -----------------------------------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    env.jumpHeld = true;
    // Airborne (no grounded bit) -> no jump.
    vs.contactFlags = 0x2;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(!f.jumped && vs.vertVel <= 0.0f);
    // Rising velocity -> no jump.
    idleState(vs);
    vs.vertVel = 3.0f;
    f = floorFrame(env, ms, vs);
    CHECK(!f.jumped);
    // Event channel busy -> no jump.
    idleState(vs);
    vs.vertVel = 0.0f;
    vs.eventIdle = 7;
    f = floorFrame(env, ms, vs);
    CHECK(!f.jumped);
    vs.eventIdle = 0;
    mdk::PlayerVerticalEnvironment e2 = env;
    e2.eventWordType = 8;
    f = floorFrame(e2, ms, vs);
    CHECK(!f.jumped);
    // Held latch -> no re-jump until release.
    idleState(vs);
    vs.vertVel = 0.0f;
    vs.jumpLatch = 1;
    f = floorFrame(env, ms, vs);
    CHECK(!f.jumped && vs.jumpLatch == 1);
    // Release while grounded re-arms the latch.
    mdk::PlayerVerticalEnvironment e3 = env;
    e3.jumpHeld = false;
    f = floorFrame(e3, ms, vs);
    CHECK(!f.jumped && vs.jumpLatch == 0);
    env.jumpHeld = false;
  }

  // ---- jump start: impulse 40, charge 6, event picks --------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    env.jumpHeld = true;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(f.jumped && f.eventType == 7 && f.eventMag == 0x2be);
    CHECK(vs.jumpActive == 1 && vs.jumpHoldCharge == 6 &&
          vs.jumpLatch == 1 && vs.jumpAux == 1);
    CHECK(near(vs.vertVel, 40.0 - kGrav, 1e-3));    // 37.8667
    CHECK(near(f.dispZ, (40.0 - kGrav) * kF4, 1e-3));
    CHECK(!f.landed);                              // rising, no contact
    // Moving jump picks 0x2bf.
    vs = mdk::PlayerVerticalState{};
    idleState(vs);
    env.moveConsumed = true;
    f = floorFrame(env, ms, vs);
    CHECK(f.jumped && f.eventMag == 0x2bf);
    env.moveConsumed = false;
    env.jumpHeld = false;
  }

  // ---- hold: charge drains 6..0 over six held frames --------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    env.jumpHeld = true;
    env.locoState = 0x2be;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(f.jumped && vs.jumpHoldCharge == 6);     // start frame: no drain
    int charges[6];
    for (int i = 0; i < 6; ++i) {
      f = floorFrame(env, ms, vs);
      charges[i] = vs.jumpHoldCharge;
    }
    CHECK(charges[0] == 5 && charges[5] == 0);
    // Late release (charge empty): no cut.
    env.jumpHeld = false;
    const float vBefore = vs.vertVel;
    f = floorFrame(env, ms, vs);
    CHECK(near(vs.vertVel, (double)vBefore - kGrav, 1e-3));
    env.jumpHeld = false;
    env.locoState = 0;
  }

  // ---- early release cuts the rise by charge*20/6 -----------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    env.jumpHeld = true;
    env.locoState = 0x2be;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(f.jumped);
    env.jumpHeld = false;
    // Charge 6 -> cut 20.0: 37.867 - 20 = 17.867, then gravity.
    f = floorFrame(env, ms, vs);
    CHECK(near(vs.vertVel, 40.0 - kGrav - 20.0 - kGrav, 1e-3));
    CHECK(vs.jumpHoldCharge == 0 && vs.jumpAux == 0);
    env.locoState = 0;
  }

  // ---- fall: air-charge seeds at c78 < -16, sustain engages -------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    vs.contactFlags = 0x2;                  // airborne, high up
    vs.posZ = 100.0f;
    vs.vertVel = -17.0f;
    vs.contactObj = 0;
    env.jumpHeld = true;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    // Seeded this frame -> sustain engages the same frame.
    CHECK(near(ms.airCharge, 1.0));
    CHECK(vs.jumpSustain == 1 && f.sustain);
    CHECK(f.eventType == 7 && f.eventMag == 0x2bd);
    // Sustain gravity + rebound: -17 - 0.711 = -17.71 < -8
    //   -> +8.533 = -9.18 (still < -8, no pin).
    CHECK(near(vs.vertVel, -17.0 - kGravS + kReb, 1e-2));
    // Next frame: charge accumulates; rebound converges to -8.
    f = floorFrame(env, ms, vs);
    CHECK(near(ms.airCharge, 2.0));
    CHECK(near(vs.vertVel, -8.0, 0.01));
    // Terminal: pinned at -8 while sustain holds (the realized-
    // velocity recompute adds sub-epsilon noise).
    f = floorFrame(env, ms, vs);
    CHECK(near(vs.vertVel, -8.0, 0.01));
    env.jumpHeld = false;
  }

  // ---- normal terminal fall clamps at -250 -------------------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    vs.vertVel = -249.0f;
    // Stationary result: isolates the integrator clamp.
    mdk::VerticalCollisionResult res;
    mdk::PlayerVerticalFrame f = frame(env, ms, vs, res);
    CHECK(vs.vertVel == -250.0f);
    f = frame(env, ms, vs, res);
    CHECK(vs.vertVel == -250.0f);            // pinned
    // Fall path always runs exactly one f4 step even at frameStep>1.
    vs.vertVel = -10.0f;
    env.frameStep = 4;
    f = frame(env, ms, vs, res);
    CHECK(near(vs.vertVel, -10.0 - kGrav, 1e-3));
    env.frameStep = 1;
  }

  // ---- apex: no state, velocity crosses zero under gravity ---------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    vs.contactFlags = 0x2;
    vs.posZ = 30.0f;                        // above the floor band
    vs.vertVel = 1.0f;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    // Rise loop still ran (entry > 0): exits negative — already
    // descending within the same frame. No apex state exists.
    CHECK(near(vs.vertVel, 1.0 - kGrav, 1e-3));
    CHECK(f.dispZ < 0.0f);
  }

  // ---- release-while-airborne: fall event + latch held -------------
  {
    mdk::PlayerMotionState ms;
    ms.airCharge = 5.0f;
    mdk::PlayerVerticalState vs;
    vs.contactFlags = 0x2;
    vs.posZ = 100.0f;
    vs.vertVel = -20.0f;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(f.eventType == 7 && f.eventMag == 700);
    CHECK(vs.jumpLatch == 1 && vs.jumpAux == 0 && vs.jumpSustain == 0);
    // With bounce set: same event, no latch/aux writes.
    vs.jumpLatch = 0;
    vs.jumpAux = 1;
    vs.bounceFlag = 1;
    env.jumpHeld = true;
    f = floorFrame(env, ms, vs);
    CHECK(f.eventMag == 700 && vs.jumpLatch == 0 && vs.jumpAux == 1);
    CHECK(vs.bounceFlag == 0);               // cleared by the tail
    env.jumpHeld = false;
  }

  // ---- hard landing: event 806 vs soft / bounce / silenced ---------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    vs.contactFlags = 0x2;                  // falling
    vs.contactObj = 0;
    vs.vertVel = -120.0f;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(f.landed && f.hardLanding);
    CHECK(f.eventType == 8 && f.eventMag == 806);
    CHECK(vs.vertVel == 0.0f && ms.airCharge == 0.0f &&
          (vs.contactFlags & 0x1) != 0);
    CHECK(vs.eventIdle == 0 && vs.landingAccum == 0.0f);
    // Bounce suppresses the event, keeps the anti-jitter path. The
    // event word is left at the seeded 700 fall event (c84 seeded
    // on the way down).
    vs = mdk::PlayerVerticalState{};
    idleState(vs);
    vs.contactFlags = 0x2;
    vs.vertVel = -120.0f;
    vs.bounceFlag = 1;
    f = floorFrame(env, ms, vs);
    CHECK(f.landed && !f.hardLanding && f.eventMag == 700);
    // e6c + e72 bit1 silences the event (no anti-jitter either).
    vs = mdk::PlayerVerticalState{};
    idleState(vs);
    vs.contactFlags = 0x2;
    vs.vertVel = -120.0f;
    mdk::PlayerVerticalEnvironment e2 = env;
    e2.sharedGateE6C = true;
    e2.flagE72bit1 = true;
    f = floorFrame(e2, ms, vs);
    CHECK(f.landed && !f.hardLanding && f.eventMag == 700);
    // e6c without e72 bit1 still emits.
    vs = mdk::PlayerVerticalState{};
    idleState(vs);
    vs.contactFlags = 0x2;
    vs.vertVel = -120.0f;
    e2.flagE72bit1 = false;
    f = floorFrame(e2, ms, vs);
    CHECK(f.hardLanding && f.eventMag == 806);
  }

  // ---- ceiling: upward contact zeroes velocity, no landing ---------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    vs.contactFlags = 0x2;
    vs.vertVel = 5.0f;
    mdk::VerticalCollisionResult res;
    res.contactObj = 7;
    res.posZ = 20.0f;
    res.normalZ = -1.0f;
    mdk::PlayerVerticalFrame f = frame(env, ms, vs, res);
    CHECK(f.ceilingHit && !f.landed);
    CHECK(vs.vertVel == 0.0f && vs.contactObj == 0 &&
          (vs.contactFlags & 0x1) == 0);
  }

  // ---- no-contact fall: realized velocity recompute ----------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    vs.contactFlags = 0x2;                  // airborne, no floor probe
    vs.posZ = 50.0f;
    vs.vertVel = -30.0f;
    // Stub clamps the applied Z mid-move (invisible blocker).
    mdk::VerticalCollisionResult res;
    res.posZ = 49.5f;                        // moved less than requested
    res.hasFloor = false;
    mdk::PlayerVerticalFrame f = frame(env, ms, vs, res);
    CHECK(!f.landed && f.realizedVelocity);
    CHECK(near(vs.vertVel, (49.5 - 50.0) / kF4, 1e-2));  // -15
    // Moving up or stationary: velocity kept.
    vs.vertVel = -30.0f;
    res.posZ = 51.0f;
    f = frame(env, ms, vs, res);
    CHECK(near(vs.vertVel, -30.0 - kGrav, 1e-3));
  }

  // ---- pre-land-no-contact: blocker refresh + floor snap -----------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    vs.vertVel = 0.0f;
    mdk::VerticalCollisionResult res;
    res.posZ = 10.05f;
    res.hasFloor = true;
    res.floorZ = 10.0f;
    res.blocker0 = 0xaa;
    res.blocker1 = 0xbb;
    res.blocker0Flag80 = true;
    // contactObj == 0 despite the clamp -> blocker copy + snap.
    mdk::PlayerVerticalFrame f = frame(env, ms, vs, res);
    CHECK(f.landed && f.blockerRefresh && !f.ceilingHit);
    CHECK(vs.moveBlocker0 == 0xaa && vs.moveBlocker1 == 0xbb &&
          vs.moveBlockerFlag == 1);
    CHECK(near(vs.posZ, 10.0));              // hard snap to floorZ
    // Next landing without the flag releases the blocker.
    f = frame(env, ms, vs, res);
    res.blocker0Flag80 = false;
    f = frame(env, ms, vs, res);
    CHECK(f.blockerReleased && vs.moveBlockerFlag == 0);
  }

  // ---- ribbon volume: forced sustain, drain floor 1.0, disp re-do --
  {
    mdk::PlayerMotionState ms;
    ms.airCharge = 10.0f;
    mdk::PlayerVerticalState vs;
    vs.contactFlags = 0x2;
    vs.posZ = 100.0f;
    vs.vertVel = 10.0f;
    mdk::PlayerVerticalEnvironment e2 = env;
    e2.insideRibbonVolume = true;
    e2.frameStep = 3;
    e2.locoState = 0x2bd;               // keeps c84 while rising
    // Frame: accumulate c84 -> 11; release latch (not held); rise
    // loop runs NORMAL gravity (sustain was 0): 3 substeps ->
    // 10 - 3*2.1333 = 3.6. The volume then forces sustain, drains
    // c84 to 9.25, and REDOES the displacement single-step.
    mdk::PlayerVerticalFrame f = floorFrame(e2, ms, vs);
    CHECK(f.inRibbonVolume && vs.jumpSustain == 1);
    CHECK(f.eventType == 7 && f.eventMag == 0x2bd);
    CHECK(near(ms.airCharge, 11.0 - 1.75, 1e-3));
    CHECK(near(vs.vertVel, 10.0 - 3.0 * kGrav, 1e-3));
    CHECK(near(f.dispZ, (10.0 - 3.0 * kGrav) * kF4, 1e-3));
    // Bit-pattern floor: drain below 1.0 -> exactly 1.0f.
    ms.airCharge = 1.2f;
    f = floorFrame(e2, ms, vs);
    CHECK(ms.airCharge == 1.0f);
  }

  // ---- rise cap: c78 > 40 clamped (cac < 800, e6c clear) -----------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    vs.contactFlags = 0x2;
    vs.posZ = 100.0f;
    vs.vertVel = 60.0f;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(near(vs.vertVel, 40.0));
    CHECK(near(f.dispZ, 40.0 * kF4, 1e-3));
    // cac >= 800 suppresses the cap.
    vs.vertVel = 60.0f;
    env.locoState = 800;
    f = floorFrame(env, ms, vs);
    CHECK(near(vs.vertVel, 60.0 - kGrav, 1e-3));
    env.locoState = 0;
  }

  // ---- gates: c6c master, c7c skip, e24 slide ----------------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    env.vertEnable = false;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(!f.collisionIssued && !f.landed);
    env.vertEnable = true;
    vs.vertSkip = 2;                         // mantle in progress
    f = floorFrame(env, ms, vs);
    CHECK(!f.collisionIssued);
    vs.vertSkip = 0;
    // Slide mode: jump machine skipped (no jump even grounded+held),
    // integrator still runs single-step, e28 NOT cleared.
    env.slideMode = true;
    env.jumpHeld = true;
    vs.bounceFlag = 1;
    vs.vertVel = -20.0f;
    f = floorFrame(env, ms, vs);
    CHECK(!f.jumped && f.collisionIssued);
    CHECK(vs.bounceFlag == 1);               // tail skipped
    env.slideMode = false;
    env.jumpHeld = false;
    vs.bounceFlag = 0;
  }

  // ---- deep-floor failsafe ------------------------------------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    vs.contactFlags = 0x0;                  // no floor probe -> no clamp
    vs.posZ = -60.0f;
    vs.vertVel = -40.0f;
    vs.fallCounter = 9;
    env.deepFloorZ = -10.0f;                 // -60 <= -10 - 50
    mdk::VerticalCollisionResult res;
    res.posZ = -60.0f;
    mdk::PlayerVerticalFrame f = frame(env, ms, vs, res);
    CHECK(f.deepFloorReset);
    CHECK(vs.vertVel == 0.0f && vs.fallCounter == 0 &&
          (vs.contactFlags & 0x1) != 0);
    env.deepFloorZ = -1000.0f;
  }

  // ---- slope assist (in_EAX vector) ---------------------------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    vs.contactNormal[0] = 0.0f;
    vs.contactNormal[1] = 0.5f;
    vs.contactNormal[2] = 0.9f;
    const float vec[2] = {1.0f, 0.5f};
    mdk::PlayerVerticalEnvironment e2 = env;
    e2.slideVec = vec;
    e2.vertEnable = false;                 // keep the assist readable
    mdk::PlayerVerticalFrame f = frame(e2, ms, vs,
                                     mdk::VerticalCollisionResult{});
    // dot = 0.25 -> vertVel = min(0, -0.25/f4) = -7.5
    CHECK(near(vs.vertVel, -7.5, 1e-2));
    (void)f;
  }

  // ---- complete golden sequence ------------------------------------
  {
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    idleState(vs);
    // f1: jump pressed (previous frame's merged flag).
    env.jumpHeld = true;
    mdk::PlayerVerticalFrame f = floorFrame(env, ms, vs);
    CHECK(f.jumped && f.eventMag == 0x2be);
    env.locoState = 0x2be;                   // dispatched
    // Rising: decelerate 2.1333/frame; apex crossed inside the loop.
    for (int i = 0; i < 18; ++i) f = floorFrame(env, ms, vs);
    CHECK(vs.vertVel < 0.0f && vs.vertVel > -3.0f);  // ~-0.53
    // Falling: accelerate -2.1333/frame (single-step path).
    f = floorFrame(env, ms, vs);
    CHECK(near(vs.vertVel, -2.6667, 1e-3));
    for (int i = 0; i < 7; ++i) f = floorFrame(env, ms, vs);
    CHECK(vs.vertVel < -16.0f);              // crossed the seed
    // Seed + sustain engage next frame (still held).
    f = floorFrame(env, ms, vs);
    CHECK(ms.airCharge > 0.0f);
    CHECK(vs.jumpSustain == 1 && f.eventMag == 0x2bd);
    env.locoState = 0x2bd;
    // Rebound brakes the fall to -8 within two frames (the
    // realized-velocity recompute adds sub-epsilon noise).
    f = floorFrame(env, ms, vs);
    CHECK(near(vs.vertVel, -8.0, 0.01));
    f = floorFrame(env, ms, vs);
    CHECK(near(vs.vertVel, -8.0, 0.01));
    // Float down at -8 until the floor (pre-land clamps -> soft land).
    int guard = 0;
    while (!f.landed && guard++ < 80) f = floorFrame(env, ms, vs);
    CHECK(f.landed && !f.hardLanding);       // -8 >= -100 -> soft
    CHECK(vs.vertVel == 0.0f && (vs.contactFlags & 0x1) != 0);
    // OBSERVED quirk: a SOFT landing does not clear c84 — the
    // grounded branch of next frame's update resets it instead.
    CHECK(ms.airCharge > 0.0f);
    f = floorFrame(env, ms, vs);
    CHECK(ms.airCharge == 0.0f && vs.jumpSustain == 0);
    // Still holding: no re-jump (the c90 latch).
    CHECK(!f.jumped);
    env.jumpHeld = false;
    env.locoState = 0;
  }

  // ---- c80 -> 5A moveBoostGate -> 5B move channel coupling ---------
  {
    // Prove the seam end-to-end: vertical sets c80 -> the next
    // consumeGameplayInput environment reports the gate -> 5B uses
    // the resulting control block. moveBoostGate suppresses the x4/3
    // lift in the turbo move branch, so the boosted cap differs.
    mdk::GameplayInputState gst;
    mdk::GameplayInputEnvironment ge;
    ge.moveBoostGate = 0;
    mdk::GameplayInputBindings bind;
    mdk::RawGameplayInput r;
    r.keyLevel[3] |= 1u << (103 & 31);       // KeyUp held
    r.keyLevel[1] |= 1u << (42 & 31);        // KeyTurbo held
    mdk::GameplayInputFrame f0 =
        mdk::consumeGameplayInput(r, bind, ge, gst);
    gst = {};
    ge.moveBoostGate = 1;                    // as vs.jumpSustain feeds
    mdk::GameplayInputFrame f1 =
        mdk::consumeGameplayInput(r, bind, ge, gst);
    CHECK(f0.moveVel != 0.0f && f1.moveVel != 0.0f);
    CHECK(f1.moveVelBoosted != f0.moveVelBoosted);
    CHECK(near(f0.moveVelBoosted, 4.0 / 3.0));
    CHECK(near(f1.moveVelBoosted, 1.0));
    mdk::PlayerMotionState ms;
    mdk::PlayerMotionEnvironment me;
    mdk::PlayerMotionOutput o =
        mdk::integratePlayerMotion(f1, me, ms);
    // First-frame increment equals the unboosted rate; the gate only
    // lifts the cap. The ORIGINAL coupling is that the boosted cap
    // (5A) changes with c80 (5C) — proved above.
    CHECK(near(ms.moveVel, f1.moveVel));
    CHECK(o.moveConsumed);
  }
}

} // namespace

// Phase 5D — FUN_004630d4 swept collision query + FUN_00435eec
// floor probe, exercised on synthetic runtime geometry built with the
// proven original record layouts (BSP node 0x2c, poly record 0x24,
// float3 verts, element/object fields at the probed offsets).
namespace {

mdk::CollisionPoly makePoly(std::uint16_t a, std::uint16_t b,
                            std::uint16_t c) {
  mdk::CollisionPoly p = {};
  p.v[0] = a;
  p.v[1] = b;
  p.v[2] = c;
  return p;
}

mdk::CollisionNode makeNode(float nx, float ny, float nz, float d,
                            std::uint32_t posSet, std::uint32_t negSet,
                            std::int16_t near_, std::int16_t far_) {
  mdk::CollisionNode n = {};
  n.nx = nx;
  n.ny = ny;
  n.nz = nz;
  n.d = d;
  n.polysPos = posSet;
  n.polysNeg = negSet;
  n.childNear = near_;
  n.childFar = far_;
  return n;
}

// {lo16 count, hi16 firstIdx} polygon-set dword.
constexpr std::uint32_t polySet(std::uint32_t count, std::uint32_t first) {
  return (first << 16) | count;
}

struct CollisionFixture {
  std::vector<float> verts;
  std::vector<mdk::CollisionPoly> polys;
  std::vector<mdk::CollisionNode> nodes;
  mdk::CollisionArena arena = {};
  void finish() {
    arena.verts = verts.data();
    arena.polys = polys.data();
    arena.nodes = nodes.data();
  }
};

// Flat floor at z=10 (plane +z facing up, approached from above).
CollisionFixture makeFloorArena() {
  CollisionFixture f;
  f.verts = {-50, -50, 10, 50, -50, 10, 50, 50, 10};
  f.polys = {makePoly(0, 1, 2)};
  f.nodes = {makeNode(0, 0, 1, -10, polySet(1, 0), 0, -1, -1)};
  f.arena.deepFloorZ = -1000.0f;
  f.finish();
  return f;
}

// Vertical wall at x=5, normal -x facing the room (player at x<5).
// Two tris tile the quad — the leaf scan accepts the first overlap.
CollisionFixture makeWallArena() {
  CollisionFixture f;
  f.verts = {5, 50, 50,  5, -50, 50, 5, 50, -50,
             5, -50, -50, 5, 50, -50, 5, -50, 50};
  f.polys = {makePoly(0, 1, 2), makePoly(3, 4, 5)};
  f.nodes = {makeNode(-1, 0, 0, 5, polySet(2, 0), 0, -1, -1)};
  f.finish();
  return f;
}

// Ceiling at z=20, normal -z facing down.
CollisionFixture makeCeilArena() {
  CollisionFixture f;
  f.verts = {-50, -50, 20, 50, -50, 20, 50, 50, 20};
  f.polys = {makePoly(0, 1, 2)};
  f.nodes = {makeNode(0, 0, -1, 20, polySet(1, 0), 0, -1, -1)};
  f.finish();
  return f;
}

// Empty space: node present but no polygons anywhere.
CollisionFixture makeEmptyArena() {
  CollisionFixture f;
  f.nodes = {makeNode(0, 0, 1, -10, 0, 0, -1, -1)};
  f.finish();
  return f;
}

struct ObjectFixture {
  std::vector<float> elemVerts;
  std::array<std::uint8_t, 0x24> triRec = {};
  mdk::CollisionElement elem = {};
  mdk::CollisionElementSet elemSet = {};
  mdk::CollisionObject obj = {};
  int modelDummy = 0;
};

// Rideable/probe-able object: one element carrying a flat tri at
// local z, identity transform, origin (0,0,0). Filled in place — the
// intra-struct pointers (elements, model) must not be moved after.
void initFloorObject(ObjectFixture& o, float z) {
  o.elemVerts = {-50, -50, z, 50, -50, z, 50, 50, z};
  auto* idx = reinterpret_cast<std::uint16_t*>(o.triRec.data());
  idx[0] = 0;
  idx[1] = 1;
  idx[2] = 2;
  o.elem.triCount = 1;
  o.elem.verts = o.elemVerts.data();
  o.elem.tris = o.triRec.data();
  const float zmin = z - 0.5f, zmax = z + 0.5f;
  float eb[6] = {-60, -60, zmin, 60, 60, zmax};
  std::memcpy(o.elem.aabb, eb, sizeof(eb));
  o.elemSet.count = 1;
  o.elemSet.elems = &o.elem;
  o.obj.next = nullptr;
  o.obj.named = true;
  o.obj.model = &o.modelDummy;
  o.obj.elements = &o.elemSet;
  o.obj.baseZ = z;
  o.obj.flags148 = 0;
  o.obj.flags149 = 1;
  o.obj.flags14a = 0;
  float ab[6] = {-60, -60, zmin, 60, 60, zmax};
  std::memcpy(o.obj.aabb, ab, sizeof(ab));
  const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::memcpy(o.obj.xform, ident, sizeof(ident));
  o.obj.origin[0] = o.obj.origin[1] = o.obj.origin[2] = 0.0f;
  o.obj.scale = 1.0f;
  o.obj.elemMaskB = 0;
  o.obj.elemMaskA = 0;
}

mdk::CollisionState makeCollisionState(const mdk::CollisionArena* arena) {
  mdk::CollisionState cs;
  cs.arena = arena;
  cs.queryEnabled = 1;
  cs.arenaValid = 1;
  cs.objectDataLoaded = 1;
  return cs;
}

// Adapt the collision layer to the semantic VerticalCollisionResult
// the 5C seam consumes (token = truncated object pointer; the vertical
// layer never dereferences it).
mdk::VerticalCollisionResult adaptCollision(
    const mdk::CollisionState& cs, const mdk::CollisionPoly* contact,
    const mdk::CollisionNode* node) {
  mdk::VerticalCollisionResult res;
  res.contactObj =
      (std::uint32_t)(uintptr_t)contact;
  res.posX = cs.pos[0];
  res.posY = cs.pos[1];
  res.posZ = cs.pos[2];
  if (node) {
    res.normalX = node->nx;
    res.normalY = node->ny;
    res.normalZ = node->nz;
  }
  res.hasFloor = (cs.contactFlags & 0x2) != 0;
  res.floorZ = cs.floorZ;
  res.blocker0 = (std::uint32_t)(uintptr_t)cs.floorObj;
  res.blocker1 = cs.floorElemMask;
  res.blocker0Flag80 =
      cs.floorObj != nullptr && (cs.floorObj->flags14a & 0x80) != 0;
  return res;
}

void test_player_collision() {
  // ---- empty space: full displacement, no contact -----------------
  {
    CollisionFixture f = makeEmptyArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 0;
    cs.pos[1] = 0;
    cs.pos[2] = 12;
    const mdk::CollisionNode* node = &f.nodes[0];
    const mdk::CollisionPoly* hit =
        mdk::collisionApply(cs, 1.0f, 2.0f, 0.0f, 0.75f, nullptr, &node);
    CHECK(hit == nullptr && node == nullptr);
    CHECK(near(cs.pos[0], 1.0) && near(cs.pos[1], 2.0) &&
          near(cs.pos[2], 12.0));
    // Player AABB refreshed around the pre-move lifted position.
    CHECK(near(cs.playerBox[0], -0.6) && near(cs.playerBox[3], 0.6));
    CHECK(near(cs.playerBox[1], -0.6) && near(cs.playerBox[4], 0.6));
    CHECK(near(cs.playerBox[2], 12.5) && near(cs.playerBox[5], 17.5));
  }

  // ---- flat floor: vertical contact resolves feet to F - 0.01 -----
  {
    CollisionFixture f = makeFloorArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 12;
    const mdk::CollisionNode* node = nullptr;
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 0.0f, -4.0f, 0.5f, nullptr, &node);
    CHECK(hit == &f.polys[0]);
    CHECK(node == &f.nodes[0]);
    CHECK(near(node->nz, 1.0));
    // Lifted centre stops at plane+2.5 -> feet at floorZ - 0.01.
    CHECK(near(cs.pos[2], 9.99f, 1e-4));
  }

  // ---- free fall: short drop misses the floor ----------------------
  {
    CollisionFixture f = makeFloorArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 12;
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 0.0f, -0.05f, 0.5f, nullptr, nullptr);
    CHECK(hit == nullptr);
    CHECK(near(cs.pos[2], 11.95));
  }

  // ---- ceiling: upward contact, normal -z --------------------------
  {
    CollisionFixture f = makeCeilArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 10;
    const mdk::CollisionNode* node = nullptr;
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 0.0f, 8.0f, 0.5f, nullptr, &node);
    CHECK(hit == &f.polys[0]);
    CHECK(node != nullptr && near(node->nz, -1.0));
    // Head (feet+5.01) stops at the ceiling plane.
    CHECK(near(cs.pos[2], 14.99f, 1e-4));
  }

  // ---- wall: horizontal contact stops at face distance -------------
  {
    CollisionFixture f = makeWallArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 2;
    cs.pos[2] = 10;
    const mdk::CollisionNode* node = nullptr;
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 4.0f, 0.0f, 0.0f, 0.75f, nullptr, &node);
    CHECK(hit == &f.polys[0]);
    CHECK(node != nullptr && near(node->nx, -1.0));
    CHECK(near(cs.pos[0], 4.4f, 1e-4)); // wall x=5 minus ext 0.6
    // Already at the face: pushing again yields zero applied motion.
    hit = mdk::collisionApply(cs, 4.0f, 0.0f, 0.0f, 0.75f, nullptr,
                              nullptr);
    CHECK(hit == &f.polys[0]);
    CHECK(near(cs.pos[0], 4.4f, 1e-4));
  }

  // ---- tangential motion along the wall is free --------------------
  {
    CollisionFixture f = makeWallArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 2;
    cs.pos[2] = 10;
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 4.0f, 0.0f, 0.75f, nullptr, nullptr);
    CHECK(hit == nullptr);
    CHECK(near(cs.pos[0], 2.0) && near(cs.pos[1], 4.0));
  }

  // ---- grazing incidence slides along the wall ---------------------
  {
    CollisionFixture f = makeWallArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 2;
    cs.pos[2] = 10;
    // dn^2 = 16 <= 0.75 * (16+64) = 60 -> slide allowed.
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 4.0f, 8.0f, 0.0f, 0.75f, nullptr, nullptr);
    // The slide resolves cleanly -> the LAST traversal is free ->
    // the seam reports no contact even though a plane was grazed.
    CHECK(hit == nullptr);
    // Contact at x=4.4, pushout margin 0.61 -> x=4.39; y slides on.
    CHECK(near(cs.pos[0], 4.39f, 1e-4));
    CHECK(near(cs.pos[1], 8.0f, 1e-4));
  }

  // ---- steep incidence stops dead (no slide) -----------------------
  {
    CollisionFixture f = makeWallArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 2;
    cs.pos[2] = 10;
    // dn^2 = 36 > 0.75 * (36+4) = 30 -> terminal contact, no slide.
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 6.0f, 2.0f, 0.0f, 0.75f, nullptr, nullptr);
    CHECK(hit == &f.polys[0]);
    CHECK(near(cs.pos[0], 4.4f, 1e-4));
    CHECK(near(cs.pos[1], 0.8f, 1e-4)); // advanced only to the contact t
  }

  // ---- zero displacement: stable result ----------------------------
  {
    CollisionFixture f = makeFloorArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 12;
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 0.0f, 0.0f, 0.75f, nullptr, nullptr);
    CHECK(hit == nullptr);
    CHECK(near(cs.pos[2], 12.0));
  }

  // ---- query disabled: full displacement, outNode cleared ----------
  {
    CollisionFixture f = makeFloorArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.queryEnabled = 0;
    cs.pos[2] = 12;
    const mdk::CollisionNode* node = &f.nodes[0];
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 0.0f, -4.0f, 0.5f, nullptr, &node);
    CHECK(hit == nullptr && node == nullptr);
    CHECK(near(cs.pos[2], 8.0));
  }

  // ---- floor probe: object floor sets c58/c60/c64/c54 bit1 ---------
  {
    CollisionFixture f = makeEmptyArena();
    ObjectFixture o;
    initFloorObject(o, 10.0f);
    f.arena.objects = &o.obj;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 10.05;
    mdk::collisionFloorProbe(cs);
    CHECK((cs.contactFlags & 0x2) != 0);
    CHECK(near(cs.floorZ, 10.0));
    CHECK(cs.floorObj == &o.obj);
    CHECK(cs.floorElemMask == 1);
    CHECK(near(cs.floorOffset, 10.0 - o.obj.baseZ));
  }

  // ---- floor probe: empty list clears bit1 + mount state -----------
  {
    CollisionFixture f = makeEmptyArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 10.05;
    cs.contactFlags = 0x3;
    // Stale mount flag with no ride object: the dc0==0 || dc8==0
    // branch scans, finds nothing, then runs the dismount path.
    static int emptyDismounts = 0;
    emptyDismounts = 0;
    cs.dismountHook = [](mdk::CollisionState&) { ++emptyDismounts; };
    cs.rideActive = 1;
    mdk::collisionFloorProbe(cs);
    CHECK((cs.contactFlags & 0x2) == 0);
    CHECK(cs.floorObj == nullptr);
    CHECK(emptyDismounts == 1);
    CHECK(cs.rideObj == nullptr && cs.rideActive == 0);
  }

  // ---- floor probe: mounted ride recomputes floorZ from baseZ ------
  {
    CollisionFixture f = makeEmptyArena();
    ObjectFixture o;
    initFloorObject(o, 10.0f);
    o.obj.flags14a = 0x80; // mountable
    f.arena.objects = &o.obj;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 10.05;
    // Mount established (dc0/dc4/dc8 as the vertical landing left it).
    cs.rideObj = &o.obj;
    cs.rideElemMask = 1;
    cs.rideActive = 1;
    cs.floorOffset = 0.25f; // c5c saved at mount time
    mdk::collisionFloorProbe(cs);
    CHECK((cs.contactFlags & 0x2) != 0);
    CHECK(cs.floorObj == &o.obj);
    CHECK(cs.floorElemMask == 1);
    // floorZ = baseZ + floorOffset — follows the object, no re-probe.
    CHECK(near(cs.floorZ, 10.0f + 0.25f));
  }

  // ---- floor probe: dismount hook on losing the 0x80 flag ----------
  {
    CollisionFixture f = makeEmptyArena();
    ObjectFixture o;
    initFloorObject(o, 10.0f);
    o.obj.flags14a = 0; // no longer mountable
    f.arena.objects = &o.obj;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 10.05;
    cs.rideObj = &o.obj;
    cs.rideActive = 1;
    static int dismountCalls = 0;
    dismountCalls = 0;
    cs.dismountHook = [](mdk::CollisionState&) { ++dismountCalls; };
    mdk::collisionFloorProbe(cs);
    CHECK(dismountCalls == 1);
    CHECK(cs.rideActive == 0);
    // Probe then re-scans: the object floor is still found.
    CHECK((cs.contactFlags & 0x2) != 0);
  }

  // ---- horizontal -> vertical ordering on one frame ----------------
  {
    CollisionFixture f = makeFloorArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 0;
    cs.pos[2] = 9.99f; // standing on the z=10 floor
    // Horizontal first: box bottom at posZ+0.5 clears the floor.
    const mdk::CollisionPoly* h = mdk::collisionApply(
        cs, 1.0f, 0.0f, 0.0f, 0.75f, nullptr, nullptr);
    CHECK(h == nullptr && near(cs.pos[0], 1.0));
    // Vertical second: gravity drop -> floor contact, feet at 9.99.
    const mdk::CollisionPoly* v = mdk::collisionApply(
        cs, 0.0f, 0.0f, -0.05f, 0.5f, nullptr, nullptr);
    CHECK(v == &f.polys[0]);
    CHECK(near(cs.pos[2], 9.99f, 1e-4));
  }

  // ---- standing player sequence (full 5C pipeline, real query) -----
  {
    CollisionFixture f = makeFloorArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 12.0f;
    mdk::PlayerVerticalEnvironment env;
    env.deepFloorZ = -1000.0f;
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    vs.posZ = 12.0f;
    vs.contactFlags = 0;
    bool grounded = false;
    for (int i = 0; i < 200 && !grounded; ++i) {
      cs.pos[0] = vs.posX;
      cs.pos[1] = vs.posY;
      cs.pos[2] = vs.posZ;
      mdk::PlayerVerticalFrame fr = mdk::integratePlayerVertical(env, ms, vs);
      if (fr.collisionIssued) {
        const mdk::CollisionNode* node = nullptr;
        const mdk::CollisionPoly* hit = mdk::collisionApply(
            cs, 0.0f, 0.0f, fr.dispZ, 0.5f, nullptr, &node);
        mdk::VerticalCollisionResult res = adaptCollision(cs, hit, node);
        mdk::applyPlayerVerticalCollision(env, ms, vs, res, fr);
      }
      mdk::playerVerticalPostStep(env, vs);
      mdk::collisionFloorProbe(cs);
      grounded = (vs.contactFlags & 0x1) != 0;
    }
    CHECK(grounded);
    CHECK(near(vs.vertVel, 0.0));
    // Landed feet ride 0.01 under the plane (the margin quirk) — then
    // each grounded frame re-contacts at t=0 and the anti-jitter keeps
    // the position pinned there.
    CHECK(near(vs.posZ, 9.99f, 0.05));
    // Ten more idle frames stay grounded and stable.
    for (int i = 0; i < 10; ++i) {
      cs.pos[2] = vs.posZ;
      mdk::PlayerVerticalFrame fr = mdk::integratePlayerVertical(env, ms, vs);
      if (fr.collisionIssued) {
        const mdk::CollisionNode* node = nullptr;
        const mdk::CollisionPoly* hit = mdk::collisionApply(
            cs, 0.0f, 0.0f, fr.dispZ, 0.5f, nullptr, &node);
        mdk::VerticalCollisionResult res = adaptCollision(cs, hit, node);
        mdk::applyPlayerVerticalCollision(env, ms, vs, res, fr);
      }
      mdk::playerVerticalPostStep(env, vs);
      mdk::collisionFloorProbe(cs);
    }
    CHECK((vs.contactFlags & 0x1) != 0);
    CHECK(near(vs.posZ, 9.99f, 0.05));
  }

  // ---- standing on an object floor (probe + pre-land snap) ---------
  {
    CollisionFixture f = makeEmptyArena();
    ObjectFixture o;
    initFloorObject(o, 10.0f);
    o.obj.flags14a = 0x80; // mountable -> blocker bit7 flows through
    f.arena.objects = &o.obj;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 12.0f;
    mdk::PlayerVerticalEnvironment env;
    env.deepFloorZ = -1000.0f;
    mdk::PlayerMotionState ms;
    mdk::PlayerVerticalState vs;
    vs.posZ = 12.0f;
    bool grounded = false;
    for (int i = 0; i < 200 && !grounded; ++i) {
      cs.pos[0] = vs.posX;
      cs.pos[1] = vs.posY;
      cs.pos[2] = vs.posZ;
      mdk::PlayerVerticalFrame fr = mdk::integratePlayerVertical(env, ms, vs);
      if (fr.collisionIssued) {
        const mdk::CollisionNode* node = nullptr;
        const mdk::CollisionPoly* hit = mdk::collisionApply(
            cs, 0.0f, 0.0f, fr.dispZ, 0.5f, nullptr, &node);
        mdk::VerticalCollisionResult res = adaptCollision(cs, hit, node);
        mdk::applyPlayerVerticalCollision(env, ms, vs, res, fr);
      }
      mdk::playerVerticalPostStep(env, vs);
      mdk::collisionFloorProbe(cs);
      // The probe runs at frame end; next frame's seam sees its state.
      grounded = (vs.contactFlags & 0x1) != 0;
    }
    CHECK(grounded);
    CHECK(near(vs.vertVel, 0.0));
    // contactObj==0 + pre-land path snaps exactly to floorZ.
    CHECK(near(vs.posZ, 10.0f, 0.05));
    // The probe blockers became the movement blockers.
    CHECK(vs.blocker0 == (std::uint32_t)(uintptr_t)&o.obj);
    CHECK(vs.blocker1 == 1);
    CHECK(vs.moveBlocker0 == vs.blocker0);
    CHECK(vs.moveBlockerFlag == 1); // +0x14a bit7 -> dc8
  }

  // ---- wall movement through the 5B seam ---------------------------
  {
    CollisionFixture f = makeWallArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 2.0f;
    cs.pos[2] = 10.0f;
    mdk::GameplayInputBindings bind;
    mdk::GameplayInputEnvironment genv;
    mdk::GameplayInputState gst;
    mdk::PlayerMotionEnvironment menv;
    mdk::PlayerMotionState ms;
    auto setBit = [](std::array<std::uint32_t, 4>& bm, int code) {
      bm[code >> 5] |= 1u << (code & 31);
    };
    mdk::RawGameplayInput r;
    setBit(r.keyLevel, 103); // KeyUp -> forward
    auto stepFrame = [&]() {
      const mdk::GameplayInputFrame fr =
          mdk::consumeGameplayInput(r, bind, genv, gst);
      mdk::PlayerMotionOutput mo = mdk::integratePlayerMotion(fr, menv, ms);
      const float oldX = cs.pos[0], oldY = cs.pos[1];
      mdk::collisionApply(cs, mo.dispX, mo.dispY, 0.0f, 0.75f, nullptr,
                          nullptr);
      const bool posChanged =
          !near(cs.pos[0], oldX, 1e-6) || !near(cs.pos[1], oldY, 1e-6);
      mdk::playerMotionPostStep(menv, posChanged, ms, mo);
      return std::make_pair(mo, posChanged);
    };
    // First frames: the request is small (accel) but real — motion
    // begins and the move event stands.
    auto first = stepFrame();
    CHECK(first.first.dispX > 0.0f);
    CHECK(first.second);
    // Drive into the wall until the face is reached.
    for (int i = 0; i < 200 && cs.pos[0] < 4.39f; ++i) stepFrame();
    CHECK(cs.pos[0] <= 4.4f + 1e-4f); // never past the face
    // Pushing from the face: zero applied motion -> positionChanged
    // goes false and the move event cancels.
    auto last = stepFrame();
    CHECK(!last.second);
    CHECK(near(cs.pos[0], 4.4f, 1e-4));
  }

  // ---- FUN_00419ee0 blob parse (synthetic stream record) -----------
  {
    // Minimal self-consistent blob: countA=0, 1 node, 1 poly,
    // 3 verts — the proven {40B A, 44B nodes, 36B polys, 12B verts}
    // chain with the dword tail.
    std::vector<std::uint8_t> blob;
    auto put32 = [&](std::uint32_t v) {
      blob.push_back((std::uint8_t)(v & 0xff));
      blob.push_back((std::uint8_t)((v >> 8) & 0xff));
      blob.push_back((std::uint8_t)((v >> 16) & 0xff));
      blob.push_back((std::uint8_t)((v >> 24) & 0xff));
    };
    auto putF = [&](float f) {
      std::uint32_t v;
      std::memcpy(&v, &f, 4);
      put32(v);
    };
    put32(0);                       // countA
    put32(1);                       // countB
    putF(0.0f); putF(0.0f); putF(1.0f); putF(-10.0f); // plane
    blob.push_back(0xff); blob.push_back(0xff);       // childFar -1
    blob.push_back(0xff); blob.push_back(0xff);       // childNear -1
    put32(1);                       // polysPos {count 1, idx 0}
    put32(0);                       // polysNeg unused
    put32(0); put32(0); put32(0); put32(0);           // +0x1c..+0x28
    put32(1);                       // countC
    blob.push_back(0); blob.push_back(0);             // v0
    blob.push_back(1); blob.push_back(0);             // v1
    blob.push_back(2); blob.push_back(0);             // v2
    for (int i = 0; i < 30; ++i) blob.push_back(0);   // poly pad
    put32(3);                       // countD
    putF(-1.0f); putF(-1.0f); putF(10.0f);
    putF(1.0f); putF(-1.0f); putF(10.0f);
    putF(1.0f); putF(1.0f); putF(10.0f);
    put32(0);                       // tail
    mdk::CollisionArena a;
    std::uint32_t counts[4] = {0, 0, 0, 0};
    CHECK(mdk::collisionBlobParse(blob.data(), blob.size(), &a,
                                  counts));
    CHECK(counts[0] == 0 && counts[1] == 1 && counts[2] == 1 &&
          counts[3] == 3);
    CHECK(a.nodes != nullptr && a.polys != nullptr &&
          a.verts != nullptr);
    CHECK(near(a.nodes[0].nz, 1.0) && a.polys[0].v[2] == 2);
    // A parsed arena answers a real query.
    mdk::CollisionState cs = makeCollisionState(&a);
    cs.pos[2] = 12.0f;
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 0.0f, -4.0f, 0.5f, nullptr, nullptr);
    CHECK(hit == a.polys);
    CHECK(near(cs.pos[2], 9.99f, 1e-4));
    // Rejections: truncated chain, bad poly index, non-unit plane.
    CHECK(!mdk::collisionBlobParse(blob.data(), blob.size() - 8, &a,
                                   counts));
    std::vector<std::uint8_t> bad = blob;
    bad[4 + 4 + 44 + 4] = 9; // poly v0 = 9 >= countD
    CHECK(!mdk::collisionBlobParse(bad.data(), bad.size(), &a,
                                   counts));
    bad = blob;
    std::memcpy(bad.data() + 4 + 4 + 8, &counts[0], 4); // nz = 0
    CHECK(!mdk::collisionBlobParse(bad.data(), bad.size(), &a,
                                   counts));
  }
}

} // namespace

int main() {
  test_framebuffer();
  test_palette_expand();
  test_viewport();
  test_binary_reader();
  test_container();
  test_file_family();
  test_sni_directory();
  test_mti_directory();
  test_mto_directory();
  test_cmi_directory();
  test_dti_structure();
  test_fti_directory();
  test_bni_directory();
  test_bni_image();
  test_bni_indexed_image();
  test_stream_context();
  test_fti_font();
  test_fti_sprite();
  test_frontend_menu();
  test_frontend_controller();
  test_options_controller();
  test_display_controller();
  test_sound_controller();
  test_mouse_controller();
  test_keyboard_controller();
  test_frontend_settings();
  test_frontend_flow();
  test_options_render();
  test_display_render();
  test_sound_render();
  test_mouse_render();
  test_keyboard_render();
  test_indexed_image_blit();
  test_data_root();
  test_mode_dispatch();
  test_input_state();
  test_gameplay_input();
  test_player_motion();
  test_player_vertical();
  test_player_collision();
  std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
