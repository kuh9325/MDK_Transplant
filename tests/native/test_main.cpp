// Phase 3A unit tests for platform-neutral logic. No SDL, no Metal —
// those paths are exercised by the runtime smoke test instead.

#include "core/binary_reader.h"
#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/cmi_directory.h"
#include "core/compat.h"
#include "core/container.h"
#include "core/data_root.h"
#include "core/dti_structure.h"
#include "core/file_family.h"
#include "core/framebuffer.h"
#include "core/fti_directory.h"
#include "core/indexed_image.h"
#include "core/mode_dispatch.h"
#include "core/mti_directory.h"
#include "core/mto_directory.h"
#include "core/sni_directory.h"
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
  test_indexed_image_blit();
  test_data_root();
  test_mode_dispatch();
  test_input_state();
  std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
