// Phase 3A unit tests for platform-neutral logic. No SDL, no Metal —
// those paths are exercised by the runtime smoke test instead.

#include "core/binary_reader.h"
#include "core/cmi_directory.h"
#include "core/compat.h"
#include "core/container.h"
#include "core/data_root.h"
#include "core/file_family.h"
#include "core/framebuffer.h"
#include "core/mode_dispatch.h"
#include "core/mti_directory.h"
#include "core/mto_directory.h"
#include "core/sni_directory.h"
#include "core/viewport.h"
#include "input/input_state.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <tuple>
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
  // Phase 3F: CMI).
  CHECK(fileFamilySupport(MdkFileFamily::kSni) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kMti) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kMto) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kCmi) ==
        FamilySupport::kDirectoryMetadata);
  CHECK(fileFamilySupport(MdkFileFamily::kDti) ==
        FamilySupport::kEnvelopeOnly);
  CHECK(fileFamilySupport(MdkFileFamily::kFti) ==
        FamilySupport::kEnvelopeOnly);
  CHECK(fileFamilySupport(MdkFileFamily::kBni) ==
        FamilySupport::kEnvelopeOnly);
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
  test_data_root();
  test_mode_dispatch();
  test_input_state();
  std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
