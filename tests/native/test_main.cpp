// Phase 3A unit tests for platform-neutral logic. No SDL, no Metal —
// those paths are exercised by the runtime smoke test instead.

#include "core/arena_mesh.h"
#include "core/arena_render.h"
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
#include "core/dynamic_objects.h"
#include "core/enemy_runtime.h"
#include "core/file_family.h"
#include "core/framebuffer.h"
#include "core/freefall_runtime.h"
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
#include "core/object_animation.h"
#include "core/object_path.h"
#include "core/options_menu.h"
#include "core/player_camera.h"
#include "core/player_fire.h"
#include "core/player_motion.h"
#include "core/player_projectiles.h"
#include "core/player_surface.h"
#include "core/player_vertical.h"
#include "core/sni_directory.h"
#include "core/sound_menu.h"
#include "core/stream_context.h"
#include "core/traversal_runtime.h"
#include "core/traversal_script.h"
#include "core/viewport.h"
#include "input/input_state.h"

#include <array>
#include <bit>
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

// ---------------------------------------------------------------------------
// Phase 5E — dynamic collision objects
// ---------------------------------------------------------------------------

// Synthetic geometry-record builder (no original data). Layout is the
// OBSERVED+CODE-CORROBORATED FUN_00428400 form:
//   {u32 flag}{u32 nameCount}{nameCount x {name[12], u32 tag}}
//   flag!=0: {u32 elemCount}{elems: name[12], field2[12], u32 vc,
//             vc x 12B verts, u32 tc, tc x 0x24B tris, 24B trailer}
//   flag==0: {u32 vc, verts, u32 tc, tris}  (one anonymous element)
//   {0x18 gap}{u32 refCount}{refCount x 12B}
struct GeoElemSpec {
  const char* name;
  std::vector<float> verts;
  std::vector<std::uint16_t> triIdx;   // 3 u16 per tri -> 0x24 record
};

std::vector<std::uint8_t> makeGeoRecord(
    std::uint32_t flag,
    std::initializer_list<const char*> names,
    const std::vector<GeoElemSpec>& elems,
    std::initializer_list<std::array<float, 3>> refPts) {
  std::vector<std::uint8_t> b;
  auto u32 = [&](std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back((v >> (k * 8)) & 0xff);
  };
  auto f32 = [&](float v) {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    u32(u);
  };
  auto name12 = [&](const char* s) {
    for (int i = 0; i < 12; ++i) b.push_back(s && s[i] ? s[i] : 0);
  };
  u32(flag);
  u32(static_cast<std::uint32_t>(names.size()));
  for (const char* n : names) {
    name12(n);
    u32(0);   // tag
  }
  if (flag != 0) u32(static_cast<std::uint32_t>(elems.size()));
  for (const auto& e : elems) {
    if (flag != 0) {
      name12(e.name);
      for (int i = 0; i < 12; ++i) b.push_back(0xAB);  // field2
    }
    u32(static_cast<std::uint32_t>(e.verts.size() / 3));
    for (float v : e.verts) f32(v);
    u32(static_cast<std::uint32_t>(e.triIdx.size() / 3));
    for (std::size_t t = 0; t < e.triIdx.size(); t += 3) {
      b.push_back(e.triIdx[t] & 0xff);
      b.push_back((e.triIdx[t] >> 8) & 0xff);
      b.push_back(e.triIdx[t + 1] & 0xff);
      b.push_back((e.triIdx[t + 1] >> 8) & 0xff);
      b.push_back(e.triIdx[t + 2] & 0xff);
      b.push_back((e.triIdx[t + 2] >> 8) & 0xff);
      for (int i = 0; i < 30; ++i) b.push_back(0);   // rest of 0x24 rec
    }
    if (flag != 0) for (int i = 0; i < 0x18; ++i) b.push_back(0xEE);
  }
  for (int i = 0; i < 0x18; ++i) b.push_back(0);      // +0x18 gap
  u32(static_cast<std::uint32_t>(refPts.size()));
  for (const auto& p : refPts) {
    f32(p[0]);
    f32(p[1]);
    f32(p[2]);
  }
  return b;
}

// A single-element flat platform model: square tri at local z, with
// an optional element name — used as the spawn "source" model.
mdk::RuntimeModel makePlatformModel(const char* modelName,
                                    const char* elemName, float z) {
  mdk::RuntimeModel m;
  m.flag = 1;
  mdk::RuntimeModel::NameRec nr;
  std::snprintf(nr.name.data(), nr.name.size(), "%s", modelName);
  m.names.push_back(nr);
  m.elems.resize(1);
  m.elemNames.resize(1);
  m.elemField2.resize(1);
  m.elemVerts.resize(1);
  m.elemTris.resize(1);
  std::snprintf(m.elemNames[0].data(), m.elemNames[0].size(), "%s",
                elemName);
  m.elemVerts[0] = {-5, -5, z, 5, -5, z, 5, 5, z};
  m.elemTris[0].assign(0x24, 0);
  auto* idx = reinterpret_cast<std::uint16_t*>(m.elemTris[0].data());
  idx[0] = 0;
  idx[1] = 1;
  idx[2] = 2;
  m.elems[0].triCount = 1;
  const float lb[6] = {-5, -5, z, 5, 5, z};
  std::memcpy(m.elems[0].localAabb, lb, sizeof(lb));
  m.rebind();
  return m;
}

// Multi-element target for the homing/punch element scans. Each
// element is a 10-unit cube named `name`, centred at height zC.
mdk::RuntimeModel makeHomingModel(
    std::initializer_list<std::pair<const char*, float>> elems) {
  mdk::RuntimeModel m;
  m.flag = 1;
  mdk::RuntimeModel::NameRec nr;
  std::snprintf(nr.name.data(), nr.name.size(), "HOMING");
  m.names.push_back(nr);
  const std::size_t n = elems.size();
  m.elems.resize(n);
  m.elemNames.resize(n);
  m.elemField2.resize(n);
  m.elemVerts.resize(n);
  m.elemTris.resize(n);
  std::size_t i = 0;
  for (const auto& pr : elems) {
    std::snprintf(m.elemNames[i].data(), m.elemNames[i].size(), "%s",
                  pr.first);
    m.elemVerts[i] = {-5, -5, 0, 5, -5, 0, 5, 5, 0};
    m.elemTris[i].assign(0x24, 0);
    auto* idx = reinterpret_cast<std::uint16_t*>(m.elemTris[i].data());
    idx[0] = 0;
    idx[1] = 1;
    idx[2] = 2;
    m.elems[i].triCount = 1;
    const float lb[6] = {-5, -5, pr.second - 5, 5, 5, pr.second + 5};
    std::memcpy(m.elems[i].localAabb, lb, sizeof(lb));
    ++i;
  }
  m.rebind();
  return m;
}

// Model-source callback over a small table of models.
struct TestModelSrc {
  std::vector<const mdk::RuntimeModel*> byIndex;
};
const mdk::RuntimeModel* testModelFor(int idx, void* ctx) {
  auto* s = static_cast<TestModelSrc*>(ctx);
  if (idx < 0 || static_cast<std::size_t>(idx) >= s->byIndex.size())
    return nullptr;
  return s->byIndex[static_cast<std::size_t>(idx)];
}

mdk::DtiSubRecord makeSpawnRec(std::uint32_t type, std::uint32_t f1,
                               float x, float y, float z,
                               const char* name) {
  mdk::DtiSubRecord r = {};
  r.type = type;
  r.fields[0] = f1;
  r.fields[1] = 0;
  auto putf = [&](int i, float v) {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    r.fields[i] = u;
  };
  putf(2, x);
  putf(3, y);
  putf(4, z);
  auto* nm = reinterpret_cast<char*>(&r.fields[5]);
  std::snprintf(nm, 12, "%s", name);
  return r;
}

// ---------------------------------------------------------------------------
// Phase 5F — surface contact effects
// ---------------------------------------------------------------------------

mdk::CollisionPoly makeSurfacePoly(std::uint16_t a, std::uint16_t b,
                                   std::uint16_t c, std::uint8_t surf,
                                   std::uint16_t flags) {
  mdk::CollisionPoly p = {};
  p.v[0] = a;
  p.v[1] = b;
  p.v[2] = c;
  p.surface = surf;
  p.flags = flags;
  return p;
}

// Records the FUN_004546ac seam's invokes (the script hook).
struct SurfaceScriptLog {
  int calls = 0;
  std::uint32_t lastOff = 0;
  std::int32_t lastEvent = 0;
  std::uint8_t resultIn = 0;
  float delta[3] = {};
};
void surfaceScriptSpy(mdk::SurfaceObjectState& /*ctx*/,
                      std::uint32_t off, std::int32_t event,
                      std::uint8_t& result, const mdk::SurfaceFxState& fx,
                      void* user) {
  auto* log = static_cast<SurfaceScriptLog*>(user);
  ++log->calls;
  log->lastOff = off;
  log->lastEvent = event;
  log->resultIn = result;
  for (int i = 0; i < 3; ++i) log->delta[i] = fx.delta[i];
  result |= 0x40;   // the VM may modify the result byte (CL return)
}

void test_player_surface() {
  const float zero3[3] = {0, 0, 0};

  // --- surface metadata decode + dispatch gate -------------------------
  {
    mdk::CollisionPoly polys[3] = {
        makeSurfacePoly(0, 1, 2, 3, 0x0),   // surface 3 -> index 2
        makeSurfacePoly(0, 1, 2, 0, 0x0),   // no surface
        makeSurfacePoly(0, 1, 2, 20, 0x0)}; // surface > 16 -> rejected
    mdk::SurfaceObjectState ctx = {};
    ctx.polys = polys;
    ctx.polyCount = 3;
    ctx.config[2] = 0x8;          // channel-8 enable only
    ctx.handlerOff[2] = 0x100;    // a CMI-relative handler
    ctx.handlerMask[2] = 0x8;
    SurfaceScriptLog log;
    mdk::SurfaceFxState fx;
    // Surface byte 0 -> no dispatch.
    CHECK(mdk::surfaceDispatch(ctx, 0, 0x8, &polys[1], -0xb, zero3, zero3,
                               zero3, fx, surfaceScriptSpy, &log) == 0);
    // Surface > 16 -> index out of range, no dispatch.
    CHECK(mdk::surfaceDispatch(ctx, 0, 0x8, &polys[2], -0xb, zero3, zero3,
                               zero3, fx, surfaceScriptSpy, &log) == 0);
    // A wrong channel (contextMask=2) gates both side-effects and the
    // handler invoke (handlerMask has only bit3 set).
    CHECK(mdk::surfaceDispatch(ctx, 0, 0x2, &polys[0], -0xb, zero3, zero3,
                               zero3, fx, surfaceScriptSpy, &log) == 0);
    CHECK(log.calls == 0);
    // The matching channel invokes the handler -> bit0 set + the spy's
    // CL modification lands in the returned byte.
    const std::uint8_t r = mdk::surfaceDispatch(
        ctx, 0, 0x8, &polys[0], -0xb, zero3, zero3, zero3, fx,
        surfaceScriptSpy, &log);
    CHECK(log.calls == 1 && log.lastOff == 0x100 && log.lastEvent == -0xb);
    CHECK((r & 0x1) != 0 && (r & 0x40) != 0);
  }

  // --- polyOp flag ops (FUN_0040a704) ----------------------------------
  {
    mdk::CollisionPoly polys[3] = {
        makeSurfacePoly(0, 0, 0, 2, 0x0),
        makeSurfacePoly(0, 0, 0, 2, 0x0),
        makeSurfacePoly(0, 0, 0, 5, 0x0)};
    // set-0x10 only hits surface-2 polys.
    mdk::surfacePolyOp(polys, 3, 2, mdk::kSurfOpSet10);
    CHECK((polys[0].flags & 0x10) && (polys[1].flags & 0x10));
    CHECK((polys[2].flags & 0x10) == 0);
    mdk::surfacePolyOp(polys, 3, 2, mdk::kSurfOpSet20);
    CHECK((polys[0].flags & 0x20) && (polys[1].flags & 0x20));
    mdk::surfacePolyOp(polys, 3, 2, mdk::kSurfOpClear10);
    CHECK((polys[0].flags & 0x10) == 0 && (polys[0].flags & 0x20) != 0);
    mdk::surfacePolyOp(polys, 3, 2, mdk::kSurfOpSet30);
    CHECK((polys[0].flags & 0x30) == 0x30);
    mdk::surfacePolyOp(polys, 3, 2, mdk::kSurfOpClear30);
    CHECK((polys[0].flags & 0x30) == 0);
    // surfId 0 / out of range is a no-op.
    mdk::surfacePolyOp(polys, 3, 0, mdk::kSurfOpSet10);
    CHECK((polys[0].flags & 0x10) == 0);
  }

  // --- dispatcher side-effects (0x80/0x40/0x20) + counters -------------
  {
    mdk::CollisionPoly polys[2] = {
        makeSurfacePoly(0, 0, 0, 1, 0x10),   // armed 0x10
        makeSurfacePoly(0, 0, 0, 1, 0x0)};
    mdk::SurfaceObjectState ctx = {};
    ctx.polys = polys;
    ctx.polyCount = 2;
    ctx.config[0] = 0x8 | 0x80;   // channel-8 + the 0x80 flag effect
    mdk::SurfaceFxState fx;
    // The 0x80 effect: marks bit (1<<surf) + clears poly flag 0x10.
    CHECK(mdk::surfaceDispatch(ctx, 0, 0x8, &polys[0], -0xb, zero3, zero3,
                               zero3, fx, nullptr, nullptr) == 0);
    CHECK((ctx.marks & 0x2) != 0);              // 1 << surfId(1)
    CHECK((polys[0].flags & 0x10) == 0);        // cleared on contact
    CHECK((polys[1].flags & 0x10) == 0);        // all surf-1 polys
    // cfg&0x20 sets result bit1; cfg&0x40 force-invokes the handler.
    mdk::CollisionPoly p2 = makeSurfacePoly(0, 0, 0, 2, 0x0);
    mdk::SurfaceObjectState ctx2 = {};
    ctx2.polys = &p2;
    ctx2.polyCount = 1;
    ctx2.config[1] = 0x8 | 0x40 | 0x20;
    ctx2.handlerOff[1] = 0x55;
    ctx2.handlerMask[1] = 0x0;    // no channel bits — but 0x40 forces it
    SurfaceScriptLog log;
    const std::uint8_t r = mdk::surfaceDispatch(
        ctx2, 0, 0x8, &p2, -0xb, zero3, zero3, zero3, fx,
        surfaceScriptSpy, &log);
    CHECK((r & 0x2) != 0);                 // cfg&0x20 result bit
    CHECK(log.calls == 1);                 // 0x40 force-invoked
    CHECK(ctx2.counters[1] == 1);          // secondary 0 -> 1 via 0x40
  }

  // --- surfaceApplyPending (FUN_0040b4dc) ------------------------------
  {
    mdk::CollisionPoly polys[2] = {
        makeSurfacePoly(0, 0, 0, 1, 0x0),
        makeSurfacePoly(0, 0, 0, 2, 0x0)};
    mdk::SurfaceObjectState ctx = {};
    ctx.polys = polys;
    ctx.polyCount = 2;
    ctx.marks = 0x2;   // bit1 = surface 1
    // mode0 re-arms flag 0x10 on marked surfaces and clears the marks.
    mdk::surfaceApplyPending(ctx, 0);
    CHECK(ctx.marks == 0);
    CHECK((polys[0].flags & 0x10) != 0);
    CHECK((polys[1].flags & 0x10) == 0);
    // mode1: polys flagged &2 get 0x30; opMaskA bit -> op4, opMaskB -> op2.
    polys[0].flags = 0x2;
    ctx.opMaskA = 0x4;   // bit2 = surface 2 -> op4 (set 0x20)
    ctx.opMaskB = 0x0;
    mdk::surfaceApplyPending(ctx, 1);
    CHECK((polys[0].flags & 0x30) == 0x30);
    CHECK((polys[1].flags & 0x20) != 0);
    CHECK((polys[1].flags & 0x10) == 0);
    ctx.opMaskA = 0x0;
    ctx.opMaskB = 0x4;   // surface 2 -> op2 (set 0x10)
    mdk::surfaceApplyPending(ctx, 1);
    CHECK((polys[1].flags & 0x10) != 0);
  }

  // --- conveyor (FUN_00412ef0) -----------------------------------------
  {
    mdk::SurfaceObjectState ctx = {};
    mdk::CollisionPoly p = makeSurfacePoly(0, 0, 0, 4, 0x0);
    const float dir[3] = {3.0f, 0.0f, 4.0f};   // len 5 -> unit (0.6,0,0.8)
    mdk::SurfaceRecord* r =
        mdk::surfaceRecordCreate(ctx, 4, dir, 2.0f);
    CHECK(r && r->kind == -1 && r->surfType == 4);
    // normalized direction
    CHECK(std::fabs(r->v[0] - 0.6f) < 1e-5f &&
          std::fabs(r->v[2] - 0.8f) < 1e-5f);
    float out[3] = {0, 0, 0};
    const float dt = 1.0f / 30.0f;
    mdk::surfaceConveyorDelta(ctx, &p, dt, out);
    // out += (dir*rate)*dt : (0.6*2)*dt, (0.8*2)*dt
    CHECK(std::fabs(out[0] - 0.6f * 2.0f * dt) < 1e-5f);
    CHECK(std::fabs(out[1] - 0.0f) < 1e-6f);
    CHECK(std::fabs(out[2] - 0.8f * 2.0f * dt) < 1e-5f);
    // A non-matching surface produces nothing.
    mdk::CollisionPoly pOther = makeSurfacePoly(0, 0, 0, 7, 0x0);
    float out2[3] = {0, 0, 0};
    mdk::surfaceConveyorDelta(ctx, &pOther, dt, out2);
    CHECK(out2[0] == 0.0f && out2[2] == 0.0f);
    // A volume record on the same surface is skipped by the conveyor.
    const float box[6] = {0, 0, 0, 1, 1, 1};
    mdk::surfaceVolumeCreate(ctx, 2, box, 5.0f, ~0u);
    float out3[3] = {0, 0, 0};
    mdk::surfaceConveyorDelta(ctx, &p, dt, out3);
    CHECK(std::fabs(out3[0] - out[0]) < 1e-6f);   // only the surface rec
    // Two surface records on the same type accumulate.
    const float dir2[3] = {-1.0f, 0.0f, 0.0f};
    mdk::surfaceRecordCreate(ctx, 4, dir2, 1.0f);
    float out4[3] = {0, 0, 0};
    mdk::surfaceConveyorDelta(ctx, &p, dt, out4);
    CHECK(std::fabs(out4[0] - (0.6f * 2.0f - 1.0f * 1.0f) * dt) < 1e-5f);
    mdk::surfaceRecordsDestroy(ctx);
    CHECK(ctx.records == nullptr);
  }

  // --- record update: the rate ramp (FUN_004134a0 subset) --------------
  {
    mdk::SurfaceObjectState ctx = {};
    const float dir[3] = {1, 0, 0};
    mdk::SurfaceRecord* r = mdk::surfaceRecordCreate(ctx, 1, dir, 0.0f);
    r->target = 2.0f;
    r->ramp = 1.0f;              // rate/sec toward target
    const float dt = 1.0f / 30.0f;
    mdk::surfaceRecordUpdate(ctx, dt);
    CHECK(std::fabs(r->rate - (0.0f + 1.0f * dt)) < 1e-6f);
    // Keep ramping; when it would overshoot the target it clamps +
    // zeroes the ramp.
    for (int i = 0; i < 200 && r->rate != r->target; ++i)
      mdk::surfaceRecordUpdate(ctx, dt);
    CHECK(r->rate == 2.0f && r->ramp == 0.0f);
    mdk::surfaceRecordsDestroy(ctx);
  }

  // --- volume/ribbon query (FUN_00412e94 + FUN_00412f84) ---------------
  {
    mdk::SurfaceObjectState ctx = {};
    const float box[6] = {0, 0, 0, 10, 10, 20};   // min0, max{10,10,20}
    mdk::surfaceVolumeCreate(ctx, 5, box, 30.0f, ~0u);  // shape5: t=1-t
    float pos[3] = {5, 5, 10};
    float vec[3] = {0, 0, 0};
    const float dt = 1.0f / 30.0f;
    // shape5: t = 1 - (posz-minz)/(maxz-minz) = 1 - 10/20 = 0.5
    // target = 30*0.5 = 15; vec.z(0) < target -> vec.z += 15*dt.
    CHECK(mdk::surfaceVolumeQuery(ctx, ~0u, pos, dt, vec) == 1);
    CHECK(std::fabs(vec[2] - 15.0f * dt) < 1e-5f);
    // Outside the box -> no hit.
    float outPos[3] = {50, 5, 10};
    float vec2[3] = {0, 0, 0};
    CHECK(mdk::surfaceVolumeQuery(ctx, ~0u, outPos, dt, vec2) == 0);
    CHECK(vec2[2] == 0.0f);
    // The z-top pad (+5.0): pos.z just above maxz still counts.
    float topPos[3] = {5, 5, 23.0f};   // 20 < 23 <= 25
    CHECK(mdk::surfaceVolumeQuery(ctx, ~0u, topPos, dt, vec2) == 1);
    // queryMask gate: a record masked to bit0 fails a bit1 query.
    mdk::SurfaceObjectState ctx2 = {};
    mdk::surfaceVolumeCreate(ctx2, 5, box, 30.0f, 0x1);
    float vec3[3] = {0, 0, 0};
    CHECK(mdk::surfaceVolumeQuery(ctx2, 0x2, pos, dt, vec3) == 0);
    CHECK(mdk::surfaceVolumeQuery(ctx2, 0x1, pos, dt, vec3) == 1);
    // Above target -> halve the gap: z=(z+target)*0.5.
    float vec4[3] = {0, 0, 30.0f};
    mdk::surfaceVolumeQuery(ctx, ~0u, pos, dt, vec4);  // target 15
    CHECK(std::fabs(vec4[2] - (30.0f + 15.0f) * 0.5) < 1e-4f);
    mdk::surfaceRecordsDestroy(ctx);
    mdk::surfaceRecordsDestroy(ctx2);
  }

  // --- slide-zone trigger (opcode 0xe0) --------------------------------
  {
    // One type-9 record: box fields[3..8] = {minx,miny,minz,maxx,maxy,maxz}.
    mdk::DtiSubRecord rec = {};
    rec.type = 9;
    auto putf = [&](int i, float v) {
      std::uint32_t u;
      std::memcpy(&u, &v, 4);
      rec.fields[i] = u;
    };
    putf(3, 0); putf(4, 0); putf(5, 0);
    putf(6, 10); putf(7, 10); putf(8, 20);
    const float posIn[3] = {5, 5, 10};
    const float posOut[3] = {50, 5, 10};
    const float dt = 1.0f / 30.0f;
    // flag==0 -> clear-slide path, no scan.
    mdk::SlideZoneResult z0 = mdk::slideZoneTrigger(
        &rec, 1, 0, posIn, false, false, 0, 0, dt);
    CHECK(z0.clearedSlide && !z0.inside && !z0.setBounceFlag);
    // outside the box -> no trigger.
    mdk::SlideZoneResult z1 = mdk::slideZoneTrigger(
        &rec, 1, 1, posOut, false, false, 0, 0, dt);
    CHECK(!z1.inside && !z1.setBounceFlag);
    // inside, airborne (no slide/contact normal) -> down-slam.
    mdk::SlideZoneResult z2 = mdk::slideZoneTrigger(
        &rec, 1, 1, posIn, false, false, 0, 0, dt);
    CHECK(z2.inside && z2.setBounceFlag);
    CHECK(z2.downSlam && !z2.slideRedirect);
    CHECK(std::fabs(z2.downSlamDelta - (-(dt * 128.0))) < 1e-4f);
    // inside, grounded (contact normal) -> slide redirect + impulse.
    mdk::SlideZoneResult z3 = mdk::slideZoneTrigger(
        &rec, 1, 1, posIn, false, true, 90.0f, 10.0f, dt);
    CHECK(z3.slideRedirect && !z3.downSlam);
    CHECK(z3.yawDeg == 90.0f);
    // impulse = {sin(90),cos(90)}*10 -> {10, ~0}.
    CHECK(std::fabs(z3.impulseX - 10.0f) < 1e-3f);
    CHECK(std::fabs(z3.impulseZ) < 1e-3f);
    // A non-type-9 record is skipped.
    mdk::DtiSubRecord other = {};
    other.type = 7;
    mdk::SlideZoneResult z4 = mdk::slideZoneTrigger(
        &other, 1, 1, posIn, false, false, 0, 0, dt);
    CHECK(!z4.inside);
  }

  // --- real callback path: sweep -> surfaceContactHook -> dispatch -----
  {
    // Flat floor at z=10 carrying surface byte 3.
    CollisionFixture f = makeFloorArena();
    f.polys[0].surface = 3;
    f.polys[0].flags = 0x10;                  // armed
    f.finish();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 0.0f;
    cs.pos[1] = 0.0f;
    cs.pos[2] = 20.0f;
    mdk::SurfaceObjectState ctx = {};
    ctx.polys = const_cast<mdk::CollisionPoly*>(f.arena.polys);
    ctx.polyCount = 1;
    ctx.config[2] = 0x8 | 0x80;               // channel-8 + 0x80 effect
    cs.surface = &ctx;
    cs.surfaceContextMask = 0x8;
    cs.contactHook = &mdk::surfaceContactHook;
    // Fall onto the floor — the contact should dispatch the 0x80 effect.
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 0.0f, -15.0f, 0.5f, nullptr, nullptr);
    CHECK(hit == &f.polys[0]);
    CHECK((ctx.marks & 0x8) != 0);            // 1 << surfId(3)
    CHECK((f.polys[0].flags & 0x10) == 0);    // flag cleared on contact
  }

  // --- conveyor -> 5B displacement (contact -> record -> move) ---------
  {
    CollisionFixture f = makeFloorArena();
    f.polys[0].surface = 2;
    f.finish();
    mdk::SurfaceObjectState ctx = {};
    const float dir[3] = {1.0f, 0.0f, 0.0f};
    mdk::surfaceRecordCreate(ctx, 2, dir, 6.0f);
    const float dt = 1.0f / 30.0f;
    // The 5B seam: env.conveyor* now comes from the real record query on
    // the contact poly instead of a synthetic injection.
    float conv[3] = {0, 0, 0};
    mdk::surfaceConveyorDelta(ctx, &f.polys[0], dt, conv);
    CHECK(std::fabs(conv[0] - 1.0f * 6.0f * dt) < 1e-5f);
    mdk::PlayerMotionEnvironment env = {};
    env.groundContact = true;
    env.conveyorX = conv[0];
    env.conveyorY = conv[1];
    env.conveyorZ = conv[2];
    mdk::PlayerMotionState s = {};
    const mdk::PlayerMotionOutput o =
        mdk::integratePlayerMotion(mdk::GameplayInputFrame{}, env, s);
    CHECK(std::fabs(o.dispX - conv[0]) < 1e-5f);   // conveyor carried +X
    CHECK(o.dispY == 0.0f && o.dispZ == 0.0f);
    mdk::surfaceRecordsDestroy(ctx);
  }

  // --- bounce -> 5C: the slide-zone sets res.bounce -> vs.bounceFlag ---
  {
    mdk::DtiSubRecord rec = {};
    rec.type = 9;
    auto putf = [&](int i, float v) {
      std::uint32_t u;
      std::memcpy(&u, &v, 4);
      rec.fields[i] = u;
    };
    putf(3, -50); putf(4, -50); putf(5, 0);
    putf(6, 50); putf(7, 50); putf(8, 5);
    const float pos[3] = {0, 0, 2};
    const float dt = 1.0f / 30.0f;
    mdk::SlideZoneResult zr = mdk::slideZoneTrigger(
        &rec, 1, 1, pos, false, true, 0.0f, 0.0f, dt);
    CHECK(zr.setBounceFlag);
    // Feed the proven flag into the 5C seam.
    mdk::VerticalCollisionResult res = {};
    res.bounce = zr.setBounceFlag;
    res.contactObj = 1;                // a contact token
    res.hasFloor = true;
    res.floorZ = 0.0f;
    res.posZ = 0.0f;
    mdk::PlayerVerticalState vs = {};
    mdk::PlayerMotionState ms = {};
    mdk::PlayerVerticalEnvironment env = {};
    env.frameStep = 1;
    env.deltaSeconds = dt;
    env.deepFloorZ = -1000.0f;
    vs.vertVel = -60.0f;               // a hard impact
    mdk::PlayerVerticalFrame vf;
    mdk::applyPlayerVerticalCollision(env, ms, vs, res, vf);
    CHECK(vs.bounceFlag == 1);
    // bounceFlag suppresses the hard-landing event path.
    CHECK(!vf.hardLanding);
    // The flag clears on the post-step (FUN_00466aec tail).
    mdk::playerVerticalPostStep(env, vs);
    CHECK(vs.bounceFlag == 0);
  }
}

void test_dynamic_objects() {
  using mdk::DynamicArena;
  using mdk::DynamicObject;

  // ---- geometry record parse: flag=1 (named elements) ------------
  {
    auto rec = makeGeoRecord(1, {"MODEL_A", "SECOND"},
        {{"XG1_BODY", {0, 0, 0, 4, 0, 0, 0, 4, 0}, {0, 1, 2}},
         {"XG1_HEAD", {0, 0, 10, 2, 0, 10, 0, 2, 10}, {0, 1, 2}}},
        {{1, 2, 3}});
    auto m = mdk::parseGeometryRecord(rec.data(), rec.data() + rec.size());
    CHECK(m.has_value());
    CHECK(m->flag == 1);
    CHECK(m->names.size() == 2);
    CHECK(m->modelName() == "MODEL_A");
    CHECK(m->elems.size() == 2);
    CHECK(m->elemName(0) == "XG1_BODY");
    CHECK(m->bodyElemIndex == 0);
    CHECK(m->elemName(1) == "XG1_HEAD");
    CHECK(m->headElemMask == 2u);
    CHECK(m->elems[0].triCount == 1);
    CHECK(m->elemVerts[0].size() == 9);
    // FUN_00459d54 local AABB of elem0 = {0,0,0,4,4,0}.
    CHECK(near(m->elems[0].localAabb[0], 0.f, 1e-5) &&
          near(m->elems[0].localAabb[3], 4.f, 1e-5) &&
          near(m->elems[0].localAabb[4], 4.f, 1e-5) &&
          near(m->elems[0].localAabb[2], 0.f, 1e-5));
    CHECK(m->refPointCount == 1);
    CHECK(near(m->refPoints[0][0], 1.f, 1e-5) &&
          near(m->refPoints[0][2], 3.f, 1e-5));
    // views point into owned storage
    CHECK(m->elems[0].verts == m->elemVerts[0].data());
    CHECK(m->elems[0].tris == m->elemTris[0].data());
    CHECK(m->elementSet().count == 2);
  }

  // ---- geometry record parse: flag=0 (anonymous element) ---------
  {
    auto rec = makeGeoRecord(0, {"SW_GATT"},
        {{"", {1, 2, 3, 4, 5, 6, 7, 8, 9}, {0, 1, 2}}}, {});
    auto m = mdk::parseGeometryRecord(rec.data(), rec.data() + rec.size());
    CHECK(m.has_value());
    CHECK(m->flag == 0);
    CHECK(m->names.size() == 1 && m->modelName() == "SW_GATT");
    CHECK(m->elems.size() == 1);
    CHECK(m->elemName(0).empty());       // anonymous
    CHECK(m->elems[0].triCount == 1);
    CHECK(near(m->elems[0].localAabb[0], 1.f, 1e-5) &&
          near(m->elems[0].localAabb[5], 9.f, 1e-5));
    CHECK(m->refPointCount == 0);
  }

  // ---- parse bounds rejection ------------------------------------
  {
    auto rec = makeGeoRecord(1, {"M"},
        {{"E", {0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 1, 2}}}, {});
    CHECK(!mdk::parseGeometryRecord(rec.data(), rec.data() + 20));
    // Corrupt: huge nameCount.
    std::vector<std::uint8_t> bad = rec;
    bad[4] = 0x40;
    CHECK(!mdk::parseGeometryRecord(bad.data(), bad.data() + bad.size()));
  }

  // ---- FUN_00403720 deep-copy independence ------------------------
  {
    auto rec = makeGeoRecord(1, {"M"},
        {{"E", {0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 1, 2}}}, {});
    auto m = mdk::parseGeometryRecord(rec.data(), rec.data() + rec.size());
    auto cp = mdk::deepCopyModel(*m);
    CHECK(cp.elems.size() == 1);
    CHECK(cp.elems[0].verts == cp.elemVerts[0].data());
    CHECK(cp.elems[0].verts != m->elems[0].verts);   // fresh storage
    cp.elemVerts[0][0] = 99.0f;
    CHECK(m->elemVerts[0][0] != 99.0f);              // source untouched
  }

  // ---- FUN_0046b2f8 matrix: identity / yaw90 / scale --------------
  {
    float m[9], o[3];
    const float pos[3] = {10, 20, 30};
    mdk::buildObjectMatrix(0, 0, 0, 1.0f, pos, m, o);
    CHECK(near(m[0], 1.f, 1e-5) && near(m[4], 1.f, 1e-5) &&
          near(m[8], 1.f, 1e-5) && near(m[1], 0.f, 1e-5));
    CHECK(near(o[0], 10.f, 1e-5) && near(o[2], 30.f, 1e-5));
    // yaw=90deg: local +x -> world +y (rows {0,-1,0},{1,0,0},{0,0,1}).
    mdk::buildObjectMatrix(0, 0, 90.0f, 2.0f, pos, m, o);
    CHECK(near(m[0], 0.f, 1e-5) && near(m[1], -2.f, 1e-5) &&
          near(m[3], 2.f, 1e-5) && near(m[4], 0.f, 1e-5) &&
          near(m[8], 2.f, 1e-5));
  }

  // ---- FUN_0045612c rebuild: Euler path + AABB seed quirk ----------
  {
    DynamicArena ar;
    DynamicObject& o = ar.allocFront();
    o.model = makePlatformModel("PLAT", "ELEM", 0.0f);
    o.setPosition(10, 20, 30);
    o.prevYawDeg = o.yawDeg = 0;
    mdk::initObjectCollision(o);
    // first build: seed {0,0,0}/{0,0,0} then union elem world
    // {-5,-5,0,5,5,0}+pos = {5,15,30,15,25,30} -> min clamps to 0.
    CHECK(near(o.col.aabb[0], 0.f, 1e-5) &&
          near(o.col.aabb[3], 15.f, 1e-5) &&
          near(o.col.aabb[4], 25.f, 1e-5) &&
          near(o.col.aabb[5], 30.f, 1e-5));
    CHECK(near(o.model.elems[0].aabb[0], 5.f, 1e-5) &&
          near(o.model.elems[0].aabb[3], 15.f, 1e-5) &&
          near(o.model.elems[0].aabb[5], 30.f, 1e-5));
    // second build at same pos: seed = {minZ x3}/{maxZ x3} of the
    // PREVIOUS aabb {0,0,0,15,25,30} -> {0,0,0}/{30,30,30}, then the
    // element union -> {0,0,0,30,30,30} (z-seed quirk reproduced).
    mdk::rebuildObjectTransform(o);
    CHECK(near(o.col.aabb[2], 0.f, 1e-5) &&
          near(o.col.aabb[5], 30.f, 1e-5) &&
          near(o.col.aabb[3], 30.f, 1e-5));
    // elemMaskB excludes the element: AABB stays the degenerate seed.
    o.col.elemMaskB = 1;
    mdk::rebuildObjectTransform(o);
    CHECK(near(o.col.aabb[0], 0.f, 1e-5) &&
          near(o.col.aabb[3], 30.f, 1e-5));
    o.col.elemMaskB = 0;
  }

  // ---- rebuild: raw-matrix path + zBias fold ----------------------
  {
    DynamicArena ar;
    DynamicObject& o = ar.allocFront();
    o.model = makePlatformModel("PLAT", "ELEM", 0.0f);
    o.setPosition(0, 0, 100);
    mdk::initObjectCollision(o);
    o.col.flags148 |= 0x40;
    o.zBias = 7.0f;
    const float twice[9] = {2, 0, 0, 0, 2, 0, 0, 0, 2};
    std::memcpy(o.rawMatrix, twice, sizeof(twice));
    o.col.scale = 1.0f;
    mdk::rebuildObjectTransform(o);
    CHECK(near(o.col.xform[0], 2.f, 1e-5) &&
          near(o.col.xform[4], 2.f, 1e-5));
    CHECK(near(o.col.origin[2], 107.f, 1e-5));   // z + zBias
    // world AABB: local {-5..5} * 2 + {0,0,107} -> {-10,-10,107,10,10,107}
    CHECK(near(o.model.elems[0].aabb[0], -10.f, 1e-5) &&
          near(o.model.elems[0].aabb[3], 10.f, 1e-5) &&
          near(o.model.elems[0].aabb[2], 107.f, 1e-5));
  }

  // ---- enemy table + DTI name rewrite -----------------------------
  {
    mdk::EnemyTable enemies;
    enemies.entries = {{"XGS", 0x100, false}, {"XT", 0, true},
                       {"SW_GATT", 0x200, false}};
    CHECK(enemies.indexOf("XT") == 1);
    CHECK(enemies.indexOf("MISSING") == -1);
    mdk::DtiArenaRecord rec = {};
    rec.subRecords = {makeSpawnRec(2, 9, 1, 2, 3, "XGS"),
                      makeSpawnRec(4, 0, 5, 6, 7, "SW_GATT"),
                      makeSpawnRec(6, 0, 0, 0, 0, "CONN"),
                      makeSpawnRec(2, 2, 8, 9, 0, "NOPE")};
    auto failed = mdk::resolveArenaRecordNames(rec, enemies);
    CHECK(rec.subRecords[0].fields[0] == (0u << 16 | 9u));
    CHECK(rec.subRecords[1].fields[0] == 2u);
    CHECK(rec.subRecords[2].fields[0] == 0u);   // type 6 untouched
    CHECK(failed.size() == 1 && failed[0] == 3);
  }

  // ---- spawnArenaObjects: type-2 + type-4 + dedup + push-front -----
  {
    mdk::RuntimeModel plat = makePlatformModel("PLAT", "ELEM", 0.0f);
    mdk::RuntimeModel sw = makePlatformModel("SW_DUMMY", "SW_DUMMY", 0.0f);
    // second element in `sw` that must stay enabled
    sw.elemNames.push_back({});
    sw.elemField2.push_back({});
    sw.elemVerts.push_back({-1, -1, 0, 1, -1, 0, 1, 1, 0});
    sw.elemTris.push_back(std::vector<std::uint8_t>(0x24, 0));
    sw.elems.push_back(mdk::CollisionElement{});
    sw.elems[1].triCount = 1;
    sw.elemNames[0] = {};
    std::snprintf(sw.elemNames[0].data(), 12, "SW_DUMMY");
    std::snprintf(sw.elemNames[1].data(), 12, "KEEP");
    sw.rebind();

    TestModelSrc src{{&plat, &sw}};
    DynamicArena ar;
    ar.name = "HMO_9";
    mdk::DtiArenaRecord rec = {};
    rec.subRecords = {
        makeSpawnRec(2, (1u << 16) | 9u, -174, 2603, -4660, "XGS"),
        makeSpawnRec(4, 1, 5, 6, 7, "SW_DUMMY"),
        makeSpawnRec(6, 0, 0, 0, 0, "CONN")};
    std::vector<DynamicObject*> spawned;
    const int n =
        mdk::spawnArenaObjects(ar, rec, testModelFor, &src, &spawned);
    CHECK(n == 2 && spawned.size() == 2);
    CHECK(ar.storage.size() == 2);
    // push-front: type-4 object (spawned second) is the +0x68 head.
    CHECK(ar.col.objects == &spawned[1]->col);
    CHECK(ar.col.objects->next == &spawned[0]->col);
    // type-2 fields
    DynamicObject& t2 = *spawned[0];
    CHECK(t2.enemyIndex == 1);
    CHECK(t2.spawnId == 9);
    CHECK(t2.behaviorByte == 7);
    CHECK(t2.health == 10);
    CHECK(t2.col.named && t2.col.model != nullptr);
    CHECK(t2.col.elements == &t2.elemSet);
    CHECK(near(t2.pos[0], -174.f, 1e-4) && near(t2.pos[2], -4660.f, 1e-4));
    CHECK(near(t2.prevPos[2], -4660.f, 1e-4));
    CHECK(near(t2.col.baseZ, -4660.f, 1e-4));
    CHECK(t2.arena == &ar);
    // deep copy: spawned model verts differ from the source's storage
    CHECK(t2.model.elems[0].verts != plat.elems[0].verts);
    // type-4 fields: +0x148 dword 0x2008a0 -> bytes a0/08/20.
    DynamicObject& t4 = *spawned[1];
    CHECK(t4.health == 1);
    CHECK((t4.col.flags148 & 0x08a0) == 0x08a0);
    CHECK((t4.col.flags149 & 0x08) == 0x08);
    CHECK((t4.col.flags14a & 0x20) == 0x20);
    // SW_DUMMY model: element named SW_DUMMY masked, KEEP enabled.
    CHECK((t4.col.elemMaskB & 1u) == 1u);
    CHECK((t4.col.elemMaskB & 2u) == 0u);
    // dedup: same records again -> nothing new.
    const int n2 =
        mdk::spawnArenaObjects(ar, rec, testModelFor, &src, nullptr);
    CHECK(n2 == 0 && ar.storage.size() == 2);
  }

  // ---- attach / detach / transfer ---------------------------------
  {
    DynamicArena a, b;
    a.name = "A";
    b.name = "B";
    DynamicObject& o1 = a.allocFront();
    DynamicObject& o2 = a.allocFront();
    DynamicObject& o3 = a.allocFront();
    CHECK(a.col.objects == &o3.col);
    CHECK(a.col.objects->next == &o2.col);
    CHECK(a.col.objects->next->next == &o1.col);
    a.transfer(o2, b);
    CHECK(b.col.objects == &o2.col);
    CHECK(o2.arena == &b && o2.pendingArena == nullptr);
    CHECK(a.col.objects == &o3.col);
    CHECK(a.col.objects->next == &o1.col);
    CHECK(a.storage.size() == 2 && b.storage.size() == 1);
    a.detach(o3);
    CHECK(a.col.objects == &o1.col);
    CHECK(a.storage.size() == 1);
    // FUN_0045cf90 — detach wipes the record in place (the original's
    // memset) and pushes it onto the global freelist; FUN_0045cffc
    // pops it back, so a same-arena re-alloc reuses the same record.
    DynamicObject& o4 = a.allocFront();
    CHECK(&o4 == &o3);                       // freelist pop (LIFO)
    CHECK(o4.col.named && o4.arena == &a);
    CHECK(o4.health == 0 && o4.enemyIndex == 0 && o4.field108 == nullptr);
    CHECK(a.col.objects == &o4.col && a.col.objects->next == &o1.col);
  }

  // ---- reapUnnamed: FUN_0045cf18 corpse sweep ----------------------
  {
    DynamicArena a;
    a.name = "R";
    DynamicObject& keep = a.allocFront();
    DynamicObject& dead1 = a.allocFront();
    DynamicObject& dead2 = a.allocFront();
    keep.scriptClass = "KEEP";
    dead1.col.named = false;                 // teardown'd corpse
    dead2.col.named = false;
    a.reapUnnamed();
    CHECK(a.storage.size() == 1);
    CHECK(a.col.objects == &keep.col);
    CHECK(keep.col.named && keep.scriptClass == "KEEP");
    // Both corpses are memset-wiped before the freelist push. The
    // sweep walks the list front-to-back and each free push-fronts,
    // so the FIRST-reaped record sits on top: dead2 is list head
    // (allocFront push-front), reaped first, dead1 pushed over it.
    DynamicObject& r1 = a.allocFront();
    CHECK(&r1 == &dead1);
    CHECK(r1.scriptClass.empty() && r1.health == 0);
    DynamicObject& r2 = a.allocFront();
    CHECK(&r2 == &dead2);
    CHECK(a.storage.size() == 3);
    CHECK(a.col.objects == &r2.col);
    CHECK(a.col.objects->next == &r1.col);
    CHECK(a.col.objects->next->next == &keep.col);
  }

  // ---- floor probe consumes a spawned object's rebuilt transform --
  {
    mdk::RuntimeModel plat = makePlatformModel("PLAT", "ELEM", 0.0f);
    TestModelSrc src{{&plat}};
    DynamicArena ar;
    mdk::DtiArenaRecord rec = {};
    rec.subRecords = {makeSpawnRec(4, 0, 0, 0, 50, "PLAT")};
    mdk::spawnArenaObjects(ar, rec, testModelFor, &src, nullptr);
    DynamicObject& o = *ar.storage.front();
    o.col.flags149 |= 1;                       // standable (script bit)
    CollisionFixture f = makeEmptyArena();
    f.arena.objects = ar.col.objects;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 0;
    cs.pos[1] = 0;
    cs.pos[2] = 51.0f;                         // 1 above the platform
    mdk::collisionFloorProbe(cs);
    CHECK(cs.floorObj == &o.col);
    CHECK((cs.contactFlags & 2) != 0);
    CHECK(near(cs.floorZ, 50.f, 1e-3));
    CHECK(near(cs.floorOffset, 50.f - 50.f, 1e-3));  // bot.z - baseZ
  }

  // ---- ride displacement: moving platform carries the player ------
  {
    mdk::RuntimeModel plat = makePlatformModel("PLAT", "ELEM", 0.0f);
    TestModelSrc src{{&plat}};
    DynamicArena ar;
    mdk::DtiArenaRecord rec = {};
    rec.subRecords = {makeSpawnRec(4, 0, 0, 0, 50, "PLAT")};
    mdk::spawnArenaObjects(ar, rec, testModelFor, &src, nullptr);
    DynamicObject& o = *ar.storage.front();
    o.col.flags149 |= 1;
    o.col.flags14a |= 0x80;                    // mountable (script op)
    CollisionFixture f = makeEmptyArena();
    f.arena.objects = ar.col.objects;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 0;
    cs.pos[1] = 0;
    cs.pos[2] = 51.0f;
    mdk::collisionFloorProbe(cs);
    CHECK(cs.floorObj == &o.col);
    // vertical landing establishes the ride (the +0x14a&0x80 gate in
    // FUN_00467180); the probe leaves the object as the blocker.
    cs.rideObj = &o.col;
    cs.rideElemMask = cs.floorElemMask;
    cs.rideActive = 1;
    // platform rises 5 and yaws 30 degrees this frame.
    o.setPosition(0, 0, 55);
    o.yawDeg = 30.0f;
    float playerYaw = 0.0f;
    mdk::updateMoverCollision(o, cs, &playerYaw);
    CHECK(near(cs.pos[2], 56.0f, 1e-4));       // carried +5
    CHECK(near(playerYaw, 30.0f, 1e-4));       // yaw delta applied
    CHECK(near(o.prevPos[2], 55.f, 1e-4));     // prev latched
    CHECK(near(o.prevYawDeg, 30.f, 1e-4));
    // next frame floorZ follows the carrier's baseZ + stored offset.
    mdk::collisionFloorProbe(cs);
    CHECK(near(cs.floorZ, 55.f + cs.floorOffset, 1e-3));
  }

  // ---- sweep object pass consumes spawned objects -----------------
  {
    // Tall box: the object pass is a segment-vs-element-AABB test, so
    // the player's z must fall inside the expanded element box.
    mdk::RuntimeModel wall = makePlatformModel("WALL", "ELEM", 0.0f);
    const float tall[6] = {-5, -5, -10, 5, 5, 10};
    std::memcpy(wall.elems[0].localAabb, tall, sizeof(tall));
    TestModelSrc src{{&wall}};
    DynamicArena ar;
    mdk::DtiArenaRecord rec = {};
    rec.subRecords = {makeSpawnRec(2, (0u << 16) | 1u, 0, 0, 0, "P")};
    mdk::spawnArenaObjects(ar, rec, testModelFor, &src, nullptr);
    DynamicObject& o = *ar.storage.front();
    CollisionFixture f = makeEmptyArena();
    f.arena.objects = ar.col.objects;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 0;
    cs.pos[1] = -8.0f;
    cs.pos[2] = 3.0f;
    // horizontal move +y through the object's element AABB
    // {-5,-5,-10,5,5,10} expanded by ext {0.6,0.6,2.5}.
    mdk::collisionApply(cs, 0.0f, 12.0f, 0.0f, 0.75f, nullptr, nullptr);
    CHECK(cs.lastObjContact == &o.col);
    CHECK(cs.pos[1] < -5.0f);                  // blocked before the box
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Phase 5G — traversal runtime (synthetic arenas + records)
// ---------------------------------------------------------------------------

namespace {

mdk::DtiSubRecord travSub(std::uint32_t type,
                          std::initializer_list<std::uint32_t> f) {
  mdk::DtiSubRecord r;
  r.type = type;
  std::size_t i = 0;
  for (std::uint32_t v : f)
    if (i < r.fields.size()) r.fields[i++] = v;
  return r;
}

std::uint32_t fbits(float v) {
  std::uint32_t u;
  std::memcpy(&u, &v, 4);
  return u;
}

mdk::TraversalArena* travArenaAdd(mdk::TraversalRuntime& rt,
                                  const char* name) {
  // rec points into level.work — reserve so later push_backs can't
  // reallocate and dangle earlier arenas' record pointers.
  rt.level.work.reserve(64);
  rt.level.work.push_back(mdk::DtiArenaRecord{});
  auto a = std::make_unique<mdk::TraversalArena>();
  a->name = name;
  a->index = static_cast<int>(rt.arenas.size());
  a->rec = &rt.level.work.back();
  a->dyn.name = a->name;
  a->dyn.owner = a.get();
  rt.arenas.push_back(std::move(a));
  return rt.arenas.back().get();
}

} // namespace

void test_traversal_connect_pairing() {
  // FUN_00434e54 — file-form connect ids (>999) rewrite fields[0] to
  // the partner arena index; side codes must XOR to 1, boxes equal.
  std::vector<mdk::DtiArenaRecord> arenas(2);
  const float box[6] = {-19.f, 654.f, 104.f, 17.f, 654.f, 126.f};
  mdk::DtiSubRecord a = travSub(6, {1000, 2, 0, 0, 0, 0, 0, 0});
  mdk::DtiSubRecord b = travSub(6, {1000, 3, 0, 0, 0, 0, 0, 0});
  for (int i = 0; i < 6; ++i) {
    a.fields[2 + i] = fbits(box[i]);
    b.fields[2 + i] = fbits(box[i]);
  }
  arenas[0].subRecords.push_back(a);
  arenas[1].subRecords.push_back(b);
  std::string detail;
  CHECK(mdk::traversalConnectPairing(arenas, &detail) ==
        mdk::TraversalLoadError::kOk);
  CHECK(arenas[0].subRecords[0].fields[0] == 1);  // -> partner index
  CHECK(arenas[1].subRecords[0].fields[0] == 0);

  // Mismatched side codes are load-fatal.
  std::vector<mdk::DtiArenaRecord> bad(2);
  bad[0].subRecords.push_back(a);
  bad[1].subRecords.push_back(a);  // side 2 vs side 2 — not XOR-1
  CHECK(mdk::traversalConnectPairing(bad, &detail) !=
        mdk::TraversalLoadError::kOk);
}

void test_traversal_portal_test() {
  // FUN_00435178 — side 0: x-plane crossing toward -x with y-slab and
  // z-slab (z0-5.0) segment overlap on prev->current.
  mdk::TraversalRuntime rt;
  mdk::TraversalArena* cur = travArenaAdd(rt, "CHMO_1");
  travArenaAdd(rt, "HMO_1");
  mdk::DtiSubRecord portal = travSub(
      6, {1, 0, fbits(42.f), fbits(720.f), fbits(133.f),
          fbits(42.f), fbits(744.f), fbits(147.f)});
  rt.level.work[0].subRecords.push_back(portal);
  rt.cur = cur;

  // Cross x=42 going -x inside the slabs -> pass, returns arena 1.
  rt.cs.pos[0] = 41.0f;  rt.cs.pos[1] = 730.f; rt.cs.pos[2] = 140.f;
  rt.cs.entryPos[0] = 43.0f; rt.cs.entryPos[1] = 730.f;
  rt.cs.entryPos[2] = 140.f;
  CHECK(mdk::traversalPortalTest(rt) == rt.arenas[1].get());

  // Same crossing +x direction -> fail (side 0 requires -x motion).
  rt.cs.pos[0] = 43.0f; rt.cs.entryPos[0] = 41.0f;
  CHECK(mdk::traversalPortalTest(rt) == nullptr);

  // Crossing -x but outside the y slab -> fail.
  rt.cs.pos[0] = 41.0f; rt.cs.pos[1] = 750.f;
  rt.cs.entryPos[0] = 43.0f; rt.cs.entryPos[1] = 750.f;
  CHECK(mdk::traversalPortalTest(rt) == nullptr);

  // Crossing -x, y inside, z below z0-5.0 (128) -> fail.
  rt.cs.pos[1] = 730.f; rt.cs.pos[2] = 120.f;
  rt.cs.entryPos[1] = 730.f; rt.cs.entryPos[2] = 120.f;
  CHECK(mdk::traversalPortalTest(rt) == nullptr);

  // Diagonal sides — OBSERVED correction: side 5 passes on cross > 0,
  // side 6 (and unmatched codes) on cross < 0.
  mdk::TraversalRuntime rt2;
  mdk::TraversalArena* cur2 = travArenaAdd(rt2, "A");
  travArenaAdd(rt2, "B");
  // Box x[0,10], y[0,10], z[0,10] — the diagonal line runs from
  // (x0,y0)=(0,0) to (x1,y1)=(10,10): cross = (py-0)*10 - (10)*(px-0)
  // = 10(py - px); positive when py > px.
  mdk::DtiSubRecord d5 = travSub(
      6, {1, 5, fbits(0.f), fbits(0.f), fbits(0.f),
          fbits(10.f), fbits(10.f), fbits(10.f)});
  rt2.level.work[0].subRecords.push_back(d5);
  rt2.cur = cur2;
  rt2.cs.pos[0] = 5.f; rt2.cs.pos[1] = 8.f; rt2.cs.pos[2] = 5.f;  // cross=30>0
  rt2.cs.entryPos[0] = 5.f; rt2.cs.entryPos[1] = 2.f;
  rt2.cs.entryPos[2] = 5.f;
  CHECK(mdk::traversalPortalTest(rt2) == rt2.arenas[1].get());
  rt2.cs.pos[1] = 2.f;                                            // cross=-30<0
  CHECK(mdk::traversalPortalTest(rt2) == nullptr);

  mdk::TraversalRuntime rt3;
  mdk::TraversalArena* cur3 = travArenaAdd(rt3, "A");
  travArenaAdd(rt3, "B");
  mdk::DtiSubRecord d6 = d5;
  d6.fields[1] = 6;
  rt3.level.work[0].subRecords.push_back(d6);
  rt3.cur = cur3;
  rt3.cs.pos[0] = 5.f; rt3.cs.pos[1] = 2.f; rt3.cs.pos[2] = 5.f;  // cross<0
  rt3.cs.entryPos[0] = 5.f; rt3.cs.entryPos[1] = 8.f;
  rt3.cs.entryPos[2] = 5.f;
  CHECK(mdk::traversalPortalTest(rt3) == rt3.arenas[1].get());
  rt3.cs.pos[1] = 8.f;                                            // cross>0
  CHECK(mdk::traversalPortalTest(rt3) == nullptr);
}

void test_traversal_trigger_scan() {
  // FUN_00434b44 — type-1 attach / -1 detach, type-3 cold prefetch.
  mdk::TraversalRuntime rt;
  mdk::TraversalArena* cur = travArenaAdd(rt, "CHMO_1");
  mdk::TraversalArena* hmo = travArenaAdd(rt, "HMO_2");
  rt.level.work[0].subRecords.push_back(travSub(
      1, {1, 0, fbits(147.f), fbits(681.f), fbits(88.f),
          fbits(154.f), fbits(716.f), fbits(100.f)}));
  rt.level.work[0].subRecords.push_back(travSub(
      1, {0xffffffffu, 0, fbits(140.f), fbits(681.f), fbits(88.f),
          fbits(146.f), fbits(716.f), fbits(100.f)}));
  rt.level.work[0].subRecords.push_back(travSub(
      3, {0, 0, fbits(73.f), fbits(684.f), fbits(88.f),
          fbits(80.f), fbits(716.f), fbits(100.f)}));
  rt.cur = cur;

  // Inside the attach strip: type-1 fires, partner attaches hot.
  rt.cs.pos[0] = 150.f; rt.cs.pos[1] = 700.f;
  rt.cs.entryPos[0] = 150.f; rt.cs.entryPos[1] = 700.f;
  mdk::traversalTriggerScan(rt);
  CHECK(rt.seams.type1Triggers == 1);
  CHECK(rt.partner == hmo && rt.cs.carrier == &hmo->dyn.col);
  CHECK(rt.partnerActive && rt.cs.carrierValid == 1);

  // Idempotent re-fire while in-zone (OBSERVED: attach short-circuits
  // on equal ca4) — still counted, still attached.
  mdk::traversalTriggerScan(rt);
  CHECK(rt.seams.type1Triggers == 2);
  CHECK(rt.partner == hmo && rt.partnerActive);

  // Inside the detach strip: -1 -> FUN_00432bf8 clears everything.
  rt.cs.pos[0] = 143.f; rt.cs.entryPos[0] = 143.f;
  mdk::traversalTriggerScan(rt);
  CHECK(rt.seams.type1Triggers == 3);
  CHECK(rt.partner == nullptr && rt.cs.carrier == nullptr);
  CHECK(!rt.partnerActive && rt.cs.carrierValid == 0);

  // Inside the type-3 prefetch strip: partner slotted but cold —
  // ca8=0 means no carrierValid and no partnerActive.
  rt.cs.pos[0] = 76.f; rt.cs.entryPos[0] = 76.f;
  mdk::traversalTriggerScan(rt);
  CHECK(rt.seams.type3Prefetches == 1);
  CHECK(rt.partner == rt.arenas[0].get());  // fields[0]==0 -> arena 0
  CHECK(!rt.partnerActive && rt.cs.carrierValid == 0);

  // Outside all strips: nothing fires.
  rt.cs.pos[0] = 60.f; rt.cs.entryPos[0] = 60.f;
  const int t1 = rt.seams.type1Triggers, t3 = rt.seams.type3Prefetches;
  mdk::traversalTriggerScan(rt);
  CHECK(rt.seams.type1Triggers == t1 && rt.seams.type3Prefetches == t3);
}

void test_traversal_deep_floor() {
  // +0x44e has no writer in MDK95 — a fresh CollisionArena carries the
  // observed zero-init: the 0x4673ee failsafe is a flat posZ<=-50.
  mdk::CollisionArena arena;
  CHECK(arena.deepFloorZ == 0.0f);
}

// ---------------------------------------------------------------------------
// Phase 5H — tr_alcmd arena script VM
// ---------------------------------------------------------------------------

namespace {
// Build a synthetic CMI image: file bytes where the script bytecode
// sits at image offset `codeOff` (file offset 4+codeOff). The image
// base is file+4, so image[4+off] is byte `off`.
struct ScriptFixture {
  std::vector<std::byte> image;
  mdk::TraversalRuntime rt;
  mdk::TraversalArena* arena = nullptr;
  mdk::TraversalScriptEnv env;
  std::vector<std::string> diag;

  ScriptFixture() {
    image.assign(0x4000, std::byte{0});
    arena = travArenaAdd(rt, "TEST_1");
    rt.cur = arena;
    env.image = std::span<const std::byte>(image.data(), image.size());
    env.imageBase = 4;
    env.rt = &rt;
    env.currentArena = arena;
    env.selfArena = arena;
    env.playerPos = rt.cs.pos;
    env.diagLog = &diag;
  }
  void write(std::uint32_t off, std::initializer_list<int> bytes) {
    std::uint32_t p = off;
    for (int b : bytes) image[env.imageBase + p++] = std::byte(b & 0xff);
  }
  void writeF(std::uint32_t off, float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    for (int k = 0; k < 4; ++k)
      image[env.imageBase + off + k] = std::byte((v >> (8 * k)) & 0xff);
  }
  void writeW(std::uint32_t off, std::uint32_t v) {
    for (int k = 0; k < 4; ++k)
      image[env.imageBase + off + k] = std::byte((v >> (8 * k)) & 0xff);
  }
  void writeStr(std::uint32_t off, const char* s) {
    std::size_t n = std::strlen(s) + 1;   // counted incl NUL
    image[env.imageBase + off] = std::byte(n & 0xff);
    for (std::size_t i = 0; i < n; ++i)
      image[env.imageBase + off + 1 + i] = std::byte(s[i]);
  }
};
} // namespace

void test_traversal_script() {
  const std::uint32_t C = 0x200;    // code image offset

  // --- checkpoint + end: PC persists at the post-ckpt byte ---------
  {
    ScriptFixture f;
    f.write(C, {0x01, 0xff});                 // ckpt; end
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.halted && !r.error);
    CHECK(f.arena->script.pcImageOff == C + 1);  // ckpt wrote +0x220
  }

  // --- stop clears the gate -----------------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x09, 0xff});
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.stopped && f.arena->script.pcImageOff == 0);
  }

  // --- wait suspends, then resumes ----------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x40, 0x03});                 // wait, mode3 inline f32
    f.writeF(C + 2, 0.01f);                    // 0.01 s < 1/30 step
    f.write(C + 6, {0xff});
    f.arena->script.pcImageOff = C;
    auto r1 = mdk::traversalScriptRun(f.env);   // hits wait, exits
    CHECK(r1.waited && f.arena->script.waitSeconds > 0.0f);
    CHECK(f.arena->script.waitResumeImageOff == C + 6);
    auto r2 = mdk::traversalScriptRun(f.env);   // decrements, resumes
    CHECK(r2.halted);                            // ran the ff at C+6
  }

  // --- call/return (0xfe two-way) via a flag branch -----------------
  {
    ScriptFixture f;
    // 47 grp bit link  — branch if bit set. grp=0 (global),bit=0.
    // set bit first: 44 00 00 then 47 00 00 fe <callT> <elseT>
    f.write(C, {0x44, 0x00, 0x00});           // set global bit0
    f.write(C + 3, {0x47, 0x00, 0x00, 0xfe}); // brIfSet g0 b0 fe
    f.writeW(C + 7, 0x300);                    // call target
    f.writeW(C + 0xb, 0x000);                  // else target
    f.write(C + 0xf, {0xff});                  // end
    // call target @0x300: 62 surfop 1 2; fd return
    f.write(0x300, {0x62, 0x01, 0x02, 0xfd});
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.halted && !r.error);
    CHECK(f.env.gFlags & 1);                    // bit0 set
    CHECK(f.arena->script.callDepth == 0);      // returned
    CHECK(f.arena->surface.opMaskB & (1u << 1)); // surfop ran
  }

  // --- runaway loop hits the 1000 cap --------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x0c, 0x01});                 // rgoto n1
    f.writeW(C + 2, C);                        // -> C (self loop)
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.error && r.instructions == 1000);
    CHECK(r.diag.find("looped") != std::string::npos);
    CHECK(f.arena->script.pcImageOff == 0);
  }

  // --- box2d fires linkage only inside ------------------------------
  {
    ScriptFixture f;
    // 60 box2d x0 y0 x1 y1 link. Put player inside -> call runs.
    f.write(C, {0x60});
    f.writeF(C + 1, 0.0f); f.writeF(C + 5, 0.0f);
    f.writeF(C + 9, 10.0f); f.writeF(C + 0xd, 10.0f);
    f.write(C + 0x11, {0xfc});                  // call mode
    f.writeW(C + 0x12, 0x300);
    f.write(C + 0x16, {0xff});
    f.write(0x300, {0x09});                     // target: stop
    f.rt.cs.pos[0] = 5.f; f.rt.cs.pos[1] = 5.f;
    f.env.playerPos = f.rt.cs.pos;
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.stopped);                           // call ran the 09
  }

  // --- malformed read: opcode fetch out of bounds --------------------
  {
    ScriptFixture f;
    f.arena->script.pcImageOff = 0x3fff;        // near image end
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.error);                             // bounded, no OOB
  }

  // --- spawn records a dormant object --------------------------------
  {
    ScriptFixture f;
    // 0x95 is the connector variant: it needs the class in the enemy
    // table and a resolvable destination arena (FUN_00454794 aborts
    // the spawn on either miss).
    travArenaAdd(f.rt, "HMO_3");
    f.rt.level.enemies.entries.push_back({"XCORDOOR", 0, false});
    f.write(C, {0x95});                          // spawn
    f.writeF(C + 1, 1.0f); f.writeF(C + 5, 2.0f); f.writeF(C + 9, 3.0f);
    f.writeF(C + 0xd, 90.0f);                    // yaw
    f.writeW(C + 0x11, 0x7ce);                   // flags
    const std::uint32_t s1 = C + 0x15;           // lenstr "XCORDOOR" (10B)
    const std::uint32_t s2 = s1 + 10;            // lenstr "HMO_3" (7B)
    const std::uint32_t so = s2 + 7;             // u32 scriptOff
    f.writeStr(s1, "XCORDOOR");
    f.writeStr(s2, "HMO_3");
    f.writeW(so, 0x777);
    f.write(so + 4, {0xff});
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.halted && !r.error);
    CHECK(f.env.seamsSpawned == 1);
    CHECK(!f.arena->dyn.storage.empty());
    const auto& o = *f.arena->dyn.storage.front();
    CHECK(o.scriptClass == "XCORDOOR");
    CHECK(o.scriptName == "HMO_3");
    CHECK(o.scriptOff == 0x777);
    CHECK(o.pos[0] == 1.0f && o.pos[2] == 3.0f);
  }

  // --- surfbind writes handler table ----------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x63, 0x05, 0x03});             // surfbind mask5 sid3
    f.writeW(C + 3, 0xabc);                      // handlerOff
    f.write(C + 7, {0xff});
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.halted);
    CHECK(f.arena->surface.handlerMask[2] == 5);   // slot (3-1)
    CHECK(f.arena->surface.handlerOff[2] == 0xabc);
  }

  // --- unknown opcode halts with diagnostic ---------------------------
  {
    ScriptFixture f;
    f.write(C, {0x02, 0xff});                    // 0x02 not implemented
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.error);
    CHECK(r.diag.find("Unrecognised") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Phase 5I — FUN_004566f0 table-2 object-init script + connector door
// ---------------------------------------------------------------------------

void test_traversal_object_init() {
  // cmiObjectScriptOffset — table-2 "%s$%s" -> image-relative code off
  // (no {str}{str}{u32} wrapper like table-3).
  auto mkRec = [](const char* nm, std::uint32_t v) {
    mdk::CmiRecord r;
    const std::size_t n = std::strlen(nm) + 1;   // counted incl. NUL
    r.nameLength = static_cast<std::uint8_t>(n);
    const auto* b = reinterpret_cast<const std::byte*>(nm);
    r.nameBytes.assign(b, b + n);
    r.nameEndsWithTerminator = true;
    r.value = v;
    return r;
  };
  {
    mdk::CmiDirectory cmi;
    cmi.tables.resize(4);
    cmi.tables[2].records.push_back(mkRec("CHMO_2$XCORDOOR", 0x204c0));
    cmi.tables[2].records.push_back(mkRec("HMO_2$XG", 0x7e31));
    CHECK(mdk::cmiObjectScriptOffset(cmi, "CHMO_2$XCORDOOR") == 0x204c0);
    CHECK(mdk::cmiObjectScriptOffset(cmi, "HMO_2$XG") == 0x7e31);
    CHECK(mdk::cmiObjectScriptOffset(cmi, "CHMO_2$XTUR") == 0);
    mdk::CmiDirectory empty;
    CHECK(mdk::cmiObjectScriptOffset(empty, "CHMO_2$XCORDOOR") == 0);
  }

  // Helper that writes the observed CHMO_2$XCORDOOR init stream at C
  // with the two 0x96 anim operands retargeted to `an`/`af` (so they
  // land inside a fixture image), plus fake {rate,pad,frameCount}
  // anim records there.
  auto writeDoorScript = [&](ScriptFixture& f, std::uint32_t C,
                             std::uint32_t an, std::uint32_t af) {
    f.write(C,     {0x10, 0xe8, 0xfd});        // +0x8=+0x2a2=0xfde8, +0x21f=1
    f.write(C + 3, {0x53, 0x03});              // +0x58 = inline f32
    f.writeF(C + 5, 1.01021f);
    f.write(C + 9, {0x96});                    // +0x306/+0x30a anim recs
    f.writeW(C + 0xa, an);
    f.writeW(C + 0xe, af);
    f.write(C + 0x12, {0x97});                 // 4 counted sound names
    f.writeStr(C + 0x13, "DOOR");              // op0 -> +0x31a
    f.writeStr(C + 0x19, "DOOR");              // op1 -> +0x322
    f.writeStr(C + 0x1f, "");                  // op2 -> +0x316 (cleared)
    f.writeStr(C + 0x21, "");                  // op3 -> +0x31e (cleared)
    f.write(C + 0x23, {0x98});                 // +0x312 hi nibble
    f.writeW(C + 0x24, 0x10);
    f.write(C + 0x28, {0x99});                 // +0x30e radius
    f.writeF(C + 0x29, 20.0f);
    f.write(C + 0x2d, {0xff});
    // anim records: [f32 rate][u32][i32 frameCount]
    f.writeF(an, 1.0f); f.writeW(an + 4, 0); f.writeW(an + 8, 16);
    f.writeF(af, 1.0f); f.writeW(af + 4, 0); f.writeW(af + 8, 21);
  };

  // traversalObjectInitScript applies the door's config opcodes.
  {
    ScriptFixture f;
    const std::uint32_t C = 0x200;
    writeDoorScript(f, C, 0x600, 0x620);

    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.connState = 8;                             // connector-create defaults
    o.connRadius = 20.0f;
    o.col.flags148 = 0x8000;
    o.col.flags149 = 0x80;
    o.col.flags14a = 0x10;
    mdk::initObjectDefaults(o);
    auto r = mdk::traversalObjectInitScript(f.env, o, C);
    CHECK(r.halted && !r.error);
    CHECK(o.health == 0xfde8);
    CHECK(o.healthMirror2a2 == 0xfde8);
    CHECK(o.flag21f == 1);
    CHECK(near(o.col.scale, 1.01021, 1e-5));
    CHECK(o.animRecNear ==
          static_cast<const void*>(f.image.data() + 4 + 0x600));
    CHECK(o.animRecFar ==
          static_cast<const void*>(f.image.data() + 4 + 0x620));
    CHECK(o.connState == 0x18);                  // closed | collision-toggle
    CHECK(near(o.connRadius, 20.0, 1e-5));
    CHECK(o.connSound31a == "DOOR" && o.connSound322 == "DOOR");
    CHECK(o.connSound316.empty() && o.connSound31e.empty());
  }

  // Field-set family beyond the door's: 0x5a->+0xe8, 0xc7->+0x104
  // (same {mode,[f32|idx]} grammar as 0x53/0x54), 0x76->+0x118
  // (u32 slot, low16-1), and 0x75->+0x148 &= ~u32. Confirmed from
  // MDK95.EXE handler disasm — the +0x104 writer is dispatch-table
  // slot 0xc7 (handler 0x45188f), not 0xc6; the +0x118 writer is
  // 0x76 (handler 0x43976b) and 0x75 is the AND-clear at 0x44d025.
  {
    ScriptFixture f;
    const std::uint32_t C = 0x200;
    f.write(C,      {0x5a, 0x03});  f.writeF(C + 2, 4.5f);   // +0xe8
    f.write(C + 6,  {0xc7, 0x03});  f.writeF(C + 8, 7.25f);  // +0x104
    f.write(C + 12, {0x76});        f.writeW(C + 13, 9);     // +0x118=8
    f.write(C + 17, {0x75});        f.writeW(C + 18, 0x10);  // clr bit4
    f.write(C + 22, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    mdk::initObjectDefaults(o);
    o.col.flags148 |= 0x14;                  // set bits 2+4
    o.col.flags149 = static_cast<std::uint8_t>(o.col.flags148 >> 8);
    auto r = mdk::traversalObjectInitScript(f.env, o, C);
    CHECK(r.halted && !r.error);
    CHECK(near(o.fieldE8, 4.5, 1e-5));
    CHECK(near(o.field104, 7.25, 1e-5));
    CHECK(o.animLatch == 8);                 // 9 - 1
    CHECK((o.col.flags148 & 0x14) == 0x4);   // 0x75 cleared bit4
  }

  // Opcode 0xc6 — element-set declaration (handler 0x4394d0,
  // OBSERVED): {str8 prefix, u8 digitOfs, u32 hpThresh, u32 extra}.
  // Sets +0x149 bit5, stores the prefix at +0x302, digitOfs at
  // +0x306, extra at +0x30a, and fills all eight +0x30e/+0x31e
  // int16 slots with low16(hpThresh). Operands mirror the real
  // LEVEL3 HMO_1$XH1_DOOR script (XH1_KEY / 7 / 120 / 0).
  {
    ScriptFixture f;
    const std::uint32_t C = 0x200;
    f.write(C,      {0xc6});
    f.writeStr(C + 1, "XH1_KEY");                // 8 incl NUL -> C+9
    f.write(C + 10, {0x07});                     // digitOfs
    f.writeW(C + 11, 120);                       // hpThresh
    f.writeW(C + 15, 0xdead);                    // extra -> +0x30a
    f.write(C + 19, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    mdk::initObjectDefaults(o);
    auto r = mdk::traversalObjectInitScript(f.env, o, C);
    CHECK(r.halted && !r.error);
    CHECK((o.col.flags149 & 0x20) != 0);
    CHECK(o.homingPrefix == "XH1_KEY");
    CHECK(o.homingDigitOfs == 7);
    CHECK(o.field30a == 0xdead);
    CHECK(o.elemHp.size() == 8 && o.elemThresh.size() == 8);
    for (int i = 0; i < 8; ++i) {
      CHECK(o.elemHp[i] == 120);
      CHECK(o.elemThresh[i] == 120);
    }
  }

  // End-to-end: the 0xc6-declared element set drives the punch's
  // element-damage path — a script-initialised "XH1_KEY" object
  // (model element XH1_KEY1) takes per-element damage, exactly the
  // LEVEL3 HMO_1$XH1_DOOR wiring.
  {
    ScriptFixture sf;
    const std::uint32_t C = 0x200;
    sf.write(C,      {0xc6});
    sf.writeStr(C + 1, "XH1_KEY");
    sf.write(C + 10, {0x07});
    sf.writeW(C + 11, 120);
    sf.writeW(C + 15, 0);
    sf.write(C + 19, {0xff});

    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "PNCH");
    a->dyn.col.verts = f.verts.data();
    a->dyn.col.polys = f.polys.data();
    a->dyn.col.nodes = f.nodes.data();
    rt.cur = a; rt.cs.arena = &a->dyn.col;
    rt.cs.pos[2] = 10.0f; rt.motion.yawDeg = 0;
    rt.fieldC74 = 1; rt.ammo[0] = 0;           // uncharged, dmg 1
    mdk::DynamicObject& o = a->dyn.allocFront();
    o.model = makeHomingModel({{"XH1_KEY1", 0.0f}});
    o.setPosition(8, 0, 15);
    mdk::initObjectCollision(o);
    auto ir = mdk::traversalObjectInitScript(sf.env, o, C);
    CHECK(ir.halted && !ir.error);
    CHECK((o.col.flags149 & 0x20) != 0);
    const float wb[6] = {3, -5, 10, 13, 5, 20};
    std::memcpy(o.col.aabb, wb, sizeof(wb));
    mdk::playerPunch(rt, 1);
    CHECK(o.elemHp[0] == 119);                 // 120 - punchStep 1
    CHECK(o.elemHp[1] == 120);                 // untouched slots
    CHECK(o.field21d == 0xff && o.field21e == 0xff);
  }

  // End-to-end: traversalScriptSpawn runs the init script on a real
  // connector, then FUN_004555bc animates it open and the collision
  // toggle makes it passable. CHMO_2 -> HMO_3.
  {
    ScriptFixture f;
    f.arena->name = "CHMO_2";
    f.arena->dyn.name = "CHMO_2";
    mdk::TraversalArena* dst = travArenaAdd(f.rt, "HMO_3");
    f.rt.level.enemies.entries.push_back({"XCORDOOR", 0, false});
    mdk::RuntimeModel mdl = makePlatformModel("XCORDOOR", "ELEM", 0.0f);
    TestModelSrc src{{&mdl}};
    f.env.modelFor = testModelFor;
    f.env.modelCtx = &src;
    const std::uint32_t C = 0x200;
    writeDoorScript(f, C, 0x600, 0x620);
    f.rt.level.cmi.tables.resize(4);
    f.rt.level.cmi.tables[2].records.push_back(
        mkRec("CHMO_2$XCORDOOR", C));

    // variant 0 connector: cls=XCORDOOR, name=dest arena HMO_3.
    mdk::traversalScriptSpawn(f.env, 0, 0, 0, 0.0f, 0, "XCORDOOR",
                              "HMO_3", 0, 0);
    CHECK(f.env.seamsSpawned == 1);
    CHECK(!f.arena->dyn.storage.empty());
    mdk::DynamicObject& o = *f.arena->dyn.storage.front();
    CHECK(o.connDest == dst);
    CHECK(o.connState == 0x18);                  // closed | toggle
    CHECK(near(o.connRadius, 20.0, 1e-5));
    CHECK(o.animRecNear != nullptr && o.animRecFar != nullptr);
    CHECK(o.health == 0xfde8);
    CHECK(near(o.col.scale, 1.01021, 1e-5));
    CHECK((o.col.flags148 & 0x10) == 0);         // born solid

    // Player inside the radius -> the connector starts opening and
    // attaches the far-side arena (HMO_3) as the partner.
    f.rt.cur = f.arena;
    f.rt.cs.pos[0] = o.pos[0];
    f.rt.cs.pos[1] = o.pos[1];
    f.rt.cs.pos[2] = o.pos[2];
    mdk::traversalConnectorUpdate(o, f.rt);
    CHECK((o.connState & 0xf) == 2);             // opening
    CHECK(o.animRec == o.animRecNear);
    CHECK(f.rt.partner == dst && f.rt.partnerActive);

    // FUN_004555bc: the open anim (16 frames @ rate1, animRate30)
    // advances +0xdc by 1.0/frame to the frameCount-1 latch.
    for (int i = 0; i < 40 && !o.animDone(); ++i)
      mdk::traversalObjectAnimUpdate(o, nullptr);
    CHECK(o.animDone());
    CHECK(static_cast<std::uint16_t>(o.animLatch) == 0xff00u);

    // Done -> open; the +0x312 bit4 collision toggle sets the
    // sweep-skip bit so the door becomes passable.
    mdk::traversalConnectorUpdate(o, f.rt);
    CHECK((o.connState & 0xf) == 1);             // open
    CHECK(o.col.flags148 & 0x10);
  }
}

// ---------------------------------------------------------------------------
// Phase 11A — persistent per-object script VM: FUN_004388d8(obj) tick
// gated on +0x108 (OBSERVED FUN_004572ac body + handler disasm). The
// object ctx maps the arena VM's +0x220/+0x22c/+0x230 wait-resume trio
// to +0x108/+0x22c/+0x230 and +0x234/+0x244/+0x312 ctx slots to the
// DynamicObject script block.
// ---------------------------------------------------------------------------

void test_traversal_object_script() {
  const std::uint32_t C = 0x200;    // code image offset

  // --- ckpt + suspend: +0x108 persists across ticks --------------------
  {
    ScriptFixture f;
    f.write(C, {0x01, 0xff});                 // ckpt; suspend
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.col.named = true;
    o.field108 = f.image.data() + 4 + C;      // entry pc (image ptr)
    auto r1 = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r1.halted && !r1.error);
    CHECK(o.field108 ==
          static_cast<const void*>(f.image.data() + 4 + C + 1));
    // Second tick resumes at the checkpoint and suspends again.
    auto r2 = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r2.halted && !r2.error);
    CHECK(o.field108 ==
          static_cast<const void*>(f.image.data() + 4 + C + 1));
  }

  // --- wait: +0x22c decrements 1/30 per tick, resumes at +0x230 --------
  {
    ScriptFixture f;
    f.write(C, {0x40, 0x03});                 // wait, mode3 inline f32
    f.writeF(C + 2, 0.05f);                    // 0.05 s: 2 wait ticks
    f.write(C + 6, {0x01, 0xff});              // resume: ckpt; suspend
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r1 = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r1.waited && !r1.error);
    CHECK(near(o.field22c, 0.05, 1e-5));
    CHECK(o.field230 ==
          static_cast<const void*>(f.image.data() + 4 + C + 6));
    // tick2: 0.05 - 1/30 ~= 0.0167 > 0 -> still waiting, no fetch.
    auto r2 = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r2.waited && r2.instructions == 0);
    // tick3: crosses zero -> resumes at +0x230, runs ckpt+suspend.
    auto r3 = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r3.halted && !r3.error);
    CHECK(o.field108 ==
          static_cast<const void*>(f.image.data() + 4 + C + 7));
  }

  // --- stop (0x09) clears +0x108 + call depth --------------------------
  {
    ScriptFixture f;
    f.write(C, {0x09, 0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    o.scriptCallDepth = 2;                     // cleared by the stop
    o.scriptMark[0] = 7;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.stopped && o.field108 == nullptr);
    CHECK(o.scriptCallDepth == 0 && o.scriptMark[0] == 0);
    // Gate cleared -> subsequent ticks are no-ops.
    auto r2 = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r2.instructions == 0 && !r2.error);
  }

  // --- rcall/return (0xfc/0xfd): 4-deep stack, object locals -----------
  {
    ScriptFixture f;
    f.write(C, {0xfc, 0x01});                  // rcall n=1
    f.writeW(C + 2, 0x300);                    // -> callee
    f.write(C + 6, {0x01, 0xff});              // ckpt; suspend after
    // callee @0x300: flagop grp2 bit3 (obj local flags) ; return
    f.write(0x300, {0x44, 0x02, 0x03, 0xfd});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(o.scriptCallDepth == 0);             // returned
    CHECK(o.scriptFlagsLocal & (1u << 3));     // callee's flag write
    CHECK(o.field108 ==
          static_cast<const void*>(f.image.data() + 4 + C + 7));
  }

  // --- wcall (0x5f): pick=0 -> first positive weight -------------------
  {
    ScriptFixture f;
    // 5f 02 {w:0, a:0x320} {w:7, a:0x340} — entry0 weight 0, so the
    // cumulative>0 pick lands on the second entry.
    f.write(C, {0x5f, 0x02, 0x00});
    f.writeW(C + 3, 0x320);
    f.write(C + 7, {0x07});
    f.writeW(C + 8, 0x340);
    f.write(C + 12, {0xff});
    f.write(0x340, {0x44, 0x02, 0x04, 0xfd});  // callee: set bit4, ret
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(o.scriptFlagsLocal & (1u << 4));     // the w:7 callee ran
    CHECK(o.scriptCallDepth == 0);
  }

  // --- object-bound var write + read-back (0x41 / 0x32) ----------------
  {
    ScriptFixture f;
    f.write(C, {0x41, 0x02, 0x01});            // locals[1] = f32
    f.writeF(C + 3, 3.5f);
    f.write(C + 7, {0x32, 0x02, 0x01});        // +0x38 = locals[1]
    f.write(C + 10, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(near(o.scriptLocals[1], 3.5, 1e-5));
    CHECK(near(o.field38, 3.5, 1e-5));         // read back via varop
  }

  // --- path bind (0x02): +0xec, flags, frame, offset -------------------
  {
    ScriptFixture f;
    const std::uint32_t P = 0x500;             // path record
    // OBSERVED record form: {u32 count; entry[count] x 0x28 bytes}.
    // frame==0 + f2&2 -> +0xf0 = last entry's first u32 - 1
    // (0x438f96: *(u32*)(path + 4 + (count-1)*0x28) - 1).
    f.writeW(P, 2);                            // count
    f.writeW(P + 0x2c, 0x1234);                // entry[1].u32[0]
    f.write(C, {0x02});
    f.writeW(C + 1, P);                        // pathRef
    f.write(C + 5, {0x01, 0x02, 0x00, 0x00, 0x00});  // f1 f2 frame mode
    f.writeF(C + 10, 1.0f);                    // mode0 -> 3 f32 offsets
    f.writeF(C + 14, 2.0f);
    f.writeF(C + 18, 3.0f);
    f.write(C + 22, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(o.fieldEC ==
          static_cast<const void*>(f.image.data() + 4 + P));
    CHECK((o.col.flags149 & 0x2) != 0);        // f1 bit0
    CHECK((o.col.flags149 & 0x4) == 0);        // f2 bit0 clear
    CHECK(o.fieldE8 == -1.0f);                 // f2 bit1 -> -1.0
    CHECK(o.fieldF0 == 4659.0f);               // 0x1234 - 1
    CHECK(o.fieldF4[0] == 1.0f && o.fieldF4[2] == 3.0f);
    CHECK(o.fieldE6 == -1);                    // path cursor reset
  }

  // --- subtype set (0x4e): +0x120..128, +0x11e, unbinds path -----------
  {
    ScriptFixture f;
    f.write(C, {0x4e});
    f.writeW(C + 1, 0x111);
    f.writeW(C + 5, 0x222);
    f.writeW(C + 9, 0x333);
    f.write(C + 13, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.fieldEC = f.image.data() + 4;            // path bound -> cleared
    o.field2a0 = 9; o.field2a1 = 9;
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(o.field11e == 0x4e);
    CHECK(o.field120[0] == std::bit_cast<float>(0x111u) &&
          o.field120[2] == std::bit_cast<float>(0x333u));
    CHECK(o.fieldEC == nullptr);
    CHECK(o.field2a0 == 0 && o.field2a1 == 0);
  }

  // --- death handoff: FUN_00458140 -> the tick runs +0x110 --------------
  {
    ScriptFixture f;
    f.write(C, {0x44, 0x02, 0x05, 0x09});      // set local bit5; stop
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.col.named = true;
    o.health = 4;
    o.field110 = f.image.data() + 4 + C;       // pending death script
    const float hp[3] = {0, 0, 0};
    mdk::objectDeathBoundary(f.rt, o, hp, 0.0f);
    CHECK(o.field110 == nullptr);
    CHECK(o.field108 ==
          static_cast<const void*>(f.image.data() + 4 + C));
    CHECK(o.health == 0 && (o.col.flags148 & 0x20));
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.stopped && !r.error);
    CHECK(o.scriptFlagsLocal & (1u << 5));     // the death script ran
    CHECK(o.field108 == nullptr);              // 0x09 cleared it
  }

  // --- bounds: a foreign PC kills the script with a diagnostic ---------
  {
    ScriptFixture f;
    const std::byte foreign = std::byte(0xff);
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = &foreign;                     // outside the image
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.error && o.field108 == nullptr);
    CHECK(!f.diag.empty());
  }

  // --- runaway loop hits the 1000 cap ----------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x0c, 0x01});                  // rgoto n1 -> self
    f.writeW(C + 2, C);
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.error && r.instructions == 1000);
    CHECK(r.diag.find("looped") != std::string::npos);
    CHECK(o.field108 == nullptr);
  }

  // --- init stays transient: traversalObjectInitScript clears +0x108 ---
  {
    ScriptFixture f;
    f.write(C, {0x01, 0xff});                  // ckpt would set +0x108
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    auto r = mdk::traversalObjectInitScript(f.env, o, C);
    CHECK(r.halted && !r.error);
    CHECK(o.field108 == nullptr);              // FUN_004566f0 clears
  }

  // --- DTI spawn binds +0x108 via the table-0 script source ------------
  {
    mdk::RuntimeModel xg = makePlatformModel("XG", "ELEM", 0.0f);
    TestModelSrc src{{&xg}};
    mdk::DynamicArena ar;
    ar.name = "HMO_1";
    mdk::DtiArenaRecord rec = {};
    rec.subRecords = {makeSpawnRec(2, (0u << 16) | 7u, 0, 0, 0, "XG")};
    std::byte fakeImg[8] = {};
    auto scriptFor = [](const char* an, const char* mn,
                        std::uint16_t id, void* ctx) -> const void* {
      return (std::string(an) == "HMO_1" && std::string(mn) == "XG" &&
              id == 7)
                 ? static_cast<const void*>(ctx)
                 : nullptr;
    };
    std::vector<mdk::DynamicObject*> sp;
    const int n = mdk::spawnArenaObjects(
        ar, rec, testModelFor, &src, &sp, scriptFor, fakeImg);
    CHECK(n == 1 && sp.size() == 1);
    CHECK(sp[0]->field108 == static_cast<const void*>(fakeImg));
    // No source -> +0x108 stays null.
    mdk::DynamicArena ar2;
    ar2.name = "HMO_1";
    const int n2 =
        mdk::spawnArenaObjects(ar2, rec, testModelFor, &src, nullptr);
    CHECK(n2 == 1 && ar2.storage.front()->field108 == nullptr);
  }

  // -- obj op 0x28: yaw accumulate, raw (dead-wrap) store -----------------
  {
    ScriptFixture f;
    f.write(C, {0x28, 0x03});               // mode3 inline f32
    f.writeF(C + 2, 200.0f);                // XGREN-style turn rate
    f.write(C + 6, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(near(o.yawDeg, 200.0 * (1.0 / 30.0), 1e-4));
    // 350 + 6.667 = 356.667 stays raw — the original's wrap loops are
    // unreachable (OBSERVED dead code at 0x441cca..).
    o.yawDeg = 350.0f;
    o.field108 = f.image.data() + 4 + C;
    auto r2 = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(near(o.yawDeg, 356.6667, 1e-3));
  }

  // -- obj op 0x3e: facing-angle compare link ------------------------------
  {
    ScriptFixture f;
    // {0x3e, kind, f32 a, linkage} — camera due +y -> bearing 90.
    f.write(C, {0x3e, 0x02});               // kind 2 = angle > a
    f.writeF(C + 2, 45.0f);
    f.write(C + 6, {0x0c});                 // rgoto
    f.writeW(C + 7, 0x300);
    f.write(C + 11, {0xff});
    f.write(0x300, {0x44, 0x02, 0x03, 0xff}); // mark scriptFlagsLocal bit2
    f.rt.camera.pose.pos[1] = 10.0f;
    // Obj yaw 0 -> |90 - 0| = 90 > 45 -> goto fires.
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && (o.scriptFlagsLocal & 8));
    // Obj yaw 80 -> |90 - 80| = 10 <= 45 -> fallthrough to 0xff.
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    o2.yawDeg = 80.0f;
    o2.field108 = f.image.data() + 4 + C;
    auto r2 = mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(r2.halted && !r2.error && !(o2.scriptFlagsLocal & 8));
    // Wrap normalize: yaw 350 -> 90-350=-260 -> +360 = 100 -> >45 fires.
    mdk::DynamicObject& o3 = f.arena->dyn.allocFront();
    o3.yawDeg = 350.0f;
    o3.field108 = f.image.data() + 4 + C;
    auto r3 = mdk::traversalObjectScriptTick(f.env, o3);
    CHECK(r3.halted && !r3.error && (o3.scriptFlagsLocal & 8));
    // >180 fold: camera due -y (bearing 270), yaw 0 -> |270|>180 ->
    // 360-270 = 90 -> fires. (same normalization as FUN_0045ad40).
    f.rt.camera.pose.pos[1] = -10.0f;
    mdk::DynamicObject& o4 = f.arena->dyn.allocFront();
    o4.field108 = f.image.data() + 4 + C;
    auto r4 = mdk::traversalObjectScriptTick(f.env, o4);
    CHECK(r4.halted && (o4.scriptFlagsLocal & 8));
  }

  // -- obj op 0x68: aim at camera + spread --------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x68});
    f.writeF(C + 1, 100.0f);                // spread 100 = no jitter
    f.write(C + 5, {0xff});
    f.rt.camera.pose.pos[0] = 100.0f;
    f.rt.camera.pose.pos[1] = 0.0f;
    f.rt.camera.pose.pos[2] = 53.0f;        // dz = 53+3 = 56
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    const std::uint32_t rng0 = f.rt.rngState;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(near(o.yawDeg, 0.0, 1e-4));       // due +x
    CHECK(near(o.bankDeg, 29.2497, 1e-3));  // atan2(56, 100)
    CHECK(f.rt.rngState == rng0);           // spread==100 draws no rand
    // spread 75 -> two rand(0x14) draws, jitter scaled by 25/d3.
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    o2.field108 = f.image.data() + 4 + C;
    f.writeF(C + 1, 75.0f);
    f.rt.rngState = 1;
    auto r2 = mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(r2.halted && !r2.error);
    // Expected jitter replays the port's own RNG draws on a side state.
    const double d3 = std::sqrt(10000.0 + 3136.0) + 0.1;
    std::uint32_t side = 1;
    const int j1 = mdk::enemyRandBelow(side, 0x14);
    const int j2 = mdk::enemyRandBelow(side, 0x14);
    const double eyaw = 0.0 + (j1 - 10) * 25.0 / d3;
    const double ebank = std::atan2(56.0, 100.0) * 180.0 / M_PI +
                         (j2 - 10) * 25.0 / (d3 * 4.0);
    CHECK(near(o2.yawDeg, eyaw, 1e-3));
    CHECK(near(o2.bankDeg, ebank, 1e-3));
    CHECK(f.rt.rngState != 1);              // two draws consumed
  }

  // -- obj op 0x6b: voice/sfx rebind seam ----------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x6b});
    f.writeStr(C + 1, "SHOOT");             // 5+1+1=7 bytes
    f.write(C + 8, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(o.field15c == "SHOOT");
    CHECK(f.rt.seams.fireSoundCalls == 1);  // FUN_00402160 respawn
    // Bound handle -> release + respawn (2 seam calls), field158 null.
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    mdk::DynamicObject& voice = f.arena->dyn.allocFront();
    o2.field158 = &voice;
    o2.field108 = f.image.data() + 4 + C;
    auto r2 = mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(r2.halted && !r2.error);
    CHECK(o2.field158 == nullptr && o2.field15c == "SHOOT");
    CHECK(f.rt.seams.fireSoundCalls == 3);
  }

  // -- obj op 0x6d: player damage (FUN_00467888) ----------------------------
  {
    ScriptFixture f;
    f.write(C, {0x6d, 0x02, 0xff});           // BOLT's hit: dmg 2
    f.rt.fieldHealth = 100;                 // gates open
    f.rt.difficulty = 1;
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(f.rt.fieldHealth == 98);          // health -= 2 (Skill 1)
    CHECK(f.rt.fieldDac == 75);             // 2*25=50 floored at 75
    CHECK(near(f.rt.vert.landingAccum, 2.0));
    // Skill 0 -> 2d/3 floor 1; Skill 2 -> 2d.
    f.rt.fieldHealth = 100; f.rt.fieldDac = 0; f.rt.vert.landingAccum = 0;
    f.rt.difficulty = 0;
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    o2.field108 = f.image.data() + 4 + C;
    mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(f.rt.fieldHealth == 99);          // max(2*2/3,1) = 1
    f.rt.fieldHealth = 100; f.rt.difficulty = 2;
    mdk::DynamicObject& o3 = f.arena->dyn.allocFront();
    o3.field108 = f.image.data() + 4 + C;
    mdk::traversalObjectScriptTick(f.env, o3);
    CHECK(f.rt.fieldHealth == 96);          // 2*2 = 4
    // Suppressed (fieldE10 > 0) -> no damage, landingAccum cleared.
    f.rt.fieldHealth = 100; f.rt.fieldE10 = 0.5f;
    f.rt.vert.landingAccum = 9;
    mdk::DynamicObject& o4 = f.arena->dyn.allocFront();
    o4.field108 = f.image.data() + 4 + C;
    mdk::traversalObjectScriptTick(f.env, o4);
    CHECK(f.rt.fieldHealth == 100 && f.rt.vert.landingAccum == 0);
  }

  // -- obj op 0x86: pitch drift, raw (dead-wrap) store ---------------------
  {
    ScriptFixture f;
    f.write(C, {0x86, 0x03});
    f.writeF(C + 2, 200.0f);
    f.write(C + 6, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.pitchDeg = 350.0f;
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(near(o.pitchDeg, 356.6667, 1e-3)); // unwrapped (OBSERVED)
  }

  // -- BOLT-shaped script: aim-if-facing<45 then touch/pitch ---------------
  {
    ScriptFixture f;
    // {rate34; angleLink>45 -> skip aim; aim68; ckpt; pitchDrift; end}
    // models the L8 GUNT_2$XGREN_0 pipeline end-to-end.
    f.write(C, {0x35, 0x03});               // rate34 mode3
    f.writeF(C + 2, 75.0f);
    f.write(C + 6, {0x3e, 0x02});           // angle > 45 -> skip aim
    f.writeF(C + 8, 45.0f);
    f.write(C + 12, {0x0c});
    f.writeW(C + 13, C + 22);               // goto ckpt
    f.write(C + 17, {0x68});                // aim68 spread 75
    f.writeF(C + 18, 75.0f);
    f.write(C + 22, {0x01});                // ckpt
    f.write(C + 23, {0x86, 0x03});          // pitchDrift 180
    f.writeF(C + 25, 180.0f);
    f.write(C + 29, {0xff});
    f.rt.camera.pose.pos[0] = 50.0f;        // bearing 0, obj yaw 0
    f.rt.camera.pose.pos[2] = -50.0f;       // dz = -47
    f.rt.rngState = 7;
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(near(o.field34, 75.0));
    CHECK(near(o.pitchDeg, 180.0 * (1.0 / 30.0), 1e-4));
    CHECK(f.rt.rngState != 7);              // angle<=45 -> aim ran
  }

  // -- golden: enemy 0x3d spawn -> BOLT script -> move -> player ------
  // contact -> op-0x6d damage -> teardown. Models the real L8
  // GUNT_2$XG chain (spawn pc 0x1705b; hit pc 0x1717c:
  // `6d 02; 4c 0; 6e` = dmg 2, clear death-ref, teardown — OBSERVED).
  {
    ScriptFixture f;
    const std::uint32_t P = 0x300;          // BOLT persistent script
    const std::uint32_t TAIL = 0x340;
    const std::uint32_t HIT = 0x360;
    const std::uint32_t DIE = 0x380;        // death-ref target
    // Enemy script: 0x3d mode0 refIdx0 "BOLT" pc=P; end.
    f.write(C, {0x3d, 0x00, 0x00});
    f.writeStr(C + 3, "BOLT");
    f.writeW(C + 9, P);
    f.write(C + 13, {0xff});
    // BOLT head (0x1705b): deathRef; rate34 75; life302 5; set148;
    // aim68 spread 100 (no jitter); rgoto TAIL.
    f.write(P, {0x4c}); f.writeW(P + 1, DIE);
    f.write(P + 5, {0x35, 0x03}); f.writeF(P + 7, 75.0f);
    f.write(P + 11, {0x6a}); f.writeF(P + 12, 5.0f);
    f.write(P + 16, {0x61, 0x00});
    f.write(P + 18, {0x68}); f.writeF(P + 19, 100.0f);
    f.write(P + 23, {0x0c, 0x01}); f.writeW(P + 25, TAIL);
    f.write(P + 29, {0xff});
    // TAIL (0x17162): touchLink->HIT; pitchDrift; end (re-runs each
    // frame while field108 sits at TAIL).
    f.write(TAIL, {0x6c, 0x0c}); f.writeW(TAIL + 2, HIT);
    f.write(TAIL + 6, {0x86, 0x03}); f.writeF(TAIL + 8, 180.0f);
    f.write(TAIL + 12, {0xff});
    // HIT (0x1717c): dmg 2; deathRef=0; teardown.
    f.write(HIT, {0x6d, 0x02, 0x4c}); f.writeW(HIT + 3, 0);
    f.write(HIT + 7, {0x6e, 0xff});
    // DIE (0x1718e): silent teardown on wall/expiry.
    f.write(DIE, {0x6e, 0xff});

    // Enemy gun object with a muzzle refpoint at the origin.
    mdk::DynamicObject& xg = f.arena->dyn.allocFront();
    xg.col.named = true;
    xg.pos[0] = 0.0f; xg.pos[1] = 0.0f; xg.pos[2] = 0.0f;
    xg.worldRef[0][0] = 0.0f;               // muzzle = origin
    xg.worldRef[0][1] = 0.0f;
    xg.worldRef[0][2] = 0.0f;
    xg.field108 = f.image.data() + 4 + C;

    f.rt.level.enemies.entries.push_back({"BOLT", 0, false});
    mdk::RuntimeModel bolt = makePlatformModel("BOLT", "BODY", 0.0f);
    TestModelSrc src{{&bolt}};
    f.env.modelFor = testModelFor;
    f.env.modelCtx = &src;

    // Player/camera at (100, 0, 53) — aim68 adds +3 to camera z.
    f.rt.camera.pose.pos[0] = 100.0f;
    f.rt.camera.pose.pos[1] = 0.0f;
    f.rt.camera.pose.pos[2] = 53.0f;
    f.rt.fieldHealth = 100;
    f.rt.difficulty = 1;
    const float pbox[6] = {20.0f, -5.0f, 0.0f, 60.0f, 5.0f, 40.0f};
    std::copy(pbox, pbox + 6, f.rt.cs.playerBox);

    // Tick 1: enemy script spawns the projectile.
    auto r1 = mdk::traversalObjectScriptTick(f.env, xg);
    CHECK(r1.halted && !r1.error);
    mdk::DynamicObject& b = *f.arena->dyn.storage.front();
    CHECK(b.col.named);
    CHECK(b.field11e == 0x3d);
    CHECK(b.field108 == f.image.data() + 4 + P);
    CHECK(near(b.pos[0], 0.0f) && near(b.pos[2], 0.0f));

    // Tick 2: BOLT head runs — aims at the camera, suspends at TAIL.
    auto r2 = mdk::traversalObjectScriptTick(f.env, b);
    CHECK(r2.halted && !r2.error);
    CHECK(near(b.field34, 75.0));
    CHECK(b.yawDeg == 0.0f);                // due +x toward camera
    CHECK(b.field108 == f.image.data() + 4 + TAIL);

    // Move: subtype-0x3d flies toward the player box and touches it.
    const int teardowns0 = f.rt.seams.objectTeardownCalls;
    mdk::objectSubtypeUpdate(f.rt, b, f.arena->dyn, 0.5f);
    CHECK((b.col.flags14c & 4) != 0);       // player contact set
    CHECK(b.pos[0] >= 19.0f);               // clamped at the box

    // Tick 3: touchLink fires -> damage -> clear deathRef -> teardown.
    auto r3 = mdk::traversalObjectScriptTick(f.env, b);
    CHECK(r3.halted && !r3.error);
    CHECK(f.rt.fieldHealth == 98);          // 2 base at Skill 1
    CHECK(f.rt.fieldDac == 75);             // accumulator floor
    CHECK(!b.col.named);                    // 0x6e teardown wiped it
    CHECK(b.health == 0);
    CHECK(f.rt.seams.objectTeardownCalls == teardowns0 + 1);

    // No repeat hit — the wiped record is inert on the next tick.
    auto r4 = mdk::traversalObjectScriptTick(f.env, b);
    (void)r4;
    CHECK(f.rt.fieldHealth == 98);
  }

  // -- XT_MISS hit variant: `6d 14; 4c 0; 10 0` (dmg 20 + health=0) --
  // OBSERVED at 0x42a9: op-0x10 sets +0x10 health to zero instead of
  // 0x6e — the object dies through the health boundary, not teardown.
  {
    ScriptFixture f;
    const std::uint32_t HIT = 0x320;
    // head: touchLink->HIT; pitchDrift; end.
    f.write(C, {0x6c, 0x0c}); f.writeW(C + 2, HIT);
    f.write(C + 6, {0x86, 0x03}); f.writeF(C + 8, 90.0f);
    f.write(C + 12, {0xff});
    // HIT: dmg 20; deathRef=0; setHealth 0.
    f.write(HIT, {0x6d, 0x14, 0x4c}); f.writeW(HIT + 3, 0);
    f.write(HIT + 7, {0x10, 0x00, 0x00, 0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.col.named = true;
    o.field11e = 0x3d;                      // spawned-projectile subtype
    o.health = 5;
    o.col.flags14c |= 4;                    // player contact already set
    o.field108 = f.image.data() + 4 + C;
    f.rt.fieldHealth = 100;
    f.rt.difficulty = 1;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error);
    CHECK(f.rt.fieldHealth == 80);          // 20 base at Skill 1
    CHECK(o.health == 0);                   // 0x10 — dies via health
    CHECK(o.col.named);                     // no 0x6e wipe this path
  }
}

// ---------------------------------------------------------------------------
// Phase 11A — object animation: FUN_004555bc driver + FUN_00455890
// vertex/ref-point applier + FUN_00455c48 rigid channel (OBSERVED,
// BUILD_A disasm + real record bytes). Synthetic records only.
// ---------------------------------------------------------------------------

namespace {

// Append helpers for the little-endian record builder.
void aW(std::vector<std::uint8_t>& b, std::uint32_t v) {
  b.push_back(static_cast<std::uint8_t>(v));
  b.push_back(static_cast<std::uint8_t>(v >> 8));
  b.push_back(static_cast<std::uint8_t>(v >> 16));
  b.push_back(static_cast<std::uint8_t>(v >> 24));
}
void aH(std::vector<std::uint8_t>& b, std::int16_t v) {
  b.push_back(static_cast<std::uint8_t>(v));
  b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void aF(std::vector<std::uint8_t>& b, float f) {
  std::uint32_t v; std::memcpy(&v, &f, 4); aW(b, v);
}
void aV3(std::vector<std::uint8_t>& b, float x, float y, float z) {
  aF(b, x); aF(b, y); aF(b, z);
}
void aName(std::vector<std::uint8_t>& b, const char* s) {
  for (int i = 0; i < 12; ++i)
    b.push_back(static_cast<std::uint8_t>(s[i] ? s[i] : 0));
}

// Object bound to `rec` the way ops 0x03/0x3b leave it: armed at
// +0xdc=-1.0 / +0xe4=-1 / +0x118=-1, rate multiplier +0xe0=30.
mdk::DynamicObject& animObject(mdk::DynamicArena& da,
                               const std::vector<std::uint8_t>& rec) {
  da.storage.push_back(std::make_unique<mdk::DynamicObject>());
  mdk::DynamicObject& o = *da.storage.back();
  o.col.named = true;
  o.enemyIndex = 0;                        // +0x04 != -1: normal path
  o.animRec = rec.data();
  o.animAcc = -1.0f;
  o.animFrame = -1;
  o.animLatch = -1;
  o.animRate = 30.0f;
  // Identity object transform for the root-key rotation.
  o.col.xform[0] = o.col.xform[4] = o.col.xform[8] = 1.0f;
  return o;
}

} // namespace

void test_enemy_runtime() {
  // -- FUN_0047d2b5 / FUN_00401ed4 — the MSVC CRT rand LCG -------------
  {
    std::uint32_t s = 1;
    const std::uint32_t r0 = mdk::enemyRandNext(s);
    CHECK(s == 0x41c67ea6u);               // 1*0x41c64e6d + 0x3039
    CHECK(r0 == ((0x41c67ea6u >> 16) & 0x7fff));   // 16838
    s = 1;
    CHECK(mdk::enemyRandBelow(s, 10000) ==
          static_cast<int>((16838u * 10000u) >> 15));   // 5137
    s = 1;
    for (int i = 0; i < 8; ++i)
      CHECK(mdk::enemyRandBelow(s, 7) >= 0 &&
            mdk::enemyRandBelow(s, 7) < 7);
  }

  // -- bearingDeg / sincosDeg (FUN_00437f30 / FUN_00437f98) ------------
  {
    CHECK(near(mdk::bearingDeg(1, 0), 90.0) &&
          near(mdk::bearingDeg(0, 0), 0.0) &&
          near(mdk::bearingDeg(-1, 0), 270.0) &&
          near(mdk::bearingDeg(0, -1), 180.0));
    float sn, cs;
    mdk::sincosDeg(90.0f, &sn, &cs);
    CHECK(near(sn, 1.0, 1e-5) && near(cs, 0.0, 1e-5));
    mdk::sincosDeg(450.0f, &sn, &cs);      // no wrap inside the helper
    CHECK(near(sn, 1.0, 1e-5) && near(cs, 0.0, 1e-5));  // sin(450)=1
  }

  // -- pathSample — the FUN_00456bc8 cubic Hermite --------------------
  {
    // {i32 count=3; entry x 0x28} — frames 0/10/20, x = frame, zero
    // tangents (each entry: {frame,pos[3],tanIn[3],tanOut[3]}).
    alignas(4) std::int32_t rec[1 + 3 * 10] = {};
    rec[0] = 3;
    auto setEntry = [&](int i, std::int32_t frame, float x) {
      rec[1 + i * 10] = frame;
      float fx = x;
      std::memcpy(&rec[1 + i * 10 + 1], &fx, 4);   // pos.x (field 1)
    };
    setEntry(0, 0, 0.0f);
    setEntry(1, 10, 10.0f);
    setEntry(2, 20, 20.0f);
    float pt[3];
    CHECK(mdk::pathSample(rec, 5.0f, pt) && near(pt[0], 5.0) &&
          near(pt[1], 0.0) && near(pt[2], 0.0));
    CHECK(mdk::pathSample(rec, 15.0f, pt) && near(pt[0], 15.0));
    CHECK(mdk::pathSample(rec, 10.0f, pt) && near(pt[0], 10.0)); // tie
    // No clamping: t=1.5 extrapolates along the Hermite basis.
    CHECK(mdk::pathSample(rec, 25.0f, pt) && near(pt[0], 10.0));
    CHECK(near(mdk::pathFirstFrame(rec), 0.0) &&
          near(mdk::pathLastFrame(rec), 20.0));
    CHECK(!mdk::pathSample(nullptr, 0.0f, pt));
  }

  // -- objectPathSnap / objectPathFollow (FUN_00457264/FUN_00456d28) --
  {
    alignas(4) std::int32_t rec[1 + 3 * 10] = {};
    rec[0] = 3;
    auto setEntry = [&](int i, std::int32_t frame, float x) {
      rec[1 + i * 10] = frame;
      float fx = x;
      std::memcpy(&rec[1 + i * 10 + 1], &fx, 4);
    };
    setEntry(0, 0, 0.0f);
    setEntry(1, 10, 10.0f);
    setEntry(2, 20, 20.0f);
    const float player[3] = {0, 0, 0};

    mdk::DynamicObject o;
    o.fieldEC = rec;
    o.fieldF0 = 5.0f;
    o.fieldF4[0] = 1.0f; o.fieldF4[1] = 2.0f; o.fieldF4[2] = 3.0f;
    mdk::objectPathSnap(o);
    CHECK(near(o.pos[0], 6.0) && near(o.pos[1], 2.0) &&
          near(o.pos[2], 3.0));

    // Frame advance: step = fieldE8 * 1.0, sample at the new frame.
    // Zero tangents -> smoothstep Hermite: h01(0.1) = 0.028 -> x 0.28.
    o.fieldF0 = 0.0f; o.fieldE8 = 1.0f; o.fieldE6 = -1;
    o.fieldF4[0] = o.fieldF4[1] = o.fieldF4[2] = 0.0f;
    mdk::objectPathFollow(o, player, 0.0f);
    CHECK(near(o.fieldF0, 1.0) && near(o.pos[0], 0.28, 1e-3));

    // Cursor halt: fieldE6 == FRNDINT(+0xf0) freezes the follower.
    o.fieldE6 = 2; o.fieldF0 = 1.0f;
    mdk::objectPathFollow(o, player, 0.0f);          // lands on cursor
    CHECK(near(o.fieldF0, 2.0));
    o.pos[0] = 99.0f;
    mdk::objectPathFollow(o, player, 0.0f);          // halted
    CHECK(near(o.fieldF0, 2.0) && near(o.pos[0], 99.0));

    // Release: +0x149&4 unbinds and clamps to last-1 (endFrame-2).
    mdk::DynamicObject rel;
    rel.fieldEC = rec; rel.fieldF0 = 19.0f; rel.fieldE8 = 1.0f;
    rel.fieldE6 = -1; rel.col.flags149 = 0x4;
    mdk::objectPathFollow(rel, player, 0.0f);
    CHECK(rel.fieldEC == nullptr && near(rel.fieldF0, 18.0));

    // Ghost: path displacement becomes +0x294 impulse, pos restored.
    mdk::DynamicObject gh;
    gh.fieldEC = rec; gh.fieldF0 = 0.0f; gh.fieldE8 = 1.0f;
    gh.fieldE6 = -1; gh.col.flags14b = 0x8;
    mdk::objectPathFollow(gh, player, 0.0f);
    CHECK(near(gh.pos[0], 0.0) &&
          near(gh.animImpulse[0], 0.28 * 30.0, 1e-3));
  }

  // -- seekOpcodeTail — the shared 0x4e/0x2b seek clear ---------------
  {
    mdk::DynamicObject o;
    o.field2a0 = 9; o.field2a1 = 9; o.field2a4 = 1.0f;
    o.field2a8 = 2.0f; o.field2ac = 3.0f;
    o.col.flags14c |= 0x08;
    mdk::seekOpcodeTail(o);
    CHECK(o.field2a0 == 0 && o.field2a1 == 0 && o.field2a4 == 0.0f &&
          o.field2a8 == 0.0f && o.field2ac == 0.0f &&
          (o.col.flags14c & 0x08) == 0);
  }

  // -- objectOpSeekCamera — camera-relative seek target ----------------
  {
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "SEEK");
    rt.cur = a;
    rt.camera.pose.pos[0] = 100.0f;
    rt.camera.pose.pos[1] = 0.0f;
    rt.camera.pose.pos[2] = 50.0f;
    mdk::DynamicObject& o = a->dyn.allocFront();
    o.col.named = true;
    // bearing 0 (camera due +x): point = pos + fwd*(d*0.01)*(cos,sin),
    // z = cam.z.
    mdk::objectOpSeekCamera(rt, o, a->dyn, 10.0f, 0.0f);
    const float d = std::sqrt(100.0f * 100.0f + 50.0f * 50.0f);
    CHECK(near(o.field120[0], 10.0 * d * 0.01, 1e-3) &&
          near(o.field120[1], 0.0, 1e-3) &&
          near(o.field120[2], 50.0));
    CHECK(o.field11e == 0x2b && o.fieldEC == nullptr);
  }

  // -- objectOpSeekAway — flee + clamp + jitter (0x442de1) -------------
  {
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "FLEE");
    mdk::DynamicObject anchor;
    anchor.pos[0] = 10.0f; anchor.pos[1] = 0.0f; anchor.pos[2] = 4.0f;
    rt.cmdObj60 = &anchor;
    mdk::DynamicObject& o = a->dyn.allocFront();
    o.col.named = true;
    // dist clamps to 6; rng state 1 -> draw 16838 -> jit =
    // (16838-0x4000) * 6.103515625e-5 * 6.
    mdk::objectOpSeekAway(rt, o, 20.0f);
    const float jit =
        static_cast<float>(16838 - 0x4000) * 6.103515625e-5f * 6.0f;
    CHECK(near(o.field120[0], -10.0, 1e-4) &&
          near(o.field120[1], -jit, 1e-4) &&
          near(o.field120[2], 4.0 + 2.5 + o.zBias, 1e-4));
    CHECK(o.field11e == 0x4e && o.fieldEC == nullptr);
    // No anchor -> no-op.
    mdk::DynamicObject o2;
    rt.cmdObj60 = nullptr;
    mdk::objectOpSeekAway(rt, o2, 5.0f);
    CHECK(o2.field11e == 0);
  }

  // -- objectOrbit — pendulum step (FUN_00457ab8) ----------------------
  {
    mdk::DynamicObject o;
    o.bankDeg = 30.0f;
    o.field1c[0] = 0.0f; o.field1c[1] = 0.0f; o.field1c[2] = 0.0f;
    o.field302 = std::bit_cast<std::uint32_t>(0.0f);   // phase
    o.field306 = std::bit_cast<std::uint32_t>(10.0f);  // radius
    o.field30a = std::bit_cast<std::uint32_t>(1.0f);   // accel coeff
    o.field30e = 0;                                  // target yaw
    o.yawDeg = 0.0f;
    const float player[3] = {0, 0, 0};
    mdk::objectOrbit(o, player, 1.0f / 30.0f);
    // phase = 0 - sin30*1*1 = -0.5; bank += phase*1 -> 29.5.
    CHECK(near(std::bit_cast<float>(o.field302), -0.5) &&
          near(o.bankDeg, 29.5, 1e-4));
    // pos = center + yaw-rotated (sin30*10, -cos30*10): (5,0,-8.66).
    CHECK(near(o.pos[0], 5.0, 1e-4) && near(o.pos[1], 0.0, 1e-4) &&
          near(o.pos[2], -8.660254, 1e-4));
    CHECK(o.field2d0 == 1 && o.field2d1 == 1);
  }

  // -- objectRollRide — rawMatrix premultiply (FUN_0045d578) -----------
  {
    mdk::DynamicObject o;
    o.prevPos[0] = 0.0f; o.prevPos[1] = 0.0f;
    o.pos[0] = 1.0f; o.pos[1] = 0.0f;
    o.connMaskLock = std::bit_cast<std::uint32_t>(1.0f); // +0x326 cnt=1
    o.rawMatrix[0] = o.rawMatrix[4] = o.rawMatrix[8] = 1.0f;
    mdk::objectRollRide(o);
    // dx=1 -> b'=90*(1/90)*rate with rate = 360/(2pi); R = Ry(b').
    const float rate = 360.0f / 6.28318531f;
    const float rad = rate * (3.14159265358979323846 / 180.0);
    CHECK(near(o.rawMatrix[0], std::cos(rad), 1e-4) &&
          near(o.rawMatrix[2], std::sin(rad), 1e-4) &&
          near(o.rawMatrix[4], 1.0) &&
          near(o.rawMatrix[6], -std::sin(rad), 1e-4) &&
          near(o.rawMatrix[8], std::cos(rad), 1e-4));
  }

  // -- objectArenaTransfer — connector yaw flip (FUN_004574d0) ---------
  {
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "SRC");
    mdk::TraversalArena* b = travArenaAdd(rt, "DST");
    rt.cur = a;
    mdk::DynamicObject& o = a->dyn.allocFront();
    o.col.named = true;
    o.col.flags14a = 0x10;               // connector
    o.yawDeg = 270.0f;
    o.pendingArena = &b->dyn;
    b->dyn.owner = a;                    // dst owned by current
    CHECK(mdk::objectArenaTransfer(rt, o, a->dyn) == 1);
    CHECK(o.arena == &b->dyn && near(o.yawDeg, 90.0));  // +180 wrap
    // Same-arena pend: warn + clear, returns 0.
    o.pendingArena = &b->dyn;
    CHECK(mdk::objectArenaTransfer(rt, o, b->dyn) == 0 &&
          o.pendingArena == nullptr);
  }

  const std::uint32_t C = 0x200;    // code image offset (script tests)

  // -- obj op 0x3a: varop -> +0xe0 animRate ----------------------------
  {
    ScriptFixture f;
    f.write(C, {0x3a, 0x03});               // mode3 inline f32
    f.writeF(C + 2, 5.5f);
    f.write(C + 6, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && near(o.animRate, 5.5));
  }
  {
    ScriptFixture f;                        // mode2 = ctx local slot
    f.write(C, {0x41, 0x02, 0x01});         // locals[1] = f32
    f.writeF(C + 3, 7.25f);
    f.write(C + 7, {0x3a, 0x02, 0x01});     // +0xe0 = locals[1]
    f.write(C + 10, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && near(o.animRate, 7.25));
  }

  // -- obj op 0xb0: +0x14a&4 conditional link ---------------------------
  {
    ScriptFixture f;
    f.write(C, {0xb0, 0x0c});               // 0x0c goto target
    f.writeW(C + 2, 0x300);
    f.write(C + 6, {0xff});                 // fallthrough
    f.write(0x300, {0x44, 0x02, 0x03, 0xff});  // set local bit3, end
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.col.flags14a = 0x04;
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && (o.scriptFlagsLocal & 8));
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    o2.col.flags14a = 0;                    // bit clear -> fallthrough
    o2.field108 = f.image.data() + 4 + C;
    auto r2 = mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(r2.halted && !r2.error && !(o2.scriptFlagsLocal & 8));
  }

  // -- obj op 0x12: timed link — mark >= wait*30 ------------------------
  {
    ScriptFixture f;
    f.write(C, {0x12});
    f.writeF(C + 1, 1.0f);                  // wait 1.0s -> 30 marks
    f.write(C + 5, {0x0c});
    f.writeW(C + 6, 0x300);
    f.write(C + 10, {0xff});
    f.write(0x300, {0x44, 0x02, 0x03, 0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.scriptMark[0] = 31;                   // >= 30 -> fires
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && (o.scriptFlagsLocal & 8) &&
          o.scriptMark[0] == 0);            // fired mark resets
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    o2.scriptMark[0] = 10;                  // < 30 -> fallthrough
    o2.field108 = f.image.data() + 4 + C;
    auto r2 = mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(r2.halted && !r2.error && !(o2.scriptFlagsLocal & 8));
  }

  // -- obj op 0x2f: probability link (rng state 1 -> draw 5137) --------
  {
    ScriptFixture f;
    f.write(C, {0x2f});
    f.writeF(C + 1, 60.0f);                 // 60% > 51.37% -> fires
    f.write(C + 5, {0x0c});
    f.writeW(C + 6, 0x300);
    f.write(C + 10, {0xff});
    f.write(0x300, {0x44, 0x02, 0x03, 0xff});
    f.rt.rngState = 1;
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && (o.scriptFlagsLocal & 8));
    f.writeF(C + 1, 50.0f);                 // 50% < 51.37% -> not
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    o2.field108 = f.image.data() + 4 + C;
    f.rt.rngState = 1;
    auto r2 = mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(r2.halted && !r2.error && !(o2.scriptFlagsLocal & 8));
  }

  // -- obj op 0x11: anim-done link --------------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x11, 0x0c});
    f.writeW(C + 2, 0x300);
    f.write(C + 6, {0xff});
    f.write(0x300, {0x44, 0x02, 0x03, 0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.animRec = nullptr;                    // done -> fires
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && (o.scriptFlagsLocal & 8));
  }

  // -- obj op 0x2c: no-subtype link --------------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x2c, 0x0c});
    f.writeW(C + 2, 0x300);
    f.write(C + 6, {0xff});
    f.write(0x300, {0x44, 0x02, 0x03, 0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field11e = 0;                         // no subtype -> fires
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && (o.scriptFlagsLocal & 8));
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    o2.field11e = 0x2b;
    o2.field108 = f.image.data() + 4 + C;
    auto r2 = mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(r2.halted && !r2.error && !(o2.scriptFlagsLocal & 8));
  }

  // -- obj op 0xcf: yaw morph (FUN_0045dc18) -----------------------------
  {
    ScriptFixture f;
    f.write(C, {0xcf});
    f.writeF(C + 1, 300.0f);                // rate*dt = 10/tick
    f.writeF(C + 5, 20.0f);                 // target 20
    f.write(C + 9, {0x0c});
    f.writeW(C + 10, 0x300);                // arrived link
    f.write(C + 14, {0xff});
    f.write(0x300, {0x44, 0x02, 0x03, 0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.yawDeg = 10.0f;                       // 10 + 10 = 20 -> arrived
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && near(o.yawDeg, 20.0) &&
          (o.scriptFlagsLocal & 8));
    // Not arrived: no link, approach continues next tick.
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    o2.yawDeg = 5.0f;
    o2.field108 = f.image.data() + 4 + C;
    auto r2 = mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(r2.halted && !r2.error && near(o2.yawDeg, 15.0) &&
          !(o2.scriptFlagsLocal & 8));
    // Wrap-aware: 350 -> target 10 crosses 0.
    mdk::DynamicObject& o3 = f.arena->dyn.allocFront();
    o3.yawDeg = 350.0f;
    o3.field108 = f.image.data() + 4 + C;
    auto r3 = mdk::traversalObjectScriptTick(f.env, o3);
    CHECK(r3.halted && !r3.error && near(o3.yawDeg, 0.0, 1e-4));
  }

  // -- obj op 0x3c: face camera -----------------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x3c, 0xff});
    f.rt.camera.pose.pos[0] = 0.0f;
    f.rt.camera.pose.pos[1] = 10.0f;        // camera due +y -> yaw 90
    f.rt.camera.pose.pos[2] = 0.0f;
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && near(o.yawDeg, 90.0, 1e-4));
  }

  // -- obj op 0x52: var -> +0x44 -----------------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x52, 0x03});               // mode3 inline f32
    f.writeF(C + 2, 64.0f);
    f.write(C + 6, {0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && near(o.field44, 64.0));
  }

  // -- obj op 0x2a: bound-name match link --------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x2a});
    f.writeStr(C + 1, "FOO");               // s1 non-empty (5 bytes)
    f.write(C + 6, {0x0c});
    f.writeW(C + 7, 0x300);
    f.write(C + 11, {0xff});
    f.write(0x300, {0x44, 0x02, 0x03, 0xff});
    mdk::DynamicObject& o = f.arena->dyn.allocFront();
    mdk::RuntimeModel::NameRec nr;
    std::snprintf(nr.name.data(), nr.name.size(), "%s", "FOO");
    o.model.names.push_back(nr);
    o.field21e = 1;                         // bound to names[0]
    o.field108 = f.image.data() + 4 + C;
    auto r = mdk::traversalObjectScriptTick(f.env, o);
    CHECK(r.halted && !r.error && (o.scriptFlagsLocal & 8) &&
          o.field21e == 0);                 // fired mark clears (s1)
    // Wrong name -> fallthrough, mark kept.
    mdk::DynamicObject& o2 = f.arena->dyn.allocFront();
    o2.model.names.push_back(nr);
    o2.field21e = 1;
    o2.field108 = f.image.data() + 4 + C;
    f.write(C + 1, {0x04, 'B', 'A', 'R', 0x00});   // s1 = "BAR"
    auto r2 = mdk::traversalObjectScriptTick(f.env, o2);
    CHECK(r2.halted);
    CHECK(!r2.error);
    CHECK(!(o2.scriptFlagsLocal & 8));
    CHECK(o2.field21e == 0);   // kept through 0x2a, cleared by 0xff
  }

  // -- obj op 0x04: broadcast remote call (object ctx) -------------------
  {
    ScriptFixture f;
    // {0x04, outer=7, linkage 0x0c target, inner=3(all)} — remote CALL
    // dispatch to every home-arena object.
    f.write(C, {0x04, 0x07, 0x0c});
    f.writeW(C + 3, 0x300);
    f.write(C + 7, {0x03, 0xff});
    mdk::DynamicObject& src = f.arena->dyn.allocFront();
    src.field108 = f.image.data() + 4 + C;
    mdk::DynamicObject& tgt = f.arena->dyn.allocFront();
    tgt.col.named = true;
    tgt.health = 5;
    tgt.field11b = 0;                       // rank gate passes
    auto r = mdk::traversalObjectScriptTick(f.env, src);
    CHECK(r.halted && !r.error);
    CHECK(tgt.field108 == f.image.data() + 4 + 0x300 &&
          tgt.field230 == tgt.field108 && tgt.field10c == tgt.field108 &&
          tgt.field138 == &src && tgt.scriptCallDepth == 0);
    // The ctx object itself is not a broadcast target.
    CHECK(src.field10c == nullptr);
  }

  // -- arena op 0x77: model-count link -----------------------------------
  {
    ScriptFixture f;
    f.write(C, {0x77});
    f.writeStr(C + 1, "FOO");               // C+1..C+5 (incl NUL)
    f.write(C + 6, {0x02});                 // kind 2 = '>'
    f.writeF(C + 7, 1.0f);                  // count > 1 ?
    f.write(C + 11, {0x0c});
    f.writeW(C + 12, 0x300);
    f.write(C + 16, {0xff});
    f.write(0x300, {0x41, 0x02, 0x00});     // locals[0] = f32
    f.writeF(0x303, 7.0f);
    f.write(0x307, {0x09});
    auto mk = [&](const char* nm) -> mdk::DynamicObject& {
      mdk::DynamicObject& o = f.arena->dyn.allocFront();
      mdk::RuntimeModel::NameRec nr;
      std::snprintf(nr.name.data(), nr.name.size(), "%s", nm);
      o.model.names.push_back(nr);
      return o;
    };
    mk("FOO"); mk("FOO"); mk("BAR");        // count(FOO) = 2 > 1
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.stopped && !r.error &&
          near(f.arena->script.locals[0], 7.0));
  }

  // -- arena op 0xaf: inventory count (unmodelled -> 0) -------------------
  {
    ScriptFixture f;
    f.write(C, {0xaf, 0x06, 0x01});         // id 6, kind 1 = '<'
    f.writeF(C + 3, 1.0f);                  // 0 < 1 -> fires
    f.write(C + 7, {0x0c});
    f.writeW(C + 8, 0x300);
    f.write(C + 12, {0xff});
    f.write(0x300, {0x41, 0x02, 0x00});
    f.writeF(0x303, 9.0f);
    f.write(0x307, {0x09});
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.stopped && !r.error &&
          near(f.arena->script.locals[0], 9.0));
  }

  // -- arena op 0xd8: var += operand*(1/30) -------------------------------
  {
    ScriptFixture f;
    f.write(C, {0xd8, 0x02, 0x01});         // locals[1] += 30 * (1/30)
    f.writeW(C + 3, 30);
    f.write(C + 7, {0x09});
    f.arena->script.pcImageOff = C;
    f.arena->script.locals[1] = 2.0f;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(r.stopped && !r.error &&
          near(f.arena->script.locals[1], 3.0, 1e-5));
  }

  // -- arena op 0x04: broadcast formation (outer 1) ----------------------
  {
    ScriptFixture f;
    // {0x04, outer=1, inner=3}: formation slot command — side/fwd/vert
    // offsets alternate per hit, subtype = 1, leader = ctx.
    f.write(C, {0x04, 0x01, 0x03, 0xff});
    f.arena->eventLatch.yawDeg = 0.0f;
    mdk::DynamicObject& t1 = f.arena->dyn.allocFront();
    mdk::DynamicObject& t2 = f.arena->dyn.allocFront();
    t1.col.named = t2.col.named = true;
    t1.health = t2.health = 5;
    f.arena->script.pcImageOff = C;
    auto r = mdk::traversalScriptRun(f.env);
    CHECK(!r.error);
    // yaw 0 -> sin 0 cos 1: pos.x = ctx.x - (-4)*0 + side*5*1*1 =
    // side*5, pos.y = ctx.y - (-4)*1 - side*5*0 = 4, pos.z = ctx.z+8.
    // allocFront pushes FRONT: t2 is iterated first (hitFlag 0 ->
    // side -1), t1 second (hitFlag 1 -> side +1).
    CHECK(near(t2.field12c[0], -5.0) && near(t2.field12c[1], -4.0) &&
          near(t2.field12c[2], 8.0) && t2.field11e == 1 &&
          t2.field138 == &f.arena->eventLatch);
    CHECK(near(t2.field120[0], -5.0) && near(t2.field120[1], 4.0) &&
          near(t2.field120[2], 8.0));
    CHECK(near(t1.field12c[0], 5.0) && near(t1.field120[0], 5.0));
  }

  // -- XCORDOOR negative control — the enemy machinery must be inert ---
  {
    // A live connector door: +0x14a&0x10, connState 8 (closed), no
    // +0xec path, no +0x149&0x10 dispatch, no +0x14b&0x40 runner, no
    // subtype. Run the FUN_004572ac per-object steps with their loop
    // gates — every gated piece must be skipped, the connector update
    // alone owns the door (OBSERVED gates).
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    mdk::DynamicObject& door = home.allocFront();
    door.col.named = true;
    door.scriptClass = "XCORDOOR";
    door.health = 10;
    door.col.flags14a = 0x10;             // connector bit
    door.connState = 8;                   // +0x312 closed
    door.setPosition(-4.0f, 0.0f, 190.0f);
    const float px = door.pos[0], py = door.pos[1], pz = door.pos[2];
    // Gate checks in loop order — all must be clear for a connector.
    CHECK(door.fieldEC == nullptr);        // +0xec path gate
    CHECK((door.col.flags149 & 0x10) == 0);// dispatch gate
    CHECK((door.col.flags14b & 0x40) == 0);// runner gate
    CHECK((door.col.flags14a & 0x40) == 0);// orbit gate
    CHECK(door.field11e == 0);             // subtype dispatch
    CHECK(door.pendingArena == nullptr);   // transfer gate
    // The ungated steps (subtype no-ops on +0x11e==0; gravity needs
    // +0x148&2 which a door lacks; collide integrates zero velocity).
    mdk::objectSubtypeUpdate(rt, door, home, 1.0f / 30.0f);
    mdk::objectGravity(rt, door, home, 1.0f / 30.0f);
    mdk::objectCollide(rt, door, home, nullptr, 1.0f / 30.0f);
    CHECK(door.pos[0] == px && door.pos[1] == py && door.pos[2] == pz);
    CHECK(door.health == 10 && door.col.named);
    CHECK(door.connState == 8 && door.field11e == 0);
    CHECK(door.fieldF0 == 0.0f && door.field18c[0] == 0.0f);
  }
}

void test_mover_runtime() {
  // ---- FUN_004585c4 — hop gate, chute lifecycle, name dispatch ------
  const float dt = 1.0f / 30.0f;

  // Helper: a mover object (flags14a&0x20) with the op-0xa1 spawn
  // dword 0x2008a6 (gravity + mover) in `ar`.
  auto mkMover = [](mdk::DynamicArena& ar,
                    const char* modelName) -> mdk::DynamicObject& {
    mdk::DynamicObject& o = ar.allocFront();
    o.col.named = true;
    o.health = 10;
    o.model = makePlatformModel(modelName, "ELEM", 0.0f);
    o.col.flags148 = 0x08a6;          // dword 0x2008a6 (0xa1/0xce tail)
    o.col.flags149 = 0x08;
    o.col.flags14a = 0x20;
    return o;
  };

  // -- hop branch: airborne + vel < -15 -> clamp + chute spawn --------
  {
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    rt.level.enemies.entries = {{"DUMMY0", 0, false},
                                {"SW_CHUTE", 0, false}};
    rt.level.models.resize(2);
    rt.level.models[1] = makePlatformModel("SW_CHUTE", "CHUTE", 0.0f);
    mdk::DynamicObject& o = mkMover(home, "SW_HOME");
    o.arena = &home;
    o.field30 = -20.0f;                        // vel.z below the clamp
    const std::size_t before = home.storage.size();
    mdk::objectMover(rt, o, home, dt, 1);
    CHECK(near(o.field30, -15.0, 1e-5));       // vel clamped (C 0x498070)
    CHECK(o.moverChild != nullptr);            // +0x312 child
    CHECK(home.storage.size() == before + 1);  // child on +0x68 list
    mdk::DynamicObject* c = o.moverChild;
    CHECK(c->col.named && c->arena == &home);
    CHECK(c->model.modelName() == "SW_CHUTE");
    CHECK(c->enemyIndex == 1 && c->spawnId == 1);
    CHECK(c->col.flags148 == 0x820 && c->col.flags149 == 0x08);
    CHECK(c->field278 == &o);                  // +0x278 reverse link
    CHECK(c->field11e == 0x4a);                // refpoint attach
    CHECK(c->field276 == 0 && c->field277 == 0);
    CHECK(c->behaviorByte == 7);               // +0x11c
    CHECK(near(c->col.scale, 1.0, 1e-6));
    // No duplicate spawn on the next hop frame.
    mdk::objectMover(rt, o, home, dt, 1);
    CHECK(o.moverChild == c && home.storage.size() == before + 1);
  }

  // -- hop branch: floor contact -> zBias 1.5, latch, z bump ----------
  {
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    mdk::DynamicObject& o = mkMover(home, "SW_HOME");
    o.arena = &home;
    o.col.flags14c |= 2;                        // floor contact
    o.field30 = -20.0f;
    const float z0 = o.pos[2];
    mdk::objectMover(rt, o, home, dt, 1);
    CHECK(near(o.zBias, 1.5, 1e-6));            // +0x5c = 0x3fc00000
    CHECK((o.col.flags14a & 2) != 0);           // hop-landed latch
    CHECK(near(o.pos[2], z0 + 1.5, 1e-5));      // C(0x498078)
    CHECK(o.moverChild == nullptr);             // no chute on contact
  }

  // -- child fade + teardown on the non-hop path ----------------------
  {
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    mdk::DynamicObject& o = mkMover(home, "SW_HOME");
    o.arena = &home;
    o.col.flags14a |= 2;                        // landed -> non-hop
    mdk::DynamicObject& c = home.allocFront();
    c.col.named = true;
    c.col.scale = 0.5f;
    o.moverChild = &c;
    mdk::objectMover(rt, o, home, dt, 1);
    CHECK(near(c.col.scale, 0.5 - 1.0 / 30.0, 1e-5));   // -= dt
    CHECK(o.moverChild == &c);                          // still linked
    // Drive scale below 0.2 -> teardown + pointer clear.
    int calls = 0;
    while (o.moverChild != nullptr && calls < 30) {
      mdk::objectMover(rt, o, home, dt, 1);
      ++calls;
    }
    CHECK(o.moverChild == nullptr);
    CHECK(!c.col.named);                        // FUN_0045828c wipe
  }

  // -- parent death: reverse link detaches via subtype 0x4a -----------
  {
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    rt.level.enemies.entries = {{"DUMMY0", 0, false},
                                {"SW_CHUTE", 0, false}};
    rt.level.models.resize(2);
    rt.level.models[1] = makePlatformModel("SW_CHUTE", "CHUTE", 0.0f);
    mdk::DynamicObject& o = mkMover(home, "SW_HOME");
    o.field30 = -20.0f;
    mdk::objectMover(rt, o, home, dt, 1);       // chute attached
    mdk::DynamicObject* c = o.moverChild;
    CHECK(c != nullptr && c->field11e == 0x4a);
    // FUN_0045828c on the parent wipes it but the storage stays —
    // the child's +0x278 then reads named==0 and self-detaches, the
    // same as the original's memset'd pool slot.
    mdk::objectTeardownNow(rt, o);
    CHECK(o.moverChild == nullptr);
    mdk::objectSubtypeUpdate(rt, *c, home, dt);
    CHECK(c->field11e == 0);                    // detached
    CHECK(c->col.named);                        // child survives
  }

  // -- child death: +0x312 stays set (OBSERVED quirk) -----------------
  {
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    rt.level.enemies.entries = {{"DUMMY0", 0, false},
                                {"SW_CHUTE", 0, false}};
    rt.level.models.resize(2);
    rt.level.models[1] = makePlatformModel("SW_CHUTE", "CHUTE", 0.0f);
    mdk::DynamicObject& o = mkMover(home, "SW_HOME");
    o.field30 = -20.0f;
    mdk::objectMover(rt, o, home, dt, 1);
    mdk::DynamicObject* c = o.moverChild;
    mdk::objectTeardownNow(rt, *c);             // child dies first
    o.col.flags14a |= 2;                        // landed -> non-hop
    mdk::objectMover(rt, o, home, dt, 1);
    // The wiped child's +0x58 == 0 -> the fade gate skips it and
    // +0x312 never clears (the original leaves the dead pointer too).
    CHECK(o.moverChild == c);
  }

  // -- name dispatch: default spin / SW_SEAL / SW_SBONE no-ops --------
  {
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    mdk::DynamicObject& a = mkMover(home, "SW_HOME");
    mdk::DynamicObject& b = mkMover(home, "SW_SEAL");
    mdk::DynamicObject& c = mkMover(home, "SW_SBONE");
    a.col.flags14a |= 2; b.col.flags14a |= 2; c.col.flags14a |= 2;
    mdk::objectMover(rt, a, home, dt, 1);
    mdk::objectMover(rt, b, home, dt, 1);
    mdk::objectMover(rt, c, home, dt, 1);
    CHECK(near(a.yawDeg, 180.0 / 30.0, 1e-4));  // dt * C(0x498030)
    CHECK(b.yawDeg == 0.0f && c.yawDeg == 0.0f);// explicit no-ops
  }

  // -- SW_H150: idle -> react on approach, flee steering, drop-back ---
  {
    static const std::byte h150i[1] = {std::byte{0x11}};
    static const std::byte h150r[1] = {std::byte{0x22}};
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    rt.animH150I = h150i;
    rt.animH150R = h150r;
    rt.rngState = 1;            // next rand = 16838 -> lottery skipped
    mdk::DynamicObject& o = mkMover(home, "SW_H150");
    o.arena = &home;
    o.col.flags14a |= 2;                        // landed
    o.animRec = rt.animH150I;
    o.animLatch = 0;                            // idle record running
    rt.cs.pos[0] = 5.0f; rt.cs.pos[1] = 0.0f; rt.cs.pos[2] = 0.0f;
    mdk::objectMover(rt, o, home, dt, 1);       // dist2=25 < 400
    CHECK(o.animRec == rt.animH150R);           // reaction bound
    CHECK((o.col.flags148 & 8) != 0);           // looping
    CHECK(rt.seams.moverSfxCalls == 1);         // RUNNER seam
    CHECK(o.animFrame == -1 && o.animLatch == -1 && o.animAcc == 0.0f);
    // Reacting: impulse + away-steer + gravity arm.
    o.yawDeg = 0.0f;
    mdk::objectMover(rt, o, home, dt, 1);
    CHECK(near(o.animImpulse[0], 40.0, 1e-4));  // cos(0)*40 -> +0x294
    CHECK(near(o.animImpulse[1], 0.0, 1e-4));   // sin(0)*40 -> +0x298
    CHECK((o.col.flags148 & 2) != 0);
    // bearing(5,0)=90 + 180 -> target 270; from 0 the short way is
    // counterclockwise: one dt*270=9 step -> 351 (0x4587df..0x45880b).
    CHECK(near(o.yawDeg, 351.0, 1e-3));
    // Player retreats past XY dist2 1000 -> idle rebind.
    rt.cs.pos[0] = 40.0f;                       // dist2=1600
    mdk::objectMover(rt, o, home, dt, 1);
    CHECK(o.animRec == rt.animH150I);
    CHECK((o.col.flags148 & 8) == 0);           // one-shot again
    // Far idle: dist2 >= 400 -> nothing changes.
    o.animLatch = 0;
    const float yaw0 = o.yawDeg;
    mdk::objectMover(rt, o, home, dt, 1);
    CHECK(o.yawDeg == yaw0 && rt.seams.moverSfxCalls == 1);
    // Chute attached -> the whole H150 body is skipped.
    mdk::DynamicObject& chute = home.allocFront();
    chute.col.named = true;
    o.moverChild = &chute;
    o.animRec = rt.animH150I;
    mdk::objectMover(rt, o, home, dt, 1);
    CHECK(rt.seams.moverSfxCalls == 1);
  }

  // -- lottery: animDone + rand < frameStep*72 rebinds idle -----------
  {
    static const std::byte h150i[1] = {std::byte{0x33}};
    static const std::byte h150r[1] = {std::byte{0x44}};
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    rt.animH150I = h150i;
    rt.animH150R = h150r;
    mdk::DynamicObject& o = mkMover(home, "SW_H150");
    o.arena = &home;
    o.col.flags14a |= 2;
    o.animRec = nullptr;                        // animDone via +0x114==0
    rt.cs.pos[0] = 500.0f;                      // far — react gate fails
    rt.rngState = 0;                            // next rand = 0 < 72
    mdk::objectMover(rt, o, home, dt, 1);
    CHECK(o.animRec == rt.animH150I);           // lottery rebind
    CHECK(o.animLatch == -1 && o.animFrame == -1);
    CHECK(near(o.animRate, 30.0, 1e-5));
    CHECK((o.col.flags148 & 8) == 0);           // one-shot cleared
  }

  // -- gates: mover/dispatch mutual exclusion + XCORDOOR --------------
  {
    mdk::DynamicObject o;
    o.col.flags149 = 0x10;                      // enemy dispatch
    o.col.flags14a = 0x20 | 0x10;               // mover + connector
    // FUN_004572ac order: dispatch REPLACES the mover (else-if).
    bool ranDispatch = false, ranMover = false;
    if (o.col.flags149 & 0x10) ranDispatch = true;
    else if (o.col.flags14a & 0x20) ranMover = true;
    CHECK(ranDispatch && !ranMover);
    // XCORDOOR connector: no mover bit -> loop never reaches the mover.
    mdk::DynamicObject door;
    door.col.flags14a = 0x10;
    CHECK((door.col.flags14a & 0x20) == 0);
  }
}

void test_object_animation() {
  // -- Record header / view accessors ---------------------------------
  // {rate=1, chanCount=1, frameCount=4, chanOff, rootKeys, refCount,
  //  refKeys, channel} — matches the OBSERVED XGS record shape.
  std::vector<std::uint8_t> rec;
  aF(rec, 1.0f);                            // rate
  aW(rec, 1);                               // channelCount
  aW(rec, 4);                               // frameCount
  const std::size_t offPos = rec.size();
  aW(rec, 0);                               // chanOff[0] (patched)
  for (int f = 0; f < 4; ++f)
    aV3(rec, static_cast<float>(f), 0, 0);  // rootKey[f] = {f,0,0}
  aW(rec, 2);                               // refCount
  for (int s = 0; s < 2; ++s)               // refKey[s][f]
    for (int f = 0; f < 4; ++f)
      aV3(rec, static_cast<float>(s * 10 + f), 0, 0);
  // channel at rec+4+chanOff — patch it to land here.
  const std::size_t chanAt = rec.size();
  rec[offPos + 0] = static_cast<std::uint8_t>(chanAt - 4);
  rec[offPos + 1] = static_cast<std::uint8_t>((chanAt - 4) >> 8);
  rec[offPos + 2] = static_cast<std::uint8_t>((chanAt - 4) >> 16);
  rec[offPos + 3] = static_cast<std::uint8_t>((chanAt - 4) >> 24);
  aName(rec, "ELEM");                       // +0x00 name[12]
  aW(rec, 3);                               // +0x0c vertCount
  aF(rec, 0.5f);                            // +0x10 scale
  aV3(rec, 0, 0, 0); aV3(rec, 1, 0, 0); aV3(rec, 2, 0, 0); // basePose
  aH(rec, 1);                               // key tag = frame 1
  for (int i = 0; i < 9; ++i) rec.push_back(4);  // i8 deltas +4 x each
  aH(rec, -1);                              // terminator

  {
    mdk::ObjectAnimView av{rec.data(), rec.data() + rec.size()};
    CHECK(av.ok);
    CHECK(near(av.rate(), 1.0, 1e-6));
    CHECK(av.channelCount() == 1);
    CHECK(av.frameCount() == 4);
    CHECK(av.refCount() == 2);
    const std::uint8_t* ch = av.channel(0);
    CHECK(ch == rec.data() + chanAt);
    CHECK(std::string(mdk::animChannelName(ch)) == "ELEM");
    CHECK(mdk::animChannelVertCount(ch) == 3);
    CHECK(near(mdk::animChannelScale(ch), 0.5, 1e-6));
    CHECK(!mdk::animChannelIsRigid(ch));
    CHECK(near(av.rootKey(2)[0], 2.0, 1e-6));
    CHECK(near(av.refKey(1, 3)[0], 13.0, 1e-6));
  }

  // -- Delta channel + driver (one-shot) ------------------------------
  {
    mdk::DynamicArena da;
    mdk::DynamicObject& o = animObject(da, rec);
    o.model = makePlatformModel("MDL", "ELEM", 0.0f);   // 3 verts
    o.col.flags148 &= ~0x8u;               // one-shot (op 0x03 form)

    const std::uint8_t* lim = rec.data() + rec.size();
    // tick1: acc -1->0 -> frame 0: absolute base pose copy.
    mdk::objectAnimTick(o, lim);
    CHECK(o.animFrame == 0);
    CHECK(near(o.model.elemVerts[0][0], 0.0, 1e-6));
    CHECK(near(o.model.elemVerts[0][3], 1.0, 1e-6));
    CHECK(near(o.model.elemVerts[0][6], 2.0, 1e-6));
    // refPoints[0/1] = refKey[s][0]
    CHECK(near(o.model.refPoints[0][0], 0.0, 1e-6));
    CHECK(near(o.model.refPoints[1][0], 10.0, 1e-6));

    // tick2: frame 1 — tagged delta key applies (+4 * 0.5 = +2 x),
    // root key {1,0,0} rotates by identity into +0x294 *30.
    mdk::objectAnimTick(o, lim);
    CHECK(o.animFrame == 1);
    CHECK(near(o.model.elemVerts[0][0], 2.0, 1e-6));
    CHECK(near(o.model.elemVerts[0][3], 3.0, 1e-6));
    CHECK(near(o.model.elemVerts[0][6], 4.0, 1e-6));
    CHECK(near(o.animImpulse[0], 30.0, 1e-4));
    CHECK(near(o.model.refPoints[0][0], 1.0, 1e-6));
    // localAabb recomputed (FUN_00459d54): min.x = 2, max.x = 4.
    CHECK(near(o.model.elems[0].localAabb[0], 2.0, 1e-6));
    CHECK(near(o.model.elems[0].localAabb[3], 4.0, 1e-6));

    // tick3: frame 2 — no tagged key; verts hold; impulse adds
    // rootKey[2]={2,0,0}*30 = +60.
    mdk::objectAnimTick(o, lim);
    CHECK(o.animFrame == 2);
    CHECK(near(o.model.elemVerts[0][0], 2.0, 1e-6));
    CHECK(near(o.animImpulse[0], 90.0, 1e-3));

    // tick4: frame 3 = frameCount-1 on a one-shot -> 0xff00 latch.
    mdk::objectAnimTick(o, lim);
    CHECK(o.animFrame == 3);
    CHECK(static_cast<std::uint16_t>(o.animLatch) == 0xff00u);
    CHECK(near(o.animImpulse[0], 180.0, 1e-2));
    // tick5: latched idle — accumulator resyncs, nothing applies.
    mdk::objectAnimTick(o, lim);
    CHECK(o.animFrame == 3);
    CHECK(near(o.animImpulse[0], 180.0, 1e-2));
  }

  // -- Looping (op 0x3b form): wrap re-applies the frame-0 base pose --
  {
    mdk::DynamicArena da;
    mdk::DynamicObject& o = animObject(da, rec);
    o.model = makePlatformModel("MDL", "ELEM", 0.0f);
    o.col.flags148 |= 0x8u;                // loop bit
    const std::uint8_t* lim = rec.data() + rec.size();
    for (int i = 0; i < 4; ++i) mdk::objectAnimTick(o, lim);
    CHECK(o.animFrame == 3);
    CHECK(static_cast<std::uint16_t>(o.animLatch) != 0xff00u);
    // tick5: acc hits 4 -> wraps; frame 0 re-copies the base pose
    // (verts return to base, undoing the frame-1 delta).
    mdk::objectAnimTick(o, lim);
    CHECK(o.animFrame == 0);
    CHECK(near(o.model.elemVerts[0][0], 0.0, 1e-6));
    CHECK(near(o.model.elemVerts[0][3], 1.0, 1e-6));
  }

  // -- Target latch (+0x118 >= 0): hold at the target frame -----------
  {
    mdk::DynamicArena da;
    mdk::DynamicObject& o = animObject(da, rec);
    o.model = makePlatformModel("MDL", "ELEM", 0.0f);
    o.col.flags148 &= ~0x8u;
    o.animLatch = 2;                        // run to frame 2 and hold
    const std::uint8_t* lim = rec.data() + rec.size();
    for (int i = 0; i < 10; ++i) mdk::objectAnimTick(o, lim);
    CHECK(o.animFrame == 2);
    CHECK(near(o.animAcc, 2.0, 1e-6));
    CHECK(static_cast<std::uint16_t>(o.animLatch) != 0xff00u);
  }

  // -- Rigid channel (scale == 0): FUN_00455c48 fixed-point 3x4 ------
  {
    std::vector<std::uint8_t> rr;
    aF(rr, 1.0f); aW(rr, 1); aW(rr, 3);    // rate, 1 chan, 3 frames
    const std::size_t rop = rr.size();
    aW(rr, 0);
    for (int f = 0; f < 3; ++f) aV3(rr, 0, 0, 0);   // root keys
    aW(rr, 0);                             // refCount 0
    const std::size_t rchan = rr.size();
    rr[rop] = static_cast<std::uint8_t>(rchan - 4);
    aName(rr, "ELEM");
    aW(rr, 3);                             // vc 3
    aF(rr, 0.0f);                          // scale 0 -> rigid
    rr.push_back(1);                       // +0x14 rotShift: /16384
    rr.push_back(2);                       // +0x15 transShift: /8192
    aV3(rr, 0, 0, 0); aV3(rr, 1, 0, 0); aV3(rr, 2, 0, 0); // base pose
    for (int f = 0; f < 3; ++f) {          // i16 xform[f][12]
      for (int i = 0; i < 12; ++i) {
        // Record = {r00,r01,r02,tx, r10,r11,r12,ty, r20,r21,r22,tz}.
        // Frames 0/2 = identity; frame 1 = identity + T {1,2,3}.
        const bool trans = (i % 4 == 3);
        std::int16_t v = 0;
        if (!trans && (i == 0 || i == 5 || i == 10)) v = 16384; // 1.0
        if (f == 1 && trans)
          v = static_cast<std::int16_t>(
              (i == 3 ? 1 : i == 7 ? 2 : 3) * 8192);
        aH(rr, v);
      }
    }
    mdk::DynamicArena da;
    mdk::DynamicObject& o = animObject(da, rr);
    o.model = makePlatformModel("MDL", "ELEM", 0.0f);
    o.col.flags148 &= ~0x8u;
    const std::uint8_t* lim = rr.data() + rr.size();
    mdk::objectAnimTick(o, lim);           // frame 0: identity -> base
    CHECK(o.animFrame == 0);
    CHECK(near(o.model.elemVerts[0][3], 1.0, 1e-5));
    mdk::objectAnimTick(o, lim);           // frame 1: +{1,2,3}
    CHECK(o.animFrame == 1);
    CHECK(near(o.model.elemVerts[0][0], 1.0, 1e-5));
    CHECK(near(o.model.elemVerts[0][1], 2.0, 1e-5));
    CHECK(near(o.model.elemVerts[0][2], 3.0, 1e-5));
    CHECK(near(o.model.elemVerts[0][3], 2.0, 1e-5));
  }

  // -- Unmatched channel name -> element untouched --------------------
  {
    std::vector<std::uint8_t> nr = rec;    // copy, rename the channel
    const std::size_t nm = chanAt;         // channel name field
    nr[nm] = 'O'; nr[nm + 1] = 'T'; nr[nm + 2] = 'H'; nr[nm + 3] = 'E';
    nr[nm + 4] = 'R'; nr[nm + 5] = 0;
    mdk::DynamicArena da;
    mdk::DynamicObject& o = animObject(da, nr);
    o.model = makePlatformModel("MDL", "ELEM", 0.0f);
    o.col.flags148 &= ~0x8u;
    const std::uint8_t* lim = nr.data() + nr.size();
    const float keep[9] = {-5,-5,0, 5,-5,0, 5,5,0};
    for (int i = 0; i < 3; ++i) mdk::objectAnimTick(o, lim);
    CHECK(o.animFrame == 2);
    for (int i = 0; i < 9; ++i)
      CHECK(near(o.model.elemVerts[0][i], keep[i], 1e-6));
  }

  // -- +0x14b bit7 suppresses the root-motion impulse ------------------
  {
    mdk::DynamicArena da;
    mdk::DynamicObject& o = animObject(da, rec);
    o.model = makePlatformModel("MDL", "ELEM", 0.0f);
    o.col.flags148 &= ~0x8u;
    o.col.flags14b |= 0x80u;
    const std::uint8_t* lim = rec.data() + rec.size();
    for (int i = 0; i < 3; ++i) mdk::objectAnimTick(o, lim);
    CHECK(near(o.animImpulse[0], 0.0, 1e-6));
    CHECK(o.animFrame == 2);               // verts still applied
    CHECK(near(o.model.elemVerts[0][0], 2.0, 1e-6));
  }
}

// ---------------------------------------------------------------------------
// Phase 5J — player look and view orientation
// ---------------------------------------------------------------------------

namespace {
mdk::PlayerLookEnvironment lookEnv(float scalar = 0.0f) {
  mdk::PlayerLookEnvironment e;
  e.deltaSeconds = 1.0f / 30.0f;
  e.arenaScalar = scalar;
  e.eventPriority = 0;
  e.locoState = 0;
  e.vertVelZero = true;
  e.grounded = true;
  return e;
}
mdk::GameplayInputFrame lookCtl(bool up, bool down) {
  mdk::GameplayInputFrame c{};
  c.lookUp = up ? 1u : 0u;
  c.lookDown = down ? 1u : 0u;
  return c;
}
} // namespace

void test_player_look() {
  // ---- idle at zero: the d58==0 self-store posts nothing ---------
  {
    mdk::PlayerLookState s;
    auto f = mdk::integratePlayerLook(lookCtl(false, false),
                                      lookEnv(), s);
    CHECK(!f.eventPosted);
    CHECK(s.lookPitchOffset == 0.0f);
  }

  // ---- LookUp held: -90 deg/s, event 8/0x324 each frame ----------
  {
    mdk::PlayerLookState s;
    for (int i = 1; i <= 3; ++i) {
      auto f = mdk::integratePlayerLook(lookCtl(true, false),
                                        lookEnv(), s);
      CHECK(f.eventPosted);
      CHECK(near(s.lookPitchOffset, -3.0 * i, 1e-5));
    }
  }

  // ---- LookDown held: +90 deg/s ----------------------------------
  {
    mdk::PlayerLookState s;
    auto f = mdk::integratePlayerLook(lookCtl(false, true),
                                      lookEnv(), s);
    CHECK(f.eventPosted);
    CHECK(near(s.lookPitchOffset, 3.0, 1e-5));
  }

  // ---- both pressed: lookUp is tested first, it wins -------------
  {
    mdk::PlayerLookState s;
    auto f = mdk::integratePlayerLook(lookCtl(true, true),
                                      lookEnv(), s);
    CHECK(f.eventPosted);
    CHECK(near(s.lookPitchOffset, -3.0, 1e-5));
  }

  // ---- clamps are arena-scalar relative: [-60-a462, +90-a462] ----
  {
    mdk::PlayerLookState s;
    for (int i = 0; i < 40; ++i)
      mdk::integratePlayerLook(lookCtl(false, true), lookEnv(), s);
    CHECK(near(s.lookPitchOffset, 90.0, 1e-4));       // +90 - 0
    for (int i = 0; i < 60; ++i)
      mdk::integratePlayerLook(lookCtl(true, false), lookEnv(), s);
    CHECK(near(s.lookPitchOffset, -60.0, 1e-4));      // -60 - 0
    // scalar=10 shifts both bounds: absolute pitch stays [-60,+90].
    mdk::PlayerLookState s2;
    for (int i = 0; i < 40; ++i)
      mdk::integratePlayerLook(lookCtl(false, true), lookEnv(10), s2);
    CHECK(near(s2.lookPitchOffset, 80.0, 1e-4));      // +90 - 10
    for (int i = 0; i < 60; ++i)
      mdk::integratePlayerLook(lookCtl(true, false), lookEnv(10), s2);
    CHECK(near(s2.lookPitchOffset, -70.0, 1e-4));     // -60 - 10
  }

  // ---- release: recenter at 200 deg/s, sign-snapped to 0 ---------
  {
    mdk::PlayerLookState s;
    s.lookPitchOffset = -30.0f;
    auto f = mdk::integratePlayerLook(lookCtl(false, false),
                                      lookEnv(), s);
    CHECK(f.eventPosted);              // posts while draining
    CHECK(near(s.lookPitchOffset, -30.0 + 200.0 / 30.0, 1e-4));
    while (s.lookPitchOffset != 0.0f)
      f = mdk::integratePlayerLook(lookCtl(false, false),
                                   lookEnv(), s);
    CHECK(s.lookPitchOffset == 0.0f);  // exact zero, no overshoot
    f = mdk::integratePlayerLook(lookCtl(false, false),
                                 lookEnv(), s);
    CHECK(!f.eventPosted);             // settled: self-store only
    // Same from the positive side.
    s.lookPitchOffset = 5.0f;
    mdk::integratePlayerLook(lookCtl(false, false), lookEnv(), s);
    CHECK(s.lookPitchOffset == 0.0f);  // 5 - 6.67 -> snapped to 0
  }

  // ---- eligibility gates route to the recenter branch ------------
  {
    mdk::PlayerLookState s;
    s.lookPitchOffset = -20.0f;
    // Airborne (vertVel nonzero): holding lookUp still recenters.
    auto e = lookEnv();
    e.vertVelZero = false;
    auto f = mdk::integratePlayerLook(lookCtl(true, false), e, s);
    CHECK(f.eventPosted);
    CHECK(near(s.lookPitchOffset, -20.0 + 200.0 / 30.0, 1e-4));
    // Not grounded: same.
    s.lookPitchOffset = -20.0f;
    e = lookEnv();
    e.grounded = false;
    mdk::integratePlayerLook(lookCtl(true, false), e, s);
    CHECK(near(s.lookPitchOffset, -20.0 + 200.0 / 30.0, 1e-4));
    // cbc >= 8 with a non-look cac: ineligible.
    s.lookPitchOffset = -20.0f;
    e = lookEnv();
    e.eventPriority = 8;
    e.locoState = 0x323;
    mdk::integratePlayerLook(lookCtl(true, false), e, s);
    CHECK(near(s.lookPitchOffset, -20.0 + 200.0 / 30.0, 1e-4));
    // cbc >= 8 but cac == 0x324: the state override keeps it live.
    s.lookPitchOffset = -20.0f;
    e.locoState = mdk::kLookEventCode;
    f = mdk::integratePlayerLook(lookCtl(true, false), e, s);
    CHECK(f.eventPosted);
    CHECK(near(s.lookPitchOffset, -20.0 - 3.0, 1e-4));
  }

  // ---- non-default timing: the integrator is f4-scaled -----------
  {
    mdk::PlayerLookState s;
    auto e = lookEnv();
    e.deltaSeconds = 2.0f / 30.0f;
    mdk::integratePlayerLook(lookCtl(true, false), e, s);
    CHECK(near(s.lookPitchOffset, -6.0, 1e-5));
  }

  // ---- view tail: z-delta clamp + EMA ----------------------------
  {
    mdk::PlayerViewTail t;
    mdk::PlayerViewTailEnvironment e;
    e.dz = 2.0f;                          // clamps to +0.5
    mdk::updatePlayerViewTail(e, t);
    CHECK(near(t.viewZDelta, 0.5 * 0.03, 1e-6));
    // EMA converges toward the clamped input.
    for (int i = 0; i < 600; ++i) mdk::updatePlayerViewTail(e, t);
    CHECK(near(t.viewZDelta, 0.5, 1e-3));
  }

  // ---- view tail: sign-disagreement slew never crosses the raw ---
  {
    mdk::PlayerViewTail t;
    mdk::PlayerViewTailEnvironment e;
    t.viewZDelta = 0.4f;
    e.dz = -0.1f;                          // opposite sign -> slew
    mdk::updatePlayerViewTail(e, t);
    // EMA: 0.4*0.97 - 0.1*0.03 = 0.385, then -0.02 slew = 0.365.
    CHECK(near(t.viewZDelta, 0.365, 1e-6));
    // With dz = 0 held, the slew drains to exactly 0 (snap, never
    // crosses the raw value).
    t.viewZDelta = 0.03f;
    e.dz = 0.0f;
    for (int i = 0; i < 4; ++i) mdk::updatePlayerViewTail(e, t);
    CHECK(t.viewZDelta == 0.0f);
  }

  // ---- view tail: lookEff arena-relative clamp -------------------
  {
    mdk::PlayerViewTail t;
    mdk::PlayerViewTailEnvironment e;
    e.arenaScalar = 10.0f;
    e.viewScalar = 20.0f;                  // mid-blend (!= scalar)
    e.lookOffset = 80.0f;
    mdk::updatePlayerViewTail(e, t);
    // sum = 100 > hi (90-10=80) -> lookEff = 80 - 20 = 60.
    CHECK(near(t.lookEffDeg, 60.0, 1e-5));
    // viewScalar == arenaScalar: the clamp is skipped entirely.
    e.viewScalar = 10.0f;
    mdk::updatePlayerViewTail(e, t);
    CHECK(near(t.lookEffDeg, 80.0, 1e-5));
  }

  // ---- view tail: view yaw mirrors locomotion yaw about +90 ------
  {
    mdk::PlayerViewTail t;
    mdk::PlayerViewTailEnvironment e;
    e.yawDeg = 96.0f;
    mdk::updatePlayerViewTail(e, t);
    CHECK(near(t.viewYawDeg, -6.0, 1e-6));
  }

  // ---- view tail: air-charge lift, cap and decay -----------------
  {
    mdk::PlayerViewTail t;
    mdk::PlayerViewTailEnvironment e;
    e.airCharge = 30.0f;
    mdk::updatePlayerViewTail(e, t);
    CHECK(near(t.viewPitchLift, 20.0, 1e-5));   // 30 * 2/3
    e.airCharge = 90.0f;
    mdk::updatePlayerViewTail(e, t);
    CHECK(near(t.viewPitchLift, 40.0, 1e-5));   // capped at 40
    e.airCharge = 0.0f;
    mdk::updatePlayerViewTail(e, t);
    CHECK(near(t.viewPitchLift, 40.0 - 40.0 / 30.0, 1e-4));
    t.viewPitchLift = 0.5f;
    mdk::updatePlayerViewTail(e, t);
    CHECK(t.viewPitchLift == 0.0f);             // snapped, no neg
  }

  // ---- view tail: effective pitch = scalar + look - dip + lift ---
  {
    mdk::PlayerViewTail t;
    mdk::PlayerViewTailEnvironment e;
    e.arenaScalar = 6.0f;
    e.viewScalar = 6.0f;
    e.lookOffset = -30.0f;
    e.airCharge = 15.0f;
    t.viewZDelta = 0.1f;
    // EMA keeps sign (dz=0 -> slew applies: 0.1*0.97 - 0.02).
    mdk::updatePlayerViewTail(e, t);
    const float zd = 0.1f * 0.97f - 0.02f;
    CHECK(near(t.viewPitchDeg,
               6.0 + (-30.0) - zd * 40.0 + 15.0 * (2.0 / 3.0), 1e-4));
  }

  // ---- golden sequence: press -> hold -> release -> settle -------
  {
    mdk::PlayerLookState s;
    const mdk::PlayerLookEnvironment e = lookEnv();
    const mdk::GameplayInputFrame up = lookCtl(true, false);
    const mdk::GameplayInputFrame idle = lookCtl(false, false);
    // f0 idle: nothing.
    CHECK(!mdk::integratePlayerLook(idle, e, s).eventPosted);
    // f1..f4 lookUp: -3 per frame, posting.
    for (int i = 1; i <= 4; ++i) {
      CHECK(mdk::integratePlayerLook(up, e, s).eventPosted);
      CHECK(near(s.lookPitchOffset, -3.0 * i, 1e-5));
    }
    // f5..f6 release: +6.667 per frame, still posting while nonzero.
    CHECK(mdk::integratePlayerLook(idle, e, s).eventPosted);
    CHECK(near(s.lookPitchOffset, -12.0 + 200.0 / 30.0, 1e-4));
    CHECK(mdk::integratePlayerLook(idle, e, s).eventPosted);
    CHECK(s.lookPitchOffset == 0.0f);   // snapped on this frame
    // f7: settled.
    CHECK(!mdk::integratePlayerLook(idle, e, s).eventPosted);
  }

  // ---- dispatch: the look state's entry, scripted gate, exit -----
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "TEST");
    a->dyn.col.verts = f.verts.data();
    a->dyn.col.polys = f.polys.data();
    a->dyn.col.nodes = f.nodes.data();
    a->dyn.col.deepFloorZ = -1000.0f;
    rt.cur = a;
    rt.cs.arena = &a->dyn.col;
    rt.cs.queryEnabled = 1;
    rt.cs.arenaValid = 1;
    rt.cs.objectDataLoaded = 1;
    rt.cs.pos[2] = 12.0f;
    rt.cs.entryPos[2] = 12.0f;
    const mdk::GameplayInputBindings bindings;
    const mdk::FrontendTimingState timing;
    const mdk::RawGameplayInput idle{};

    // Land: gravity settles the player onto the z=10 floor.
    for (int i = 0; i < 30 && !(rt.vert.contactFlags & 1); ++i)
      mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK((rt.vert.contactFlags & 1) != 0);
    CHECK(rt.vert.vertVel == 0.0f);
    CHECK(rt.locoState == 0x65);       // idle restore latched

    // Hold lookUp through the raw key path (factory code 30) — the
    // merged control lands in prevFrame ONE step later (latency).
    mdk::RawGameplayInput lookKey{};
    lookKey.keyLevel[30 >> 5] |= 1u << (30 & 31);
    mdk::stepTraversalRuntime(rt, lookKey, bindings, timing);
    CHECK(rt.locoState != mdk::kLookEventCode); // still idle: N-1
    const float preX = rt.cs.pos[0], preY = rt.cs.pos[1];
    auto out = mdk::stepTraversalRuntime(rt, lookKey, bindings, timing);
    CHECK(out.locoState == mdk::kLookEventCode);
    CHECK(rt.eventPriority == mdk::kLookEventPri);
    CHECK(near(rt.look.lookPitchOffset, -3.0, 1e-5));
    CHECK(near(out.lookOffsetDeg, -3.0, 1e-5));
    // The 0x324 scripted branch suppresses horizontal motion: a
    // nonzero move request in the merged block produces no
    // displacement while the look state is dispatched.
    rt.prevFrame.moveVel = -1.0f;
    rt.prevFrame.moveVelBoosted = -1.0f;
    out = mdk::stepTraversalRuntime(rt, lookKey, bindings, timing);
    CHECK(rt.cs.pos[0] == preX && rt.cs.pos[1] == preY);
    CHECK(near(rt.look.lookPitchOffset, -6.0, 1e-5));
    rt.prevFrame.moveVel = 0.0f;
    rt.prevFrame.moveVelBoosted = 0.0f;

    // Release: one frame of latency — the merged block still holds
    // lookUp for one more step before the release lands.
    out = mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(out.locoState == mdk::kLookEventCode);
    CHECK(near(rt.look.lookPitchOffset, -9.0, 1e-5));
    // Now the released block arrives: recenter at 200 deg/s.
    out = mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(near(rt.look.lookPitchOffset, -9.0 + 200.0 / 30.0, 1e-4));
    // Next frame snaps to exactly 0; the FUN_00461954 fold clears
    // the priority while cac is still 0x324.
    out = mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(rt.look.lookPitchOffset == 0.0f);
    CHECK(rt.eventPriority == 0);      // the anim-end fold ran
    CHECK(out.locoState == mdk::kLookEventCode);
    // The NEXT dispatch's idle restore returns cac.
    out = mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(out.locoState == 0x65);      // idle restore returned cac
  }
}

// ---------------------------------------------------------------------------
// Phase 5K — normal traversal camera pose and view matrix.
// FUN_004301e0 tail (position branches, basis, M1/M2 pair, view
// config) + FUN_00431100 (overhead). Expected values are computed
// from the OBSERVED formulas; the trig helper's degree constant is
// the stored f64 0x497924 (4 ULP below pi/180).
// ---------------------------------------------------------------------------

namespace {

constexpr double kCamDegToRad =
    std::bit_cast<double>(0x3f91df46a2529d35ULL);

float camTrigSin(float deg) {
  return static_cast<float>(
      std::sin(static_cast<double>(deg) * kCamDegToRad));
}
float camTrigCos(float deg) {
  return static_cast<float>(
      std::cos(static_cast<double>(deg) * kCamDegToRad));
}

mdk::PlayerCameraEnvironment camEnv(float px, float py, float pz,
                                    float viewYawDeg,
                                    float effPitchDeg) {
  mdk::PlayerCameraEnvironment e;
  e.playerPos[0] = px;
  e.playerPos[1] = py;
  e.playerPos[2] = pz;
  e.viewYawDeg = viewYawDeg;
  e.effPitchDeg = effPitchDeg;
  e.bankDeg = 0.0f;
  e.yawDeg = 90.0f - viewYawDeg;
  return e;
}

} // namespace

void test_player_camera() {
  // ---- pose: pitch = 0 (D branch, D = pullback = 8.0) -----------
  {
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f); // yaw 0 -> vY 90
    mdk::updatePlayerCamera(e, st);
    // camX = px - sin(90)*8*cos(0); camY = py - cos(90)*8; z = +4.5.
    CHECK(near(st.pose.pos[0], -8.0, 1e-5));
    CHECK(near(st.pose.pos[1], 0.0, 1e-5));
    CHECK(near(st.pose.pos[2], 4.5, 1e-5));
    // back = (-sinY*cosP, -cosY*cosP, sinP) ~ (-1, 0, 0) -> fwd +X.
    CHECK(near(st.pose.back[0], -1.0, 1e-5));
    CHECK(near(st.pose.back[1], 0.0, 1e-5));
    CHECK(near(st.pose.back[2], 0.0, 1e-5));
    // bank 0 -> up = +Z; right = up x back ~ (0,-1,0); down = -up.
    CHECK(near(st.pose.up[0], 0.0, 1e-6));
    CHECK(near(st.pose.up[1], 0.0, 1e-6));
    CHECK(near(st.pose.up[2], 1.0, 1e-6));
    CHECK(near(st.pose.view[0][0], 0.0, 1e-6));
    CHECK(near(st.pose.view[0][1], -0.8333333, 1e-6));
    CHECK(near(st.pose.view[0][2], 0.0, 1e-6));
    CHECK(near(st.pose.view[1][0], 0.0, 1e-6));
    CHECK(near(st.pose.view[1][1], 0.0, 1e-6));
    CHECK(near(st.pose.view[1][2], -1.3888889, 1e-5));
    // row2 = scaleZ*back = -1 * (-1,0,0) = (1,0,0); t = -(back.cam)
    // = -((-1)(-8)) = -8 -> scaleZ*t = +8.
    CHECK(near(st.pose.view[2][0], 1.0, 1e-6));
    CHECK(near(st.pose.view[2][1], 0.0, 1e-6));
    CHECK(near(st.pose.view[2][2], 0.0, 1e-6));
    CHECK(near(st.pose.view[2][3], 8.0, 1e-4));
    // view[1][3] = scaleY * -(down.cam) = 1.3889 * 4.5 = 6.25.
    CHECK(near(st.pose.view[1][3], 6.25, 1e-4));
    // M2 = basis*|basis| with t = -(row.cam)*|row| (|row|=1 here).
    CHECK(near(st.pose.basis[0][1], -1.0, 1e-5));
    CHECK(near(st.pose.basis[1][2], -1.0, 1e-5));
    CHECK(near(st.pose.basis[2][0], -1.0, 1e-5));
    CHECK(near(st.pose.basis[2][3], -8.0, 1e-4));
    // Scales + view config (normal viewport).
    CHECK(near(st.pose.scaleX, 0.8333333, 1e-6));
    CHECK(near(st.pose.scaleY, 1.3888889, 1e-5));
    CHECK(st.pose.scaleZ == -1.0f);
    CHECK(st.pose.modeZoom == 2.4f);
    CHECK(st.pose.viewW == 600 && st.pose.viewH == 360);
    CHECK(st.pose.viewCX == 300 && st.pose.viewCY == 180);
    CHECK(st.pose.viewOX == 0 && st.pose.viewOY == 0);
  }

  // ---- pose: yaw 90 (viewYaw 0) ---------------------------------
  {
    mdk::PlayerCameraState st;
    auto e = camEnv(10.0f, 20.0f, 5.0f, 0.0f, 0.0f);
    mdk::updatePlayerCamera(e, st);
    // facing +Y -> camera 8 behind along -Y.
    CHECK(near(st.pose.pos[0], 10.0, 1e-5));
    CHECK(near(st.pose.pos[1], 12.0, 1e-5));
    CHECK(near(st.pose.pos[2], 9.5, 1e-5));
    CHECK(near(st.pose.back[0], 0.0, 1e-6));
    CHECK(near(st.pose.back[1], -1.0, 1e-5));
    CHECK(near(st.pose.back[2], 0.0, 1e-5));
  }

  // ---- pose: pitch > 0 branch -----------------------------------
  {
    mdk::PlayerCameraState st;
    auto e = camEnv(10.0f, 20.0f, 5.0f, 90.0f, 30.0f);
    mdk::updatePlayerCamera(e, st);
    const double sP = camTrigSin(30.0f), cP = camTrigCos(30.0f);
    const double T = (1.0 - cP) * 5.0;
    const double sY = camTrigSin(90.0f), cY = camTrigCos(90.0f);
    CHECK(near(st.pose.pos[0],
               10.0 - sY * 8.0 * cP + T * sY, 1e-5));
    CHECK(near(st.pose.pos[1],
               20.0 - cY * 8.0 * cP + T * cY, 1e-5));
    CHECK(near(st.pose.pos[2], 5.0 + 4.5 + 8.0 * sP, 1e-5));
    CHECK(near(st.pose.back[0], -sY * cP, 1e-6));
    CHECK(near(st.pose.back[2], sP, 1e-6));
    // Orthonormal basis at nonzero pitch.
    const double rl =
        std::sqrt(st.pose.view[0][0] * st.pose.view[0][0] +
                  st.pose.view[0][1] * st.pose.view[0][1] +
                  st.pose.view[0][2] * st.pose.view[0][2]);
    CHECK(near(rl, st.pose.scaleX, 1e-5));
    // right . back == 0, up . back == 0.
    const double rb =
        st.pose.view[0][0] / st.pose.scaleX * st.pose.back[0] +
        st.pose.view[0][1] / st.pose.scaleX * st.pose.back[1] +
        st.pose.view[0][2] / st.pose.scaleX * st.pose.back[2];
    CHECK(near(rb, 0.0, 1e-5));
    const double ub = st.pose.up[0] * st.pose.back[0] +
                      st.pose.up[1] * st.pose.back[1] +
                      st.pose.up[2] * st.pose.back[2];
    CHECK(near(ub, 0.0, 1e-5));
    CHECK(near(std::sqrt(st.pose.up[0] * st.pose.up[0] +
                         st.pose.up[1] * st.pose.up[1] +
                         st.pose.up[2] * st.pose.up[2]),
               1.0, 1e-5));
  }

  // ---- pose: -20 < pitch < 0 (D = pullback) ---------------------
  {
    mdk::PlayerCameraState st;
    auto e = camEnv(10.0f, 20.0f, 5.0f, 90.0f, -10.0f);
    mdk::updatePlayerCamera(e, st);
    const double sP = camTrigSin(-10.0f), cP = camTrigCos(-10.0f);
    CHECK(near(st.pose.pos[0], 10.0 - 8.0 * cP, 1e-5));
    CHECK(near(st.pose.pos[2], 5.0 + 4.5 + 8.0 * sP, 1e-5));
  }

  // ---- pose: pitch < -20 (D shrinks toward 0 at pitch = -100) ---
  {
    mdk::PlayerCameraState st;
    auto e = camEnv(10.0f, 20.0f, 5.0f, 90.0f, -50.0f);
    mdk::updatePlayerCamera(e, st);
    // D = (-50 + 100) * 8 * 0.0125 = 5.0 — continuous at -20.
    const double cP = camTrigCos(-50.0f), sP = camTrigSin(-50.0f);
    CHECK(near(st.pose.pos[0], 10.0 - 5.0 * cP, 1e-5));
    CHECK(near(st.pose.pos[2], 5.0 + 4.5 + 5.0 * sP, 1e-5));
    // Boundary: pitch = -20 -> D = pullback on both sides.
    mdk::PlayerCameraState st2;
    auto e2 = camEnv(10.0f, 20.0f, 5.0f, 90.0f, -20.0f);
    mdk::updatePlayerCamera(e2, st2);
    CHECK(near(st2.pose.pos[0],
               10.0 - 8.0 * camTrigCos(-20.0f), 1e-5));
    mdk::PlayerCameraState st3;
    auto e3 = camEnv(10.0f, 20.0f, 5.0f, 90.0f, -20.001f);
    mdk::updatePlayerCamera(e3, st3);
    CHECK(near(st3.pose.pos[0], st2.pose.pos[0], 1e-3));
  }

  // ---- bank rolls the up/right vectors, not the position --------
  {
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    e.bankDeg = 45.0f;
    mdk::updatePlayerCamera(e, st);
    CHECK(near(st.pose.pos[0], -8.0, 1e-5));
    const double s45 = camTrigSin(45.0f);
    CHECK(near(st.pose.up[1], -s45, 1e-5));
    CHECK(near(st.pose.up[2], s45, 1e-5));
    // right = up x back — banked: (0, -cos45, -sin45)-ish.
    CHECK(near(st.pose.view[0][2] / st.pose.scaleX, -s45, 1e-5));
  }

  // ---- projection: alt aspect + zoom -----------------------------
  {
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    e.altAspect = true;
    mdk::updatePlayerCamera(e, st);
    // 1/(2.4 * 280 * (1/384) * 0.5) = 1/0.875.
    CHECK(near(st.pose.scaleY, 1.1428571, 1e-5));
    mdk::PlayerCameraState st2;
    st2.zoom = 1.2f;
    auto e2 = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    mdk::updatePlayerCamera(e2, st2);
    CHECK(near(st2.pose.scaleX, 1.0 / 0.6, 1e-5));
    CHECK(near(st2.pose.scaleY,
               1.0 / (1.2 * 360.0 * (1.0 / 600.0) * 0.5), 1e-5));
  }

  // ---- shake: |ce4| gate + 0.2 scale ----------------------------
  {
    mdk::PlayerCameraState st;
    st.shakeMag = 1.0f;
    st.shakeX = 10.0f;
    st.shakeY = -5.0f;
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    mdk::updatePlayerCamera(e, st);
    CHECK(near(st.pose.pos[0], -8.0 + 10.0 * 0.2, 1e-5));
    CHECK(near(st.pose.pos[1], 0.0 - 5.0 * 0.2, 1e-5));
    // Gate: shakeMag == 0 suppresses the add.
    mdk::PlayerCameraState st2;
    st2.shakeX = 10.0f;
    auto e2 = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    mdk::updatePlayerCamera(e2, st2);
    CHECK(near(st2.pose.pos[0], -8.0, 1e-5));
  }

  // ---- world->camera: player lands at (0, +6.25, +8) in view ----
  {
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    mdk::updatePlayerCamera(e, st);
    // FUN_0046b4f8 convention: out_i = row_i . p + t_i.
    const float pt[3] = {0.0f, 0.0f, 0.0f};
    const double ox = st.pose.view[0][0] * pt[0] +
                      st.pose.view[0][1] * pt[1] +
                      st.pose.view[0][2] * pt[2] + st.pose.view[0][3];
    const double oy = st.pose.view[1][0] * pt[0] +
                      st.pose.view[1][1] * pt[1] +
                      st.pose.view[1][2] * pt[2] + st.pose.view[1][3];
    const double oz = st.pose.view[2][0] * pt[0] +
                      st.pose.view[2][1] * pt[1] +
                      st.pose.view[2][2] * pt[2] + st.pose.view[2][3];
    CHECK(near(ox, 0.0, 1e-5));
    CHECK(near(oy, 6.25, 1e-4));   // screenY numerator (scaled down)
    CHECK(near(oz, 8.0, 1e-4));    // positive depth ahead
  }

  // ---- obstruction seam gate ------------------------------------
  {
    mdk::PlayerCameraState st;
    st.obstructionEnabled = true;
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    CHECK(mdk::updatePlayerCamera(e, st).obstructionSeam);
    // |d58| != 0 suppresses the call (OBSERVED).
    e.lookActive = true;
    CHECK(!mdk::updatePlayerCamera(e, st).obstructionSeam);
    // b710 == 0 suppresses the call (the field now defaults to the
    // OBSERVED image init 1 — clear it explicitly to test the gate).
    mdk::PlayerCameraState st2;
    st2.obstructionEnabled = false;
    e.lookActive = false;
    CHECK(!mdk::updatePlayerCamera(e, st2).obstructionSeam);
  }

  // ---- sniper viewport rect (state write only) -------------------
  {
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    e.sniperViewport = true;
    mdk::updatePlayerCamera(e, st);
    CHECK(st.pose.modeZoom == 1.0f);
    CHECK(st.pose.viewW == 384 && st.pose.viewH == 280);
    CHECK(st.pose.viewCX == 299 && st.pose.viewCY == 219);
    CHECK(st.pose.viewOX == 107 && st.pose.viewOY == 79);
  }

  // ---- FUN_00431100 overhead path --------------------------------
  {
    mdk::PlayerCameraState st;
    st.overheadHeight = 50.0f;
    st.pose.back[0] = 7.0f;        // stale sentinel — must survive
    auto e = camEnv(1.0f, 2.0f, 3.0f, 90.0f, 0.0f);
    e.yawDeg = 0.0f;               // raw 0x540c2c, NOT viewYaw
    mdk::updatePlayerCameraOverhead(e, st);
    CHECK(near(st.pose.pos[0], 1.0, 1e-6));
    CHECK(near(st.pose.pos[1], 2.0, 1e-6));
    CHECK(near(st.pose.pos[2], 53.0, 1e-5));
    CHECK(st.pose.scaleZ == 1.0f);
    CHECK(st.pose.back[0] == 7.0f);      // not written by overhead
    // M2 rows verbatim: right=(sin,-cos,0), down=(-cos,-sin,0),
    // back=(0,0,-1); tA = py*cos - px*sin, tB = px*cos + py*sin.
    CHECK(near(st.pose.basis[0][0], 0.0, 1e-6));
    CHECK(near(st.pose.basis[0][1], -1.0, 1e-5));
    CHECK(near(st.pose.basis[0][2], 0.0, 1e-6));
    CHECK(near(st.pose.basis[0][3], 2.0, 1e-5));    // tA
    CHECK(near(st.pose.basis[1][0], -1.0, 1e-5));
    CHECK(near(st.pose.basis[1][1], 0.0, 1e-6));
    CHECK(near(st.pose.basis[1][3], 1.0, 1e-5));    // tB
    CHECK(near(st.pose.basis[2][2], -1.0, 1e-6));
    CHECK(near(st.pose.basis[2][3], 53.0, 1e-4));   // tC = camZ
    // M1 folds the scales; row2 = (0,0,-1,camZ) with scaleZ=+1.
    CHECK(near(st.pose.view[0][1], -0.8333333, 1e-5));
    CHECK(near(st.pose.view[1][0], -1.3888889, 1e-4));
    CHECK(near(st.pose.view[2][2], -1.0, 1e-6));
    CHECK(near(st.pose.view[2][3], 53.0, 1e-4));
    // Raw yaw 45 vs viewYaw — proves the overhead path does NOT use
    // 90-yaw: with yaw=45 raw, right = (sin45,-cos45,0).
    mdk::PlayerCameraState st2;
    st2.overheadHeight = 50.0f;
    auto e2 = camEnv(1.0f, 2.0f, 3.0f, 90.0f, 0.0f);
    e2.yawDeg = 45.0f;
    mdk::updatePlayerCameraOverhead(e2, st2);
    CHECK(near(st2.pose.basis[0][0], camTrigSin(45.0f), 1e-6));
  }

  // ---- runtime: camera produced per frame + portal tail ---------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "CAM_A");
    mdk::TraversalArena* b = travArenaAdd(rt, "CAM_B");
    a->dyn.col.verts = f.verts.data();
    a->dyn.col.polys = f.polys.data();
    a->dyn.col.nodes = f.nodes.data();
    a->dyn.col.deepFloorZ = -1000.0f;
    // Type-6 side-0 portal at x = -4: the settled camera sits at
    // x ~ -8 behind a player at x ~ 0 facing +X (viewYaw = 90).
    rt.level.work[0].subRecords.push_back(mdk::DtiSubRecord{});
    rt.level.work[0].subRecords.back() = travSub(
        6, {1, 0, fbits(-4.f), fbits(-10.f), fbits(5.f),
            fbits(-4.f), fbits(10.f), fbits(25.f)});
    rt.cur = a;
    rt.partner = b;
    rt.partnerActive = true;
    rt.cs.arena = &a->dyn.col;
    rt.cs.queryEnabled = 1;
    rt.cs.arenaValid = 1;
    rt.cs.objectDataLoaded = 1;
    rt.cs.pos[2] = 12.0f;
    rt.cs.entryPos[2] = 12.0f;
    const mdk::GameplayInputBindings bindings;
    const mdk::FrontendTimingState timing;
    const mdk::RawGameplayInput idle{};
    mdk::TraversalFrameResult out;
    for (int i = 0; i < 40; ++i)
      out = mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    // Settled on the z=10 floor: camera behind at -X, ~+4.5 z.
    CHECK(out.grounded);
    CHECK(std::isfinite(out.camera.pos[0]) &&
          std::isfinite(out.camera.pos[1]) &&
          std::isfinite(out.camera.pos[2]));
    CHECK(out.camera.pos[0] < out.pos[0] - 4.0f);
    CHECK(near(out.camera.pos[1], out.pos[1], 1e-3));
    CHECK(out.camera.pos[2] > out.pos[2]);
    CHECK(near(out.camera.scaleX, 0.8333333, 1e-5));
    CHECK(out.camera.scaleZ == -1.0f);
    CHECK(out.camera.viewW == 600 && out.camera.viewH == 360);
    // The eye->camPos segment crosses x=-4 going -x -> viewOnPartner.
    CHECK(out.viewOnPartner);
    CHECK(!out.overheadViewActive);
    // Overhead path: flag49b740 routes to FUN_00431100.
    rt.flag49b740 = 1;
    rt.camera.overheadHeight = 50.0f;
    out = mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(out.overheadViewActive);
    CHECK(out.seams.overheadViewCalls >= 1);
    CHECK(near(out.camera.pos[2], out.pos[2] + 50.0f, 1e-3));
    CHECK(out.camera.scaleZ == 1.0f);
    // OBSERVED: the overhead path does NOT run the portal tail —
    // viewOnPartner keeps its previous value.
    CHECK(out.viewOnPartner);
    rt.flag49b740 = 0;
    out = mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(!out.overheadViewActive);
    CHECK(out.camera.scaleZ == -1.0f);
  }

  // ---- golden sequence: deterministic across identical runs -----
  {
    auto runSeq = []() -> std::uint64_t {
      CollisionFixture f = makeFloorArena();
      mdk::TraversalRuntime rt;
      mdk::TraversalArena* a = travArenaAdd(rt, "CAM_A");
      a->dyn.col.verts = f.verts.data();
      a->dyn.col.polys = f.polys.data();
      a->dyn.col.nodes = f.nodes.data();
      a->dyn.col.deepFloorZ = -1000.0f;
      rt.cur = a;
      rt.cs.arena = &a->dyn.col;
      rt.cs.queryEnabled = 1;
      rt.cs.arenaValid = 1;
      rt.cs.objectDataLoaded = 1;
      rt.cs.pos[2] = 12.0f;
      rt.cs.entryPos[2] = 12.0f;
      const mdk::GameplayInputBindings bindings;
      const mdk::FrontendTimingState timing;
      const mdk::RawGameplayInput idle{};
      mdk::RawGameplayInput lookKey{};
      lookKey.keyLevel[30 >> 5] |= 1u << (30 & 31);
      std::uint64_t h = 1469598103934665603ull;
      auto mix = [&](float v) {
        std::uint32_t b;
        std::memcpy(&b, &v, 4);
        h ^= b;
        h *= 1099511628211ull;
      };
      for (int i = 0; i < 60; ++i) {
        const auto& key = (i >= 20 && i < 40) ? lookKey : idle;
        const auto out =
            mdk::stepTraversalRuntime(rt, key, bindings, timing);
        for (int k = 0; k < 3; ++k) mix(out.camera.pos[k]);
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 4; ++c) mix(out.camera.view[r][c]);
        mix(out.camera.scaleX);
        mix(out.camera.scaleY);
        mix(out.viewPitchDeg);
        mix(out.viewYawDeg);
      }
      return h;
    };
    CHECK(runSeq() == runSeq());
  }
}

// Phase 5M — camera obstruction (FUN_00430bf8) + camera nudge
// (FUN_0042b0c0). Expected values are computed from the OBSERVED
// disassembly: eye = pos + (0,0,5.5), swept-box {0.1,0.1,0.1},
// flag=0; displacement = 2D |hitPos - camPos| * flipped plane XY;
// the PLAYER is moved by FUN_004630d4 and the camera follows the
// applied delta.
namespace {

// Vertical wall at x = -4 facing +x (the eye->camera segment of a
// player at the origin facing +X crosses it at ~(-3.9, 0, 10)).
CollisionFixture makeCamWallArena() {
  CollisionFixture f;
  f.verts = {-4, -10, 0, -4, 10, 0, -4, 0, 20};
  f.polys = {makePoly(0, 1, 2)};
  f.nodes = {makeNode(1, 0, 0, 4, polySet(1, 0), 0, -1, -1)};
  f.finish();
  return f;
}

// Wall at x = -4 whose far side holds a partial floor at z = 5
// covering only x in [3,6] x y in [1.5,3]: candidate (4.1, 0) and
// retry1 (4.1, -2.05) miss it; retry2 (4.1, +2.05) lands inside.
CollisionFixture makeCamWallFloorArena() {
  CollisionFixture f;
  f.verts = {-4, -10, 0, -4, 10, 0, -4, 0, 20,
             3, 1.5f, 5, 6, 1.5f, 5, 4, 3, 5};
  f.polys = {makePoly(0, 1, 2), makePoly(3, 4, 5)};
  f.nodes = {makeNode(1, 0, 0, 4, polySet(1, 0), 0, 1, -1),
             makeNode(0, 0, 1, -5, polySet(1, 1), 0, -1, -1)};
  f.finish();
  return f;
}

} // namespace

void test_camera_obstruction() {
  // ---- static hit: wall clips the arm; player pushed +4.1, camera
  //      follows the applied delta to the box margin --------------
  {
    CollisionFixture f = makeCamWallArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[0] = 0.0f;
    cs.pos[1] = 0.0f;
    cs.pos[2] = 5.0f;
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e.collision = &cs;
    e.contactToken = nullptr;
    const mdk::PlayerCameraFrame fr = mdk::updatePlayerCamera(e, st);
    CHECK(fr.obstructionSeam);
    // eye (0,0,10.5) -> cam (-8,0,9.5) crosses x=-4; the swept box
    // face stops 0.1 short: hitPos.x = -3.9, dist = 4.1 along +x.
    CHECK(near(cs.pos[0], 4.1, 1e-4) && near(cs.pos[1], 0.0, 1e-6) &&
          near(cs.pos[2], 5.0, 1e-6));
    CHECK(near(st.pose.pos[0], -3.9, 1e-4) &&
          near(st.pose.pos[1], 0.0, 1e-6) &&
          near(st.pose.pos[2], 9.5, 1e-4));
    // The env channel returns the post-obstruction player pos.
    CHECK(near(e.playerPos[0], cs.pos[0], 1e-6));
    // The M1 commit ran AFTER the shift: view[2][3] folds the new
    // camPos — -(back . cam) * scaleZ = -(-1 * -3.9) * -1 = +3.9.
    CHECK(near(st.pose.view[2][3], 3.9, 1e-4));
  }

  // ---- miss: no geometry on the segment -> no displacement -------
  {
    CollisionFixture f = makeEmptyArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 5.0f;
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e.collision = &cs;
    mdk::updatePlayerCamera(e, st);
    CHECK(near(cs.pos[0], 0.0) && near(cs.pos[2], 5.0));
    CHECK(near(st.pose.pos[0], -8.0, 1e-5) &&
          near(st.pose.pos[2], 9.5, 1e-5));
  }

  // ---- gates: disabled flag / look-active suppress the pass ------
  {
    CollisionFixture f = makeCamWallArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 5.0f;
    mdk::PlayerCameraState st;
    st.obstructionEnabled = false;
    auto e = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e.collision = &cs;
    mdk::updatePlayerCamera(e, st);
    CHECK(near(cs.pos[0], 0.0) && near(st.pose.pos[0], -8.0, 1e-5));

    mdk::PlayerCameraState st2;
    auto e2 = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e2.collision = &cs;
    e2.lookActive = true;
    mdk::updatePlayerCamera(e2, st2);
    CHECK(near(cs.pos[0], 0.0) && near(st2.pose.pos[0], -8.0, 1e-5));
  }

  // ---- carrier retry: primary misses, carrier arena hits ---------
  {
    CollisionFixture fp = makeEmptyArena();
    CollisionFixture fc = makeCamWallArena();
    mdk::CollisionState cs = makeCollisionState(&fp.arena);
    cs.carrier = &fc.arena;
    cs.carrierBusy = 0;
    cs.pos[2] = 5.0f;
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e.collision = &cs;
    mdk::updatePlayerCamera(e, st);
    CHECK(near(cs.pos[0], 4.1, 1e-4));
    CHECK(near(st.pose.pos[0], -3.9, 1e-4));
    // carrierBusy (0x540d3c) suppresses the retry.
    mdk::CollisionState cs2 = makeCollisionState(&fp.arena);
    cs2.carrier = &fc.arena;
    cs2.carrierBusy = 1;
    cs2.pos[2] = 5.0f;
    mdk::PlayerCameraState st2;
    auto e2 = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e2.collision = &cs2;
    mdk::updatePlayerCamera(e2, st2);
    CHECK(near(cs2.pos[0], 0.0) && near(st2.pose.pos[0], -8.0, 1e-5));
  }

  // ---- grounding probe: 0x540e4c != 0 requires the +-4 stab to
  //      land on primary-arena geometry; perpendicular retries ----
  {
    CollisionFixture f = makeCamWallFloorArena();
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 5.0f;
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e.collision = &cs;
    e.contactToken = &f.polys[0];  // last-contact token nonzero
    mdk::updatePlayerCamera(e, st);
    // h = 0.5 * 4.1 = 2.05; candidate (4.1,0) misses the partial
    // floor, retry1 (4.1,-2.05) misses, retry2 (4.1,+2.05) lands —
    // the mirrored perpendicular displacement is applied.
    CHECK(near(cs.pos[0], 4.1, 1e-4) && near(cs.pos[1], 2.05, 1e-4));
    CHECK(near(st.pose.pos[0], -3.9, 1e-4) &&
          near(st.pose.pos[1], 2.05, 1e-4));
  }

  // ---- grounding probe exhausted: all three stabs miss -> early
  //      return, no apply AND no object pass -----------------------
  {
    CollisionFixture f = makeCamWallFloorArena();
    // Move the floor tri where no candidate reaches it.
    f.verts = {-4, -10, 0, -4, 10, 0, -4, 0, 20,
               8, 8, 5, 10, 8, 5, 9, 10, 5};
    f.finish();
    // An object that WOULD clamp the segment if the pass ran.
    ObjectFixture o;
    float eb[6] = {-6, -1, 8, -5, 1, 11};
    std::memcpy(o.elem.aabb, eb, sizeof(eb));
    o.elemSet.count = 1;
    o.elemSet.elems = &o.elem;
    o.obj.named = true;
    o.obj.model = &o.modelDummy;
    o.obj.elements = &o.elemSet;
    o.obj.flags14b = 1;
    std::memcpy(o.obj.aabb, eb, sizeof(eb));
    f.arena.objects = &o.obj;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 5.0f;
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e.collision = &cs;
    e.contactToken = &f.polys[0];
    mdk::updatePlayerCamera(e, st);
    CHECK(near(cs.pos[0], 0.0) && near(cs.pos[1], 0.0));
    CHECK(near(st.pose.pos[0], -8.0, 1e-5) &&
          near(st.pose.pos[1], 0.0, 1e-6));
  }

  // ---- object pass: AABB clamp pulls the camera in front of the
  //      box; the player is pushed by (clamp - camPos).xy ----------
  {
    CollisionFixture f = makeEmptyArena();
    ObjectFixture o;
    float eb[6] = {-6, -1, 8, -5, 1, 11};
    std::memcpy(o.elem.aabb, eb, sizeof(eb));
    o.elemSet.count = 1;
    o.elemSet.elems = &o.elem;
    o.obj.named = true;
    o.obj.model = &o.modelDummy;
    o.obj.elements = &o.elemSet;
    o.obj.flags14b = 1;
    std::memcpy(o.obj.aabb, eb, sizeof(eb));
    f.arena.objects = &o.obj;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 5.0f;
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e.collision = &cs;
    mdk::updatePlayerCamera(e, st);
    // Segment enters the box at x = -5 (t = 0.625): clamp
    // (-5, 0, 9.875); delta = clamp.xy - camPos.xy = (+3, 0).
    CHECK(near(cs.pos[0], 3.0, 1e-4) && near(cs.pos[1], 0.0, 1e-6));
    CHECK(near(st.pose.pos[0], -5.0, 1e-4) &&
          near(st.pose.pos[2], 9.5, 1e-4));
  }

  // ---- object flag filters: 0x810 and !bit0 skip the object ------
  {
    CollisionFixture f = makeEmptyArena();
    ObjectFixture o;
    float eb[6] = {-6, -1, 8, -5, 1, 11};
    std::memcpy(o.elem.aabb, eb, sizeof(eb));
    o.elemSet.count = 1;
    o.elemSet.elems = &o.elem;
    o.obj.named = true;
    o.obj.model = &o.modelDummy;
    o.obj.elements = &o.elemSet;
    std::memcpy(o.obj.aabb, eb, sizeof(eb));
    f.arena.objects = &o.obj;

    // flags148 & 0x800 -> skipped.
    o.obj.flags148 = 0x800;
    o.obj.flags14b = 1;
    mdk::CollisionState cs = makeCollisionState(&f.arena);
    cs.pos[2] = 5.0f;
    mdk::PlayerCameraState st;
    auto e = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e.collision = &cs;
    mdk::updatePlayerCamera(e, st);
    CHECK(near(cs.pos[0], 0.0) && near(st.pose.pos[0], -8.0, 1e-5));

    // flags14b bit0 clear -> skipped.
    o.obj.flags148 = 0;
    o.obj.flags14b = 0;
    mdk::CollisionState cs2 = makeCollisionState(&f.arena);
    cs2.pos[2] = 5.0f;
    mdk::PlayerCameraState st2;
    auto e2 = camEnv(0.0f, 0.0f, 5.0f, 90.0f, 0.0f);
    e2.collision = &cs2;
    mdk::updatePlayerCamera(e2, st2);
    CHECK(near(cs2.pos[0], 0.0) && near(st2.pose.pos[0], -8.0, 1e-5));
  }
}

// Phase 5M — FUN_0042b0c0 nudge + FUN_0042b20c mode->scale.
void test_camera_nudge() {
  // ---- arg=+1: pos += M2row0*0.25; tick=20; M1 row0 += row2*r ----
  {
    mdk::PlayerCameraState st;
    st.obstructionEnabled = false;   // isolate the nudge math
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    mdk::updatePlayerCamera(e, st);
    // Baseline: camPos (-8,0,4.5); M2 row0 = right*|right| = (0,-1,0);
    // M1 rows: (0,-5/6,0) / (0,0,-25/18) / (1,0,0).
    CHECK(near(st.pose.basis[0][1], -1.0, 1e-5));
    mdk::cameraNudge(1, st);
    // pos += (0,-1,0) * (1 * 0.25).
    CHECK(near(st.pose.pos[0], -8.0, 1e-5) &&
          near(st.pose.pos[1], -0.25, 1e-5) &&
          near(st.pose.pos[2], 4.5, 1e-5));
    // tick = trunc(1 * 20.0) = 20; r = 20/600 = 1/30.
    CHECK(st.nudgeTick == 20);
    const double r = 20.0 / 600.0;
    // M1[0] += M1[2] * r: (0,-5/6,0) + (1,0,0)*r -> (r, -5/6, 0).
    CHECK(near(st.pose.view[0][0], r, 1e-6));
    CHECK(near(st.pose.view[0][1], -0.8333333, 1e-6));
    CHECK(near(st.pose.view[0][2], 0.0, 1e-6));
    // Translations refolded against the UPDATED row0 + camPos:
    //   t0 = -(r*-8 + (-5/6)*-0.25 + 0*4.5) = -(-8r + 5/24).
    CHECK(near(st.pose.view[0][3], -(-8.0 * r + 5.0 / 24.0), 1e-5));
    CHECK(near(st.pose.view[1][3], 6.25, 1e-4));
    CHECK(near(st.pose.view[2][3], 8.0, 1e-4));
    // M2 untouched.
    CHECK(near(st.pose.basis[2][3], -8.0, 1e-4));
  }

  // ---- arg=-1 mirrors the shift and the tick sign ----------------
  {
    mdk::PlayerCameraState st;
    st.obstructionEnabled = false;
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    mdk::updatePlayerCamera(e, st);
    mdk::cameraNudge(-1, st);
    CHECK(near(st.pose.pos[1], 0.25, 1e-5));
    CHECK(st.nudgeTick == -20);
    CHECK(near(st.pose.view[0][0], -20.0 / 600.0, 1e-6));
  }

  // ---- truncation: FRNDINT under the trunc control word ----------
  {
    mdk::PlayerCameraState st;
    st.obstructionEnabled = false;
    st.nudgeScale = 12.7f;
    auto e = camEnv(0.0f, 0.0f, 0.0f, 90.0f, 0.0f);
    mdk::updatePlayerCamera(e, st);
    mdk::cameraNudge(1, st);
    CHECK(st.nudgeTick == 12);   // toward zero, not round-nearest 13
    mdk::cameraNudge(-1, st);
    CHECK(st.nudgeTick == -12);  // toward zero, not floor -13
  }

  // ---- FUN_0042b20c mode -> nudgeScale map -----------------------
  {
    mdk::PlayerCameraState st;
    mdk::cameraNudgeApplyMode(st, 0);
    CHECK(st.nudgeScale == 12.0f);
    mdk::cameraNudgeApplyMode(st, 1);
    CHECK(st.nudgeScale == 20.0f);
    mdk::cameraNudgeApplyMode(st, 2);
    CHECK(st.nudgeScale == 15.0f);
    mdk::cameraNudgeApplyMode(st, 3);
    CHECK(st.nudgeScale == 20.0f);
    mdk::cameraNudgeApplyMode(st, 9);
    CHECK(st.nudgeScale == 20.0f);
  }
}

// Phase 5N — player weapon fire. Direct unit tests on the bounded
// functions: the FUN_0045f138 dispatch (the sniper-scoped spawn), the
// FUN_00432f84 punch hitscan, the FUN_00437660 cadence machine, the
// FUN_00469b98 selector, the FUN_00465228 fire latch, and the
// FUN_00437aa8 charge probe — all ported from BUILD_A.
void test_player_fire() {
  const mdk::GameplayInputBindings bindings;
  const mdk::FrontendTimingState timing;
  const mdk::RawGameplayInput idle{};

  // ---- FUN_00437660 cadence/burst/ammo machine --------------------
  {
    mdk::TraversalRuntime rt;
    // Pending weapon -> the blend leg: cadence += dt*8, adopt at 3.0.
    rt.wpnSel0 = 0; rt.wpnSel1 = 2; rt.fireCadence = 2.9f;
    rt.burstIndex = 5; rt.ammo[2] = 9;
    mdk::playerWeaponCadence(rt, 0.02f);    // 2.9 + 0.16 = 3.06 >= 3.0
    CHECK(rt.wpnSel0 == 2 && rt.burstIndex == 0);
    // Same weapon -> the cadence decays then a burst pip recharges.
    rt.wpnSel0 = rt.wpnSel1 = 2; rt.fireCadence = 1.0f;
    rt.burstIndex = 1; rt.ammo[2] = 4;
    mdk::playerWeaponCadence(rt, 0.1f);     // 1.0 - 0.4 = 0.6
    CHECK(near(rt.fireCadence, 0.6, 1e-5) && rt.burstIndex == 2);
    // Empty weapon stuck at 0 -> reloads to weapon 0.
    rt.wpnSel0 = rt.wpnSel1 = 3; rt.fireCadence = 1.0f;
    rt.burstIndex = 0; rt.ammo[3] = 0;
    mdk::playerWeaponCadence(rt, 0.1f);
    CHECK(rt.wpnSel1 == 0 && rt.burstIndex == 1);
    // (0x4999d0 && 0x541548) -> the machine is skipped entirely.
    rt.flag4999d0 = true; rt.flag541548 = true;
    rt.wpnSel0 = rt.wpnSel1 = 0; rt.fireCadence = 1.0f; rt.burstIndex = 0;
    mdk::playerWeaponCadence(rt, 0.1f);
    CHECK(rt.burstIndex == 0 && near(rt.fireCadence, 1.0, 1e-5));
  }

  // ---- FUN_00469b98 scoped weapon selector ------------------------
  {
    mdk::TraversalRuntime rt;
    mdk::GameplayInputFrame ctrl{};
    rt.ammo = {0, 5, 0, 3, 0, 7};
    ctrl.weaponSelect[1] = 1;
    mdk::playerWeaponSelect(rt, ctrl);
    CHECK(rt.wpnSel1 == 1);                  // ammo[1] > 0 -> 1
    ctrl = mdk::GameplayInputFrame{};
    ctrl.weaponSelect[2] = 1;
    mdk::playerWeaponSelect(rt, ctrl);
    CHECK(rt.wpnSel1 == 1);                  // ammo[2]==0 -> unchanged
    ctrl = mdk::GameplayInputFrame{};
    ctrl.weaponSelect[0] = 1;
    mdk::playerWeaponSelect(rt, ctrl);
    CHECK(rt.wpnSel1 == 0);                  // weapon 0 unconditional
    // itemNext wrap-scan skipping empty slots: 0 -> 2 (1 empty).
    ctrl = mdk::GameplayInputFrame{};
    ctrl.itemNext = 1; rt.wpnSel1 = 0;
    rt.ammo = {0, 0, 2, 4, 0, 6};
    mdk::playerWeaponSelect(rt, ctrl);
    CHECK(rt.wpnSel1 == 2);
    // itemPrev from 0 wraps to the top non-empty slot (5).
    ctrl = mdk::GameplayInputFrame{};
    ctrl.itemPrev = 1; rt.wpnSel1 = 0;
    mdk::playerWeaponSelect(rt, ctrl);
    CHECK(rt.wpnSel1 == 5);
  }

  // ---- FUN_00465228 normal-mode fire latch ------------------------
  {
    mdk::TraversalRuntime rt;
    mdk::GameplayInputFrame ctrl{};
    // Press with a free event slot -> latch + the 0x12c/3 fire event.
    ctrl.fire = 1;
    mdk::playerFireLatch(rt, ctrl);
    CHECK(rt.fieldC74 == 1 && rt.eventMag == 0x12c && rt.eventType == 3);
    // A busy pending event (eventMag=400) suppresses the fire event.
    rt.eventMag = 400; rt.eventType = 1;
    mdk::playerFireLatch(rt, ctrl);
    CHECK(rt.eventMag == 400 && rt.fieldC74 == 1);
    // Release -> the latch clears (notify off).
    ctrl.fire = 0; rt.eventPriority = 0; rt.eventType = 0;
    mdk::playerFireLatch(rt, ctrl);
    CHECK(rt.fieldC74 == 0);
  }

  // ---- FUN_00437aa8 charge-probe flag ------------------------------
  {
    mdk::TraversalRuntime rt;
    rt.wpnSel0 = 5; rt.weapon5Probe = 1; rt.fieldE14 = 0;
    mdk::playerChargeProbe(rt);
    CHECK(rt.fieldE14 == 1 && rt.seams.chargeProbeCalls == 1);
    // Gated off while wpnSel != 5 or the cadence hasn't elapsed.
    rt.wpnSel0 = 0; rt.fieldE14 = 0;
    mdk::playerChargeProbe(rt);
    CHECK(rt.fieldE14 == 0);
    rt.wpnSel0 = 5; rt.fireCadence = 1.0f;
    mdk::playerChargeProbe(rt);
    CHECK(rt.fieldE14 == 0);
  }

  // ---- FUN_0045f138 dispatch — weapons 0..4 shot spawn ------------
  {
    mdk::TraversalRuntime rt;
    rt.level.enemies.entries = {{"SW_HOME", 7, false},
                                {"SW_SGREN", 8, false},
                                {"SW_HGREN", 9, false},
                                {"SW_LGREN", 10, false}};
    rt.camera.pose.pos[0] = 1; rt.camera.pose.pos[1] = 2;
    rt.camera.pose.pos[2] = 3;
    rt.motion.yawDeg = 45.0f;
    rt.viewScalar = 10.0f; rt.look.lookPitchOffset = -5.0f;
    rt.fieldD0c = 9; rt.burstIndex = 2;
    // weapon 0 — the tracer, no ammo decrement, default class record.
    rt.wpnSel0 = 0;
    mdk::playerFireDispatch(rt);
    const mdk::PlayerShot& s0 = rt.shots[0];
    CHECK(s0.state == 1 && s0.type == 0 &&
          s0.flyKind == mdk::kShotFlyTracer);
    CHECK(s0.lifetime == 0x4b && near(s0.speedH, 1100.0, 1e-5));
    CHECK(s0.classIdx == -1);                // 0x4edd48 default
    CHECK(near(s0.yawDeg, 45.0, 1e-5) && near(s0.pitchDeg, 5.0, 1e-5));
    CHECK(near(s0.pos[0], 1.0, 1e-5) && near(s0.fieldCc, 2.0, 1e-5));
    CHECK(rt.fieldD0c == 0 && rt.burstIndex == 1 &&
          near(rt.fireCadence, 1.0, 1e-5) && rt.shotSerial == 1);
    CHECK(rt.seams.shotSpawnCalls == 1 && rt.seams.fireSoundCalls == 1);

    // Busy pool -> silent return (no spawn, no serial, no ammo drain).
    rt.shots[0].state = rt.shots[1].state = rt.shots[2].state = 1;
    const int serial = rt.shotSerial, ammo1 = rt.ammo[1];
    mdk::playerFireDispatch(rt);
    CHECK(rt.shotSerial == serial && rt.ammo[1] == ammo1);

    // weapon 1 — homing grenade: ammo[1]-- + the SW_HOME class index.
    rt = mdk::TraversalRuntime{};
    rt.level.enemies.entries = {{"SW_HOME", 7, false}};
    rt.wpnSel0 = 1; rt.ammo[1] = 6; rt.burstIndex = 1;
    mdk::playerFireDispatch(rt);
    CHECK(rt.shots[0].type == 1 &&
          rt.shots[0].flyKind == mdk::kShotFlyGrenade &&
          rt.shots[0].lifetime == 0xf0 &&
          near(rt.shots[0].speedH, 400.0, 1e-5));
    CHECK(rt.ammo[1] == 5 && rt.shots[0].classIdx == 0);
    CHECK(rt.seams.classLookupCalls == 1);

    // weapon 4 — the lobbed shot: speedH/V derive from the pitch.
    rt = mdk::TraversalRuntime{};
    rt.level.enemies.entries = {{"SW_LGREN", 10, false}};
    rt.wpnSel0 = 4; rt.ammo[4] = 8;
    rt.viewScalar = 0.0f; rt.look.lookPitchOffset = 30.0f;
    mdk::playerFireDispatch(rt);
    const mdk::PlayerShot& s4 = rt.shots[0];
    CHECK(s4.type == 4 && s4.flyKind == mdk::kShotFlyLobbed &&
          s4.lifetime == 0x1c2 && rt.ammo[4] == 7 && s4.classIdx == 0);
    CHECK(near(s4.speedH, std::cos(30.0 * M_PI / 180.0) * 150.0, 1e-3));
    CHECK(near(s4.speedV, -std::sin(30.0 * M_PI / 180.0) * 150.0, 1e-3));
  }

  // ---- FUN_0045f138 weapon 5 — the thrown-object path -------------
  {
    mdk::TraversalRuntime rt;
    rt.wpnSel0 = 5; rt.ammo[5] = 9; rt.burstIndex = 2;
    // Dead probe -> the can't-fire notify, no spawn, no ammo drain.
    rt.fieldE14 = 0; rt.field54163b = 0;
    mdk::playerFireDispatch(rt);
    CHECK(rt.seams.fireDenyCalls == 1 &&
          rt.seams.weapon5SpawnCalls == 0 && rt.ammo[5] == 9);
    // Live probe + charge >= 4 -> latches AND still fires this shot.
    rt.fieldE14 = 1; rt.field541498 = 4;
    mdk::playerFireDispatch(rt);
    CHECK(rt.field54163b == 1 && rt.ammo[5] == 8 &&
          rt.seams.weapon5SpawnCalls == 1 && rt.burstIndex == 1);
    // The latch now blocks the next shot.
    const int spawns = rt.seams.weapon5SpawnCalls;
    mdk::playerFireDispatch(rt);
    CHECK(rt.seams.fireDenyCalls == 2 &&
          rt.seams.weapon5SpawnCalls == spawns);
  }

  // ---- FUN_0045a4dc weapon-5 spawn + shared path record -----------
  {
    auto pathF = [](const mdk::TraversalRuntime& rt, int k, int w) {
      float v = 0.0f;
      std::memcpy(&v, &rt.weapon5Path[1 + k * 10 + w], 4);
      return v;
    };
    // Charged throw (charge >= 4 latches before the spawn).
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* cur = travArenaAdd(rt, "A");
    rt.cur = cur;
    CollisionFixture empty = makeEmptyArena();
    rt.cs.arena = &empty.arena;
    rt.level.enemies.entries = {{"X_STRIKE", 0, false}};
    rt.level.models.resize(1);
    rt.level.models[0] = makePlatformModel("X_STRIKE", "BOMB_0", 0.0f);
    rt.cs.pos[0] = 100.0f; rt.cs.pos[1] = 200.0f; rt.cs.pos[2] = 50.0f;
    rt.motion.yawDeg = 0.0f;
    rt.weapon5Aim[0] = 500.0f; rt.weapon5Aim[1] = 200.0f;
    rt.weapon5Aim[2] = 68.0f;
    rt.wpnSel0 = 5; rt.ammo[5] = 9; rt.burstIndex = 2;
    rt.fieldE14 = 1; rt.field541498 = 4;
    mdk::playerFireDispatch(rt);
    CHECK(rt.field54163b == 1 && rt.ammo[5] == 8);
    CHECK(rt.seams.bombCinematicCalls == 1);   // FUN_0042f310(0)
    CHECK(cur->dyn.storage.size() == 1);
    const mdk::DynamicObject& o = *cur->dyn.storage.front();
    CHECK(o.model.modelName() == "X_STRIKE");
    // Spawn = player - 50*(cos,sin) on the yaw circle, z = 30.
    CHECK(near(o.pos[0], 50.0, 1e-4) && near(o.pos[1], 200.0, 1e-4) &&
          near(o.pos[2], 30.0, 1e-4));
    CHECK(o.health == 0xfde8);
    CHECK(o.col.flags148 == 0x5e20 && o.col.flags149 == 0x5e &&
          o.col.flags14a == 0x08);
    CHECK(o.field30a == 0x80 && o.fieldEC == rt.weapon5Path);
    CHECK(o.fieldE6 == -1 && near(o.fieldF0, 1.0, 1e-6) &&
          near(o.fieldE8, 1.0, 1e-6));
    CHECK(o.spawnId == 1 && o.behaviorByte == 7 && o.enemyIndex == 0);
    CHECK(near(o.yawDeg, 0.0, 1e-6));
    // +0x120 anchor = the probe aim + 32 z.
    CHECK(near(o.field120[0], 500.0, 1e-4) &&
          near(o.field120[1], 200.0, 1e-4) &&
          near(o.field120[2], 100.0, 1e-4));
    // The five-key shared record — {0, S, anchor, A, M}; empty arena
    // -> one march step -> anchor = S + (A-S)*0.09 = {90.5, 200, 30}.
    CHECK(rt.weapon5Path[0] == 5);
    CHECK(near(pathF(rt, 1, 1), 50.0, 1e-3) &&
          near(pathF(rt, 1, 3), 30.0, 1e-3));      // key1 = S
    CHECK(near(pathF(rt, 2, 1), 90.5, 1e-3) &&
          near(pathF(rt, 2, 3), 30.0, 1e-3));      // key2 = anchor
    CHECK(near(pathF(rt, 3, 1), 500.0, 1e-3) &&
          near(pathF(rt, 3, 3), 100.0, 1e-3));     // key3 = A
    CHECK(near(pathF(rt, 4, 1), 725.0, 1e-3) &&
          near(pathF(rt, 4, 3), 135.0, 1e-3));     // key4 = mid(A,V)
    // tanIn = 0; tanOut = next - cur (key4's "next" is V).
    CHECK(near(pathF(rt, 1, 4), 0.0, 1e-6) &&
          near(pathF(rt, 1, 7), 40.5, 1e-3) &&
          near(pathF(rt, 1, 9), 0.0, 1e-6));
    CHECK(near(pathF(rt, 3, 7), 225.0, 1e-3) &&
          near(pathF(rt, 3, 9), 35.0, 1e-3));
    CHECK(near(pathF(rt, 4, 7), 225.0, 1e-3) &&
          near(pathF(rt, 4, 9), 35.0, 1e-3));
    // Frames: cumulative round(dist * 30/150) along S->anchor->A->M->V.
    const float d1 = 40.5f;
    const float d2 = std::sqrt(409.5f * 409.5f + 70.0f * 70.0f);
    const float d3 = std::sqrt(225.0f * 225.0f + 35.0f * 35.0f);
    CHECK(mdk::pathEntryFrame(rt.weapon5Path, 0) == 0.0f);
    CHECK(mdk::pathEntryFrame(rt.weapon5Path, 1) ==
          std::lround(d1 * 0.2));
    CHECK(mdk::pathEntryFrame(rt.weapon5Path, 2) ==
          std::lround(d1 * 0.2) + std::lround(d2 * 0.2));
    CHECK(mdk::pathEntryFrame(rt.weapon5Path, 4) ==
          std::lround(d1 * 0.2) + std::lround(d2 * 0.2) +
              2 * std::lround(d3 * 0.2));

    // Uncharged throw — V stays on the spawn plane; M starts at V and
    // marches back toward A at the 0.1 step (one step: {905,200,30}).
    mdk::TraversalRuntime rt2;
    mdk::TraversalArena* cur2 = travArenaAdd(rt2, "B");
    rt2.cur = cur2;
    rt2.cs.arena = &empty.arena;
    rt2.level.enemies.entries = {{"X_STRIKE", 0, false}};
    rt2.level.models.resize(1);
    rt2.level.models[0] = makePlatformModel("X_STRIKE", "BOMB_0", 0.0f);
    rt2.cs.pos[0] = 100.0f; rt2.cs.pos[1] = 200.0f;
    rt2.motion.yawDeg = 0.0f;
    rt2.weapon5Aim[0] = 500.0f; rt2.weapon5Aim[1] = 200.0f;
    rt2.weapon5Aim[2] = 68.0f;
    rt2.wpnSel0 = 5; rt2.ammo[5] = 9; rt2.burstIndex = 2;
    rt2.fieldE14 = 1; rt2.field541498 = 0;    // charge < 4 -> no latch
    mdk::playerFireDispatch(rt2);
    CHECK(rt2.field54163b == 0);
    CHECK(cur2->dyn.storage.size() == 1);
    CHECK(near(pathF(rt2, 4, 1), 905.0, 1e-3) &&
          near(pathF(rt2, 4, 3), 30.0, 1e-3));     // key4 = marched M
    CHECK(near(pathF(rt2, 4, 7), 45.0, 1e-3) &&
          near(pathF(rt2, 4, 9), 0.0, 1e-6));      // V - M = {45,0,0}

    // Obstructed march — a wall at x=300 keeps A->anchor blocked until
    // the anchor steps past it (7 steps: anchor.x = 50 + 7*40.5).
    mdk::TraversalRuntime rt3;
    mdk::TraversalArena* cur3 = travArenaAdd(rt3, "C");
    rt3.cur = cur3;
    CollisionFixture wall;
    wall.verts = {300, 0, 200, 300, 400, 200, 300, 0, -100,
                  300, 400, -100, 300, 0, -100, 300, 400, 200};
    wall.polys = {makePoly(0, 1, 2), makePoly(3, 4, 5)};
    wall.nodes = {makeNode(-1, 0, 0, 300, 0, polySet(2, 0), -1, -1)};
    wall.finish();
    rt3.cs.arena = &wall.arena;
    rt3.level.enemies.entries = {{"X_STRIKE", 0, false}};
    rt3.level.models.resize(1);
    rt3.level.models[0] = makePlatformModel("X_STRIKE", "BOMB_0", 0.0f);
    rt3.cs.pos[0] = 100.0f; rt3.cs.pos[1] = 100.0f;
    rt3.motion.yawDeg = 0.0f;
    rt3.weapon5Aim[0] = 500.0f; rt3.weapon5Aim[1] = 100.0f;
    rt3.weapon5Aim[2] = 68.0f;
    rt3.wpnSel0 = 5; rt3.ammo[5] = 9; rt3.burstIndex = 2;
    rt3.fieldE14 = 1; rt3.field541498 = 4;
    mdk::playerFireDispatch(rt3);
    CHECK(cur3->dyn.storage.size() == 1);
    CHECK(near(pathF(rt3, 2, 1), 333.5, 1e-3) &&
          near(pathF(rt3, 2, 3), 30.0, 1e-3));     // anchor past wall
    CHECK(near(pathF(rt3, 2, 2), 100.0, 1e-3));
  }

  // ---- cmd 0x80 — deterministic dropper gate + X_TOOTH drops -------
  {
    // A dropper model with two bomb slots at distinct local centers.
    auto makeDropperModel = [] {
      mdk::RuntimeModel m;
      m.flag = 1;
      mdk::RuntimeModel::NameRec nr;
      std::snprintf(nr.name.data(), nr.name.size(), "X_STRIKE");
      m.names.push_back(nr);
      const float boxes[2][6] = {{-5, -5, -5, 5, 5, 5},
                                 {95, -5, -5, 105, 5, 5}};
      const char* names[2] = {"BOMB_0", "BOMB_1"};
      m.elems.resize(2);
      m.elemNames.resize(2);
      m.elemField2.resize(2);
      m.elemVerts.resize(2);
      m.elemTris.resize(2);
      for (int i = 0; i < 2; ++i) {
        std::snprintf(m.elemNames[i].data(), m.elemNames[i].size(),
                      "%s", names[i]);
        m.elemVerts[i] = {95.f * i, -5, 0, 5.f + 95.f * i, -5, 0,
                          5.f + 95.f * i, 5, 0};
        m.elemTris[i].assign(0x24, 0);
        auto* idx = reinterpret_cast<std::uint16_t*>(m.elemTris[i].data());
        idx[0] = 0; idx[1] = 1; idx[2] = 2;
        m.elems[i].triCount = 1;
        std::memcpy(m.elems[i].localAabb, boxes[i], sizeof(boxes[i]));
      }
      m.rebind();
      return m;
    };
    // Path record: {frame,pos,tanIn,tanOut} x5 — entry1 frame f1,
    // entry2 frame f2 drive the gate n = round((f2-f0)/3 + 5).
    mdk::TraversalRuntime rt;
    mdk::DynamicArena home;
    home.col.objects = nullptr;
    rt.level.enemies.entries = {{"X_TOOTH", 0, false}};
    rt.level.models.resize(1);
    rt.level.models[0] = makePlatformModel("X_TOOTH", "ELEM", 0.0f);
    mdk::DynamicObject o;
    o.col.named = true;
    o.health = 10;
    o.model = makeDropperModel();
    o.setPosition(10.0f, 20.0f, 30.0f);
    o.prevPos[0] = 10.0f; o.prevPos[1] = 20.0f; o.prevPos[2] = 30.0f;
    mdk::initObjectCollision(o);
    o.col.flags149 |= 0x40;                 // EXEC phase
    o.field30a = 0x80;
    std::int32_t rec[1 + 5 * 10] = {};
    rec[0] = 5;
    rec[1 + 1 * 10] = 100;                  // entry1 frame
    rec[1 + 2 * 10] = 113;                  // entry2 frame
    o.fieldEC = rec;
    o.fieldF0 = 1.0f;
    // n = round((113-1)/3 + 5) = round(42.33) = 42 -> not < 10: no drop.
    mdk::enemyCommandDispatch(rt, o, home, 1.0f / 30.0f);
    CHECK(home.storage.empty() && o.field306 == 0 && rt.cmdFlag54 == 1);
    // f2 = 14 -> n = round(13/3 + 5) = 9: drops while +0x306 < 9.
    rec[1 + 2 * 10] = 14;
    mdk::enemyCommandDispatch(rt, o, home, 1.0f / 30.0f);
    CHECK(o.field306 == 1 && home.storage.size() == 1);
    const mdk::DynamicObject* c = home.storage.front().get();
    CHECK(c->model.modelName() == "X_TOOTH");       // fixed child class
    CHECK(c->field30e == 900 && c->field30a == 5 &&
          near(c->field30, -5.0, 1e-6));
    CHECK(c->field15c == "X_STRIKE");               // parent token
    // BOMB_0's world center = dropper pos + local center {0,0,0}.
    CHECK(near(c->pos[0], 10.0, 1e-4) && near(c->pos[1], 20.0, 1e-4) &&
          near(c->pos[2], 30.0, 1e-4));
    // Next drop -> BOMB_1 (digit = the drop count), center {100,0,0}.
    mdk::enemyCommandDispatch(rt, o, home, 1.0f / 30.0f);
    CHECK(o.field306 == 2 && home.storage.size() == 2);
    const mdk::DynamicObject* c2 = home.storage.front().get();
    CHECK(near(c2->pos[0], 110.0, 1e-4) && near(c2->pos[1], 20.0, 1e-4));
    // field306=9 reaches the n<9 boundary -> the gate closes.
    o.field306 = 9;
    const std::size_t before = home.storage.size();
    mdk::enemyCommandDispatch(rt, o, home, 1.0f / 30.0f);
    CHECK(home.storage.size() == before && o.field306 == 9);

    // Kamikaze release — midpoint = (f1+f2)/2 = 57; +0xf0 past it ->
    // the path unbinds into velocity; contact -> splash + death.
    mdk::TraversalRuntime rt4;
    mdk::DynamicArena home4;
    mdk::DynamicObject k;
    k.col.named = true;
    k.health = 10;
    k.model = makeDropperModel();
    k.setPosition(50.0f, 0.0f, 0.0f);
    k.prevPos[0] = 47.0f; k.prevPos[1] = 0.0f; k.prevPos[2] = 0.0f;
    mdk::initObjectCollision(k);
    k.col.flags149 |= 0x40;
    k.field30a = 0x80;
    k.fieldEC = rec;                          // f1=100, f2=14 -> mid 57
    rt4.field54163b = 1;                      // weapon-5 latch set
    k.fieldF0 = 50.0f;                        // before the midpoint
    mdk::enemyCommandDispatch(rt4, k, home4, 1.0f / 30.0f);
    CHECK(k.fieldEC != nullptr && rt4.cmdFlag54 == 1);
    k.fieldF0 = 60.0f;                        // past mid 57 -> release
    mdk::enemyCommandDispatch(rt4, k, home4, 1.0f / 30.0f);
    CHECK(k.fieldEC == nullptr && (k.col.flags148 & 6) != 0);
    CHECK(near(k.field28, 90.0, 1e-4));       // (50-47) * 30
    CHECK(k.health > 0);                      // no contact yet
    // Wall contact — +0x14c bit 0 as objectCollide would re-arm it
    // each frame. Die -> boundary (FUN_00457cf4 seam) -> record wipe.
    k.col.flags14c |= 1;
    mdk::enemyCommandDispatch(rt4, k, home4, 1.0f / 30.0f);
    CHECK(rt4.seams.objectDeathCalls == 1 &&
          rt4.seams.objectTeardownCalls == 2);   // boundary + wipe
    CHECK(k.col.named == false && k.health == 0);
    CHECK(rt4.combatFx.size() == 1 &&
          rt4.combatFx.back().kind ==
              mdk::CombatFxKind::kObjectTeardown &&
          rt4.combatFx.back().obj == &k);
  }

  // ---- FUN_0046145c charge probe — real ray/stab path --------------
  {
    // Camera looks +x at a wall 300 away -> live probe, aim at the
    // crossing + one basis step.
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* cur = travArenaAdd(rt, "A");
    rt.cur = cur;
    CollisionFixture wall;
    wall.verts = {300, 0, 200, 300, 400, 200, 300, 0, -100,
                  300, 400, -100, 300, 0, -100, 300, 400, 200};
    wall.polys = {makePoly(0, 1, 2), makePoly(3, 4, 5)};
    wall.nodes = {makeNode(-1, 0, 0, 300, polySet(2, 0), 0, -1, -1)};
    wall.finish();
    rt.cs.arena = &wall.arena;
    rt.wpnSel0 = 5; rt.fireCadence = 0.0f;
    rt.camera.pose.pos[0] = 0.0f; rt.camera.pose.pos[1] = 100.0f;
    rt.camera.pose.pos[2] = 50.0f;
    rt.camera.pose.basis[2][0] = -1.0f; rt.camera.pose.basis[2][1] = 0.0f;
    rt.camera.pose.basis[2][2] = 0.0f;   // basis[2] = the backward axis
    mdk::playerChargeProbe(rt);
    CHECK(rt.fieldE14 == 1);
    CHECK(near(rt.weapon5Aim[0], 299.0, 1e-3) &&
          near(rt.weapon5Aim[1], 100.0, 1e-3) &&
          near(rt.weapon5Aim[2], 50.0, 1e-3));
    // A ceiling over the aim point -> the overhead stab kills it.
    CollisionFixture ceil = makeCeilArena();
    mdk::TraversalRuntime rt2;
    rt2.cs.arena = &ceil.arena;
    rt2.wpnSel0 = 5;
    rt2.camera.pose.pos[0] = 0.0f; rt2.camera.pose.pos[1] = 0.0f;
    rt2.camera.pose.pos[2] = 0.0f;
    rt2.camera.pose.basis[2][0] = 0.0f; rt2.camera.pose.basis[2][1] = 0.0f;
    rt2.camera.pose.basis[2][2] = -1.0f;   // ray goes +z, hits ceil@20
    mdk::playerChargeProbe(rt2);
    CHECK(rt2.fieldE14 == 0);
    // cmdFlag54 (a live dropper) suppresses the probe entirely.
    rt.cmdFlag54 = 1; rt.fieldE14 = 9;
    mdk::playerChargeProbe(rt);
    CHECK(rt.fieldE14 == 0);
  }

  // ---- FUN_0045f138 homing tail — HEAD + predicate elements -------
  {
    mdk::TraversalRuntime rt;
    rt.level.enemies.entries = {{"SW_HOME", 7, false}};
    // Lock target: standable, "0"-prefix/digit elements, one HEAD.
    mdk::DynamicObject target;
    target.col.named = true; target.health = 10;
    target.model = makeHomingModel({{"0PQR", 50.0f}, {"XHEADZ", 100.0f}});
    target.setPosition(0, 0, 0);
    mdk::initObjectCollision(target);
    target.col.flags149 |= 0x20;
    target.homingPrefix = "0"; target.homingDigitOfs = 0;
    rt.focusObj = &target; rt.fieldCc8 = 1;
    rt.wpnSel0 = 1; rt.ammo[1] = 5;
    rt.camera.pose.basis[2][0] = 0; rt.camera.pose.basis[2][1] = 0;
    rt.camera.pose.basis[2][2] = -1;   // the homing ray goes +z
    mdk::playerFireDispatch(rt);
    // The HEAD element short-circuits (index 1) over the nearer
    // predicate candidate (index 0) — OBSERVED priority.
    CHECK(rt.shots[0].homeObj == &target);
    CHECK(rt.shots[0].homeElem == &target.model.elems[1] &&
          rt.shots[0].homeElemIdx == 1);

    // No HEAD -> the nearest predicate+clip element wins.
    rt = mdk::TraversalRuntime{};
    rt.level.enemies.entries = {{"SW_HOME", 7, false}};
    mdk::DynamicObject t2;
    t2.col.named = true; t2.health = 10;
    t2.model = makeHomingModel({{"0PQR", 50.0f}, {"0PQR", 100.0f}});
    t2.setPosition(0, 0, 0);
    mdk::initObjectCollision(t2);
    t2.col.flags149 |= 0x20;
    t2.homingPrefix = "0"; t2.homingDigitOfs = 0;
    rt.focusObj = &t2; rt.fieldCc8 = 1;
    rt.wpnSel0 = 1; rt.ammo[1] = 5;
    rt.camera.pose.basis[2][0] = 0; rt.camera.pose.basis[2][1] = 0;
    rt.camera.pose.basis[2][2] = -1;
    mdk::playerFireDispatch(rt);
    // z=50 element is nearer to the ray (|out| = 45 < 95).
    CHECK(rt.shots[0].homeElem == &t2.model.elems[0] &&
          rt.shots[0].homeElemIdx == 0);

    // Non-homing weapons store the lock object but skip the scan.
    rt = mdk::TraversalRuntime{};
    rt.level.enemies.entries = {{"SW_SGREN", 8, false}};
    rt.focusObj = &t2; rt.fieldCc8 = 1;
    rt.wpnSel0 = 2; rt.ammo[2] = 5;
    mdk::playerFireDispatch(rt);
    CHECK(rt.shots[0].homeObj == &t2 && rt.shots[0].homeElem == nullptr);
  }

  // ---- FUN_00432f84 punch — the hit mark + the wall seam ----------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "PNCH");
    a->dyn.col.verts = f.verts.data();
    a->dyn.col.polys = f.polys.data();
    a->dyn.col.nodes = f.nodes.data();
    rt.cur = a; rt.cs.arena = &a->dyn.col;
    rt.cs.pos[0] = 0; rt.cs.pos[1] = 0; rt.cs.pos[2] = 10;
    rt.motion.yawDeg = 0;
    rt.fieldC74 = 1; rt.ammo[0] = 20;
    // Nearest-candidate tracking: farObj is the list head (scanned
    // first) but the nearer nearObj wins on score.
    mdk::DynamicObject& nearObj = a->dyn.allocFront();
    nearObj.model = makePlatformModel("T", "E", 0.0f);
    nearObj.setPosition(6, 0, 15);
    mdk::initObjectCollision(nearObj);
    mdk::DynamicObject& farObj = a->dyn.allocFront();
    farObj.model = makePlatformModel("T", "E", 0.0f);
    farObj.setPosition(8, 0, 15);
    mdk::initObjectCollision(farObj);
    // rebuildObjectTransform seeds the object AABB from its prior
    // aabb[0]/aabb[5] (the {0,0,0,0,0,0} of a fresh record) — set the
    // clean world AABBs for a deterministic whole-object test.
    const float nb[6] = {1, -5, 15, 11, 5, 15};
    const float fb[6] = {3, -5, 15, 13, 5, 15};
    std::memcpy(nearObj.col.aabb, nb, sizeof(nb));
    std::memcpy(farObj.col.aabb, fb, sizeof(fb));
    const int pt = rt.punchTime;
    mdk::playerPunch(rt, 1);
    CHECK(nearObj.field21e == 0xff && farObj.field21e == 0);
    CHECK(rt.seams.punchHitCalls == 1 && rt.punchHitTime == 1);
    CHECK(rt.punchTime == pt + 6 && rt.ammo[0] == 19);   // charged *6
    // The 0x540c74 gate off -> no scan at all.
    nearObj.field21e = 0; rt.fieldC74 = 0;
    mdk::playerPunch(rt, 1);
    CHECK(nearObj.field21e == 0);
  }
  {
    // Miss into a wall -> the wall-impact seam.
    CollisionFixture f = makeWallArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "PNCH");
    a->dyn.col.verts = f.verts.data();
    a->dyn.col.polys = f.polys.data();
    a->dyn.col.nodes = f.nodes.data();
    rt.cur = a; rt.cs.arena = &a->dyn.col;
    rt.cs.pos[0] = 0; rt.cs.pos[1] = 0; rt.cs.pos[2] = 10;
    rt.motion.yawDeg = 0;                 // +x, into the x=5 wall
    rt.fieldC74 = 1; rt.ammo[0] = 20;
    mdk::playerPunch(rt, 1);
    CHECK(rt.seams.punchWallCalls == 1 && rt.seams.punchHitCalls == 0);
    // Phase 10B — the wall stab posts kPunchWallImpact (mode 1; the
    // handlerless dispatch result bit0 is 0 -> variant 1).
    CHECK(rt.combatFx.size() == 1);
    CHECK(rt.combatFx[0].kind == mdk::CombatFxKind::kPunchWallImpact);
    CHECK(rt.combatFx[0].mode == 1 && rt.combatFx[0].variant == 1);
    CHECK(near(rt.combatFx[0].pos[0], 4.0, 1e-4));  // wall x=5, -1 back
  }

  // ---- scope gating: d0c++/cadence only under (c9c && ca0 > 1) ----
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "GATE");
    a->dyn.col.verts = f.verts.data();
    a->dyn.col.polys = f.polys.data();
    a->dyn.col.nodes = f.nodes.data();
    rt.cur = a; rt.cs.arena = &a->dyn.col;
    rt.cs.queryEnabled = 1; rt.cs.arenaValid = 1;
    rt.cs.objectDataLoaded = 1;
    rt.cs.pos[2] = 12.0f; rt.cs.entryPos[2] = 12.0f;
    rt.hudActive = 1;
    rt.fieldD0c = 5; rt.fireCadence = 1.0f; rt.burstIndex = 0;
    rt.wpnSel0 = rt.wpnSel1 = 0;
    for (int i = 0; i < 5; ++i)
      mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    // Unscoped: the shared d0c counter and the cadence machine are
    // both gated by (c9c && ca0 > 1) — neither advances.
    CHECK(rt.fieldD0c == 5 && rt.burstIndex == 0 &&
          near(rt.fireCadence, 1.0, 1e-5));
  }
}

// Phase 10A — FUN_0045f9b8 shot-pool update, the fly callbacks, the
// FUN_00460b7c/0x60d44/0x60c08 detonate/splash, FUN_0046771c player
// damage, and the FUN_00458140/0x581a4 death boundary — all OBSERVED
// in BUILD_A. Quirks preserved by the port are asserted explicitly:
// the stale-state flight fallthrough, the partner-active double tick,
// the int16 element wrap, and the punch's element->whole fallthrough.
void test_player_projectiles() {
  const float kDt = 1.0f / 30.0f;        // 0x49b6f4 fixed tick
  auto makeShotRt = [](mdk::TraversalRuntime& rt, CollisionFixture& f,
                       const char* name) {
    mdk::TraversalArena* a = travArenaAdd(rt, name);
    a->dyn.col.verts = f.verts.data();
    a->dyn.col.polys = f.polys.data();
    a->dyn.col.nodes = f.nodes.data();
    a->dyn.col.deepFloorZ = -1000.0f;
    rt.cur = a;
    rt.cs.arena = &a->dyn.col;
    rt.cs.pos[2] = 20.0f;
    return a;
  };

  // ---- pool tick: free slots are skipped, the seam counts --------
  {
    mdk::TraversalRuntime rt;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(rt.seams.shotPoolTickCalls == 1);
  }

  // ---- tracer flight (FUN_004601d4) on an empty arena -----------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SHOT");
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 1; s.type = 0; s.flyKind = mdk::kShotFlyTracer;
    s.arena = a; s.yawDeg = 0.0f; s.pitchDeg = 0.0f;
    s.speedH = 1100.0f; s.fieldCc = 2.0f; s.lifetime = 75;
    s.pos[2] = 20.0f;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    // +x flight at the fixed 1/30 tick (z=20 stays above the floor).
    CHECK(near(s.pos[0], 1100.0 / 30.0, 1e-3) &&
          near(s.pos[2], 20.0, 1e-5));
    CHECK(near(s.tailLen, 10.0 / 30.0, 1e-4));
    CHECK(near(s.fieldCc, 2.0 - 0.5 / 30.0, 1e-4));
    CHECK(near(s.spinDeg, 720.0 / 30.0, 1e-3));   // no-hit spin
    CHECK(s.lifetime == 74 && s.state == 1);
    // tail = pos - tailLen*dir (dir = +x at yaw/pitch 0).
    CHECK(near(s.tail[0], s.pos[0] - s.tailLen, 1e-4));
  }

  // ---- stale state > 1 with +0x14 <= 0 re-flies (OBSERVED quirk) -
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SHOT");
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 5; s.dyingTimer = 0;              // stale detonated
    s.type = 0; s.flyKind = mdk::kShotFlyTracer;
    s.arena = a; s.speedH = 1100.0f; s.lifetime = 75; s.pos[2] = 20;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(near(s.pos[0], 1100.0 / 30.0, 1e-3) && s.state == 5);
  }

  // ---- dying branch: lifetime+tail shrink, release at 0 ----------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SHOT");
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 4; s.dyingTimer = 2; s.lifetime = 5; s.type = 0;
    s.tailLen = 15.0f; s.arena = a;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(s.lifetime == 4 && s.dyingTimer == 1 && s.state == 4);
    CHECK(near(s.tailLen, 15.0 - 5.0 / 30.0, 1e-4));  // no clamp
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(s.dyingTimer == 0 && s.state == 0);         // released
  }

  // ---- lifetime expiry -> state 4; kill floor -> state 4 ---------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SHOT");
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 1; s.type = 0; s.flyKind = mdk::kShotFlyTracer;
    s.arena = a; s.speedH = 1100.0f; s.lifetime = 1; s.pos[2] = 20;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(s.state == 4 && s.lifetime == 0 && s.dyingTimer == 30 &&
          s.remnantIdx == 0);
    // Below the arena deep floor -> the same state-4 transition.
    mdk::PlayerShot& s2 = rt.shots[1];
    s2.state = 1; s2.type = 0; s2.flyKind = mdk::kShotFlyTracer;
    s2.arena = a; s2.speedH = 0.0f; s2.lifetime = 100;
    s2.pos[2] = -2000.0f;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(s2.state == 4 && s2.dyingTimer == 30);
  }

  // ---- type-4 expiry detonates -> state 5 (FUN_00460b7c) ---------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SHOT");
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 1; s.type = 4; s.flyKind = mdk::kShotFlyLobbed;
    s.arena = a; s.lifetime = 1; s.pos[2] = 20;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(s.state == 5 && s.lifetime == 30 && s.dyingTimer == 30 &&
          s.remnantIdx == 0);
    CHECK(rt.seams.remnantSpawnCalls == 1);
    // Phase 10B — the detonation posts a kDetonation presentation
    // event at the shot position (FUN_004575fc remnant seam).
    CHECK(rt.combatFx.size() == 1);
    CHECK(rt.combatFx[0].kind == mdk::CombatFxKind::kDetonation);
    CHECK(near(rt.combatFx[0].pos[2], s.pos[2], 1e-5));
  }

  // ---- Phase 10B — the shot-render gate (0x436dd3): only the fully
  // scoped sniper state renders the pool -----------------------------
  {
    mdk::TraversalRuntime rt;
    CHECK(!mdk::playerShotRenderGate(rt));
    rt.flagC9c = 1;
    CHECK(!mdk::playerShotRenderGate(rt));        // transitionPhase 0
    rt.transitionPhase = 1;
    CHECK(!mdk::playerShotRenderGate(rt));        // needs > 1
    rt.transitionPhase = 2;
    CHECK(mdk::playerShotRenderGate(rt));
  }

  // ---- Phase 10B — PlayerShotVisual snapshot: type 0 --------------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SHOT");
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 1; s.type = 0; s.flyKind = mdk::kShotFlyTracer;
    s.arena = a; s.yawDeg = 30.0f; s.pitchDeg = -10.0f;
    s.spinDeg = 45.0f; s.fieldCc = 1.5f; s.tailLen = 8.0f;
    s.lifetime = 75;
    s.pos[0] = 100; s.pos[1] = 50; s.pos[2] = 20;
    s.tail[0] = 90; s.tail[1] = 45; s.tail[2] = 20;
    auto vs = mdk::playerShotVisuals(rt);
    const mdk::PlayerShotVisual& v = vs[0];
    CHECK(v.slot == 0 && v.state == 1 && v.type == 0);
    CHECK(v.arena == a);
    CHECK(near(v.pos[0], 100.0, 1e-6) && near(v.tail[0], 90.0, 1e-6));
    CHECK(near(v.tailLen, 8.0, 1e-6));
    // Billboard basis: render yaw = 90 - yawDeg; pitch = pitchDeg.
    CHECK(near(v.billboardYawDeg, 60.0, 1e-6));
    CHECK(near(v.billboardPitchDeg, -10.0, 1e-6));
    CHECK(near(v.spinDeg, 45.0, 1e-6) && near(v.fieldCc, 1.5, 1e-6));
    CHECK(v.worldRenderable);
    CHECK(v.hudFrame == -1);                      // active -> no frame
    // Free + expired slots expose HUD frames (FUN_0045ee7c iVar7).
    CHECK(vs[1].hudFrame == 0);                   // state 0 -> frame 0
    CHECK(!vs[1].worldRenderable);
  }

  // ---- Phase 10B — snapshot: type-4 velocity-derived billboard ----
  {
    mdk::TraversalRuntime rt;
    mdk::PlayerShot& s = rt.shots[2];
    s.state = 1; s.type = 4; s.lifetime = 100;
    s.yawDeg = 10.0f;                             // unused for type 4
    s.speedV = 40.0f;                             // pitch = -40*0.5 = -20
    s.pos[0] = 10; s.pos[1] = 0; s.pos[2] = 5;
    s.tail[0] = 0; s.tail[1] = 0; s.tail[2] = 5;  // delta = +x
    auto vs = mdk::playerShotVisuals(rt);
    const mdk::PlayerShotVisual& v = vs[2];
    // atan2deg(dx=10, dy=0) = 90 -> billboardYaw = 90 - 90 = 0.
    CHECK(near(v.billboardYawDeg, 0.0, 1e-4));
    CHECK(near(v.billboardPitchDeg, -20.0, 1e-5));
    // Clamp: speedV -300 -> pitch +150 -> clamped to +60.
    s.speedV = -300.0f;
    vs = mdk::playerShotVisuals(rt);
    CHECK(near(vs[2].billboardPitchDeg, 60.0, 1e-6));
    // atan2 path: tail +x of pos -> atan2deg(-10, 0) = 270 -> 90-270.
    s.tail[0] = 20;
    vs = mdk::playerShotVisuals(rt);
    CHECK(near(vs[2].billboardYawDeg, 90.0 - 270.0, 1e-4));
  }

  // ---- Phase 10B — expired-state HUD frame mapping -----------------
  {
    mdk::TraversalRuntime rt;
    rt.shots[0].state = 4; rt.shots[0].lifetime = 0;   // -> frame 3
    rt.shots[1].state = 3; rt.shots[1].lifetime = 0;   // -> 0x3c
    rt.shots[2].state = 5; rt.shots[2].lifetime = 0;   // -> 0xf4
    auto vs = mdk::playerShotVisuals(rt);
    CHECK(vs[0].hudFrame == 3 && !vs[0].worldRenderable);
    CHECK(vs[1].hudFrame == 0x3c);
    CHECK(vs[2].hudFrame == 0xf4);
    // Dying-but-live records are still world-rendered, no HUD frame.
    rt.shots[0].lifetime = 5;
    vs = mdk::playerShotVisuals(rt);
    CHECK(vs[0].worldRenderable && vs[0].hudFrame == -1);
    CHECK(mdk::kShotHudRect[0].x == 72 && mdk::kShotHudRect[2].x == 384);
  }

  // ---- Phase 10B — wall impact posts kShotWallImpact ---------------
  {
    CollisionFixture f = makeWallArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SHOT");
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 1; s.type = 0; s.flyKind = mdk::kShotFlyTracer;
    s.arena = a; s.yawDeg = 0.0f; s.speedH = 1100.0f; s.lifetime = 75;
    s.pos[2] = 10.0f;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(s.state == 4);
    CHECK(rt.combatFx.size() == 1);
    const mdk::CombatFxEvent& ev = rt.combatFx[0];
    CHECK(ev.kind == mdk::CombatFxKind::kShotWallImpact);
    // res bit0 of the (handlerless) dispatch is 0 -> mode 3 variant 1.
    CHECK(ev.mode == 3 && ev.variant == 1 && ev.aux == 0);
    CHECK(ev.obj == nullptr);
  }

  // ---- wall impact: type 0 -> state 4; type 2 -> detonate --------
  {
    CollisionFixture f = makeWallArena();      // x=5 wall, normal -x
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SHOT");
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 1; s.type = 0; s.flyKind = mdk::kShotFlyTracer;
    s.arena = a; s.yawDeg = 0.0f; s.speedH = 1100.0f; s.lifetime = 75;
    s.pos[2] = 10.0f;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(s.state == 4 && s.lifetime == 0 && s.dyingTimer == 30);
    CHECK(rt.seams.shotImpactFxCalls == 1);    // FUN_00460164 tail
    // Types 2/3 detonate at the wall instead.
    mdk::PlayerShot& s2 = rt.shots[1];
    s2.state = 1; s2.type = 2; s2.flyKind = mdk::kShotFlyGrenade;
    s2.arena = a; s2.yawDeg = 0.0f; s2.speedH = 400.0f; s2.lifetime = 75;
    s2.pos[2] = 10.0f;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(s2.state == 5 && s2.dyingTimer == 30);
  }

  // ---- object hit: survived -> state 3, killed -> state 2 --------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SHOT");
    mdk::DynamicObject& o = a->dyn.allocFront();
    o.model = makePlatformModel("PLAT", "ELEM", 0.0f);
    o.setPosition(8, 2, 15);
    mdk::initObjectCollision(o);
    o.health = 20;
    // Thick AABB for the prefilter — the flat element box is a
    // degenerate seed like the original's z-seeded union.
    const float wb[6] = {3, -3, 10, 13, 7, 20};
    std::memcpy(o.col.aabb, wb, sizeof(wb));
    // Straight-down tracer (pitch 90 -> dir {0,0,-1}) through the
    // element tri at world (3,-3) local — inside the quad half.
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 1; s.type = 0; s.flyKind = mdk::kShotFlyTracer;
    s.arena = a; s.pitchDeg = 90.0f; s.speedH = 1100.0f;
    s.lifetime = 75; s.pos[0] = 8; s.pos[1] = 2; s.pos[2] = 25;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(o.health == 12);                     // 20 - 8
    CHECK(s.state == 3 && s.lifetime == 30 && s.dyingTimer == 45);
    CHECK(near(s.tailLen, 15.0, 1e-5));
    CHECK(o.field21e == 1 && o.field21c == 1 && o.field21d == 0);
    CHECK(o.field220 >= 0 && near(o.field228, 90.0, 1e-5));
    CHECK(rt.shotHitCount == 1);
    // Phase 10B — the survived hit posts kShotObjectImpact carrying
    // the FUN_00437444 args (mode 3, variant flag21f, aux field150).
    CHECK(rt.combatFx.size() == 1);
    CHECK(rt.combatFx[0].kind == mdk::CombatFxKind::kShotObjectImpact);
    CHECK(rt.combatFx[0].mode == 3 && rt.combatFx[0].obj == &o);
    CHECK(near(rt.combatFx[0].pos[0], o.field210[0], 1e-5));
    // Killed -> state 2 + the tally/death boundary.
    mdk::DynamicObject& o2 = a->dyn.allocFront();
    o2.model = makePlatformModel("PLAT", "ELEM", 0.0f);
    o2.setPosition(8, 2, 40);
    mdk::initObjectCollision(o2);
    o2.health = 8;
    const float wb2[6] = {3, -3, 35, 13, 7, 45};
    std::memcpy(o2.col.aabb, wb2, sizeof(wb2));
    mdk::PlayerShot& s2 = rt.shots[1];
    s2.state = 1; s2.type = 0; s2.flyKind = mdk::kShotFlyTracer;
    s2.arena = a; s2.pitchDeg = 90.0f; s2.speedH = 1100.0f;
    s2.lifetime = 75; s2.pos[0] = 8; s2.pos[1] = 2; s2.pos[2] = 50;
    mdk::playerShotPoolTick(rt, 1, kDt, 1.0f);
    CHECK(o2.health <= 0 && s2.state == 2);
    CHECK(rt.seams.objectDeathCalls == 1);
    // Phase 10B — the kill posts kObjectTeardown (no +0x110 script).
    CHECK(rt.combatFx.size() == 2);
    CHECK(rt.combatFx[1].kind == mdk::CombatFxKind::kObjectTeardown);
    CHECK(rt.combatFx[1].obj == &o2);
    // pitchDeg bits are nonzero -> the tally gate ran, but "PLAT"
    // is not in the 34-name table -> no tally.
    CHECK(rt.killTally == 0);
  }

  // ---- playerDamageApply (FUN_0046771c) gates + scaling ----------
  {
    mdk::TraversalRuntime rt;
    const float pt[3] = {0, 0, 0};
    rt.fieldHealth = 100; rt.difficulty = 1;
    mdk::playerDamageApply(rt, 30, pt);
    CHECK(rt.fieldHealth == 70);
    CHECK(rt.fieldDac == 180);                 // 30*25 clamped [75,180]
    CHECK(near(rt.vert.landingAccum, 30.0, 1e-5));
    // difficulty 0 -> 2d/3 min 1; difficulty 2 -> 2d.
    rt.fieldHealth = 100; rt.difficulty = 0;
    mdk::playerDamageApply(rt, 30, pt);
    CHECK(rt.fieldHealth == 80);
    rt.fieldHealth = 100; rt.difficulty = 2;
    mdk::playerDamageApply(rt, 30, pt);
    CHECK(rt.fieldHealth == 40);
    // Health floors at 0.
    rt.fieldHealth = 10;
    mdk::playerDamageApply(rt, 50, pt);
    CHECK(rt.fieldHealth == 0);
    // Suppress window -> landingAccum cleared, no damage.
    rt.fieldHealth = 50; rt.fieldE10 = 0.5f; rt.vert.landingAccum = 9;
    mdk::playerDamageApply(rt, 30, pt);
    CHECK(rt.fieldHealth == 50 && near(rt.vert.landingAccum, 0.0, 1e-5));
    // health==0 && gate==0 -> dead, no-op.
    rt = mdk::TraversalRuntime{};
    mdk::playerDamageApply(rt, 30, pt);
    CHECK(rt.fieldHealth == 0);
  }

  // ---- objectKillTally / objectDeathBoundary ---------------------
  {
    mdk::TraversalRuntime rt;
    mdk::DynamicObject o;
    o.model = makePlatformModel("XT", "ELEM", 0.0f);   // in table
    const float pt[3] = {0, 0, 0};
    mdk::objectKillTally(rt, o, 0);
    CHECK(rt.killTally == 0);                  // gate off -> no tally
    mdk::objectKillTally(rt, o, 1);
    CHECK(rt.killTally == 1);
    mdk::DynamicObject o2;
    o2.model = makePlatformModel("PLAT", "ELEM", 0.0f);
    mdk::objectKillTally(rt, o2, 1);
    CHECK(rt.killTally == 1);                  // not in table
    // Death boundary — the +0x110 script handoff.
    mdk::DynamicObject d;
    const int sentinel = 0x1234;
    d.field110 = &sentinel; d.field11e = 7; d.health = 9;
    d.field22c = 3.0f;
    mdk::objectDeathBoundary(rt, d, pt, 45.0f);
    CHECK(d.field11e == 0 && d.health == 0 && d.field22c == 0.0f);
    CHECK((d.col.flags148 & 0x20) != 0);
    CHECK(d.field108 == &sentinel && d.field230 == &sentinel &&
          d.field110 == nullptr);
    // Phase 10B — the script handoff posts kObjectDeathScript.
    CHECK(rt.combatFx.size() == 1 &&
          rt.combatFx[0].kind == mdk::CombatFxKind::kObjectDeathScript &&
          rt.combatFx[0].obj == &d);
    // The teardown path — +0x11e == 0xf arms fieldD2c; the latches
    // clear; a cleared mount deals 50 to the player.
    mdk::TraversalRuntime rt2;
    mdk::DynamicObject t;
    t.field11e = 0xf;
    rt2.fieldB85c = &t;
    rt2.cs.excludeObj = &t.col;
    rt2.cs.lastObjContact = &t.col;
    rt2.fieldHealth = 100; rt2.difficulty = 1;
    mdk::objectDeathBoundary(rt2, t, pt, 0.0f);
    CHECK(rt2.fieldD2c == 10 && rt2.fieldB85c == nullptr &&
          rt2.cs.excludeObj == nullptr && rt2.cs.lastObjContact == nullptr);
    CHECK(rt2.fieldHealth == 50);              // the mount-kill 50
    CHECK(rt2.seams.objectTeardownCalls == 1);
    CHECK(rt2.combatFx.size() == 1 &&
          rt2.combatFx[0].kind == mdk::CombatFxKind::kObjectTeardown);
  }

  // ---- splash (FUN_00460d44): int16 element wrap + kill ----------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SPL");
    mdk::DynamicObject& o = a->dyn.allocFront();
    o.model = makeHomingModel({{"0E", 0.0f}});
    o.setPosition(8, 0, 15);
    mdk::initObjectCollision(o);
    o.health = 10;
    o.col.flags149 |= 0x20;
    o.homingPrefix = "0"; o.homingDigitOfs = 0;
    o.elemHp = {1}; o.field2c4 = 1000.0f;
    const float wb[6] = {3, -5, 10, 13, 5, 20};
    std::memcpy(o.col.aabb, wb, sizeof(wb));
    // Blast at the element centre: dmg 150 -> int16 1-150 < 0 ->
    // element death marks + the whole-object kill -> teardown seam.
    const float blast[3] = {8, 0, 15};
    mdk::splashDamage(rt, blast, 150.0f, 50.0f, 1, nullptr, 2, -7);
    CHECK(o.elemHp[0] == 0 && o.field21c == 1 && o.field220 == 0);
    CHECK(o.health <= 0 && o.field21e == 1 && o.field21d == 0xf9);
    CHECK(rt.seams.objectDeathCalls == 1 &&
          rt.seams.objectTeardownCalls == 1);
  }
  {
    // The wrap quirk: 1 - 40000 wraps the int16 to +25537 — the
    // element SURVIVES a massive hit (OBSERVED `sub word`).
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "SPL");
    mdk::DynamicObject& o = a->dyn.allocFront();
    o.model = makeHomingModel({{"0E", 0.0f}});
    o.setPosition(8, 0, 15);
    mdk::initObjectCollision(o);
    o.health = 10;
    o.col.flags149 |= 0x20;
    o.homingPrefix = "0"; o.homingDigitOfs = 0;
    o.elemHp = {1}; o.field2c4 = 1000.0f;
    const float wb[6] = {3, -5, 10, 13, 5, 20};
    std::memcpy(o.col.aabb, wb, sizeof(wb));
    const float blast[3] = {8, 0, 15};
    mdk::splashDamage(rt, blast, 40000.0f, 50.0f, 1, nullptr, 2, -7);
    CHECK(o.elemHp[0] == 25537 && o.field21c == 0);
    CHECK(o.field21e == 0xfe);                 // bestElemMark -2
  }

  // ---- splash player pass: falloff + cap + occlusion -------------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    makeShotRt(rt, f, "SPL");
    rt.cs.pos[0] = 0; rt.cs.pos[1] = 0; rt.cs.pos[2] = 10;
    rt.fieldHealth = 100; rt.difficulty = 1;
    // blast at (0,0,15): pt=(0,0,11) d2=16, dist=8 -> dmg=126 cap 15.
    const float blast[3] = {0, 0, 15};
    mdk::splashDamage(rt, blast, 150.0f, 50.0f, 1, nullptr, 1, -7);
    CHECK(rt.fieldHealth == 85);
    CHECK(near(rt.vert.landingAccum, 30.0, 1e-5));  // *2 after +15
    // Below the floor -> the stab occludes -> out of range, no dmg.
    const float blast2[3] = {0, 0, 5};
    mdk::splashDamage(rt, blast2, 150.0f, 50.0f, 1, nullptr, 1, -7);
    CHECK(rt.fieldHealth == 85);
  }

  // ---- punch element -> whole-object fallthrough (OBSERVED) -----
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = travArenaAdd(rt, "PNCH");
    a->dyn.col.verts = f.verts.data();
    a->dyn.col.polys = f.polys.data();
    a->dyn.col.nodes = f.nodes.data();
    rt.cur = a; rt.cs.arena = &a->dyn.col;
    rt.cs.pos[2] = 10.0f; rt.motion.yawDeg = 0;
    rt.fieldC74 = 1; rt.ammo[0] = 0;           // uncharged, dmg 1
    mdk::DynamicObject& o = a->dyn.allocFront();
    o.model = makeHomingModel({{"0E", 0.0f}});
    o.setPosition(8, 0, 15);
    mdk::initObjectCollision(o);
    o.col.flags149 |= 0x20;
    o.homingPrefix = "0"; o.homingDigitOfs = 0;
    o.elemHp = {3}; o.elemThresh = {3};
    const float wb[6] = {3, -5, 10, 13, 5, 20};
    std::memcpy(o.col.aabb, wb, sizeof(wb));
    mdk::playerPunch(rt, 1);
    // Element survived -> whole-object marks still run; the event
    // latch took the pseudo-object (elemThresh <= 900).
    CHECK(o.elemHp[0] == 2 && o.health == 9);
    CHECK(o.field21d == 0xff && o.field21e == 0xff);   // state -1
    CHECK(rt.eventTimerObj == &a->eventLatch && rt.eventTimer == 1.0f);
    CHECK(a->eventLatch.col.named && a->eventLatch.health == 2);
    // Phase 10B — the survived punch posts kPunchObjectImpact with
    // the FUN_00437444 args (mode 1, variant flag21f, aux field150).
    CHECK(rt.combatFx.size() == 1);
    CHECK(rt.combatFx[0].kind == mdk::CombatFxKind::kPunchObjectImpact);
    CHECK(rt.combatFx[0].mode == 1 && rt.combatFx[0].obj == &o);
    // Element killed -> the e+1 marks close the +0x21e == -1 gate,
    // so the whole-object state marks are skipped — but the health
    // subtraction still runs (the fallthrough quirk).
    mdk::TraversalRuntime rt2;
    mdk::TraversalArena* a2 = travArenaAdd(rt2, "PNCH");
    a2->dyn.col.verts = f.verts.data();
    a2->dyn.col.polys = f.polys.data();
    a2->dyn.col.nodes = f.nodes.data();
    rt2.cur = a2; rt2.cs.arena = &a2->dyn.col;
    rt2.cs.pos[2] = 10.0f; rt2.motion.yawDeg = 0;
    rt2.fieldC74 = 1; rt2.ammo[0] = 0;
    mdk::DynamicObject& o2 = a2->dyn.allocFront();
    o2.model = makeHomingModel({{"0E", 0.0f}});
    o2.setPosition(8, 0, 15);
    mdk::initObjectCollision(o2);
    o2.col.flags149 |= 0x20;
    o2.homingPrefix = "0"; o2.homingDigitOfs = 0;
    o2.elemHp = {1}; o2.elemThresh = {5000};   // >900 -> no latch
    const float wb2[6] = {3, -5, 10, 13, 5, 20};
    std::memcpy(o2.col.aabb, wb2, sizeof(wb2));
    mdk::playerPunch(rt2, 1);
    CHECK(o2.elemHp[0] == 0 && o2.field21e == 1 && o2.field21c == 1);
    CHECK(o2.field21d == 0);                   // state marks gated off
    CHECK(o2.health == 9);                     // fallthrough damage
    CHECK(rt2.eventTimerObj == nullptr);       // thresh > 900
  }

  // ---- the FUN_004572ac tail: per-arena double tick (OBSERVED) --
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    mdk::TraversalArena* a = makeShotRt(rt, f, "TICK");
    mdk::TraversalArena* b = travArenaAdd(rt, "TICK_B");
    b->dyn.col.verts = f.verts.data();
    b->dyn.col.polys = f.polys.data();
    b->dyn.col.nodes = f.nodes.data();
    rt.partner = b; rt.partnerActive = true;
    rt.cs.arenaValid = 1;
    rt.hudActive = 1;
    mdk::PlayerShot& s = rt.shots[0];
    s.state = 4; s.dyingTimer = 100; s.lifetime = 100; s.type = 0;
    s.arena = a;
    const mdk::GameplayInputBindings bindings;
    const mdk::FrontendTimingState timing;
    const mdk::RawGameplayInput idle{};
    mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    // Two arena invocations -> the shared pool ticked twice.
    CHECK(rt.seams.shotPoolTickCalls == 2);
    CHECK(s.dyingTimer == 100 - 2 * timing.frameStep);
  }
}

// Phase 5L — sniper scope lifecycle + mounted reticle. OBSERVED
// constants and ordering from MDK95.EXE BUILD_A disassembly: the
// dispatch picks mounted > sniper > normal each frame, the scope
// phase advances one step per frame at the frame head, and the
// mounted reticle stays a SEPARATE state path from the sniper.
void test_player_sniper() {
  auto setBit = [](std::array<std::uint32_t, 4>& bm, int code) {
    bm[code >> 5] |= 1u << (code & 31);
  };
  auto press = [&](mdk::RawGameplayInput& r, int code) {
    setBit(r.keyLevel, code);
    setBit(r.keyEdge, code);
  };
  auto makeRt = [](mdk::TraversalRuntime& rt, CollisionFixture& f) {
    mdk::TraversalArena* a = travArenaAdd(rt, "SNP_A");
    a->dyn.col.verts = f.verts.data();
    a->dyn.col.polys = f.polys.data();
    a->dyn.col.nodes = f.nodes.data();
    a->dyn.col.deepFloorZ = -1000.0f;
    rt.cur = a;
    rt.cs.arena = &a->dyn.col;
    rt.cs.queryEnabled = 1;
    rt.cs.arenaValid = 1;
    rt.cs.objectDataLoaded = 1;
    rt.cs.pos[2] = 12.0f;
    rt.cs.entryPos[2] = 12.0f;
  };
  const mdk::GameplayInputBindings bindings;   // factory: Sniper=57 Fire=29
  const mdk::FrontendTimingState timing;       // step1 smoothed1 dt=1/30
  const mdk::RawGameplayInput idle{};

  // ---- sniper entry + scope-in + raw-mouse aim + unscope ----------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    makeRt(rt, f);
    rt.hudActive = 1;      // the d0c++ world-tick counter needs this up
    mdk::TraversalFrameResult out;
    for (int i = 0; i < 40; ++i)
      out = mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(out.grounded);

    mdk::RawGameplayInput snip{};
    press(snip, 57);       // KeySniper — the entry gate reads it N-1.
    mdk::stepTraversalRuntime(rt, snip, bindings, timing);
    CHECK(rt.flagC9c == 0);   // not yet — one-frame input latency

    // Next frame: prevFrame.sniperPulse -> the FUN_00465228 entry.
    mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(rt.flagC9c == 1);
    CHECK(rt.locoState == 0x323);       // scope-in anim state
    CHECK(rt.eventPriority == 8);
    CHECK(rt.motion.moveVel == 0.0f && rt.motion.strafeVel == 0.0f &&
          rt.motion.turnVel == 0.0f && rt.motion.zoomChannel == 0.0f);
    // The 0x323 anim (world-tick) pins the scope camera + advances
    // ca0 to 1 on the pending frame.
    CHECK(rt.transitionPhase == 1);
    CHECK(rt.camera.pullback == 0.0f);
    CHECK(rt.camera.eyeHeight == 4.0f);
    CHECK(rt.scopeHudOffset != -101);   // HUD offset computed

    // ca0 walks 1->2->3, one phase per frame at the frame head.
    mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(rt.transitionPhase == 2);     // overlay requested
    CHECK(rt.scopeAnimLatch == 1);
    CHECK(rt.flag5414bc);
    mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(rt.transitionPhase == 3);     // overlay committed
    CHECK(rt.scopeAnimLatch == 0);
    CHECK(rt.camera.zoom == 1.0f);      // b58 clamps to the ceiling

    // Raw mouse aim — CURRENT frame, zero latency (0x464795): only
    // with no semantic channel + mouseOn. yaw -= dx*0.12*f0*zoom*0.41667.
    rt.motion.yawDeg = 90.0f;   // mid-range so the -5 delta can't wrap
    mdk::RawGameplayInput look{};
    look.mouseDx = 100;
    mdk::stepTraversalRuntime(rt, look, bindings, timing);
    CHECK(near(rt.motion.yawDeg, 85.0, 0.05));   // 90 - 100*0.12*1*1*0.41667

    // Manual unscope — sniperPulse (N-1) -> the 0x464986 unscope block
    // (ca0 is left; only the abort reset clears it).
    mdk::stepTraversalRuntime(rt, snip, bindings, timing);
    mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(rt.flagC9c == 0);
    CHECK(rt.locoState == 0x384);       // the unscope anim state
    CHECK(rt.camera.pullback == 8.0f);
    CHECK(rt.camera.eyeHeight == 4.5f);
    CHECK(rt.camera.zoom == 2.4f);
    CHECK(rt.scopeHudOffset == -101);
    // The 0x384 anim steady-state then the idle restore -> idle 0x65.
    mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(rt.locoState == 0x65);
  }

  // ---- mounted reticle (X_STRIKE, class 4) ------------------------
  {
    CollisionFixture f = makeFloorArena();
    mdk::TraversalRuntime rt;
    makeRt(rt, f);
    mdk::TraversalFrameResult out;
    for (int i = 0; i < 40; ++i)
      out = mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(out.grounded);

    // A standalone mount object: named + +0x14b&2 + the X_STRIKE
    // model name. Standalone (not in col.objects) so the object
    // prepass can't touch it; lastObjContact is the mount candidate.
    mdk::DynamicObject mount;
    mount.col.named = true;
    mount.col.flags14b |= 0x02;
    mount.model = makePlatformModel("X_STRIKE", "ELEM", 0.0f);
    mount.yawDeg = 90.0f;
    mount.pos[0] = 3.0f;
    mount.pos[1] = 4.0f;
    mount.pos[2] = 10.0f;
    mount.health = 10000;    // the reticle energy sentinel -> no drain
    rt.cs.lastObjContact = &mount.col;

    // The mount-scan (normal-path tail) -> class4Entry -> MOUNT.
    mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(rt.cs.excludeObj == &mount.col);
    CHECK(rt.mountClass == 0x40031u);
    CHECK(rt.flag49b740 == 1);                // overhead-cam gate
    CHECK(near(rt.camera.overheadHeight, 50.0, 1e-4));
    CHECK(near(rt.motion.moveVel, 300.0, 1e-4));    // reticle X
    CHECK(near(rt.motion.strafeVel, 180.0, 1e-4));  // reticle Y
    CHECK(rt.bombs == 10);
    CHECK(near(rt.bombRecharge, 1.0, 1e-4));
    CHECK(near(rt.motion.yawDeg, 90.0, 1e-4));      // pinned to mount
    CHECK(near(rt.cs.pos[0], 3.0, 1e-4) &&
          near(rt.cs.pos[2], 10.0, 1e-4));          // pos pinned

    // The mounted dispatch outranks normal/sniper: the reticle update
    // decays the overhead settle (25/s -> -0.8333/frame) and pins the
    // player to the mount each frame.
    mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(rt.cs.excludeObj == &mount.col);
    CHECK(near(rt.camera.overheadHeight, 50.0 - 25.0 / 30.0, 1e-3));
    CHECK(near(rt.motion.moveVel, 300.0, 1e-3));    // reticle stays clamped

    // The semi-auto latch (0x46935b): with hudActive down the shared
    // d0c counter isn't refilled by the world-tick, so held fire
    // spawns once on the armed frame then drains negative.
    const int bombsArmed = rt.bombs;    // 10
    mdk::RawGameplayInput fire{};
    press(fire, 29);                    // KeyFire — held for 4 frames
    for (int i = 0; i < 4; ++i)
      mdk::stepTraversalRuntime(rt, fire, bindings, timing);
    CHECK(rt.bombs == bombsArmed - 1);  // one spawn per press
    CHECK(rt.fieldD0c < 999);           // latch drained past armed
    // Releasing re-arms (d0c=999); recharge refills one bomb/second.
    for (int i = 0; i < 40; ++i)
      mdk::stepTraversalRuntime(rt, idle, bindings, timing);
    CHECK(rt.bombs == 10);
  }
}

// Phase 6A (G1-RE) — arena render-data boundary tests. All fixtures
// are synthetic; layouts follow the instruction-level observations in
// src/core/arena_render.h.
void test_arena_render() {
  using namespace mdk;

  // ---- poly decode: the render fields of the shared 0x24 record ---
  {
    std::uint8_t rec[0x24] = {};
    const auto put16 = [&](std::size_t o, std::uint16_t v) {
      rec[o] = std::uint8_t(v & 0xff);
      rec[o + 1] = std::uint8_t(v >> 8);
    };
    const auto putf = [&](std::size_t o, float v) {
      std::memcpy(rec + o, &v, 4);
    };
    put16(0, 5); put16(2, 9); put16(4, 2);      // v[3]
    put16(6, 0xfff0);                           // material s16 = -16
    putf(0x08, 1.5f); putf(0x0c, -2.0f);        // uv0
    putf(0x10, 3.25f); putf(0x14, 0.5f);        // uv1
    putf(0x18, 64.0f); putf(0x1c, 128.0f);      // uv2
    rec[0x20] = 0x11;                           // flags: bit4 skip+bit0
    rec[0x21] = 0xab;                           // aux21
    rec[0x22] = 0x90;                           // aux22: edge v10 + b7
    rec[0x23] = 0x07;                           // surface

    CollisionPoly cp;
    std::memcpy(&cp, rec, sizeof(cp));
    const ArenaRenderPoly rp = arenaRenderPolyDecode(cp);
    CHECK(rp.v[0] == 5 && rp.v[1] == 9 && rp.v[2] == 2);
    CHECK(rp.material == -16);
    CHECK(near(rp.uv[0][0], 1.5) && near(rp.uv[0][1], -2.0));
    CHECK(near(rp.uv[1][0], 3.25) && near(rp.uv[1][1], 0.5));
    CHECK(near(rp.uv[2][0], 64.0) && near(rp.uv[2][1], 128.0));
    CHECK(rp.flags == 0x11 && rp.aux21 == 0xab && rp.aux22 == 0x90);
    CHECK(rp.surface == 0x07);
    CHECK(arenaMatClassFor(rp.material) == ArenaMatClass::kPen);
    CHECK(arenaPenIndex(rp.material) == 16);
    // +0x22 bit7 = edge-overlay enable; 0x10/0x20/0x40 select edges
    // v1->v0 / v2->v1 / v2->v0 (FUN_00409860 -> FUN_0040da54).
    CHECK((rp.aux22 & kArenaEdgeOverlay) != 0);
    CHECK((rp.aux22 & kArenaEdgeV10) != 0);
    CHECK((rp.aux22 & (kArenaEdgeV21 | kArenaEdgeV20)) == 0);
    // +0x20: bit4 render-skip + bit0 alt-span gate (with the unbanked
    // view flag DAT_005414b4 -> DAT_005414b8).
    CHECK((rp.flags & kArenaPolySkip) != 0);
    CHECK((rp.flags & kArenaPolyAltSpan) != 0);
  }

  // ---- dispatch classification boundaries (FUN_0040c860) ---------
  {
    CHECK(arenaMatClassFor(0) == ArenaMatClass::kTextured);
    CHECK(arenaMatClassFor(5) == ArenaMatClass::kTextured);
    CHECK(arenaMatClassFor(-1) == ArenaMatClass::kPen);
    CHECK(arenaMatClassFor(-255) == ArenaMatClass::kPen);
    CHECK(arenaMatClassFor(-1023) == ArenaMatClass::kPen);
    CHECK(arenaMatClassFor(-990) == ArenaMatClass::kEffect770);
    CHECK(arenaMatClassFor(-1010) == ArenaMatClass::kEffect770);
    CHECK(arenaMatClassFor(-989) == ArenaMatClass::kPen);
    CHECK(arenaMatClassFor(-1011) == ArenaMatClass::kPen);
    CHECK(arenaMatClassFor(-1024) == ArenaMatClass::kEffect12970);
    CHECK(arenaMatClassFor(-1027) == ArenaMatClass::kEffect12970);
    CHECK(arenaMatClassFor(-1028) == ArenaMatClass::kEffectE94);
    CHECK(arenaMatClassFor(-1029) == ArenaMatClass::kEffect12970);
    CHECK(arenaMatClassFor(-4096) == ArenaMatClass::kEffect12970);
  }

  // ---- material decode: ordinary + extended payload headers ------
  {
    // Ordinary payload record: w=64, h=32 -> shift=6, uMask=63,
    // vMask = bucket(32)=0x1f << 6.
    auto s = SyntheticMti::build("TEST.MTI",
        {{"TEX64", 0x00000000, 0x11111111, 0x22222222, 4 + 64 * 32}});
    const auto dir = inspectMtiDirectory(s.buf);
    CHECK(dir.status == MtiDirectoryStatus::kOk);
    const auto& e = dir.entries[0];
    s.put16(static_cast<std::size_t>(e.payloadFileOffset()), 64);
    s.put16(static_cast<std::size_t>(e.payloadFileOffset() + 2), 32);
    ArenaRenderMaterial m;
    CHECK(arenaRenderMaterialDecode(s.buf, e.payloadFileOffset(),
                                    e.fieldAt0x08, e.fieldAt0x0C,
                                    e.fieldAt0x10, e.nameField, &m));
    CHECK(m.name == "TEX64");
    CHECK(m.width == 64 && m.height == 32);
    CHECK(m.shift == 6 && m.uMask == 63);
    CHECK(m.vMask == (0x1fu << 6));
    CHECK(m.invUMask == ~63u);
    CHECK(m.frameCount == 0);
    CHECK(m.pixels.size() == 64 * 32);
    CHECK(m.param0c == 0x11111111u && m.param10 == 0x22222222u);
    CHECK(!m.isIndexRecord);
    // pixels alias the payload's post-header bytes
    CHECK(m.pixels.data() ==
          reinterpret_cast<const std::uint8_t*>(s.buf.data()) +
              e.payloadFileOffset() + 4);
  }
  {
    // Extended header: u16 frameCount @+0, dims @+4/+6; pixels span
    // frameCount*w*h (OBSERVED: EXPLODE = 26 x 128x128).
    auto s = SyntheticMti::build("TEST.MTI",
        {{"ANIM", 0x00010000, 0, 0, 8 + 3 * 8 * 8}});
    const auto dir = inspectMtiDirectory(s.buf);
    CHECK(dir.status == MtiDirectoryStatus::kOk);
    const auto& e = dir.entries[0];
    CHECK(e.hasExtendedHeader());
    const std::size_t po = static_cast<std::size_t>(e.payloadFileOffset());
    s.put16(po, 3);       // frameCount
    s.put16(po + 4, 8);   // width
    s.put16(po + 6, 8);   // height
    ArenaRenderMaterial m;
    CHECK(arenaRenderMaterialDecode(s.buf, e.payloadFileOffset(),
                                    e.fieldAt0x08, e.fieldAt0x0C,
                                    e.fieldAt0x10, e.nameField, &m));
    CHECK(m.frameCount == 3 && m.width == 8 && m.height == 8);
    CHECK(m.pixels.size() == 3 * 8 * 8);
    // flags = low16 of class word | frameCount<<16 (OBSERVED merge).
    CHECK(m.flags == (0x0000u | (3u << 16)));
    // square -> vMask == uMask << shift; shift = ceil(log2 8) = 3.
    CHECK(m.shift == 3 && m.uMask == 7 && m.vMask == (7u << 3));
  }
  {
    // Index record: no payload dereference; the index value lands in
    // the +0x08 (height) slot and every other field stays zero — the
    // original's table buffer is memset(0) before the record loop
    // (FUN_0041a1e0 index path).
    auto s = SyntheticMti::build("TEST.MTI",
        {{"IDX", 0xffffffffu, 0x1234, 0, 0}});
    const auto dir = inspectMtiDirectory(s.buf);
    const auto& e = dir.entries[0];
    ArenaRenderMaterial m;
    CHECK(arenaRenderMaterialDecode(s.buf, e.payloadFileOffset(),
                                    e.fieldAt0x08, e.fieldAt0x0C,
                                    e.fieldAt0x10, e.nameField, &m));
    CHECK(m.isIndexRecord && m.width == 0 && m.height == 0x1234 &&
          m.flags == 0xffffffffu && m.pixels.empty());
  }

  // ---- FUN_00409a6c submission order on a synthetic BSP -----------
  {
    // Root plane z=0 splits node1 (+halfspace) from node2 (-halfspace).
    // Spans are {lo16 count | hi16 firstIdx} into the poly table.
    CollisionNode nodes[3] = {};
    nodes[0] = {0, 0, 1, 0,   /*far*/2, /*near*/1,
                /*polysPos*/(0u << 16) | 1u, /*polysNeg*/(1u << 16) | 1u,
                0, 0, 0, 0};
    nodes[1] = {0, 0, 1, -5,  -1, -1,
                /*polysPos*/(2u << 16) | 1u, 0, 0, 0, 0, 0};
    nodes[2] = {0, 0, 1, 5,   -1, -1,
                0, /*polysNeg*/(3u << 16) | 1u, 0, 0, 0, 0};
    CollisionPoly polys[4] = {};
    CollisionArena arena;
    arena.nodes = nodes;
    arena.polys = polys;

    // Camera on the +z side, live order (DAT_00499f8c == 1, painter's
    // back-to-front): far-side subtree first — node2 (negative
    // halfspace; its camera-side +span is empty) — then the node's
    // own camera-side span (poly0), then the near subtree (node1,
    // poly2).
    float camPos[3] = {0, 0, 10};
    std::vector<std::uint32_t> order;
    arenaRenderOrder(arena, camPos, false, &order);
    CHECK(order.size() == 2 && order[0] == 0 && order[1] == 2);

    // Camera on -z: far side is now the positive halfspace (node1 —
    // empty -span), then the node's -side span (poly1), then the near
    // subtree node2 (poly3).
    camPos[2] = -10;
    arenaRenderOrder(arena, camPos, false, &order);
    CHECK(order.size() == 2 && order[0] == 1 && order[1] == 3);

    // Dead front-to-back variant (0x499f8c == 0 — never taken since
    // the flag is image-initialized to 1 with zero writers): child
    // order swaps, span selection stays tied to the camera side.
    camPos[2] = 10;
    arenaRenderOrder(arena, camPos, true, &order);
    CHECK(order.size() == 2 && order[0] == 2 && order[1] == 0);
    camPos[2] = -10;
    arenaRenderOrder(arena, camPos, true, &order);
    CHECK(order.size() == 2 && order[0] == 3 && order[1] == 1);

    // +0x20 bit4 (0x10) is the render-skip (FUN_00409860, OBSERVED):
    // flagging poly2 drops it from the submission.
    camPos[2] = 10;
    polys[2].flags = 0x10;
    arenaRenderOrder(arena, camPos, false, &order);
    CHECK(order.size() == 1 && order[0] == 0);
    polys[2].flags = 0;
  }

  // ---- matlkup: bank A first, then bank B; miss -> -1 --------------
  {
    // Region-C names: c1=3 -> "RC00000","RC00001","RC00002" fields
    // poked to SHARED, LOCAL, MISSING below. c2=1 node, c3=1 poly,
    // c4=3 verts satisfy the collision blob walk.
    auto sm = SyntheticMto::build("TEST.MTO",
        {{"OV1", "OV1.MAT",
          {{"LOCAL", 0x00000000, 0, 0, 4 + 4 * 4},
           {"SHARED", 0x00000000, 0, 0, 4 + 8 * 8}},
          0, 0, 0, 3, 1, 1, 3, 0x150, 0}});
    {
      // Pass 1: locate the interior fields to poke, then re-parse so
      // the block view carries the poked names.
      const auto mto0 = inspectMtoDirectory(sm.buf);
      CHECK(mto0.status == MtoDirectoryStatus::kOk);
      const auto& b0 = mto0.blocks[0];
      const auto putCName = [&](std::size_t off, const char* s) {
        for (int i = 0; i < 10; ++i) sm.buf[off + i] = std::byte{0};
        sm.putName(off, s, 10);
      };
      putCName(static_cast<std::size_t>(b0.regionCOffset + 4), "SHARED");
      putCName(static_cast<std::size_t>(b0.regionCOffset + 14), "LOCAL");
      putCName(static_cast<std::size_t>(b0.regionCOffset + 24), "MISSING");
      const auto putf = [&](std::size_t o, float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        sm.put32(o, bits);
      };
      // The collision blob validates interior invariants: give the
      // node a unit plane + leaf children and the poly valid indices.
      const std::size_t nodeBase =
          static_cast<std::size_t>(b0.regionCOffset) + 4 + 3 * 10 + 2 + 4;
      putf(nodeBase + 8, 1.0f);                 // nz = 1 (unit plane)
      sm.put16(nodeBase + 0x10, 0xffff);        // childFar = -1
      sm.put16(nodeBase + 0x12, 0xffff);        // childNear = -1
      const std::size_t polyBase = nodeBase + 44 + 4;
      sm.put16(polyBase + 0, 0);                // v0
      sm.put16(polyBase + 2, 1);                // v1
      sm.put16(polyBase + 4, 2);                // v2
      sm.put16(polyBase + 6, 1);                // material = 1 (LOCAL)
      // Give the two .MAT payloads their headers (w,h).
      const std::uint64_t imgBase =
          b0.fileOffset + kMtoInnerFileOffset + kMtoInnerBlobOffset;
      sm.put16(static_cast<std::size_t>(imgBase +
                                        b0.innerRecords[0].fieldAt0x14), 4);
      sm.put16(static_cast<std::size_t>(imgBase +
                                        b0.innerRecords[0].fieldAt0x14 + 2),
               4);
      sm.put16(static_cast<std::size_t>(imgBase +
                                        b0.innerRecords[1].fieldAt0x14), 8);
      sm.put16(static_cast<std::size_t>(imgBase +
                                        b0.innerRecords[1].fieldAt0x14 + 2),
               8);
    }
    const auto mto = inspectMtoDirectory(sm.buf);
    CHECK(mto.status == MtoDirectoryStatus::kOk);
    const auto& b = mto.blocks[0];

    // Shared bank (LEVELnS.MTI shape): a SHARED record (wins over the
    // bank-B duplicate) plus BANKONLY.
    auto sb = SyntheticMti::build("LEVELXS.MTI",
        {{"SHARED", 0x00000000, 0, 0, 4 + 2 * 2},
         {"BANKONLY", 0x00000000, 0, 0, 4 + 4 * 4}});
    const auto sdir = inspectMtiDirectory(sb.buf);
    CHECK(sdir.status == MtiDirectoryStatus::kOk);
    sb.put16(static_cast<std::size_t>(sdir.entries[0].payloadFileOffset()), 2);
    sb.put16(static_cast<std::size_t>(sdir.entries[0].payloadFileOffset() + 2), 2);
    sb.put16(static_cast<std::size_t>(sdir.entries[1].payloadFileOffset()), 4);
    sb.put16(static_cast<std::size_t>(sdir.entries[1].payloadFileOffset() + 2), 4);

    mdk::CollisionArena arena;
    std::uint32_t counts[4] = {};
    CHECK(mdk::collisionBlobParse(
        reinterpret_cast<const std::uint8_t*>(sm.buf.data()) +
            b.regionCOffset,
        sm.buf.size() - b.regionCOffset, &arena, counts));
    CHECK(counts[0] == 3 && counts[1] == 1 && counts[2] == 1 &&
          counts[3] == 3);

    ArenaRenderData rd;
    CHECK(arenaRenderDataBuild(
        std::span<const std::byte>(sm.buf.data(), sm.buf.size()), b,
        arena, counts[1], counts[2], counts[3],
        std::span<const std::byte>(sb.buf.data(), sb.buf.size()), &rd));
    CHECK(rd.vertCount == 3 && rd.nodeCount == 1 && rd.polys.size() == 1);
    CHECK(rd.materialNames.size() == 3);
    CHECK(rd.materialNames[0] == "SHARED" && rd.materialNames[1] == "LOCAL" &&
          rd.materialNames[2] == "MISSING");
    CHECK(rd.bankA.size() == 2 && rd.bankB.size() == 2);
    // SHARED resolves to bank A slot 0 (bank A searched first);
    // LOCAL to bank B slot 0 -> combined index bankA.size()+0 = 2;
    // MISSING -> -1 (the original's fallback).
    CHECK(rd.materialOfName[0] == 0);
    CHECK(rd.materialOfName[1] == 2);
    CHECK(rd.materialOfName[2] == -1);
    CHECK(rd.bankA[0].name == "SHARED" && rd.bankA[0].width == 2);
    CHECK(rd.bankB[0].name == "LOCAL" && rd.bankB[0].width == 4);
    CHECK(rd.paletteRgb.size() == 0x150);

    // poly0 carries material index 1 -> LOCAL in bank B (textured).
    CHECK(rd.polys[0].material == 1);
    CHECK(rd.polyMaterialClass(0) == ArenaMatClass::kTextured);
    CHECK(rd.materialFor(0) == &rd.bankB[0]);
    ArenaRenderData rd2 = rd;
    rd2.polys[0].material = 2;   // MISSING -> fallback/unresolved
    CHECK(rd2.polyMaterialClass(0) == ArenaMatClass::kUnresolved);
    CHECK(rd2.materialFor(0) == nullptr);
    rd2.polys[0].material = -37; // pen path ignores the table
    CHECK(rd2.polyMaterialClass(0) == ArenaMatClass::kPen);
    CHECK(arenaPenIndex(-37) == 37);
  }
}

// Phase 7 (G1) — src/core/arena_mesh.h. Palette composition, the
// palette-expanded masked-addressable texture images, and the
// painter-ordered triangle soup.
void test_arena_mesh() {
  using namespace mdk;

  // ---- palette composition: SYS_PAL head + region B + s3 tail ----
  {
    std::uint8_t sysPal[192], levelPal[768], regionB[336], out[768];
    for (int i = 0; i < 192; ++i) sysPal[i] = std::uint8_t(i);       // R=0..63
    for (int i = 0; i < 768; ++i) levelPal[i] = std::uint8_t(255);   // all 0xff
    for (int i = 0; i < 336; ++i) regionB[i] = std::uint8_t(7);      // all 7
    CHECK(arenaPaletteCompose(sysPal, levelPal, regionB, 112, out));
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);   // entry0 black
    CHECK(out[3] == 3 && out[4] == 4 && out[5] == 5);   // SYS_PAL[1]
    CHECK(out[189] == 189);                            // SYS_PAL[63].r
    CHECK(out[192] == 7 && out[527] == 7);             // region B[0..111]
    CHECK(out[528] == 255 && out[765] == 255);         // s3 tail [176..]
    // count=64: only region B[0..64) lands; s3 keeps [128,256).
    CHECK(arenaPaletteCompose(sysPal, levelPal, regionB, 64, out));
    CHECK(out[192] == 7 && out[383] == 7);
    CHECK(out[384] == 255);
    // Missing SYS_PAL leaves the head black (degraded, flagged by
    // the caller); count clamps to the supplied region-B triplets.
    CHECK(arenaPaletteCompose({}, levelPal, regionB, 112, out));
    CHECK(out[0] == 0 && out[192] == 7 && out[528] == 255);
    // Empty levelPal is tolerated too (black tail) — short non-empty
    // tables are rejected.
    std::uint8_t shortPal[100] = {};
    CHECK(!arenaPaletteCompose(sysPal, shortPal, regionB, 112, out));
  }

  // ---- mesh bundle: order verbatim, classes -> tex/flat slots ----
  {
    // Synthetic render data: 4 verts, 3 polys (textured, pen,
    // unresolved), one 2x2 material in bank A.
    static float verts[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    ArenaRenderData rd;
    rd.verts = verts;
    rd.vertCount = 4;
    rd.materialNames = {"TEX", "UNUSED"};
    rd.materialOfName = {0, -1};
    ArenaRenderMaterial m;
    m.name = "TEX";
    m.width = m.height = 2;
    m.shift = 1;
    m.uMask = 1;
    m.vMask = 1u << 1;  // bucket(2)=1 << shift
    static std::uint8_t px[4] = {5, 6, 7, 8};
    m.pixels = std::span<const std::uint8_t>(px, 4);
    rd.bankA.push_back(m);
    ArenaRenderPoly p{};
    p.v[0] = 0; p.v[1] = 1; p.v[2] = 2;
    p.material = 0;                 // textured (name slot 0 -> A0)
    p.uv[0][0] = 0; p.uv[0][1] = 0;
    p.uv[1][0] = 1; p.uv[1][1] = 0;
    p.uv[2][0] = 0; p.uv[2][1] = 1;
    rd.polys.push_back(p);          // poly0 textured
    p.material = -37;               // pen 37
    p.v[2] = 3;
    rd.polys.push_back(p);          // poly1 pen
    p.material = 1;                 // unresolved (slot 1 -> -1)
    rd.polys.push_back(p);          // poly2 unresolved
    p.material = -1028;             // fxe94
    rd.polys.push_back(p);          // poly3 fx placeholder

    std::uint8_t pal[768];
    for (int i = 0; i < 768; ++i) pal[i] = std::uint8_t(i & 0xff);

    // Submit order 2,0,1,3 — arbitrary; the bundle must emit in
    // exactly this sequence.
    const std::uint32_t order[4] = {2, 0, 1, 3};
    ArenaMeshBundle b;
    CHECK(arenaMeshBuild(rd, order, pal, &b));
    CHECK(b.submitted == 4 && b.tris.size() == 4);
    CHECK(b.tris[0].poly == 2 && b.tris[1].poly == 0 &&
          b.tris[2].poly == 1 && b.tris[3].poly == 3);
    CHECK(b.tris[0].cls == ArenaMatClass::kUnresolved &&
          b.tris[0].tex == -1 && b.tris[0].flatSlot == 0xff);
    CHECK(b.tris[1].cls == ArenaMatClass::kTextured &&
          b.tris[1].tex == 0);
    CHECK(b.tris[2].cls == ArenaMatClass::kPen &&
          b.tris[2].tex == -1 && b.tris[2].flatSlot == 37);
    CHECK(b.tris[3].cls == ArenaMatClass::kEffectE94 &&
          b.tris[3].flatSlot == kArenaLutFxE94);
    // Vertex positions copied in poly order (poly2 uses v0,v1,v3).
    CHECK(near(b.tris[0].pos[2][2], 1.0));
    CHECK(near(b.tris[1].pos[1][0], 1.0));
    // Texture expansion: pitch=2, bucketH=2, palette-expanded.
    CHECK(b.texs.textures.size() == 1);
    CHECK(b.texs.textures[0].pitch == 2 &&
          b.texs.textures[0].bucketH == 2);
    CHECK(b.texs.textures[0].rgba.size() == 16);
    // palette[idx] expands to pal[3*idx..3*idx+2]: px[0]=5 -> 15,16,17.
    CHECK(b.texs.textures[0].rgba[0] == 15 &&
          b.texs.textures[0].rgba[1] == 16 &&
          b.texs.textures[0].rgba[2] == 17 &&
          b.texs.textures[0].rgba[3] == 255);
    CHECK(b.texs.textures[0].rgba[4] == 18 &&
          b.texs.textures[0].rgba[12] == 24);
    // Census over ALL polys (unresolved=1 pen=1 textured=1 fxe94=1).
    CHECK(b.clsCount[0] == 1 && b.clsCount[1] == 1 &&
          b.clsCount[2] == 1 && b.clsCount[4] == 1);
    // Digests: order digest = FNV over the submitted order.
    CHECK(b.orderDigest == arenaOrderDigest(order));
    // Atlas: 2x2 texture at (0,0); LUT strip on the next row.
    CHECK(b.texs.textures[0].atlasX == 0 &&
          b.texs.textures[0].atlasY == 0);
    CHECK(b.texs.lutY == 2 && b.texs.atlasH == 3 &&
          b.texs.atlasW == 2048);

    // Malformed order index -> build fails.
    const std::uint32_t bad[1] = {9};
    CHECK(!arenaMeshBuild(rd, bad, pal, &b));
  }

  // ---- order digest matches the mdk-inspect fold -----------------
  {
    const std::uint32_t order[3] = {5, 1, 7};
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint32_t i : order) {
      for (int k = 0; k < 4; ++k) {
        h = (h ^ std::uint8_t(i >> (k * 8))) * 0x100000001b3ull;
      }
    }
    CHECK(arenaOrderDigest(order) == h);
  }
}

// ---------------------------------------------------------------------------
// Phase 13A — freefall runtime (FALL3D / mode 2). Constants asserted below
// are OBSERVED decodes from MDK95.EXE (see freefall_runtime.h ownership map).
// ---------------------------------------------------------------------------

namespace {

using mdk::FreefallCourseData;
using mdk::FreefallInput;
using mdk::FreefallObject;
using mdk::FreefallRuntime;

FreefallCourseData ffCourse(int course = 0, int skill = 0,
                            std::initializer_list<const char*> names = {}) {
  FreefallCourseData d;
  d.course = course;
  d.skill = skill;
  d.explodeAnimFrames = 30.0f;
  for (const char* n : names) {
    mdk::FreefallPickupRec r{};
    std::strncpy(r.name, n, 8);
    d.pickups.push_back(r);
  }
  return d;
}

// Standard frame: 1 int step, 1 frame unit, 1/30 s.
void ffStep(FreefallRuntime& rt, const FreefallInput& in) {
  mdk::freefallStep(rt, in, 1, 1.0f, 1.0f / 30.0f);
}
void ffStepN(FreefallRuntime& rt, int n, const FreefallInput& in = {}) {
  for (int i = 0; i < n; ++i) ffStep(rt, in);
}
void ffRunIntro(FreefallRuntime& rt) {
  while (rt.introCountdown > 0) ffStep(rt, {});
}
FreefallObject* ffPlayer(FreefallRuntime& rt) {
  return rt.listHead >= 0 ? &rt.pool[rt.listHead] : nullptr;
}
FreefallObject* ffFirstOfType(FreefallRuntime& rt, int type) {
  for (int i = rt.listHead; i >= 0; i = rt.pool[i].next)
    if (rt.pool[i].type == type) return &rt.pool[i];
  return nullptr;
}
int ffCountType(FreefallRuntime& rt, int type) {
  int n = 0;
  for (int i = rt.listHead; i >= 0; i = rt.pool[i].next)
    if (rt.pool[i].type == type) ++n;
  return n;
}
bool ffSawEvent(const FreefallRuntime& rt, int kind, int a) {
  for (const auto& e : rt.events)
    if (e.kind == kind && e.a == a) return true;
  return false;
}

} // namespace

void test_freefall_init() {
  // Difficulty block (OBSERVED formulas, c=course s=skill).
  {
    FreefallRuntime rt;
    mdk::freefallInit(rt, ffCourse(0, 0), 1);
    CHECK(rt.health == 100 && rt.introCountdown == 150);
    CHECK(rt.waveSize == 2 && rt.missileDelay == 32 && rt.radarDelay == 63);
    CHECK(near(rt.wanderScale, 7.5) && near(rt.radarSpeed, 117.6470588, 1e-4));
    CHECK(!rt.bonesCourse && rt.pickupsRemaining == 0);
    CHECK(rt.freeHead == 0 && rt.listHead == -1);
    CHECK(rt.pool[0].next == 1 && rt.pool[397].next == 398 &&
          rt.pool[398].next == -1);
  }
  {
    FreefallRuntime rt;
    mdk::freefallInit(rt, ffCourse(4, 2, {"SW_H25"}), 7);
    CHECK(rt.waveSize == 4 && rt.missileDelay == 12 && rt.radarDelay == 27);
    CHECK(near(rt.wanderScale, 1.5) &&
          near(rt.radarSpeed, 117.6470588 * (1.0 + 4.0 / 3.0), 1e-3));
    CHECK(rt.bonesCourse && rt.pickupsRemaining == 1);
  }
  {
    FreefallRuntime rt;
    mdk::freefallInit(rt, ffCourse(3, 1), 9);
    CHECK(rt.waveSize == 3 && rt.missileDelay == 11 && rt.radarDelay == 42);
    CHECK(near(rt.wanderScale, 3.5) &&
          near(rt.radarSpeed, 117.6470588 * 1.6, 1e-3));
  }
}

void test_freefall_intro() {
  FreefallRuntime rt;
  mdk::freefallInit(rt, ffCourse(4, 0), 12345);
  // 150 countdown frames at frameStep 1; player spawns once t < 90.
  for (int i = 0; i < 60; ++i) ffStep(rt, {});
  CHECK(rt.introCountdown == 90 && ffPlayer(rt) == nullptr);
  ffStep(rt, {});
  FreefallObject* pl = ffPlayer(rt);
  CHECK(pl != nullptr && pl->type == 0 && pl->model == mdk::kFfModelKurt);
  // Eased arc s = 1-(1-t)^2, t = (90-tc)/60.
  const float t = 1.0f / 60.0f;
  const float s = 1.0f - (1.0f - t) * (1.0f - t);
  CHECK(near(pl->px, s * 30.0f - 30.0f, 1e-4));
  CHECK(near(pl->py, s * 10.0f - 10.0f, 1e-4));
  CHECK(near(pl->pz, s * -10.0f, 1e-4));
  ffRunIntro(rt);
  CHECK(rt.introCountdown == 0);
  pl = ffPlayer(rt);
  CHECK(pl && near(pl->pz, 5206.0));
  // Bones on course >= 4 (0x4edaf4).
  FreefallObject* bo = rt.bonesIdx >= 0 ? &rt.pool[rt.bonesIdx] : nullptr;
  CHECK(bo && bo->type == 5 && near(bo->pz, 5290.0));
  // radarTimer seeded (rand&0xf)+7 — exact value depends on LCG seed.
  CHECK(rt.radarTimer >= 7 && rt.radarTimer <= 22);

  // No bones on lower courses.
  FreefallRuntime rt2;
  mdk::freefallInit(rt2, ffCourse(3, 0), 12345);
  ffRunIntro(rt2);
  CHECK(rt2.bonesIdx == -1 && ffCountType(rt2, 5) == 0);
}

void test_freefall_input_fold() {
  float out[4];
  mdk::freefallInputFold({}, out);
  CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
  mdk::freefallInputFold({.left = true}, out);
  CHECK(near(out[0], -11.7647059) && near(out[1], -117.647059, 1e-4));
  mdk::freefallInputFold({.down = true}, out);
  CHECK(near(out[2], -11.7647059) && near(out[3], -117.647059, 1e-4));
  // Analog: X direct, Y negated; digital wins.
  mdk::freefallInputFold({.axisX = 0.5f, .axisY = 0.5f}, out);
  CHECK(near(out[0], 0.5 * 11.7647059) && near(out[2], -0.5 * 11.7647059));
  mdk::freefallInputFold({.left = true, .axisX = 0.9f}, out);
  CHECK(near(out[0], -11.7647059));
}

void test_freefall_motion() {
  FreefallRuntime rt;
  mdk::freefallInit(rt, ffCourse(0, 0), 99);
  ffRunIntro(rt);
  rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
  FreefallObject* pl = ffPlayer(rt);
  CHECK(pl);
  const float z0 = pl->pz;
  // Neutral input: position holds (velocity decays), z falls at 66.67/s.
  ffStepN(rt, 30);
  CHECK(near(pl->px, 0.0, 1e-4) && near(pl->py, 0.0, 1e-4));
  CHECK(near(pl->pz, z0 - 66.6666667 * 1.0, 1e-2));
  // Hold right: accelerates toward cap, clamps at +58.8235. Keep the
  // total under the 30s control cutoff.
  FreefallInput in{};
  in.right = true;
  ffStepN(rt, 300, in);
  CHECK(pl->px <= 58.8235294f + 1e-4f);
  CHECK(near(pl->px, 58.8235294, 1e-3) && pl->vx == 0.0f);
  in = {};
  in.up = true;
  ffStepN(rt, 300, in);
  CHECK(pl->py <= 35.2941176f + 1e-4f);
  CHECK(near(pl->py, 35.2941176, 1e-3) && pl->vy == 0.0f);
  // Control cutoff at timeline > 30: spring-damp pulls to center.
  rt.timeline = 30.01f;
  pl->vx = 50.0f;
  const float pxPre = pl->px;
  ffStep(rt, in);
  CHECK(near(pl->vx, (50.0f - pxPre * 2.0f) * 0.5f, 1e-3));
  // AABB tracks position with {4,5,5} extents.
  CHECK(near(pl->aabb[0], pl->px - 4.0) && near(pl->aabb[3], pl->px + 4.0));
  CHECK(near(pl->aabb[1], pl->py - 5.0) && near(pl->aabb[5], pl->pz + 5.0));
}

void test_freefall_missile() {
  FreefallRuntime rt;
  mdk::freefallInit(rt, ffCourse(0, 0), 4242);
  ffRunIntro(rt);
  rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
  FreefallObject* pl = ffPlayer(rt);
  CHECK(pl);
  // Force a missile wave: budget 1, timer 1 -> spawns next tick.
  rt.missileBudget = 1;
  rt.missileTimer = 1;
  ffStep(rt, {});
  FreefallObject* m = ffFirstOfType(rt, 1);
  // Spawned with timer 60, ticked once in the same frame -> 59.
  CHECK(m != nullptr && m->timer == 59);
  const float spd = std::sqrt(m->vx * m->vx + m->vy * m->vy);
  CHECK(near(spd, 250.0, 1e-3) && near(m->vz, 250.0));
  CHECK(ffSawEvent(rt, mdk::kFfEvSound, mdk::kFfSndMLnch));
  // Launch: timer 60 -> 0 over 60 frames, position integrates.
  const float mz0 = m->pz;
  ffStepN(rt, 60);
  CHECK(m->timer == 0);
  CHECK(m->pz > mz0);  // launched upward (+vz) at the falling player
  // Homing: velocity blends toward the lead point.
  ffStepN(rt, 30);
  const float vmag =
      std::sqrt(m->vx * m->vx + m->vy * m->vy + m->vz * m->vz);
  CHECK(vmag > 200.0f && vmag < 300.0f);
  // Place it on a collision course: right at the player moving in.
  m->px = pl->px;
  m->py = pl->py;
  m->pz = pl->pz - 3.0f;
  m->vx = 0;
  m->vy = 0;
  m->vz = 60.0f;
  const int hp0 = rt.health;
  ffStep(rt, {});
  // Converted to a type-2 explosion at the player position.
  FreefallObject* ex = ffFirstOfType(rt, 2);
  CHECK(ex != nullptr && ex == m);
  CHECK(ex->model == mdk::kFfModelBang && ex->explodeFlag == 1);
  CHECK(rt.health == hp0 - 4);  // skill 0: fixed 4 damage
  CHECK(pl->animHandle == mdk::kFfAnimKurtHit);
  CHECK(ffSawEvent(rt, mdk::kFfEvSound, mdk::kFfSndExplode));
  // Explosion lifetime = explodeAnimFrames units, then freed.
  rt.explodeAnimFrames = 3.0f;
  ffStepN(rt, 4);
  CHECK(ffFirstOfType(rt, 2) == nullptr);
  // Pass behavior: once dz <= -5 the missile sinks 60 frames then frees.
  FreefallRuntime rt2;
  mdk::freefallInit(rt2, ffCourse(0, 0), 4242);
  ffRunIntro(rt2);
  rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
  FreefallObject* pl2 = ffPlayer(rt2);
  rt2.missileBudget = 1;
  rt2.missileTimer = 1;
  ffStep(rt2, {});
  FreefallObject* m2 = ffFirstOfType(rt2, 1);
  CHECK(m2);
  // Skip launch; park it above the player so homing registers a pass
  // (dz = player.z - missile.z <= -5 means the missile overtook upward).
  m2->timer = 0;
  m2->pz = pl2->pz + 10.0f;
  ffStep(rt2, {});
  CHECK(m2->timer < 0);  // post-pass linger
  CHECK(ffSawEvent(rt2, mdk::kFfEvSound, mdk::kFfSndMPass));
  ffStepN(rt2, 65);
  CHECK(ffFirstOfType(rt2, 1) == nullptr);
  // Collision window closes at timeline 30.
  FreefallRuntime rt3;
  mdk::freefallInit(rt3, ffCourse(0, 0), 4242);
  ffRunIntro(rt3);
  rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
  rt3.timeline = 30.5f;
  rt3.missileBudget = 1;
  rt3.missileTimer = 1;
  ffStep(rt3, {});
  FreefallObject* m3 = ffFirstOfType(rt3, 1);
  FreefallObject* pl3 = ffPlayer(rt3);
  m3->px = pl3->px;
  m3->py = pl3->py;
  m3->pz = pl3->pz - 3.0f;
  m3->vz = 60.0f;
  const int hp3 = rt3.health;
  ffStep(rt3, {});
  CHECK(rt3.health == hp3 && ffFirstOfType(rt3, 2) == nullptr);
}

void test_freefall_pickup() {
  FreefallRuntime rt;
  // FALLPU pops backward — the last record ("SW_H25") spawns first.
  mdk::freefallInit(rt, ffCourse(0, 0, {"SW_DUMMY", "SW_KEY", "SW_H25"}),
                    777);
  ffRunIntro(rt);
  rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
  CHECK(rt.pickupsRemaining == 3);
  FreefallObject* pl = ffPlayer(rt);
  // Force the pop timer -> pops the LAST record first (backward walk).
  const float plz0 = pl->pz;
  rt.pickupTimer = 1;
  ffStep(rt, {});
  FreefallObject* p = ffFirstOfType(rt, 4);
  CHECK(p != nullptr && p->pickupRec == 2);
  CHECK(near(p->vz, -133.333333, 1e-3));
  // Spawned at player.z+15 then integrated once this frame (-133.33*dt).
  CHECK(near(p->pz, plz0 + 15.0f - 133.333333f / 30.0f, 1e-3));
  CHECK(p->timer >= 29 && p->timer <= 92);  // (rand&0x3f)+30, -1 this tick
  CHECK(ffSawEvent(rt, mdk::kFfEvSound, mdk::kFfSndPFall));
  // Timer expiry deploys the chute + brakes toward -50.
  p->timer = 1;
  ffStep(rt, {});
  CHECK(p->chute == mdk::kFfModelChute);
  CHECK(ffSawEvent(rt, mdk::kFfEvSound, mdk::kFfSndChute));
  ffStepN(rt, 60);
  CHECK(near(p->vz, -50.0, 1e-4));
  // No-hit: the pickup is NOT freed (regression — earlier bug freed it).
  p->px = pl->px + 50.0f;
  p->py = pl->py + 40.0f;
  ffStep(rt, {});
  CHECK(ffFirstOfType(rt, 4) == p);
  // Collect: align on the player, let the segment clip.
  p->px = pl->px;
  p->py = pl->py;
  p->pz = pl->pz + 2.0f;
  rt.health = 50;
  ffStep(rt, {});
  CHECK(ffFirstOfType(rt, 4) == nullptr);
  CHECK(rt.health == 60);  // SW_H25 = +10 cap 100 (OBSERVED grant)
  CHECK(ffSawEvent(rt, mdk::kFfEvSound, mdk::kFfSndPColl));
  CHECK(ffSawEvent(rt, mdk::kFfEvGrantHealth, 5));
  // Cull: deployed pickup above the camera plane despawns.
  FreefallRuntime rt2;
  mdk::freefallInit(rt2, ffCourse(0, 0, {"SW_H50"}), 777);
  ffRunIntro(rt2);
  rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
  rt2.pickupTimer = 1;
  ffStep(rt2, {});
  FreefallObject* q = ffFirstOfType(rt2, 4);
  q->timer = 0;
  q->chute = mdk::kFfModelChute;
  ffStep(rt2, {});
  q->pz = rt2.cameraPos[2] + 5.0f;
  ffStep(rt2, {});
  CHECK(ffFirstOfType(rt2, 4) == nullptr);
}

void test_freefall_radar() {
  FreefallRuntime rt;
  mdk::freefallInit(rt, ffCourse(0, 0), 5150);
  ffRunIntro(rt);
  FreefallObject* pl = ffPlayer(rt);
  rt.radarTimer = 1;
  ffStep(rt, {});
  FreefallObject* r = ffFirstOfType(rt, 3);
  CHECK(r != nullptr && ffSawEvent(rt, mdk::kFfEvSound, mdk::kFfSndRStart));
  // Rises at 3000/s toward player.z - 3 (the plane sinks as Kurt falls;
  // a 100-unit rise overshoots it and snaps to the plane).
  r->tz = pl->pz - 103.0f;
  ffStep(rt, {});
  CHECK(near(r->tz, pl->pz - 3.0, 1e-3));
  // In-plane steer; force a lock by placing the marker at the player.
  r->tx = pl->px;
  r->ty = pl->py;
  const int budget0 = rt.missileBudget;
  ffStep(rt, {});
  CHECK(r->timer == -1);
  CHECK(rt.missileTimer == 1);
  CHECK(rt.missileBudget >= budget0 + rt.waveSize &&
        rt.missileBudget <= budget0 + rt.waveSize + 1);
  CHECK(ffSawEvent(rt, mdk::kFfEvSound, mdk::kFfSndKSeen));
  // Sink: tz drops at 1500/s, frees below 0 and re-arms the timer.
  rt.radarDelay = 63;
  r->tz = 1.0f;
  ffStep(rt, {});
  CHECK(ffFirstOfType(rt, 3) == nullptr);
  CHECK(rt.radarTimer >= 63 && rt.radarTimer <= 126);
}

void test_freefall_completion_death() {
  // Completion: timeline > 33 returns true, finished.
  {
    FreefallRuntime rt;
    mdk::freefallInit(rt, ffCourse(0, 0), 11);
    ffRunIntro(rt);
    rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
    // Completion is post-increment: 32.9 + 1/30 stays under 33.
    rt.timeline = 32.9f;
    CHECK(!mdk::freefallStep(rt, {}, 1, 1.0f, 1.0f / 30.0f));
    rt.timeline = 33.0f;
    CHECK(mdk::freefallStep(rt, {}, 1, 1.0f, 1.0f / 30.0f));
    CHECK(rt.finished && !rt.died &&
          rt.phase == FreefallRuntime::Phase::kDone);
  }
  // Death: health <= 0 -> fade to black (~1s at fade 1.0), then true.
  {
    FreefallRuntime rt;
    mdk::freefallInit(rt, ffCourse(0, 0), 11);
    ffRunIntro(rt);
    rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
    rt.timeline = 5.0f;
    rt.health = 0;
    rt.fade = 0.05f;
    CHECK(!mdk::freefallStep(rt, {}, 1, 1.0f, 1.0f / 30.0f));
    CHECK(mdk::freefallStep(rt, {}, 1, 1.0f, 1.0f / 30.0f));
    CHECK(rt.died && !rt.finished &&
          rt.phase == FreefallRuntime::Phase::kDone);
  }
  // K_FINISH edge fires once when control ends.
  {
    FreefallRuntime rt;
    mdk::freefallInit(rt, ffCourse(0, 0), 11);
    ffRunIntro(rt);
    rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
    rt.timeline = 29.99f;
    rt.camPrevTimeline = 29.9f;
    ffStep(rt, {});
    bool seen = ffSawEvent(rt, mdk::kFfEvSound, mdk::kFfSndKFinish);
    ffStep(rt, {});
    seen = seen || ffSawEvent(rt, mdk::kFfEvSound, mdk::kFfSndKFinish);
    CHECK(seen);
    int count = 0;
    for (int i = 0; i < 5; ++i) {
      ffStep(rt, {});
      for (const auto& e : rt.events)
        if (e.kind == mdk::kFfEvSound && e.a == mdk::kFfSndKFinish) ++count;
    }
    CHECK(count == 0);
  }
}

void test_freefall_determinism() {
  // Two identical runs produce identical state digests.
  auto digest = [](FreefallRuntime& rt, int frames) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    FreefallInput in{};
    for (int f = 0; f < frames; ++f) {
      in.right = (f % 40) < 20;
      in.up = (f % 60) < 30;
      mdk::freefallStep(rt, in, 1, 1.0f, 1.0f / 30.0f);
      mix(std::bit_cast<std::uint32_t>(rt.timeline));
      mix(static_cast<std::uint32_t>(rt.rng));
      mix(static_cast<std::uint32_t>(rt.health));
      for (int i = rt.listHead; i >= 0; i = rt.pool[i].next) {
        const FreefallObject& o = rt.pool[i];
        mix(std::bit_cast<std::uint32_t>(o.px));
        mix(std::bit_cast<std::uint32_t>(o.py));
        mix(std::bit_cast<std::uint32_t>(o.pz));
        mix(static_cast<std::uint32_t>(o.type) << 16 |
            static_cast<std::uint32_t>(o.timer));
      }
    }
    return h;
  };
  FreefallRuntime a, b;
  auto course = ffCourse(2, 1, {"SW_H25", "SW_SGREN", "SW_KEY"});
  mdk::freefallInit(a, course, 0xC0FFEE);
  mdk::freefallInit(b, course, 0xC0FFEE);
  const std::uint64_t h = digest(a, 1200);
  CHECK(h == digest(b, 1200));
  // Different seed -> different digest (RNG actually feeds gameplay).
  FreefallRuntime c;
  mdk::freefallInit(c, course, 0xDEAD);
  CHECK(digest(c, 1200) != h);
}

void test_freefall_freelist() {
  // LIFO freelist: alloc order pool[0], [1], ...; free pushes back on top.
  // Active-list insertion is AFTER the head, so the newest spawn walks
  // first (list order 0 -> newest -> older).
  FreefallRuntime rt;
  mdk::freefallInit(rt, ffCourse(0, 0), 3);
  ffRunIntro(rt);  // player = pool[0]
  rt.radarTimer = 0;  // disarm ambient spawns (seeded (rand&0xf)+7 by the intro)
  CHECK(rt.listHead == 0);
  rt.missileBudget = 2;
  rt.missileTimer = 1;
  ffStep(rt, {});   // missile A = pool[1]
  rt.missileTimer = 1;
  ffStep(rt, {});   // missile B = pool[2], walks before A
  FreefallObject* b = ffFirstOfType(rt, 1);
  CHECK(b == &rt.pool[2]);
  // Free pool[2] via the post-pass linger path -> LIFO push.
  b->timer = -58;
  ffStepN(rt, 4);   // -59,-60,-61 < -60 -> freed
  CHECK(ffFirstOfType(rt, 1) == &rt.pool[1]);
  rt.missileBudget = 1;
  rt.missileTimer = 1;
  ffStep(rt, {});
  FreefallObject* m = ffFirstOfType(rt, 1);
  CHECK(m == &rt.pool[2]);  // LIFO reuse — stale fields inherit
  // Stale-field inheritance: the reused record kept its old position
  // (the missile spawner never writes position — OBSERVED).
  CHECK(m->pz != 0.0f);
}

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
  test_player_surface();
  test_dynamic_objects();
  test_traversal_connect_pairing();
  test_traversal_portal_test();
  test_traversal_trigger_scan();
  test_traversal_deep_floor();
  test_traversal_script();
  test_traversal_object_init();
  test_traversal_object_script();
  test_enemy_runtime();
  test_mover_runtime();
  test_object_animation();
  test_player_look();
  test_player_camera();
  test_camera_obstruction();
  test_camera_nudge();
  test_player_sniper();
  test_player_fire();
  test_player_projectiles();
  test_arena_render();
  test_arena_mesh();
  test_freefall_init();
  test_freefall_intro();
  test_freefall_input_fold();
  test_freefall_motion();
  test_freefall_missile();
  test_freefall_pickup();
  test_freefall_radar();
  test_freefall_completion_death();
  test_freefall_determinism();
  test_freefall_freelist();
  std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
