// mdk-inspect — developer-facing read-only inspector for original MDK
// data files. Prints safe header/container/directory metadata only:
// never dumps payloads, never writes into the data root.
//
// Usage:
//   mdk-inspect --data-path DIR <relative-path>
//   mdk-inspect --data-path DIR --container <relative-path>
//   mdk-inspect --data-path DIR --entries <relative-path>
//   mdk-inspect --data-path DIR --visual-info <relative-path> <record>
//   mdk-inspect --data-path DIR --font-info <relative-path> <record>
//               [<code>]
//   mdk-inspect --data-path DIR --collision-probe <relative-path>
//               [<x> <y> <z>]   (Phase 5D: locate the level-stream
//               collision blob, run one swept query + floor probe)
//   mdk-inspect --selftest        (synthetic in-memory checks)

#include "core/binary_reader.h"
#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/cmi_directory.h"
#include "core/collision_query.h"
#include "core/container.h"
#include "core/data_root.h"
#include "core/dti_structure.h"
#include "core/dynamic_objects.h"
#include "core/file_family.h"
#include "core/fti_directory.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/gameplay_input.h"
#include "core/mti_directory.h"
#include "core/mto_directory.h"
#include "core/player_motion.h"
#include "core/player_surface.h"
#include "core/player_vertical.h"
#include "core/sni_directory.h"
#include "core/stream_context.h"
#include "core/traversal_runtime.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kInspectHeadBytes = 64;
// Whole-file read cap for --entries: far above every observed file
// (largest in BUILD_A ≈ 8 MB) while staying a sane bound.
constexpr std::size_t kEntriesMaxBytes = 512ull * 1024 * 1024;

int usage() {
  std::fprintf(stderr,
               "usage: mdk-inspect --data-path DIR [--container | "
               "--entries] <relative-path>\n"
               "       mdk-inspect --data-path DIR --visual-info "
               "<relative-path> <record>\n"
               "       mdk-inspect --data-path DIR --font-info "
               "<relative-path> <record> [<code>]\n"
               "       mdk-inspect --data-path DIR --sprite-info "
               "<relative-path> <record>\n"
               "       mdk-inspect --data-path DIR --collision-probe "
               "<relative-path> [<x> <y> <z>]\n"
               "       mdk-inspect --data-path DIR --arena-objects "
               "<relative-path>   (a .DTI path; the sibling .CMI and\n"
               "                            <stem>O.MTO are loaded too)\n"
               "       mdk-inspect --data-path DIR --surface-census "
               "<relative-path>   (a .DTI path; the sibling <stem>O.MTO\n"
               "                            is scanned for surface polys)\n"
               "       mdk-inspect --data-path DIR --traversal-runtime "
               "<relative-path>\n"
               "                            (a .DTI path; loads the sibling\n"
               "                            .CMI + <stem>O.MTO, assembles the\n"
               "                            Phase 5G runtime and steps frames.\n"
               "                            Options: --arena NAME --start X Y Z\n"
               "                            --yaw DEG --frames N)\n"
               "       mdk-inspect --selftest\n"
               "       mdk-inspect --selftest-player-surface\n");
  return 2;
}

std::string stemOf(const std::string& relPath) {
  const auto slash = relPath.find_last_of("/\\");
  const auto dot = relPath.find_last_of('.');
  if (dot == std::string::npos ||
      (slash != std::string::npos && dot < slash)) {
    return {};
  }
  const auto begin = slash == std::string::npos ? 0 : slash + 1;
  return relPath.substr(begin, dot - begin);
}

int selftest() {
  // Synthetic 28-byte tag-envelope fixture (not original data):
  //   u32 len = 24 (= size-4), name "TEST.MAT" + NUL pad.
  const std::byte raw[] = {
      std::byte{0x18}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{'T'},  std::byte{'E'},  std::byte{'S'},  std::byte{'T'},
      std::byte{'.'},  std::byte{'M'},  std::byte{'A'},  std::byte{'T'},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x0c}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
  };
  const auto info = mdk::inspectContainer(raw, sizeof(raw));
  bool ok = info.hasDeclaredLength && info.lengthValid &&
            info.declaredLength == 24 &&
            info.shape == mdk::ContainerShape::kTaggedName &&
            info.logicalName == "TEST.MAT" &&
            mdk::nameStemMatches(info, "test");
  std::fprintf(stderr, "selftest envelope: %s\n", ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic SNI-like fixture: envelope + count=1 + one 24-byte
  // record {name[12], u32, blobOff=0x18, size=4} + 4 payload bytes +
  // name trailer. Total 0x18+0x18+4+12 = 0x48 = 72 bytes.
  std::byte sni[72] = {};
  const auto put32 = [&](std::size_t off, std::uint32_t v) {
    sni[off + 0] = static_cast<std::byte>(v & 0xff);
    sni[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    sni[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    sni[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto putName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(sni); ++i) {
      sni[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  put32(0x00, sizeof(sni) - 4);
  putName(0x04, "TEST.SND");
  put32(0x10, sizeof(sni) - 12);
  put32(0x14, 1);                  // count
  putName(0x18, "ENTRY1");
  put32(0x18 + 0x0c, 3);           // unknown field
  put32(0x18 + 0x10, 0x30 - 4);    // blobOffset → file 0x30
  put32(0x18 + 0x14, 4);           // payloadSize
  putName(sizeof(sni) - 12, "TEST.SND");

  const auto dir = mdk::inspectSniDirectory(
      std::span<const std::byte>(sni, sizeof(sni)));
  ok = dir.status == mdk::SniDirectoryStatus::kOk &&
       dir.count == 1 && dir.entries.size() == 1 &&
       dir.entries[0].name() == "ENTRY1" &&
       dir.entries[0].payloadFileOffset() == 0x30 &&
       dir.entries[0].payloadSize == 4 && dir.trailerPresent &&
       dir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest sni-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic MTI-like fixture: envelope + count=2 + two 24-byte
  // records: one extended-header payload record and one index record.
  // Layout: 0x18 + 2*24 = 0x48 dir end; payload at 0x48 (8-byte ext
  // header: n=2,a=64,b=32 + 4 data bytes) then name trailer.
  // Total = 0x48 + 12 + 12 = 0x60 = 96 bytes.
  std::byte mti[96] = {};
  const auto mput32 = [&](std::size_t off, std::uint32_t v) {
    mti[off + 0] = static_cast<std::byte>(v & 0xff);
    mti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    mti[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    mti[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto mput16 = [&](std::size_t off, std::uint16_t v) {
    mti[off + 0] = static_cast<std::byte>(v & 0xff);
    mti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
  };
  const auto mputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(mti); ++i) {
      mti[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  mput32(0x00, sizeof(mti) - 4);
  mputName(0x04, "TEST.MTI");
  mput32(0x10, sizeof(mti) - 12);
  mput32(0x14, 2);                    // count
  mputName(0x18, "MAT0");             // record 0: payload (ext header)
  mput32(0x18 + 0x08, 0x00010001);    // flags: extended header
  mput32(0x18 + 0x0c, 0);             // raw param
  mput32(0x18 + 0x10, 0x40600000);    // raw param (float-looking)
  mput32(0x18 + 0x14, 0x48 - 4);      // blobOffset -> file 0x48
  mputName(0x30, "IDX0");             // record 1: index record
  mput32(0x30 + 0x08, 0xffffffff);    // index discriminator
  mput32(0x30 + 0x0c, 7);             // index value
  mput32(0x30 + 0x10, 0);
  mput32(0x30 + 0x14, 0);
  mput16(0x48, 2);                    // payload header: u16 @+0 (n)
  mput16(0x4c, 64);                   // u16 @+4 (fieldA)
  mput16(0x4e, 32);                   // u16 @+6 (fieldB)
  mputName(sizeof(mti) - 12, "TEST.MTI");

  const auto mdir = mdk::inspectMtiDirectory(
      std::span<const std::byte>(mti, sizeof(mti)));
  ok = mdir.status == mdk::MtiDirectoryStatus::kOk &&
       mdir.count == 2 && mdir.entries.size() == 2 &&
       mdir.entries[0].name() == "MAT0" &&
       !mdir.entries[0].isIndexRecord() &&
       mdir.entries[0].hasExtendedHeader() &&
       mdir.entries[0].payloadFileOffset() == 0x48 &&
       mdir.entries[0].headerCount == 2 &&
       mdir.entries[0].headerFieldA == 64 &&
       mdir.entries[0].headerFieldB == 32 &&
       mdir.entries[0].payloadDataFileOffset == 0x50 &&
       mdir.entries[1].isIndexRecord() &&
       mdir.entries[1].fieldAt0x0C == 7 &&
       mdir.trailerPresent && mdir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest mti-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic MTO-like fixture (no original data): tagged envelope +
  // count=1 + one 12-byte record {name[8]="OV1", blockOff=0x24} + one
  // overlay block + name trailer. File size 0xb8.
  //
  // Block @0x24, len=0x88 → [0x24, 0xac):
  //   +0x00 len=0x88, +0x04 ofsA=0x4c, +0x08 ofsB=0x5c, +0x0c ofsC=0x70
  //   embedded file @0x34 (innerSize=0x40 → innerEnd=0x74):
  //     innerLen=0x3c, name "OV1.MAT"@0x38, sec=0x34@0x44, count=1@0x48,
  //     rec @0x4c {"TEX", 0,0,0, imgOff=0x2c}, payload @0x64 {64,32},
  //     trailer "OV1.MAT"@0x68
  //   regionA: size=0x0c @0x74, struct {0,0,0} @0x78 → end 0x84
  //   regionB [0x84, 0x98) — off+4+0x5c = 0x84
  //   regionC @0x98 (off+4+0x70): c1..c4 = 0, u32, extra → 0xac = bend
  std::byte mto[0xb8] = {};
  const auto oput32 = [&](std::size_t off, std::uint32_t v) {
    mto[off + 0] = static_cast<std::byte>(v & 0xff);
    mto[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    mto[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    mto[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto oput16 = [&](std::size_t off, std::uint16_t v) {
    mto[off + 0] = static_cast<std::byte>(v & 0xff);
    mto[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
  };
  const auto oputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(mto); ++i) {
      mto[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  oput32(0x00, sizeof(mto) - 4);
  oputName(0x04, "TEST.MAT");
  oput32(0x10, sizeof(mto) - 12);
  oput32(0x14, 1);                // overlay count
  oputName(0x18, "OV1");          // table-1 record
  oput32(0x20, 0x24);             // block file offset
  oput32(0x24, 0x88);             // blockLength → bend 0xac
  oput32(0x28, 0x4c);             // ofsA → 0x24+8+0x4c = 0x78
  oput32(0x2c, 0x5c);             // ofsB → 0x24+4+0x5c = 0x84
  oput32(0x30, 0x70);             // ofsC → 0x24+4+0x70 = 0x98
  oput32(0x34, 0x3c);             // innerLen → innerSize 0x40
  oputName(0x38, "OV1.MAT");
  oput32(0x44, 0x34);             // inner secondary = innerSize-12
  oput32(0x48, 1);                // inner count
  oputName(0x4c, "TEX");          // inner record name[8]
  oput32(0x60, 0x2c);             // rec+0x14: img(0x38)+0x2c = 0x64
  oput16(0x64, 64);               // payload header {64, 32}
  oput16(0x66, 32);
  oputName(0x68, "OV1.MAT");      // inner trailer → innerEnd 0x74
  oput32(0x74, 0x0c);             // regionA size (self-exclusive)
  // regionA struct @0x78: ca=cb=cc=0 (zeros)
  // regionB [0x84,0x98) zeros; regionC @0x98: c1..c4=0 + u32 (zeros)
  oputName(sizeof(mto) - 12, "TEST.MAT");

  const auto odir = mdk::inspectMtoDirectory(
      std::span<const std::byte>(mto, sizeof(mto)));
  ok = odir.status == mdk::MtoDirectoryStatus::kOk &&
       odir.count == 1 && odir.entries.size() == 1 &&
       odir.entries[0].name() == "OV1" &&
       odir.entries[0].blockFileOffset == 0x24 &&
       odir.blocks.size() == 1 &&
       odir.blocks[0].blockLength == 0x88 &&
       odir.blocks[0].innerName() == "OV1.MAT" &&
       odir.blocks[0].innerCount == 1 &&
       odir.blocks[0].innerRecords.size() == 1 &&
       odir.blocks[0].innerRecords[0].name() == "TEX" &&
       odir.blocks[0].innerRecords[0].headerFieldA == 64 &&
       odir.blocks[0].innerRecords[0].headerFieldB == 32 &&
       odir.blocks[0].innerRecords[0].payloadDataFileOffset == 0x68 &&
       odir.blocks[0].innerTrailerPresent &&
       odir.blocks[0].innerSecondaryEqualsTrailerOffset &&
       odir.blocks[0].regionASize == 0x0c &&
       odir.blocks[0].regionACountA == 0 &&
       odir.blocks[0].regionBOffset == 0x84 &&
       odir.blocks[0].regionBSize == 0x14 &&
       odir.blocks[0].regionCOffset == 0x98 &&
       odir.blocks[0].regionCCount1 == 0 &&
       odir.blocks[0].regionCExtraOffset == 0xac &&
       odir.trailerPresent && odir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest mto-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic CMI-like fixture (no original data): tagged envelope +
  // the four counted variable-length tables + a small data region +
  // name trailer. Table 0 uses the zero-count variant (OBSERVED in
  // LEVEL4/5/7); records are {u8 len, name[len] incl NUL, u32 value}
  // with values that are image-relative offsets (image = file+4).
  //
  //   0x14 T0 count=0 → T1 count @0x18
  //   T1: {3,"E1\0",v} T2: {6,"FIRST\0",v}{7,"SECOND\0",0}
  //   T3: {2,"L\0",v} → data region [0x4a, 0x74)
  // Built byte-exact below; every nonzero value points into the data
  // region [dataStart, size-12).
  std::byte cmi[0x80] = {};
  const auto cput32 = [&](std::size_t off, std::uint32_t v) {
    cmi[off + 0] = static_cast<std::byte>(v & 0xff);
    cmi[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    cmi[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    cmi[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto cputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(cmi); ++i) {
      cmi[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  // Record writer: {u8 len, bytes, u32 value}; len includes the NUL.
  const auto cputRec = [&](std::size_t off, const char* n,
                           std::uint32_t v) {
    const std::size_t len = std::strlen(n) + 1;  // counted incl NUL
    cmi[off] = static_cast<std::byte>(len);
    for (std::size_t i = 0; i < len; ++i) {
      cmi[off + 1 + i] = static_cast<std::byte>(n[i]);  // copies NUL
    }
    cput32(off + 1 + len, v);
    return off + 1 + len + 4;
  };
  cput32(0x00, sizeof(cmi) - 4);
  cputName(0x04, "TEST.CMD");
  cput32(0x10, sizeof(cmi) - 12);
  cput32(0x14, 0);                          // T0: zero count
  std::size_t q = 0x18;
  cput32(q, 1); q += 4;                     // T1: one record
  q = cputRec(q, "E1", 0x50 - 4);           // value -> file 0x50
  cput32(q, 2); q += 4;                     // T2: two records
  q = cputRec(q, "FIRST", 0x50 - 4);
  q = cputRec(q, "SECOND", 0);              // null value (T1 form)
  cput32(q, 1); q += 4;                     // T3: one record
  q = cputRec(q, "L", 0x60 - 4);            // value -> file 0x60
  // q == 0x4a: data region [0x4a, 0x74). Two length-prefixed strings
  // then a u32 (the CODE-CORROBORATED T3-target head shape) at 0x60.
  cmi[0x60] = std::byte{4}; cputName(0x61, "AB"); // {len4 "AB\0?"}
  cmi[0x65] = std::byte{2}; cmi[0x66] = std::byte{'Z'};
  cmi[0x67] = std::byte{0};
  cput32(0x68, 0x6c - 4);                   // second-level offset
  cputName(sizeof(cmi) - 12, "TEST.CMD");   // trailer at 0x74

  const auto cdir = mdk::inspectCmiDirectory(
      std::span<const std::byte>(cmi, sizeof(cmi)));
  ok = cdir.status == mdk::CmiDirectoryStatus::kOk &&
       cdir.tables.size() == 4 &&
       cdir.tables[0].count == 0 && cdir.tables[0].records.empty() &&
       cdir.tables[0].endFileOffset == 0x18 &&
       cdir.tables[1].count == 1 &&
       cdir.tables[1].records[0].name() == "E1" &&
       cdir.tables[1].records[0].nameLength == 3 &&
       cdir.tables[1].records[0].nameEndsWithTerminator &&
       cdir.tables[1].records[0].value == 0x4c &&
       cdir.tables[1].records[0].valueFileOffset() ==
           std::optional<std::uint64_t>(0x50) &&
       cdir.tables[2].count == 2 &&
       cdir.tables[2].records[1].value == 0 &&
       !cdir.tables[2].records[1].valueFileOffset().has_value() &&
       cdir.tables[3].records[0].valueFileOffset() ==
           std::optional<std::uint64_t>(0x60) &&
       cdir.dataRegionOffset == q &&
       cdir.dataRegionEnd == sizeof(cmi) - 12 &&
       cdir.trailerPresent && cdir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest cmi-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic DTI-like fixture (no original data): tagged envelope +
  // the five-entry image-relative TOC + one of each section:
  //   s0  29 u32s (grid 8x4 -> plane (8+4)*4 = 48 bytes; altFillA=-1
  //       -> single plane)
  //   s1  count=1 {word0=1,key=0,f=(1.5,2.5,3.5,4.5)}
  //   s2  count=1 {"TST_1\0\0\0", imgOff->payload, f32} payload:
  //       count=2 -> type6 "connect" + type2 "HotGen" with name@0x18
  //   s3  count=0x10 + 768 palette bytes
  //   s4  48 grid bytes, then name trailer
  std::byte dti[0x460] = {};
  const auto dput32 = [&](std::size_t off, std::uint32_t v) {
    dti[off + 0] = static_cast<std::byte>(v & 0xff);
    dti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    dti[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    dti[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto dputf = [&](std::size_t off, float f) {
    dput32(off, std::bit_cast<std::uint32_t>(f));
  };
  const auto dputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(dti); ++i) {
      dti[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  // Layout (file offsets; TOC stores image offsets = file - 4):
  //   s0 0x28..0x9c  s1 0x9c..0xb8  s2 0xb8..0x118  s3 0x118..0x41c
  //   s4 0x41c..0x44c  trailer 0x44c..0x458
  const std::uint64_t dSize = 0x458;
  dput32(0x00, static_cast<std::uint32_t>(dSize - 4));
  dputName(0x04, "TEST.DAT");
  dput32(0x10, static_cast<std::uint32_t>(dSize - 12));
  dput32(0x14, 0x24);    // s0 @img 0x24 -> file 0x28
  dput32(0x18, 0x98);    // s1 -> file 0x9c
  dput32(0x1c, 0xb4);    // s2 -> file 0xb8
  dput32(0x20, 0x114);   // s3 -> file 0x118
  dput32(0x24, 0x418);   // s4 -> file 0x41c
  // s0 words: only the proven-consumed fields need sane values
  dputf(0x2c, -4.0f); dputf(0x30, 0.0f); dputf(0x34, 190.0f);
  dputf(0x38, 96.0f);
  dput32(0x3c, 0xf0); dput32(0x40, 0xe7);            // fill bytes
  dput32(0x44, 0xf0); dput32(0x48, 0x12c);           // scroll bases
  dput32(0x4c, 8); dput32(0x50, 4);                  // grid 8x4
  dput32(0x54, 0xffffffff); dput32(0x58, 0xffffffff);// altFillA/B
  // s1: count=1 {word0=1, key=0, f32 x4}
  dput32(0x9c, 1);
  dput32(0xa0, 1); dput32(0xa4, 0);
  dputf(0xa8, 1.5f); dputf(0xac, 2.5f);
  dputf(0xb0, 3.5f); dputf(0xb4, 4.5f);
  // s2: count=1, record {"TST_1\0\0\0", imgOff=0xc8->file 0xcc, 4.0f}
  dput32(0xb8, 1);
  dputName(0xbc, "TST_1");
  dput32(0xc4, 0xc8);          // image offset -> file 0xcc
  dputf(0xc8, 4.0f);
  // payload @0xcc: count=2; sub[0] type6 connect; sub[1] type2 HotGen
  dput32(0xcc, 2);
  dput32(0xd0, 6); dput32(0xd4, 1000); dput32(0xd8, 1);
  dputf(0xdc, 1.0f); dputf(0xe0, 2.0f); dputf(0xe4, 3.0f);
  dputf(0xe8, 4.0f); dputf(0xec, 5.0f); dputf(0xf0, 6.0f);
  dput32(0xf4, 2); dput32(0xf8, 9); dput32(0xfc, 0);
  dputf(0x100, -1.0f); dputf(0x104, -2.0f); dputf(0x108, -3.0f);
  dputName(0x10c, "XGS");      // name @+0x18 of sub[1]
  // s3 @0x118: count + 768-byte palette
  dput32(0x118, 0x10);
  // s4 @0x41c: 48 bytes (grid (8+4)*4)
  dputName(dSize - 12, "TEST.DAT");

  const auto dst = mdk::inspectDtiStructure(
      std::span<const std::byte>(dti, dSize));
  ok = dst.status == mdk::DtiStructureStatus::kOk &&
       dst.tocImageOffsets[0] == 0x24 &&
       dst.tocImageOffsets[4] == 0x418 &&
       dst.sections[0].fileStart == 0x28 &&
       dst.sections[0].fileEnd == 0x9c &&
       dst.sections[2].fileStart == 0xb8 &&
       dst.sections[2].fileEnd == 0x118 &&
       dst.params[9] == 8 && dst.params[10] == 4 &&
       dst.keyedRecords.size() == 1 &&
       dst.keyedRecords[0].key == 0 &&
       dst.keyedRecords[0].floatAt(0) == 1.5f &&
       dst.arenas.size() == 1 &&
       dst.arenas[0].name() == "TST_1" &&
       dst.arenas[0].nameEndsWithTerminator &&
       dst.arenas[0].payloadFileOffset == 0xcc &&
       dst.arenas[0].scalar() == 4.0f &&
       dst.arenas[0].subRecords.size() == 2 &&
       dst.arenas[0].subRecords[0].type == 6 &&
       dst.arenas[0].subRecords[0].fields[0] == 1000 &&
       dst.arenas[0].subRecords[0].fieldAsFloat(2) == 1.0f &&
       dst.arenas[0].subRecords[1].type == 2 &&
       dst.arenas[0].subRecords[1].name18() == "XGS" &&
       dst.s2PayloadRegionStart == 0xcc &&
       dst.paletteCount == 0x10 &&
       dst.paletteBytes.size() == 768 &&
       dst.gridPlaneSize == 48 && dst.gridPlaneCount == 1 &&
       dst.sections[4].fileEnd == dSize - 12 &&
       dst.trailerPresent && dst.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest dti-structure: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic FTI-like fixture (no original data): length-only
  // envelope + count=2 + two 12-byte records {name[8], imgOff}. One
  // name fills all 8 bytes (no NUL — OBSERVED legal form).
  //   0x00 len=size-4, 0x04 count=2, 0x08..0x20 records,
  //   payloads 0x20..0x38.
  std::byte fti[0x38] = {};
  const auto fput32 = [&](std::size_t off, std::uint32_t v) {
    fti[off + 0] = static_cast<std::byte>(v & 0xff);
    fti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    fti[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    fti[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto fputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(fti); ++i) {
      fti[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  fput32(0x00, sizeof(fti) - 4);
  fput32(0x04, 2);
  fputName(0x08, "EIGHTCHR");        // fills name[8] — no NUL
  fput32(0x10, 0x20 - 4);            // imgOff -> file 0x20
  fputName(0x14, "TWO");
  fput32(0x1c, 0x30 - 4);            // imgOff -> file 0x30

  const auto fdir = mdk::inspectFtiDirectory(
      std::span<const std::byte>(fti, sizeof(fti)));
  ok = fdir.status == mdk::FtiDirectoryStatus::kOk &&
       fdir.count == 2 && fdir.records.size() == 2 &&
       fdir.directoryEnd == 0x20 &&
       fdir.records[0].name() == "EIGHTCHR" &&
       !fdir.records[0].nameHasTerminator &&
       fdir.records[0].payloadFileOffset == 0x20 &&
       fdir.records[0].payloadEnd == 0x30 &&
       fdir.records[1].name() == "TWO" &&
       fdir.records[1].nameHasTerminator &&
       fdir.records[1].payloadFileOffset == 0x30 &&
       fdir.records[1].payloadEnd == sizeof(fti) &&
       fdir.offsetsSortedAscending && fdir.offsetsUnique &&
       fdir.firstPayloadAtDirectoryEnd;
  std::fprintf(stderr, "selftest fti-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic BNI-like fixture (no original data): length-only
  // envelope + count=2 + two 16-byte records {name[12], imgOff};
  // one 9-char name (OBSERVED legal form: "BONESANIM").
  //   0x00 len, 0x04 count=2, 0x08..0x28 records, payloads 0x28..0x40.
  std::byte bni[0x40] = {};
  const auto bput32 = [&](std::size_t off, std::uint32_t v) {
    bni[off + 0] = static_cast<std::byte>(v & 0xff);
    bni[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    bni[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    bni[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto bputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(bni); ++i) {
      bni[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  bput32(0x00, sizeof(bni) - 4);
  bput32(0x04, 2);
  bputName(0x08, "BONESANIM");       // 9 chars in name[12]
  bput32(0x08 + 0x0c, 0x28 - 4);     // imgOff -> file 0x28
  bputName(0x18, "RES2");
  bput32(0x18 + 0x0c, 0x38 - 4);     // imgOff -> file 0x38

  const auto bdir = mdk::inspectBniDirectory(
      std::span<const std::byte>(bni, sizeof(bni)));
  ok = bdir.status == mdk::BniDirectoryStatus::kOk &&
       bdir.count == 2 && bdir.records.size() == 2 &&
       bdir.directoryEnd == 0x28 &&
       bdir.records[0].name() == "BONESANIM" &&
       bdir.records[0].nameHasTerminator &&
       bdir.records[0].payloadFileOffset == 0x28 &&
       bdir.records[0].payloadEnd == 0x38 &&
       bdir.records[1].name() == "RES2" &&
       bdir.records[1].payloadFileOffset == 0x38 &&
       bdir.records[1].payloadEnd == sizeof(bni) &&
       bdir.offsetsSortedAscending && bdir.offsetsUnique &&
       bdir.firstPayloadAtDirectoryEnd;
  std::fprintf(stderr, "selftest bni-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic BNI image checks (no original data): record lookup by
  // name plus the Phase 4A paletted-bitmap decode
  // {rgb[768], u16 w, u16 h, px[w*h]}.
  ok = mdk::findBniRecord(bdir, "bonesanim") == &bdir.records[0] &&
       mdk::findBniRecord(bdir, "RES2") == &bdir.records[1] &&
       mdk::findBniRecord(bdir, "MISSING") == nullptr;
  std::fprintf(stderr, "selftest bni-record-lookup: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  std::vector<std::byte> pal(772 + 6);  // 2x3 paletted image
  for (int i = 0; i < 256; ++i) {
    pal[i * 3 + 0] = static_cast<std::byte>(i);
    pal[i * 3 + 1] = static_cast<std::byte>(255 - i);
    pal[i * 3 + 2] = static_cast<std::byte>(i);
  }
  pal[768] = std::byte{2};
  pal[770] = std::byte{3};
  for (int i = 0; i < 6; ++i) {
    pal[772 + i] = static_cast<std::byte>(i + 40);
  }
  const auto probe = mdk::probeBniImage(pal);
  std::string derr;
  const auto img = mdk::decodeBniPalettedImage(pal, &derr);
  ok = probe.shape == mdk::BniImageShape::kPaletted &&
       img && img->width == 2 && img->height == 3 &&
       img->stride == 2 && img->pixels.size() == 6 &&
       img->pixels[5] == 45 && img->hasPalette &&
       img->palette[7].r == 7 && img->palette[7].g == 248;
  std::fprintf(stderr, "selftest bni-paletted-image: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic indexed-only + composed-palette check (Phase 4B; no
  // original data): {u16 w, u16 h, px} decoded against a 768-byte
  // palette composed SYS_PAL-head + PAL-tail style.
  {
    std::vector<std::byte> idx(4 + 6);  // 3x2 indexed-only image
    idx[0] = std::byte{3};
    idx[2] = std::byte{2};
    for (int i = 0; i < 6; ++i) {
      idx[4 + i] = static_cast<std::byte>(i + 9);
    }
    std::vector<std::byte> sysPal(192), palRec(768);
    for (int i = 0; i < 192; ++i) {
      sysPal[i] = static_cast<std::byte>(i + 1);
    }
    for (int i = 0; i < 768; ++i) {
      palRec[i] = static_cast<std::byte>((i * 3) & 0xff);
    }
    const auto composed =
        mdk::composeStreamPalette(sysPal, palRec, &derr);
    const auto img2 = composed
        ? mdk::decodeBniIndexedImage(idx, *composed, &derr)
        : std::nullopt;
    ok = composed && img2 && img2->width == 3 && img2->height == 2 &&
         img2->pixels.size() == 6 && img2->pixels[5] == 14 &&
         img2->hasPalette &&
         img2->palette[0].r == 1 &&          // SYS_PAL head
         img2->palette[64].r ==              // PAL tail @+0xc0
             static_cast<std::uint8_t>((0xc0 * 3) & 0xff);
    std::fprintf(stderr, "selftest bni-indexed-image: %s\n",
                 ok ? "PASS" : "FAIL");
  }
  if (!ok) {
    return 1;
  }

  // Synthetic FONTSML-layout font check (Phase 4C; no original
  // data): u32 offsetTable[256] then {s8 top, s8 bottom, u8 width,
  // u8 px[w*(top+bottom+1)]} glyph records.
  {
    std::vector<std::byte> fontBuf(0x400, std::byte{0});
    // 'A' (0x41) -> 2x2 glyph at 0x400; '!' (0x21) unmapped.
    fontBuf[0x41 * 4] = std::byte{0x00};
    fontBuf[0x41 * 4 + 1] = std::byte{0x04};  // offset 0x400
    fontBuf.push_back(std::byte{1});          // top = 1
    fontBuf.push_back(std::byte{0});          // bottom = 0
    fontBuf.push_back(std::byte{2});          // width = 2
    fontBuf.push_back(std::byte{9});
    fontBuf.push_back(std::byte{0});
    fontBuf.push_back(std::byte{0});
    fontBuf.push_back(std::byte{10});
    const auto font = mdk::decodeFtiFont(fontBuf, &derr);
    const auto* g = font ? font->glyphFor(0x41) : nullptr;
    ok = font && font->mappedCount == 1 && g && g->rows() == 2 &&
         g->pixels.size() == 4 && g->pixels[0] == 9 &&
         g->pixels[3] == 10 && !font->glyphFor(0x21) &&
         mdk::ftiFontDigest(*font) != 0 &&
         !mdk::decodeFtiFont(
              std::span<const std::byte>(fontBuf.data(), 0x100), &derr)
              .has_value();  // truncated table rejected
    std::fprintf(stderr, "selftest fti-font: %s\n",
                 ok ? "PASS" : "FAIL");
  }
  return ok ? 0 : 1;
}

// Phase 5F — synthetic end-to-end surface-contact selftest. Drives the
// real seams (not the original VM): collisionApply -> surfaceContactHook
// -> surfaceDispatch (FUN_0040b5d0), surfaceConveyorDelta (FUN_00412ef0)
// -> integratePlayerMotion, slideZoneTrigger (opcode 0xe0) ->
// applyPlayerVerticalCollision, and surfaceApplyPending (FUN_0040b4dc).
// The route is contact ordinary floor -> no effect; contact a conveyor
// surface -> 5B displacement; contact a slide-zone -> 5C bounce flag;
// then the pending-flag re-arm. No --data-path required.
int selftestPlayerSurface() {
  bool ok = true;
  auto check = [&](bool c, const char* what) {
    if (!c) ok = false;
    std::fprintf(stderr, "  %-56s %s\n", what, c ? "ok" : "FAIL");
  };
  const float dt = 1.0f / 30.0f;

  // Flat floor at z=10 carrying surface byte 2 (the armed bit set).
  float verts[9] = {-50, -50, 10, 50, -50, 10, 50, 50, 10};
  mdk::CollisionPoly poly = {};
  poly.v[0] = 0;
  poly.v[1] = 1;
  poly.v[2] = 2;
  poly.surface = 2;   // surfId 2 -> dispatch slot index 1
  poly.flags = 0x10;  // the contact/re-arm bit, armed
  mdk::CollisionNode node = {};
  node.nz = 1.0f;
  node.d = -10.0f;
  node.childNear = -1;
  node.childFar = -1;
  node.polysPos = 1;  // count=1, firstIdx=0
  mdk::CollisionArena arena = {};
  arena.verts = verts;
  arena.polys = &poly;
  arena.nodes = &node;
  arena.deepFloorZ = -1000.0f;

  mdk::SurfaceObjectState ctx = {};
  ctx.polys = &poly;
  ctx.polyCount = 1;
  ctx.config[1] = 0x8 | 0x80;  // surfId 2: channel-8 + the 0x80 mark fx
  const float dir[3] = {1.0f, 0.0f, 0.0f};
  mdk::surfaceRecordCreate(ctx, 2, dir, 6.0f);   // conveyor +X, rate 6

  mdk::CollisionState cs;
  cs.arena = &arena;
  cs.arenaValid = 1;
  cs.queryEnabled = 1;
  cs.objectDataLoaded = 1;
  cs.surface = &ctx;
  cs.surfaceContextMask = 0x8;
  cs.contactHook = &mdk::surfaceContactHook;
  cs.pos[0] = 0.0f;
  cs.pos[1] = 0.0f;
  cs.pos[2] = 20.0f;

  // 1. Contact on the surface poly runs the dispatch: the 0x80 effect
  //    marks surfId 2 and clears the armed flag.
  const mdk::CollisionPoly* hit = mdk::collisionApply(
      cs, 0.0f, 0.0f, -15.0f, 0.5f, nullptr, nullptr);
  check(hit == &poly, "sweep contacted the surface floor");
  check((ctx.marks & (1u << 2)) != 0, "0x80 effect set the surf-2 mark");
  check((poly.flags & 0x10) == 0, "0x80 effect cleared the armed flag");

  // 2. Conveyor: standing on the surface poly yields dir*rate*dt, which
  //    the 5B integrator carries into the displacement while grounded.
  float conv[3] = {0, 0, 0};
  mdk::surfaceConveyorDelta(ctx, hit, dt, conv);
  check(std::fabs(conv[0] - 6.0f * dt) < 1e-5f && conv[1] == 0.0f &&
            conv[2] == 0.0f,
        "conveyor delta = dir*rate*dt");
  mdk::PlayerMotionEnvironment me = {};
  me.groundContact = true;
  me.conveyorX = conv[0];
  me.conveyorY = conv[1];
  me.conveyorZ = conv[2];
  mdk::PlayerMotionState ms = {};
  const mdk::PlayerMotionOutput mo =
      mdk::integratePlayerMotion(mdk::GameplayInputFrame{}, me, ms);
  check(std::fabs(mo.dispX - conv[0]) < 1e-5f,
        "conveyor reached the 5B displacement");

  // 3. An ordinary (surface=0) poly produces no dispatch result.
  mdk::CollisionPoly plain = {};
  plain.v[0] = 0;
  plain.v[1] = 1;
  plain.v[2] = 2;
  plain.surface = 0;
  plain.flags = 0x10;
  mdk::SurfaceFxState fx;
  const std::uint8_t r0 = mdk::surfaceDispatch(
      ctx, 0, 0x8, &plain, -0xb, cs.pos, cs.pos, cs.entryPos, fx,
      nullptr, nullptr);
  check(r0 == 0, "surface=0 poly -> no dispatch effect");

  // 4. A type-9 slide-zone over the floor sets the bounce flag on a
  //    grounded contact; fed into the 5C seam it suppresses the
  //    hard-landing event and clears on the post-step.
  mdk::DtiSubRecord z9 = {};
  z9.type = 9;
  auto putf = [&](int i, float v) {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    z9.fields[i] = u;
  };
  putf(3, -50);
  putf(4, -50);
  putf(5, 0);
  putf(6, 50);
  putf(7, 50);
  putf(8, 15);
  const float pos[3] = {0, 0, 10};
  mdk::SlideZoneResult zr = mdk::slideZoneTrigger(
      &z9, 1, 1, pos, false, true, 0.0f, 0.0f, dt);
  check(zr.inside && zr.setBounceFlag && zr.slideRedirect,
        "slide-zone inside + grounded -> bounce flag + redirect");
  mdk::VerticalCollisionResult res = {};
  res.bounce = zr.setBounceFlag;
  res.contactObj = 1;
  res.hasFloor = true;
  res.floorZ = 10.0f;
  res.posZ = 10.0f;
  mdk::PlayerVerticalState vs = {};
  mdk::PlayerMotionState vms = {};
  mdk::PlayerVerticalEnvironment ve = {};
  ve.frameStep = 1;
  ve.deltaSeconds = dt;
  ve.deepFloorZ = -1000.0f;
  vs.vertVel = -60.0f;  // a hard impact
  mdk::PlayerVerticalFrame vf;
  mdk::applyPlayerVerticalCollision(ve, vms, vs, res, vf);
  check(vs.bounceFlag == 1 && !vf.hardLanding,
        "bounce flag suppressed the hard-landing event");
  mdk::playerVerticalPostStep(ve, vs);
  check(vs.bounceFlag == 0, "post-step cleared the bounce flag");

  // 5. The pending pass re-arms the flag and clears the mark.
  mdk::surfaceApplyPending(ctx, 0);
  check((poly.flags & 0x10) != 0 && ctx.marks == 0,
        "mode-0 re-armed the flag and cleared the mark");

  mdk::surfaceRecordsDestroy(ctx);
  std::fprintf(stderr, "selftest player-surface: %s\n",
               ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  std::optional<std::string> dataPath;
  std::optional<std::string> target;
  std::optional<std::string> visualInfoName;
  std::optional<std::string> fontInfoName;
  std::optional<std::string> spriteInfoName;
  std::optional<unsigned> fontInfoCode;
  bool entriesMode = false;
  bool collisionProbe = false;
  bool arenaObjects = false;
  bool surfaceCensus = false;
  bool traversalRuntime = false;
  bool scriptDisasm = false;
  std::string scriptDisasmName;
  std::optional<std::string> travArena;
  float travStart[3] = {0.0f, 0.0f, 0.0f};
  bool travStartGiven = false;
  float travYaw = 0.0f;
  bool travYawGiven = false;
  int travFrames = 90;
  float probePos[3] = {0.0f, 0.0f, 0.0f};
  int probePosGiven = 0;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (!std::strcmp(a, "--data-path")) {
      const char* v = value(a);
      if (!v) return usage();
      dataPath = v;
    } else if (!std::strcmp(a, "--container")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
    } else if (!std::strcmp(a, "--entries")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      entriesMode = true;
    } else if (!std::strcmp(a, "--visual-info")) {
      const char* v = value(a);
      if (!v) return usage();
      const char* n = value(a);
      if (!n) return usage();
      target = v;
      visualInfoName = n;
    } else if (!std::strcmp(a, "--font-info")) {
      const char* v = value(a);
      if (!v) return usage();
      const char* n = value(a);
      if (!n) return usage();
      target = v;
      fontInfoName = n;
      // Optional glyph code: decimal or 0xNN.
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        const char* c = argv[++i];
        char* endp = nullptr;
        const unsigned long cv = std::strtoul(c, &endp, 0);
        if (!endp || *endp != '\0' || cv > 0xff) {
          std::fprintf(stderr, "invalid glyph code: %s (want 0-255)\n",
                       c);
          return usage();
        }
        fontInfoCode = static_cast<unsigned>(cv);
      }
    } else if (!std::strcmp(a, "--sprite-info")) {
      const char* v = value(a);
      if (!v) return usage();
      const char* n = value(a);
      if (!n) return usage();
      target = v;
      spriteInfoName = n;
    } else if (!std::strcmp(a, "--collision-probe")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      collisionProbe = true;
      // Optional probe position (defaults to the vert-bounds centre).
      // A leading '-' followed by a digit is a negative coordinate,
      // not a flag.
      for (int k = 0; k < 3; ++k) {
        if (i + 1 < argc &&
            (argv[i + 1][0] != '-' ||
             (argv[i + 1][1] >= '0' && argv[i + 1][1] <= '9'))) {
          const char* c = argv[++i];
          char* endp = nullptr;
          const double dv = std::strtod(c, &endp);
          if (!endp || *endp != '\0') {
            std::fprintf(stderr, "invalid probe coordinate: %s\n", c);
            return usage();
          }
          probePos[k] = static_cast<float>(dv);
          probePosGiven |= 1 << k;
        }
      }
      if (probePosGiven != 0 && probePosGiven != 7) {
        std::fprintf(stderr, "--collision-probe needs either no "
                             "position or all of <x> <y> <z>\n");
        return usage();
      }
    } else if (!std::strcmp(a, "--arena-objects")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      arenaObjects = true;
    } else if (!std::strcmp(a, "--surface-census")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      surfaceCensus = true;
    } else if (!std::strcmp(a, "--traversal-runtime")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      traversalRuntime = true;
    } else if (!std::strcmp(a, "--script-disasm")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;                 // .CMI path
      scriptDisasm = true;
      const char* n = value(a);   // arena/script record name
      if (!n) return usage();
      scriptDisasmName = n;
    } else if (!std::strcmp(a, "--arena")) {
      const char* v = value(a);
      if (!v) return usage();
      travArena = v;
    } else if (!std::strcmp(a, "--start")) {
      for (int k = 0; k < 3; ++k) {
        const char* c = value(a);
        if (!c) return usage();
        char* endp = nullptr;
        travStart[k] = static_cast<float>(std::strtod(c, &endp));
        if (!endp || *endp != '\0') {
          std::fprintf(stderr, "invalid --start coordinate: %s\n", c);
          return usage();
        }
      }
      travStartGiven = true;
    } else if (!std::strcmp(a, "--yaw")) {
      const char* c = value(a);
      if (!c) return usage();
      char* endp = nullptr;
      travYaw = static_cast<float>(std::strtod(c, &endp));
      if (!endp || *endp != '\0') {
        std::fprintf(stderr, "invalid --yaw: %s\n", c);
        return usage();
      }
      travYawGiven = true;
    } else if (!std::strcmp(a, "--frames")) {
      const char* c = value(a);
      if (!c) return usage();
      char* endp = nullptr;
      const long fv = std::strtol(c, &endp, 10);
      if (!endp || *endp != '\0' || fv < 1 || fv > 100000) {
        std::fprintf(stderr, "invalid --frames: %s\n", c);
        return usage();
      }
      travFrames = static_cast<int>(fv);
    } else if (!std::strcmp(a, "--selftest")) {
      return selftest();
    } else if (!std::strcmp(a, "--selftest-player-surface")) {
      return selftestPlayerSurface();
    } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
      return usage();
    } else if (a[0] == '-') {
      std::fprintf(stderr, "unknown argument: %s\n", a);
      return usage();
    } else {
      target = a;
    }
  }

  if (!dataPath || !target) {
    return usage();
  }

  std::string err;
  const auto root = mdk::DataRoot::open(*dataPath, &err);
  if (!root) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 2;
  }

  std::printf("request:   %s\n", target->c_str());
  const auto resolved = root->resolve(*target, &err);
  if (!resolved) {
    std::fprintf(stderr, "resolve:   FAILED (%s)\n", err.c_str());
    return 1;
  }
  std::printf("resolved:  %s\n", resolved->string().c_str());

  const auto size = root->fileSize(*target, &err);
  if (!size) {
    std::fprintf(stderr, "stat:      FAILED (%s)\n", err.c_str());
    return 1;
  }
  std::printf("size:      %llu bytes\n",
              static_cast<unsigned long long>(*size));

  const auto head = root->readPrefix(*target, kInspectHeadBytes, &err);
  if (!head) {
    std::fprintf(stderr, "read:      FAILED (%s)\n", err.c_str());
    return 1;
  }

  const auto family = mdk::fileFamilyForPath(*target);
  const auto support = mdk::fileFamilySupport(family);
  std::printf("family:    %s\n", std::string(mdk::fileFamilyName(family)).c_str());
  std::printf("envelope:  %s (by extension)\n",
              std::string(mdk::parserFamilyName(
                            mdk::parserFamilyForPath(*target))
                          )
                  .c_str());
  std::printf("support:   %s\n",
              std::string(mdk::familySupportName(support)).c_str());

  const auto info = mdk::inspectContainer(
      std::span<const std::byte>(head->data(), head->size()), *size);
  if (info.hasDeclaredLength) {
    std::printf("u32@0:     %u (0x%08x) — %s size-4\n", info.declaredLength,
                info.declaredLength,
                info.lengthValid ? "equals" : "DOES NOT equal");
  } else {
    std::printf("u32@0:     (file too small)\n");
  }

  switch (info.shape) {
    case mdk::ContainerShape::kNone:
      std::printf("envelope:  none (not this container format)\n");
      break;
    case mdk::ContainerShape::kLengthEnvelope:
      std::printf("envelope:  length-only (u32 valid; no tag/name "
                  "field)\n");
      break;
    case mdk::ContainerShape::kTaggedName:
      std::printf("envelope:  tagged-name\n");
      break;
  }

  if (info.hasTag) {
    std::printf("tag@4:     %s\n", mdk::tagToString(info.tag).c_str());
  }
  if (info.shape == mdk::ContainerShape::kTaggedName) {
    std::printf("name:      %s\n", info.logicalName.c_str());
    const std::string stem = stemOf(*target);
    if (!stem.empty()) {
      std::printf("stem:      %s — %s\n", stem.c_str(),
                  mdk::nameStemMatches(info, stem)
                      ? "match (case-insensitive)"
                      : "MISMATCH");
    }
    // Second observed u32 (offset 16): reported raw, not interpreted.
    mdk::BinaryReader r2(
        std::span<const std::byte>(head->data(), head->size()));
    if (r2.seek(16)) {
      if (const auto d2 = r2.u32le()) {
        std::printf("u32@16:    %u (0x%08x) — %s size-12 "
                    "(interior field; semantics not decoded)\n",
                    *d2, *d2,
                    *size >= 12 &&
                            static_cast<std::uint64_t>(*d2) == *size - 12
                        ? "equals"
                        : "does not equal");
      }
    }
  }

  if (!entriesMode && !visualInfoName && !fontInfoName &&
      !spriteInfoName && !collisionProbe && !arenaObjects &&
      !surfaceCensus && !traversalRuntime && !scriptDisasm) {
    return 0;
  }

  // --arena-objects: Phase 5E BUILD_A smoke. Loads the .DTI target
  // plus the sibling .CMI and <stem>O.MTO, builds the CMI enemy
  // table, resolves each arena's HotGen/HotPick records, spawns the
  // runtime objects (FUN_00456808 semantics), and reports the +0x68
  // list per arena — geometry parse, deep copy, transform rebuild and
  // list attach all exercised on original data.
  if (arenaObjects) {
    // Derive sibling paths: <dir>/<stem>.CMI and <dir>/<stem>O.MTO.
    const std::string dtiPath = *target;
    const auto slash = dtiPath.find_last_of("/\\");
    const auto dot = dtiPath.find_last_of('.');
    if (dot == std::string::npos) {
      std::fprintf(stderr, "--arena-objects wants a .DTI path\n");
      return 1;
    }
    const std::string dir =
        slash == std::string::npos ? "" : dtiPath.substr(0, slash + 1);
    const std::string stem = dtiPath.substr(
        slash == std::string::npos ? 0 : slash + 1,
        dot - (slash == std::string::npos ? 0 : slash + 1));
    const std::string cmiPath = dir + stem + ".CMI";
    const std::string mtoPath = dir + stem + "O.MTO";

    const auto dtiFile = root->readFile(dtiPath, kEntriesMaxBytes, &err);
    const auto cmiFile = root->readFile(cmiPath, kEntriesMaxBytes, &err);
    const auto mtoFile = root->readFile(mtoPath, kEntriesMaxBytes, &err);
    if (!dtiFile || !cmiFile || !mtoFile) {
      std::fprintf(stderr, "read-file: FAILED (%s) — need .DTI + "
                           "sibling .CMI + <stem>O.MTO\n",
                   err.c_str());
      return 1;
    }
    std::printf("siblings:  %s + %s\n", cmiPath.c_str(), mtoPath.c_str());

    const auto dti = mdk::inspectDtiStructure(
        std::span<const std::byte>(dtiFile->data(), dtiFile->size()));
    const auto cmi = mdk::inspectCmiDirectory(
        std::span<const std::byte>(cmiFile->data(), cmiFile->size()));
    const auto mto = mdk::inspectMtoDirectory(
        std::span<const std::byte>(mtoFile->data(), mtoFile->size()));
    if (dti.status != mdk::DtiStructureStatus::kOk ||
        cmi.status != mdk::CmiDirectoryStatus::kOk ||
        mto.status != mdk::MtoDirectoryStatus::kOk) {
      std::fprintf(stderr, "parse: FAILED (dti=%s cmi=%s mto=%s)\n",
                   std::string(mdk::dtiStructureStatusName(dti.status))
                       .c_str(),
                   std::string(mdk::cmiDirectoryStatusName(cmi.status))
                       .c_str(),
                   std::string(mdk::mtoDirectoryStatusName(mto.status))
                       .c_str());
      return 1;
    }

    const mdk::EnemyTable enemies = mdk::buildEnemyTable(cmi);
    std::printf("enemy-tbl: %zu entries (%zu unresolved -> MTO)\n",
                enemies.entries.size(),
                [&] {
                  std::size_t n = 0;
                  for (const auto& e : enemies.entries)
                    n += e.unresolved ? 1 : 0;
                  return n;
                }());

    // Lazily resolved model cache — mirrors FUN_004286c8's
    // deferred-geometry table.
    struct Cache {
      std::vector<std::optional<mdk::RuntimeModel>> models;
      std::vector<bool> tried;
      const mdk::EnemyTable* enemies;
      std::span<const std::byte> cmiFile;
      const mdk::CmiDirectory* cmiDir;
      const mdk::MtoDirectory* mtoDir;
      std::span<const std::byte> mtoFile;
      int resolved = 0;
      int failed = 0;
    } cache;
    cache.models.resize(enemies.entries.size());
    cache.tried.resize(enemies.entries.size(), false);
    cache.enemies = &enemies;
    cache.cmiFile = std::span<const std::byte>(cmiFile->data(),
                                             cmiFile->size());
    cache.cmiDir = &cmi;
    cache.mtoDir = &mto;
    cache.mtoFile = std::span<const std::byte>(mtoFile->data(),
                                              mtoFile->size());
    auto modelFor = [](int idx, void* ctx) -> const mdk::RuntimeModel* {
      auto* c = static_cast<Cache*>(ctx);
      if (idx < 0 || static_cast<std::size_t>(idx) >= c->models.size())
        return nullptr;
      const std::size_t i = static_cast<std::size_t>(idx);
      if (!c->tried[i]) {
        c->tried[i] = true;
        const auto span = mdk::enemyModelData(
            *c->enemies, idx, c->cmiFile, *c->cmiDir, c->mtoDir,
            c->mtoFile);
        if (span) {
          c->models[i] = mdk::parseGeometryRecord(
              span->data(), span->data() + span->size());
        }
        if (c->models[i]) {
          ++c->resolved;
        } else {
          ++c->failed;
        }
      }
      return c->models[i] ? &*c->models[i] : nullptr;
    };

    // Per arena: copy the record (the original rewrites fields in
    // place at load), resolve names, spawn, report.
    int totalSpawn = 0;
    for (const auto& arec : dti.arenas) {
      mdk::DtiArenaRecord work = arec;   // mutable copy for the rewrite
      const auto failed =
          mdk::resolveArenaRecordNames(work, enemies);
      int types[10] = {};
      for (const auto& sr : work.subRecords)
        if (sr.type < 10) ++types[sr.type];
      mdk::DynamicArena arena;
      arena.name = arec.name();
      std::vector<mdk::DynamicObject*> spawned;
      const int n = mdk::spawnArenaObjects(arena, work, modelFor, &cache,
                                          &spawned);
      totalSpawn += n;
      std::printf("arena %-8.8s  subs=%u t2=%d t4=%d t6=%d  spawn=%d",
                  arec.name().c_str(), arec.subRecordCount, types[2],
                  types[4], types[6], n);
      if (!failed.empty()) {
        std::printf("  unresolved-names=%zu", failed.size());
      }
      std::printf("\n");
      for (const auto* o : spawned) {
        std::printf("  obj idx=%-3u spawn=%-5u pos=(%g, %g, %g) "
                    "elems=%zd tris=%d aabb=(%g..%g, %g..%g, %g..%g)\n",
                    o->enemyIndex, o->spawnId, (double)o->pos[0],
                    (double)o->pos[1], (double)o->pos[2],
                    o->model.elems.size(),
                    o->model.elems.empty() ? 0 : o->model.elems[0].triCount,
                    (double)o->col.aabb[0], (double)o->col.aabb[3],
                    (double)o->col.aabb[1], (double)o->col.aabb[4],
                    (double)o->col.aabb[2], (double)o->col.aabb[5]);
      }
    }
    std::printf("total:     spawned=%d  models resolved=%d failed=%d\n",
                totalSpawn, cache.resolved, cache.failed);

    // Floor-probe smoke on the first spawned object: force the
    // standable bit (script-assigned in the original — opcode 0x29)
    // and probe straight down through the object's position.
    for (const auto& arec : dti.arenas) {
      mdk::DtiArenaRecord work = arec;
      mdk::resolveArenaRecordNames(work, enemies);
      mdk::DynamicArena arena;
      arena.name = arec.name();
      if (mdk::spawnArenaObjects(arena, work, modelFor, &cache,
                                 nullptr) == 0) {
        continue;
      }
      auto& o = *arena.storage.front();
      o.col.flags149 |= 1;
      mdk::CollisionArena ca = {};
      ca.objects = arena.col.objects;
      ca.deepFloorZ = -1000.0f;
      mdk::CollisionState cs;
      cs.arena = &ca;
      cs.queryEnabled = 1;
      cs.arenaValid = 1;
      cs.objectDataLoaded = 1;
      cs.pos[0] = o.pos[0];
      cs.pos[1] = o.pos[1];
      cs.pos[2] = o.pos[2] + 2.0f;
      mdk::collisionFloorProbe(cs);
      std::printf("floorprobe arena=%s obj@(%g,%g,%g): flags=0x%02x "
                  "floorZ=%g hit=%s\n",
                  arena.name.c_str(), (double)o.pos[0],
                  (double)o.pos[1], (double)o.pos[2], cs.contactFlags,
                  (double)cs.floorZ,
                  cs.floorObj == &o.col ? "spawned-object" : "none");
      break;
    }
    return 0;
  }

  // --traversal-runtime: Phase 5G BUILD_A smoke. Assembles the native
  // traversal runtime over the .DTI target's sibling triple (.DTI +
  // .CMI + <stem>O.MTO), resolves the s0 spawn, attaches the initial
  // arena (MTO region-C collision + spawn-once objects), and steps
  // the frame driver in the original FUN_00436100 order. Headless —
  // script-object calls, the scripted-move gate, the arena event
  // list, the world tick and camera/teleport blocks are counted as
  // seams, never emulated. --arena/--start/--yaw are NATIVE
  // DIAGNOSTIC OVERRIDES, not original spawn behavior.
  if (traversalRuntime) {
    const std::string dtiPath = *target;
    const auto slash = dtiPath.find_last_of("/\\");
    const auto dot = dtiPath.find_last_of('.');
    if (dot == std::string::npos) {
      std::fprintf(stderr, "--traversal-runtime wants a .DTI path\n");
      return 1;
    }
    const std::string dir =
        slash == std::string::npos ? "" : dtiPath.substr(0, slash + 1);
    const std::string stem = dtiPath.substr(
        slash == std::string::npos ? 0 : slash + 1,
        dot - (slash == std::string::npos ? 0 : slash + 1));
    const std::string cmiPath = dir + stem + ".CMI";
    const std::string mtoPath = dir + stem + "O.MTO";

    mdk::TraversalRuntime rt;
    const auto le = mdk::traversalRuntimeLoad(
        *root, dtiPath, cmiPath, mtoPath, rt, &err);
    if (le != mdk::TraversalLoadError::kOk) {
      std::fprintf(stderr, "traversal-load: FAILED (%s: %s)\n",
                   mdk::traversalLoadErrorName(le), err.c_str());
      return 1;
    }
    std::printf("level:     %s + %s + %s\n", dtiPath.c_str(),
                cmiPath.c_str(), mtoPath.c_str());
    std::printf("arenas:    %zu  enemy-tbl=%zu  models=%d ok/%d fail\n",
                rt.arenas.size(), rt.level.enemies.entries.size(),
                rt.level.modelsResolved, rt.level.modelsFailed);
    if (!rt.level.unresolvedNames.empty())
      std::printf("unresolved spawn names: %zu\n",
                  rt.level.unresolvedNames.size());
    std::printf("spawn(s0): arena=%d (%s) pos=(%g, %g, %g) yaw=%g\n",
                rt.cur->index, rt.cur->name.c_str(),
                (double)rt.cs.pos[0], (double)rt.cs.pos[1],
                (double)rt.cs.pos[2], (double)rt.motion.yawDeg);
    for (const auto& a : rt.arenas)
      std::printf(
          "  arena[%2d] %-9s geom=%d objs=%2zu subs=%u scal=%g %s\n",
          a->index, a->name.c_str(), a->geometryLoaded ? 1 : 0,
          a->dyn.storage.size(), a->rec->subRecordCount,
          (double)a->scalar,
          a->hasScriptObject ? "script-obj" : "");

    // NATIVE DIAGNOSTIC OVERRIDE — not original spawn behavior.
    if (travArena || travStartGiven || travYawGiven) {
      int arenaIdx = rt.cur->index;
      if (travArena) {
        arenaIdx = -1;
        for (const auto& a : rt.arenas)
          if (a->name == *travArena) arenaIdx = a->index;
        if (arenaIdx < 0) {
          char* endp = nullptr;
          const long v = std::strtol(travArena->c_str(), &endp, 10);
          if (endp && *endp == '\0') arenaIdx = static_cast<int>(v);
        }
      }
      float pos[3];
      for (int i = 0; i < 3; ++i)
        pos[i] = travStartGiven ? travStart[i] : rt.cs.pos[i];
      const float yaw = travYawGiven ? travYaw : rt.motion.yawDeg;
      const auto oe = mdk::traversalRuntimeDiagnosticStart(
          rt, arenaIdx, pos, yaw, &err);
      if (oe != mdk::TraversalLoadError::kOk) {
        std::fprintf(stderr, "diagnostic-start: FAILED (%s: %s)\n",
                     mdk::traversalLoadErrorName(oe), err.c_str());
        return 1;
      }
      std::printf("override:  arena=%d (%s) pos=(%g, %g, %g) yaw=%g "
                  "— NATIVE DIAGNOSTIC OVERRIDE\n",
                  rt.cur->index, rt.cur->name.c_str(), (double)pos[0],
                  (double)pos[1], (double)pos[2], (double)yaw);
    }

    // Scripted input: idle -> KeyUp (forward) -> idle -> KeyJump.
    // Key bindings are the factory defaults (KeyUp=103, KeyJump=56).
    const mdk::GameplayInputBindings bindings;
    auto rawFor = [](int phase) {
      mdk::RawGameplayInput r;
      if (phase == 1) r.keyLevel[103 >> 5] |= 1u << (103 & 31);
      if (phase == 3) r.keyLevel[56 >> 5] |= 1u << (56 & 31);
      return r;
    };
    mdk::FrontendTimingState timing; // f0=1, dt=1/30, step=1
    std::uint64_t digest = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        digest ^= (v >> (i * 8)) & 0xff;
        digest *= 1099511628211ull;
      }
    };
    const mdk::TraversalArena* startArena = rt.cur;
    int framesRun = 0;
    bool floorObjSeen = false;
    bool groundedSeen = false;
    bool airborneSeen = false;
    bool contactSeen = false;
    bool partnerSeen = false;
    for (int f = 0; f < travFrames; ++f) {
      const int phase = f < 10 ? 0 : f < 30 ? 1 : f < 35 ? 0 : f < 50 ? 3 : 0;
      const auto out =
          mdk::stepTraversalRuntime(rt, rawFor(phase), bindings, timing);
      ++framesRun;
      floorObjSeen |= (rt.cs.contactFlags & 2) != 0;
      groundedSeen |= out.grounded;
      airborneSeen |= !out.grounded;
      contactSeen |= out.contactObj != 0;
      partnerSeen |= out.partnerArenaIndex >= 0;
      std::printf(
          "f=%03d a=%d p=%d pos=(%8.2f,%8.2f,%8.2f) yaw=%6.1f "
          "mv=%5.2f sv=%5.2f vv=%6.2f gnd=%d ctc=%08x sld=%d ev=%d/%d\n",
          out.frame, out.curArenaIndex, out.partnerArenaIndex,
          (double)out.pos[0], (double)out.pos[1], (double)out.pos[2],
          (double)out.yawDeg, (double)out.moveVel,
          (double)out.strafeVel, (double)out.vertVel,
          out.grounded ? 1 : 0, out.contactObj, out.slideChannel,
          out.eventType, out.eventMag);
      // Connector (door) state dump — connState/anim/flags per frame.
      for (const auto& ap : rt.arenas)
        for (const auto& up : ap->dyn.storage) {
          const mdk::DynamicObject& o = *up;
          if (!(o.col.flags14a & 0x10)) continue;
          std::printf(
              "      door %s home=%s st=%02x anim=%c fr=%.2f cur=%d "
              "lat=%04x f148=%04x f14a=%02x r=%.1f done=%d\n",
              o.scriptClass.c_str(),
              o.arena && o.arena->owner ? o.arena->owner->name.c_str()
                                        : "?",
              o.connState, o.connAnim ? 'Y' : 'n',
              (double)o.connAnimFrame, (int)o.connAnimCurFrame,
              (unsigned)(std::uint16_t)o.connAnimLatch,
              (unsigned)o.col.flags148, (unsigned)o.col.flags14a,
              (double)o.connRadius, o.connAnimDone() ? 1 : 0);
        }
      // The digest mixes only deterministic state — raw contact
      // tokens are process addresses and are mixed as booleans.
      mix(static_cast<std::uint64_t>(out.frame));
      mix(static_cast<std::uint64_t>(out.curArenaIndex));
      mix(static_cast<std::uint64_t>(
          out.partnerArenaIndex < 0 ? 0xffff : out.partnerArenaIndex));
      for (int i = 0; i < 3; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, &out.pos[i], 4);
        mix(bits);
      }
      std::uint32_t bits;
      std::memcpy(&bits, &out.yawDeg, 4);
      mix(bits);
      std::memcpy(&bits, &out.vertVel, 4);
      mix(bits);
      mix(out.contactObj != 0 ? 1 : 0);
      mix(out.grounded ? 1 : 0);
      mix(static_cast<std::uint64_t>(out.locoState));
      mix(static_cast<std::uint64_t>(out.slideChannel));
      mix(out.currentArenaSwapped ? 1 : 0);
      // Phase 5H — fold tr_alcmd VM derived state into the digest:
      // current-arena persisted PC/wait, cumulative instruction +
      // spawn counters, and a surface-state summary. No script bytes.
      if (rt.cur) {
        mix(static_cast<std::uint64_t>(rt.cur->script.pcImageOff));
        std::uint32_t wbits;
        std::memcpy(&wbits, &rt.cur->script.waitSeconds, 4);
        mix(wbits);
        mix(static_cast<std::uint64_t>(rt.cur->script.callDepth));
        mix(static_cast<std::uint64_t>(rt.cur->surface.opMaskA));
        mix(static_cast<std::uint64_t>(rt.cur->surface.opMaskB));
        for (int i = 0; i < mdk::kSurfaceSlots; ++i)
          mix(static_cast<std::uint64_t>(rt.cur->surface.handlerOff[i]));
      }
      mix(static_cast<std::uint64_t>(rt.scriptInsnTotal));
      mix(static_cast<std::uint64_t>(rt.scriptSpawned));
      mix(static_cast<std::uint64_t>(rt.arenas.empty()
                                        ? 0 : rt.arenas.size()));
    }
    const auto& s = rt.seams;
    std::printf("digest:    %016llx  (%d frames)\n",
                (unsigned long long)digest, framesRun);
    std::printf("seams:     stream=%d prepass=%d scriptObj=%d "
                "move=%d evlist=%d slide=%d mantle=%d timers=%d "
                "world=%d xworld=%d prof=%d tail=%d teleport=%d "
                "vsnap=%d migrations=%d t1=%d t3=%d portals=%d "
                "deep=%d\n",
                s.streamStageCalls, s.objectPrepass,
                s.scriptObjectCalls, s.scriptedMoveCalls,
                s.arenaEventListCalls, s.slideHelperCalls,
                s.mantleCalls, s.timersCalls, s.worldTickCalls,
                s.extraWorldTickCalls, s.profilerHooks,
                s.postTailCalls, s.teleportCalls,
                s.pendingViewSnaps, s.objectMigrations,
                s.type1Triggers, s.type3Prefetches,
                s.portalsCrossed, s.deepFloorFallbacks);
    std::printf("script:    runs=%d insn=%d spawned=%d diag=%d\n",
                rt.scriptRuns, rt.scriptInsnTotal, rt.scriptSpawned,
                static_cast<int>(rt.scriptDiag.size()));
    for (const std::string& m : rt.scriptDiag)
      std::printf("           ! %s\n", m.c_str());
    // Bounded checks: gates + at least one grounded frame. Arenas
    // with their own MTO collision blob must also show a collision
    // contact. 'C*' corridor arenas carry no blob (OBSERVED: the MTO
    // has exactly the 10 HMO_* blocks) — their traversal is driven by
    // the per-arena CMI script VM (+0x220 -> FUN_004388d8, exposed as
    // the scriptObj seam) and the type-1/type-3/portal seams, so the
    // honest corridor check is that a partner attach fired; the
    // observed +0x44e failsafe catch (posZ <= -50) is reported but
    // not asserted — reaching it depends on start height / frames.
    // A crossed portal reclassifies the run: the start arena's
    // requirements are replaced by gates + post-swap partner state.
    // contactFlags bit1 is the OBSERVED *object*-floor flag
    // (collisionFloorProbe walks cs.arena->objects only) — reported
    // but not asserted: arenas without rideable objects never set it.
    const bool carrierGeom =
        rt.partner && rt.partner->geometryLoaded;
    bool ok;
    if (s.portalsCrossed > 0) {
      // A portal swap is itself the demonstrated traversal: the run
      // left the start arena through the proven type-6 path, so the
      // start arena's contact requirement no longer applies — the
      // honest assertions are valid gates plus the observed
      // post-swap ca4=dest partner slot.
      ok = rt.cs.arenaValid && partnerSeen;
    } else if (startArena->geometryLoaded) {
      ok = rt.cs.arenaValid && contactSeen && groundedSeen;
    } else {
      ok = rt.cs.arenaValid && partnerSeen &&
           (s.type1Triggers > 0 || s.type3Prefetches > 0);
    }
    std::printf("checks:    geom=%d gates=%d contact=%d grounded=%d "
                "airborne=%d floor-obj=%d partner=%d car-vld=%d "
                "car-bsy=%d car-geom=%d — %s\n",
                startArena->geometryLoaded ? 1 : 0, rt.cs.arenaValid,
                contactSeen ? 1 : 0, groundedSeen ? 1 : 0,
                airborneSeen ? 1 : 0, floorObjSeen ? 1 : 0,
                partnerSeen ? 1 : 0, rt.cs.carrierValid ? 1 : 0,
                rt.cs.carrierBusy ? 1 : 0, carrierGeom ? 1 : 0,
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
  }

  // --surface-census: Phase 5F BUILD_A smoke. Reads the .DTI target
  // plus the sibling <stem>O.MTO and censuses the surface-contact data
  // the dispatcher (FUN_0040b5d0) and the volume/slide-zone records
  // consume: the poly surface byte (+0x23) and flag bits (+0x20) across
  // each arena's region-C collision blob, and the type-7 (fan/volume) /
  // type-9 (slide-zone) DTI sub-records. Metadata only — no payloads.
  if (scriptDisasm) {
    // Phase 5H — decode one CMI table-3 arena script record
    // (FUN_00458550 lookup) and print its tr_alcmd instructions.
    const auto cmiFile = root->readFile(*target, kEntriesMaxBytes, &err);
    if (!cmiFile) {
      std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
      return 1;
    }
    std::span<const std::byte> img(
        reinterpret_cast<const std::byte*>(cmiFile->data()),
        cmiFile->size());
    const auto cmi = mdk::inspectCmiDirectory(img);
    if (cmi.status != mdk::CmiDirectoryStatus::kOk) {
      std::fprintf(stderr, "cmi: parse FAILED (%s)\n",
                   std::string(mdk::cmiDirectoryStatusName(cmi.status))
                       .c_str());
      return 1;
    }
    const std::uint32_t code =
        mdk::cmiScriptCodeOffset(cmi, img, scriptDisasmName);
    std::printf("cmi:   %s — %zu tables, %zu t3 records\n",
                target->c_str(), cmi.tables.size(),
                cmi.tables.size() > 3 ? cmi.tables[3].records.size()
                                      : 0);
    // "*" lists every table-3 record's resolved script offset.
    if (scriptDisasmName == "*") {
      if (cmi.tables.size() > 3)
        for (const auto& r : cmi.tables[3].records)
          std::printf("  t3 %-12s codeOff=0x%x\n", r.name().c_str(),
                      mdk::cmiScriptCodeOffset(cmi, img, r.name()));
      return 0;
    }
    std::printf("script: %s  codeOff=0x%x\n", scriptDisasmName.c_str(),
                code);
    if (code == 0) {
      std::printf("  (no script record / zero code offset — the "
                  "+0x220 gate stays cleared)\n");
      return 0;
    }
    const auto insns =
        mdk::traversalScriptDisasm(img, 4, code, 256);
    for (const auto& in : insns) {
      std::printf("  +%04x  %02x  %s\n",
                  in.off - code, in.opcode, in.text.c_str());
    }
    return 0;
  }

  if (surfaceCensus) {
    const std::string dtiPath = *target;
    const auto slash = dtiPath.find_last_of("/\\");
    const auto dot = dtiPath.find_last_of('.');
    if (dot == std::string::npos) {
      std::fprintf(stderr, "--surface-census wants a .DTI path\n");
      return 1;
    }
    const std::string dir =
        slash == std::string::npos ? "" : dtiPath.substr(0, slash + 1);
    const std::string stem = dtiPath.substr(
        slash == std::string::npos ? 0 : slash + 1,
        dot - (slash == std::string::npos ? 0 : slash + 1));
    const std::string mtoPath = dir + stem + "O.MTO";

    const auto dtiFile = root->readFile(dtiPath, kEntriesMaxBytes, &err);
    const auto mtoFile = root->readFile(mtoPath, kEntriesMaxBytes, &err);
    if (!dtiFile || !mtoFile) {
      std::fprintf(stderr, "read-file: FAILED (%s) — need .DTI + "
                           "sibling <stem>O.MTO\n",
                   err.c_str());
      return 1;
    }
    const auto dti = mdk::inspectDtiStructure(
        std::span<const std::byte>(dtiFile->data(), dtiFile->size()));
    const auto mto = mdk::inspectMtoDirectory(
        std::span<const std::byte>(mtoFile->data(), mtoFile->size()));
    if (dti.status != mdk::DtiStructureStatus::kOk ||
        mto.status != mdk::MtoDirectoryStatus::kOk) {
      std::fprintf(stderr, "parse: FAILED (dti=%s mto=%s)\n",
                   std::string(mdk::dtiStructureStatusName(dti.status))
                       .c_str(),
                   std::string(mdk::mtoDirectoryStatusName(mto.status))
                       .c_str());
      return 1;
    }
    std::printf("dti:   %s — %zu arenas\n", dtiPath.c_str(),
                dti.arenas.size());
    std::printf("mto:   %s — %u blocks\n", mtoPath.c_str(), mto.count);

    // DTI: the type-7 (fan/volume) + type-9 (slide-zone) sub-records.
    int tot7 = 0, tot9 = 0;
    for (const auto& arec : dti.arenas) {
      int t7 = 0, t9 = 0;
      for (const auto& sr : arec.subRecords) {
        if (sr.type == 7) {
          ++t7;
        } else if (sr.type == 9) {
          ++t9;
        }
      }
      tot7 += t7;
      tot9 += t9;
      if (t7 || t9) {
        std::printf("  arena %-8.8s  type7=%d type9=%d subs=%u\n",
                    arec.name().c_str(), t7, t9, arec.subRecordCount);
      }
    }
    std::printf("dti-total:  type7=%d type9=%d\n", tot7, tot9);

    // MTO: per-block region-C collision blob -> poly surface census.
    const std::uint8_t* mb =
        reinterpret_cast<const std::uint8_t*>(mtoFile->data());
    const std::size_t mn = mtoFile->size();
    std::uint32_t surfHist[17] = {};  // [0]=none, [1..16]=surface id
    std::uint32_t over16 = 0;         // surface byte beyond the 16 slots
    std::uint32_t f04 = 0, f10 = 0, f20 = 0, f30 = 0;
    std::size_t totPolys = 0, surfPolys = 0, blobs = 0;
    for (std::size_t bi = 0; bi < mto.blocks.size(); ++bi) {
      const auto& b = mto.blocks[bi];
      mdk::CollisionArena arena;
      std::uint32_t counts[4] = {};
      if (b.regionCOffset >= mn) {
        continue;
      }
      if (!mdk::collisionBlobParse(mb + b.regionCOffset,
                                   mn - b.regionCOffset, &arena,
                                   counts)) {
        continue;
      }
      ++blobs;
      std::size_t blkSurf = 0;
      for (std::uint32_t p = 0; p < counts[2]; ++p) {
        const mdk::CollisionPoly& poly = arena.polys[p];
        const std::uint8_t s = poly.surface;
        const std::uint16_t f = poly.flags;
        if (s <= 16) {
          ++surfHist[s];
        } else {
          ++over16;
        }
        if (s != 0) {
          ++blkSurf;
        }
        if (f & 0x04) ++f04;
        if (f & 0x10) ++f10;
        if (f & 0x20) ++f20;
        if (f & 0x30) ++f30;
        ++totPolys;
      }
      surfPolys += blkSurf;
      if (blkSurf) {
        const std::string nm =
            bi < mto.entries.size() ? mto.entries[bi].name() : "?";
        std::printf("  block %-8.8s  polys=%u surface=%zu\n", nm.c_str(),
                    counts[2], blkSurf);
      }
    }
    std::printf("mto-total:  blobs=%zu polys=%zu surface-polys=%zu\n",
                blobs, totPolys, surfPolys);
    std::printf("surface-id histogram (byte +0x23 = id, 0 = none):\n");
    for (int i = 1; i <= 16; ++i) {
      if (surfHist[i]) {
        std::printf("  id %2d: %u polys\n", i, surfHist[i]);
      }
    }
    if (over16) {
      std::printf("  out-of-domain (>16): %u polys\n", over16);
    }
    std::printf("poly flag bits (+0x20, all %zu polys): "
                "0x04=%u 0x10=%u 0x20=%u 0x30=%u\n",
                totPolys, f04, f10, f20, f30);
    return 0;
  }

  // --collision-probe: Phase 5D BUILD_A smoke. Scans the file for the
  // first self-consistent FUN_00419ee0 collision blob (the level
  // stream's {nodes,polys,verts} triple), then runs one real swept
  // FUN_004630d4 query + FUN_00435eec floor probe against it.
  if (collisionProbe) {
    const auto cfile = root->readFile(*target, kEntriesMaxBytes, &err);
    if (!cfile) {
      std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
      return 1;
    }
    const std::uint8_t* bytes =
        reinterpret_cast<const std::uint8_t*>(cfile->data());
    const std::size_t n = cfile->size();
    std::size_t blobOff = 0;
    mdk::CollisionArena arena;
    std::uint32_t counts[4] = {0, 0, 0, 0};
    bool found = false;
    for (std::size_t off = 0; off + 16 <= n; off += 4) {
      mdk::CollisionArena a;
      if (mdk::collisionBlobParse(bytes + off, n - off, &a, counts)) {
        blobOff = off;
        arena = a;
        found = true;
        break;
      }
    }
    if (!found) {
      std::printf("collision: no self-consistent FUN_00419ee0 blob "
                  "found\n");
      return 1;
    }
    std::printf("blob:      0x%08zx — countA=%u nodes=%u polys=%u "
                "verts=%u\n",
                blobOff, counts[0], counts[1], counts[2], counts[3]);
    // Vertex bounds for the default probe position.
    float bmin[3] = {1e30f, 1e30f, 1e30f};
    float bmax[3] = {-1e30f, -1e30f, -1e30f};
    for (std::uint32_t i = 0; i < counts[3]; ++i) {
      for (int k = 0; k < 3; ++k) {
        const float v = arena.verts[i * 3 + k];
        if (v < bmin[k]) bmin[k] = v;
        if (v > bmax[k]) bmax[k] = v;
      }
    }
    std::printf("bounds:    min (%g, %g, %g)  max (%g, %g, %g)\n",
                (double)bmin[0], (double)bmin[1], (double)bmin[2],
                (double)bmax[0], (double)bmax[1], (double)bmax[2]);
    mdk::CollisionState cs;
    cs.arena = &arena;
    cs.queryEnabled = 1;
    cs.arenaValid = 1;
    cs.objectDataLoaded = 1;
    cs.pos[0] = probePosGiven ? probePos[0]
                            : (bmin[0] + bmax[0]) * 0.5f;
    cs.pos[1] = probePosGiven ? probePos[1]
                            : (bmin[1] + bmax[1]) * 0.5f;
    cs.pos[2] = probePosGiven ? probePos[2]
                            : (bmin[2] + bmax[2]) * 0.5f;
    std::printf("probe-pos: (%g, %g, %g)\n", (double)cs.pos[0],
                (double)cs.pos[1], (double)cs.pos[2]);
    // One vertical swept query spanning the whole vert range.
    const float dz = -(bmax[2] - bmin[2] + 20.0f);
    const mdk::CollisionNode* node = nullptr;
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 0.0f, dz, 0.5f, nullptr, &node);
    std::printf("sweep:     dz=%g -> pos (%g, %g, %g)  contact=%s",
                (double)dz, (double)cs.pos[0], (double)cs.pos[1],
                (double)cs.pos[2], hit ? "yes" : "no");
    if (hit) {
      std::printf(" poly#%td", hit - arena.polys);
    }
    if (node) {
      std::printf(" node#%td n=(%g, %g, %g)", node - arena.nodes,
                  (double)node->nx, (double)node->ny,
                  (double)node->nz);
    }
    std::printf("\n");
    // FUN_00435eec at frame end: the blob carries no object list, so
    // bit1 clears — the original behaves the same on static floors.
    mdk::collisionFloorProbe(cs);
    std::printf("probe:     flags=0x%02x floorZ=%g (object list "
                "empty in blob — bit1 clears as on static floors)\n",
                cs.contactFlags, (double)cs.floorZ);
    return 0;
  }

  // --entries: enumerate interior directory metadata where a proven
  // parser exists (SNI Phase 3C; MTI Phase 3D; MTO Phase 3E; CMI
  // Phase 3F; DTI Phase 3G; FTI/BNI Phase 3H). Never prints payload
  // bytes. --visual-info: metadata-only report for one named record
  // against the proven BNI image layouts (Phase 4A) — no extraction.
  // --font-info: metadata-only report for one named FTI record against
  // the proven FONTSML/FONTBIG glyph layout (Phase 4C).
  // --sprite-info: metadata-only report for one named FTI record
  // against the proven ARROW sprite-table layout (Phase 4D).
  if (support != mdk::FamilySupport::kDirectoryMetadata) {
    const char* label = visualInfoName  ? "visual-info:"
                        : fontInfoName  ? "font-info:  "
                        : spriteInfoName ? "sprite-info:"
                                         : "entries:  ";
    std::printf("%s unsupported for family %s (support: %s) — "
                "no evidence-backed interior parser\n",
                label,
                std::string(mdk::fileFamilyName(family)).c_str(),
                std::string(mdk::familySupportName(support)).c_str());
    return 1;
  }

  const auto file = root->readFile(*target, kEntriesMaxBytes, &err);
  if (!file) {
    std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
    return 1;
  }

  if (spriteInfoName) {
    if (family != mdk::MdkFileFamily::kFti) {
      std::printf("sprite:    unsupported for family %s — Phase 4D "
                  "proves the FTI ARROW sprite-table layout only\n",
                  std::string(mdk::fileFamilyName(family)).c_str());
      return 1;
    }
    const auto dir = mdk::inspectFtiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    if (dir.status != mdk::FtiDirectoryStatus::kOk) {
      std::printf("fti:       %s — %s\n",
                  std::string(mdk::ftiDirectoryStatusName(dir.status))
                      .c_str(),
                  dir.detail.c_str());
      return 1;
    }
    const mdk::FtiRecord* rec = mdk::findFtiRecord(dir, *spriteInfoName);
    if (!rec) {
      std::printf("record:    %s — NOT FOUND\n",
                  spriteInfoName->c_str());
      return 1;
    }
    std::printf("record:    %s\n", rec->name().c_str());
    std::printf("span:      [0x%08llx, 0x%08llx) — %llu bytes\n",
                static_cast<unsigned long long>(rec->payloadFileOffset),
                static_cast<unsigned long long>(rec->payloadEnd),
                static_cast<unsigned long long>(rec->payloadSize()));
    const std::span<const std::byte> payload(
        file->data() + rec->payloadFileOffset, rec->payloadSize());
    std::string derr;
    const auto sprite = mdk::decodeFtiSprite(payload, &derr);
    if (!sprite) {
      std::printf("decode:    FAILED (%s)\n", derr.c_str());
      return 1;
    }
    std::printf("layout:    u32 blockBytes @+0; u32 frameCount @+4; "
                "u32 frameOffset[] @+8 (each +4-relative); frame "
                "{u16 w, u16 h, s16 hotX, s16 hotY, stream}\n");
    std::printf("header:    blockBytes=%u frames=%llu payload=%llu "
                "trailing=%llu\n",
                sprite->blockBytes,
                static_cast<unsigned long long>(sprite->frames.size()),
                static_cast<unsigned long long>(sprite->payloadBytes),
                static_cast<unsigned long long>(sprite->trailingBytes));
    std::printf("stream:    cmds <0x80 literal (n=cmd+1); 0x80-0xfd "
                "run (n=cmd-0x7c, value 0 = transparent); 0xfe row "
                "break; 0xff end\n");
    std::printf("colors:    final palette indices; byte 0 = "
                "transparent (skipped, not color-keyed)\n");
    for (std::size_t i = 0; i < sprite->frames.size(); ++i) {
      const auto& f = sprite->frames[i];
      std::printf("frame[%zu]:  @+0x%llx %ux%u hotspot (%d,%d) "
                  "stream %zu B | lit %u run %u (transp %u) rows %u | "
                  "adv %llu opaque %llu idx [%u..%u]\n",
                  i,
                  static_cast<unsigned long long>(f.frameFileOffset),
                  f.width, f.height, f.hotspotX, f.hotspotY,
                  f.stream.size(), f.literalPackets, f.runPackets,
                  f.transparentRuns, f.rowBreaks,
                  static_cast<unsigned long long>(f.pixelAdvances),
                  static_cast<unsigned long long>(f.opaqueWrites),
                  f.minPixelIndex, f.maxPixelIndex);
    }
    std::printf("digest:    %016llx\n",
                static_cast<unsigned long long>(
                    mdk::ftiSpriteDigest(*sprite)));
    return 0;
  }

  if (fontInfoName) {
    if (family != mdk::MdkFileFamily::kFti) {
      std::printf("font:      unsupported for family %s — Phase 4C "
                  "proves the FTI FONTSML/FONTBIG glyph layout only\n",
                  std::string(mdk::fileFamilyName(family)).c_str());
      return 1;
    }
    const auto dir = mdk::inspectFtiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    if (dir.status != mdk::FtiDirectoryStatus::kOk) {
      std::printf("fti:       %s — %s\n",
                  std::string(mdk::ftiDirectoryStatusName(dir.status))
                      .c_str(),
                  dir.detail.c_str());
      return 1;
    }
    const mdk::FtiRecord* rec = mdk::findFtiRecord(dir, *fontInfoName);
    if (!rec) {
      std::printf("record:    %s — NOT FOUND\n", fontInfoName->c_str());
      return 1;
    }
    std::printf("record:    %s\n", rec->name().c_str());
    std::printf("span:      [0x%08llx, 0x%08llx) — %llu bytes\n",
                static_cast<unsigned long long>(rec->payloadFileOffset),
                static_cast<unsigned long long>(rec->payloadEnd),
                static_cast<unsigned long long>(rec->payloadSize()));
    const std::span<const std::byte> payload(
        file->data() + rec->payloadFileOffset, rec->payloadSize());
    std::string derr;
    const auto font = mdk::decodeFtiFont(payload, &derr);
    if (!font) {
      std::printf("decode:    FAILED (%s)\n", derr.c_str());
      return 1;
    }
    std::printf("layout:    u32 offsetTable[256] @+0x000; glyph "
                "{s8 top, s8 bottom, u8 width, u8 px[w*(top+bottom+1)]}"
                " — row-major top-down indexed bytes\n");
    std::printf("mapping:   input byte -> table[byte] directly; "
                "0 = unmapped (no ASCII/CP437 transform)\n");
    std::printf("mapped:    %zu of 256 slots — first 0x%02x, last "
                "0x%02x\n", font->mappedCount, font->firstMapped,
                font->lastMapped);
    std::printf("glyphs@:   record+0x%llx; trailing slack %llu bytes\n",
                static_cast<unsigned long long>(font->glyphDataStart),
                static_cast<unsigned long long>(font->trailingBytes));
    std::printf("offsets:   %s, %s\n",
                font->offsetsSortedAscending ? "sorted ascending"
                                           : "NOT sorted",
                font->offsetsUnique ? "unique" : "NOT unique");
    std::printf("encoding:  verbatim 8-bit palette indices; byte 0 "
                "skips (transparent); max index used %u\n",
                font->maxPixelIndex);
    std::printf("missing:   table entry 0 -> advance-only; proven "
                "advance %d (FONTSML path) / %d (FONTBIG path)\n",
                mdk::kFtiFontSmlMissingAdvance,
                mdk::kFtiFontBigMissingAdvance);
    std::printf("digest:    %016llx\n",
                static_cast<unsigned long long>(
                    mdk::ftiFontDigest(*font)));
    if (fontInfoCode) {
      const auto code = static_cast<std::uint8_t>(*fontInfoCode);
      const mdk::FtiGlyph* g = font->glyphFor(code);
      std::printf("glyph[0x%02x]: ", code);
      if (!g) {
        std::printf("unmapped (advance-only)\n");
      } else {
        std::printf("@0x%08llx top=%d bottom=%d width=%u rows=%d "
                    "bitmap=%llu bytes consumed=%llu\n",
                    static_cast<unsigned long long>(g->offset),
                    int(g->top), int(g->bottom), unsigned(g->width),
                    g->rows(),
                    static_cast<unsigned long long>(g->pixels.size()),
                    static_cast<unsigned long long>(
                        g->consumedBytes()));
      }
    }
    return 0;
  }

  if (visualInfoName) {
    if (family != mdk::MdkFileFamily::kBni) {
      std::printf("visual:    unsupported for family %s — Phase 4A "
                  "proves BNI image payloads only\n",
                  std::string(mdk::fileFamilyName(family)).c_str());
      return 1;
    }
    const auto dir = mdk::inspectBniDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    if (dir.status != mdk::BniDirectoryStatus::kOk) {
      std::printf("bni:       %s — %s\n",
                  std::string(mdk::bniDirectoryStatusName(dir.status))
                      .c_str(),
                  dir.detail.c_str());
      return 1;
    }
    const mdk::BniRecord* rec = mdk::findBniRecord(dir, *visualInfoName);
    if (!rec) {
      std::printf("record:    %s — NOT FOUND\n", visualInfoName->c_str());
      return 1;
    }
    std::printf("record:    %s\n", rec->name().c_str());
    std::printf("span:      [0x%08llx, 0x%08llx) — %llu bytes\n",
                static_cast<unsigned long long>(rec->payloadFileOffset),
                static_cast<unsigned long long>(rec->payloadEnd),
                static_cast<unsigned long long>(rec->payloadSize()));
    const std::span<const std::byte> payload(
        file->data() + rec->payloadFileOffset, rec->payloadSize());
    const auto probe = mdk::probeBniImage(payload);
    std::printf("shape:     %s\n",
                std::string(mdk::bniImageShapeName(probe.shape)).c_str());
    if (probe.shape == mdk::BniImageShape::kOther) {
      std::printf("           payload does not match a proven image "
                  "layout\n");
      return 1;
    }
    std::printf("dims:      %dx%d (stride %d, row-major top-down)\n",
                probe.width, probe.height, probe.width);
    std::printf("pixels:    %llu indexed bytes @ payload+0x%zx\n",
                static_cast<unsigned long long>(probe.pixelBytes),
                probe.headerBytes);
    if (probe.shape == mdk::BniImageShape::kIndexedOnly) {
      // Indexed-only records carry no palette; the consumer binds one
      // from context. The only binding proven so far is the STREAM
      // backdrop (FUN_0042b270): SYS_PAL head + PAL record tail.
      if (!mdk::isStreamBackdropRequest(*target, *visualInfoName)) {
        std::printf("palette:   none embedded — no proven "
                    "external-palette binding for this context "
                    "(Phase 4B proves %s %s only)\n",
                    std::string(mdk::kStreamBniFile).c_str(),
                    std::string(mdk::kStreamImageRecord).c_str());
        return 0;
      }
      const auto fti = root->readFile(std::string(mdk::kStreamSystemFile),
                                      kEntriesMaxBytes, &err);
      if (!fti) {
        std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
        return 1;
      }
      std::string derr;
      const auto img = mdk::decodeStreamBackdrop(
          std::span<const std::byte>(file->data(), file->size()),
          std::span<const std::byte>(fti->data(), fti->size()), &derr);
      if (!img) {
        std::printf("decode:    FAILED (%s)\n", derr.c_str());
        return 1;
      }
      std::printf("palette:   external — STREAM context "
                  "(FUN_0042b270): %s %s[0:64] + %s[64:256] "
                  "(record bytes [0xc0,0x300))\n",
                  std::string(mdk::kStreamSystemFile).c_str(),
                  std::string(mdk::kStreamSystemRecord).c_str(),
                  std::string(mdk::kStreamPaletteRecord).c_str());
      std::printf("decode:    ok — digest=%016llx\n",
                  static_cast<unsigned long long>(
                      mdk::imageDigest(*img)));
      return 0;
    }
    std::string derr;
    const auto img = mdk::decodeBniPalettedImage(payload, &derr);
    if (!img) {
      std::printf("decode:    FAILED (%s)\n", derr.c_str());
      return 1;
    }
    std::printf("palette:   256 embedded RGB entries (R,G,B order)\n");
    std::printf("decode:    ok — digest=%016llx\n",
                static_cast<unsigned long long>(mdk::imageDigest(*img)));
    return 0;
  }

  if (family == mdk::MdkFileFamily::kMti) {
    const auto dir = mdk::inspectMtiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   MTI directory (count u32 @0x14, records "
                "24 bytes @0x18, name[8])\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::mtiDirectoryStatusName(dir.status)).c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::MtiDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("trailer:   name[12] @ size-12 %s\n",
                dir.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                dir.secondaryLength,
                dir.secondaryEqualsTrailerOffset ? "equals"
                                                 : "does not equal");

    for (std::size_t i = 0; i < dir.entries.size(); ++i) {
      const auto& e = dir.entries[i];
      if (e.isIndexRecord()) {
        std::printf("  [%3zu] %-8s INDEX (field0x08=0xffffffff) "
                    "index=%u (0x%08x) field0x10=0x%08x "
                    "field0x14=0x%08x\n",
                    i, e.name().c_str(), e.fieldAt0x0C, e.fieldAt0x0C,
                    e.fieldAt0x10, e.fieldAt0x14);
      } else if (e.headerCount) {
        std::printf("  [%3zu] %-8s field0x08=0x%08x field0x0c=0x%08x "
                    "field0x10=0x%08x blobOff=0x%08x fileOff=0x%08llx "
                    "hdr{n=%u,a=%u,b=%u} dataOff=0x%08llx\n",
                    i, e.name().c_str(), e.fieldAt0x08, e.fieldAt0x0C,
                    e.fieldAt0x10, e.fieldAt0x14,
                    static_cast<unsigned long long>(e.payloadFileOffset()),
                    *e.headerCount, e.headerFieldA, e.headerFieldB,
                    static_cast<unsigned long long>(
                        e.payloadDataFileOffset));
      } else {
        std::printf("  [%3zu] %-8s field0x08=0x%08x field0x0c=0x%08x "
                    "field0x10=0x%08x blobOff=0x%08x fileOff=0x%08llx "
                    "hdr{a=%u,b=%u} dataOff=0x%08llx\n",
                    i, e.name().c_str(), e.fieldAt0x08, e.fieldAt0x0C,
                    e.fieldAt0x10, e.fieldAt0x14,
                    static_cast<unsigned long long>(e.payloadFileOffset()),
                    e.headerFieldA, e.headerFieldB,
                    static_cast<unsigned long long>(
                        e.payloadDataFileOffset));
      }
    }
    return 0;
  }

  if (family == mdk::MdkFileFamily::kMto) {
    const auto dir = mdk::inspectMtoDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   MTO overlay directory (count u32 @0x14, "
                "records 12 bytes @0x18, name[8] + file offset)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::mtoDirectoryStatusName(dir.status)).c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::MtoDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("trailer:   name[12] @ size-12 %s\n",
                dir.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                dir.secondaryLength,
                dir.secondaryEqualsTrailerOffset ? "equals"
                                                 : "does not equal");

    for (std::size_t i = 0; i < dir.entries.size(); ++i) {
      const auto& e = dir.entries[i];
      const auto& b = dir.blocks[i];
      std::printf("  [%3zu] %-8s blockOff=0x%08llx blockLen=0x%x "
                  "inner=%-12s innerRecs=%u\n",
                  i, e.name().c_str(),
                  static_cast<unsigned long long>(e.blockFileOffset),
                  b.blockLength, b.innerName().c_str(), b.innerCount);
      std::printf("         fields{0x04=0x%x 0x08=0x%x 0x0c=0x%x} "
                  "innerTrailer=%s regionA{size=0x%x ca=%u cb=%u "
                  "cc=%u} regionB@0x%llx(0x%llx) regionC@0x%llx{c1=%u "
                  "c2=%u c3=%u c4=%u extra@0x%llx}\n",
                  b.fieldAt0x04, b.fieldAt0x08, b.fieldAt0x0C,
                  b.innerTrailerPresent ? "yes" : "no",
                  b.regionASize, b.regionACountA, b.regionACountB,
                  b.regionACountC,
                  static_cast<unsigned long long>(b.regionBOffset),
                  static_cast<unsigned long long>(b.regionBSize),
                  static_cast<unsigned long long>(b.regionCOffset),
                  b.regionCCount1, b.regionCCount2, b.regionCCount3,
                  b.regionCCount4,
                  static_cast<unsigned long long>(b.regionCExtraOffset));
      for (std::size_t j = 0; j < b.innerRecords.size(); ++j) {
        const auto& mr = b.innerRecords[j];
        if (mr.isIndexRecord()) {
          std::printf("           mat[%3zu] %-8s INDEX index=%u "
                      "field0x10=0x%08x field0x14=0x%08x\n",
                      j, mr.name().c_str(), mr.fieldAt0x0C,
                      mr.fieldAt0x10, mr.fieldAt0x14);
        } else if (mr.headerCount) {
          std::printf("           mat[%3zu] %-8s flags=0x%08x "
                      "f0x0c=0x%08x f0x10=0x%08x imgOff=0x%08x "
                      "fileOff=0x%08llx hdr{n=%u,a=%u,b=%u} "
                      "dataOff=0x%08llx\n",
                      j, mr.name().c_str(), mr.fieldAt0x08,
                      mr.fieldAt0x0C, mr.fieldAt0x10, mr.fieldAt0x14,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset -
                          mr.payloadHeaderBytes()),
                      *mr.headerCount, mr.headerFieldA, mr.headerFieldB,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset));
        } else {
          std::printf("           mat[%3zu] %-8s flags=0x%08x "
                      "f0x0c=0x%08x f0x10=0x%08x imgOff=0x%08x "
                      "fileOff=0x%08llx hdr{a=%u,b=%u} "
                      "dataOff=0x%08llx\n",
                      j, mr.name().c_str(), mr.fieldAt0x08,
                      mr.fieldAt0x0C, mr.fieldAt0x10, mr.fieldAt0x14,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset -
                          mr.payloadHeaderBytes()),
                      mr.headerFieldA, mr.headerFieldB,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset));
        }
      }
      for (std::size_t j = 0; j < b.regionAArrayA.size(); ++j) {
        const auto& nr = b.regionAArrayA[j];
        std::printf("           regA-A[%3zu] %-8s off=0x%08x\n",
                    j, nr.name().c_str(), nr.fieldAt0x08);
      }
      for (std::size_t j = 0; j < b.regionAArrayB.size(); ++j) {
        const auto& nr = b.regionAArrayB[j];
        std::printf("           regA-B[%3zu] %-8s off=0x%08x "
                    "(overlay-alien lookup target)\n",
                    j, nr.name().c_str(), nr.fieldAt0x08);
      }
      for (std::size_t j = 0; j < b.regionAArrayC.size(); ++j) {
        const auto& sr = b.regionAArrayC[j];
        std::printf("           regA-C[%3zu] {0x%08x 0x%08x 0x%08x "
                    "0x%04x 0x%04x off=0x%08x 0x%08x} "
                    "(overlay-sound record)\n",
                    j, sr.fieldAt0x00, sr.fieldAt0x04, sr.fieldAt0x08,
                    sr.fieldAt0x0C, sr.fieldAt0x0E, sr.fieldAt0x10,
                    sr.fieldAt0x14);
      }
      for (std::size_t j = 0; j < b.regionCNames.size(); ++j) {
        std::printf("           regC-name[%3zu] %s\n",
                    j, b.regionCNames[j].name().c_str());
      }
    }
    return 0;
  }

  if (family == mdk::MdkFileFamily::kCmi) {
    const auto dir = mdk::inspectCmiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   CMI directory (four counted variable-"
                "length tables @0x14: u8 len + name[len] + u32 value; "
                "then data region)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::cmiDirectoryStatusName(dir.status)).c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::CmiDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("trailer:   name[12] @ size-12 %s\n",
                dir.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                dir.secondaryLength,
                dir.secondaryEqualsTrailerOffset ? "equals"
                                                 : "does not equal");

    for (std::size_t t = 0; t < dir.tables.size(); ++t) {
      const auto& tab = dir.tables[t];
      std::printf("  table[%zu] count=%u count@0x%llx recs@0x%llx "
                  "end=0x%llx\n", t, tab.count,
                  static_cast<unsigned long long>(tab.countFileOffset),
                  static_cast<unsigned long long>(tab.recordsFileOffset),
                  static_cast<unsigned long long>(tab.endFileOffset));
      for (std::size_t i = 0; i < tab.records.size(); ++i) {
        const auto& e = tab.records[i];
        std::printf("    [%3zu] @0x%06llx len=%-3u %-18s value=0x%08x",
                    i, static_cast<unsigned long long>(e.fileOffset),
                    e.nameLength, e.name().c_str(), e.value);
        if (const auto tgt = e.valueFileOffset()) {
          std::printf(" ->file=0x%08llx%s",
                      static_cast<unsigned long long>(*tgt),
                      e.valueReachesDataRegion ? "" : " (outside data "
                                                     "region)");
        } else {
          std::printf(" (null)");
        }
        if (!e.nameEndsWithTerminator) {
          std::printf(" [no NUL in counted bytes]");
        }
        std::printf("\n");
      }
    }
    std::printf("  data region: [0x%llx, 0x%llx) — %llu bytes, "
                "structure not enumerated\n",
                static_cast<unsigned long long>(dir.dataRegionOffset),
                static_cast<unsigned long long>(dir.dataRegionEnd),
                static_cast<unsigned long long>(dir.dataRegionEnd -
                                                dir.dataRegionOffset));
    return 0;
  }

  if (family == mdk::MdkFileFamily::kDti) {
    const auto st = mdk::inspectDtiStructure(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   DTI structure (five image-relative TOC "
                "offsets @0x14: params / keyed records / arena table "
                "/ palette / grid)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::dtiStructureStatusName(st.status))
                    .c_str(),
                st.detail.empty() ? "" : " — ",
                st.detail.empty() ? "" : st.detail.c_str());
    if (st.status != mdk::DtiStructureStatus::kOk) {
      return 1;
    }
    std::printf("trailer:   name[12] @ size-12 %s\n",
                st.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                st.secondaryLength,
                st.secondaryEqualsTrailerOffset ? "equals"
                                                : "does not equal");
    std::printf("toc:       [");
    for (std::size_t i = 0; i < mdk::kDtiTocEntries; ++i) {
      std::printf("%s0x%08x", i ? " " : "", st.tocImageOffsets[i]);
    }
    std::printf("] (image-relative; file = value + 4)\n");

    const auto& s0 = st.sections[mdk::kDtiSecParams];
    std::printf("  s0 params:   [0x%llx, 0x%llx) — %llu bytes; "
                "startArena=%u view=(%g %g %g %g) fill=(0x%x,0x%x) "
                "scroll=(0x%x,0x%x) grid=%ux%u altFill=(0x%x,0x%x)\n",
                static_cast<unsigned long long>(s0.fileStart),
                static_cast<unsigned long long>(s0.fileEnd),
                static_cast<unsigned long long>(s0.size()),
                st.params[0], std::bit_cast<float>(st.params[1]),
                std::bit_cast<float>(st.params[2]),
                std::bit_cast<float>(st.params[3]),
                std::bit_cast<float>(st.params[4]), st.params[5],
                st.params[6], st.params[7], st.params[8], st.params[9],
                st.params[10], st.params[0x0b], st.params[0x0c]);
    std::printf("             matrix[16] =");
    for (std::size_t i = 13; i < mdk::kDtiParamWords; ++i) {
      std::printf("%s0x%x", i == 13 ? " " : ",", st.params[i]);
    }
    std::printf("\n");

    const auto& s1 = st.sections[mdk::kDtiSecKeyed];
    std::printf("  s1 keyed:    [0x%llx, 0x%llx) count=%zu "
                "stride=24%s\n",
                static_cast<unsigned long long>(s1.fileStart),
                static_cast<unsigned long long>(s1.fileEnd),
                st.keyedRecords.size(),
                st.s1TrailingBytes ? " (trailing bytes)" : "");
    for (std::size_t i = 0; i < st.keyedRecords.size(); ++i) {
      const auto& e = st.keyedRecords[i];
      std::printf("    [%3zu] @0x%06llx word0=%u key=%u f=(%g %g %g "
                  "%g)\n", i,
                  static_cast<unsigned long long>(e.fileOffset),
                  e.word0, e.key, e.floatAt(0), e.floatAt(1),
                  e.floatAt(2), e.floatAt(3));
    }

    const auto& s2 = st.sections[mdk::kDtiSecArenas];
    std::printf("  s2 arenas:   [0x%llx, 0x%llx) count=%zu "
                "payloads@0x%llx\n",
                static_cast<unsigned long long>(s2.fileStart),
                static_cast<unsigned long long>(s2.fileEnd),
                st.arenas.size(),
                static_cast<unsigned long long>(
                    st.s2PayloadRegionStart));
    for (std::size_t i = 0; i < st.arenas.size(); ++i) {
      const auto& a = st.arenas[i];
      std::printf("    [%3zu] @0x%06llx %-9s imgOff=0x%08x "
                  "->file=0x%08llx scalar=%g subs=%u%s\n", i,
                  static_cast<unsigned long long>(a.fileOffset),
                  a.name().c_str(), a.payloadImageOffset,
                  static_cast<unsigned long long>(a.payloadFileOffset),
                  a.scalar(), a.subRecordCount,
                  a.nameEndsWithTerminator ? "" : " [no NUL]");
      for (std::size_t j = 0; j < a.subRecords.size(); ++j) {
        const auto& sub = a.subRecords[j];
        const char* tag = sub.type == 2   ? " HotGen"
                          : sub.type == 4 ? " HotPick"
                          : sub.type == 6 ? " connect"
                                          : "";
        std::printf("         sub[%3zu] @0x%06llx type=%u%s "
                    "f1=0x%08x f2=0x%08x tail=[%08x %08x %08x %08x "
                    "%08x %08x]%s\n",
                    j,
                    static_cast<unsigned long long>(sub.fileOffset),
                    sub.type, tag, sub.fields[0], sub.fields[1],
                    sub.fields[2], sub.fields[3], sub.fields[4],
                    sub.fields[5], sub.fields[6], sub.fields[7],
                    (sub.type == 2 || sub.type == 4)
                        ? (std::string(" name=\"") + sub.name18() +
                           "\"")
                              .c_str()
                        : "");
      }
    }

    const auto& s3 = st.sections[mdk::kDtiSecPalette];
    std::printf("  s3 palette:  [0x%llx, 0x%llx) count=%u "
                "rgb=[0x%llx, 0x%llx)%s\n",
                static_cast<unsigned long long>(s3.fileStart),
                static_cast<unsigned long long>(s3.fileEnd),
                st.paletteCount,
                static_cast<unsigned long long>(
                    st.paletteBytes.fileStart),
                static_cast<unsigned long long>(
                    st.paletteBytes.fileEnd),
                st.s3TrailingBytes ? " (trailing bytes)" : "");

    const auto& s4 = st.sections[mdk::kDtiSecGrid];
    std::printf("  s4 grid:     [0x%llx, 0x%llx) — %llu bytes = %u "
                "plane(s) x 0x%llx%s\n",
                static_cast<unsigned long long>(s4.fileStart),
                static_cast<unsigned long long>(s4.fileEnd),
                static_cast<unsigned long long>(s4.size()),
                st.gridPlaneCount,
                static_cast<unsigned long long>(st.gridPlaneSize),
                st.s4TrailingBytes ? " (trailing bytes)" : "");
    return 0;
  }

  if (family == mdk::MdkFileFamily::kFti) {
    const auto dir = mdk::inspectFtiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   FTI directory (length envelope; count u32 "
                "@0x04, records 12 bytes @0x08: name[8] + imgOff)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::ftiDirectoryStatusName(dir.status))
                    .c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::FtiDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("offsets:   sorted=%s unique=%s first-at-dir-end=%s\n",
                dir.offsetsSortedAscending ? "yes" : "NO",
                dir.offsetsUnique ? "yes" : "NO",
                dir.firstPayloadAtDirectoryEnd ? "yes" : "NO");

    for (std::size_t i = 0; i < dir.records.size(); ++i) {
      const auto& e = dir.records[i];
      std::printf("  [%3zu] @0x%06llx %-8s imgOff=0x%08x "
                  "->file=0x%08llx span=[0x%08llx,0x%08llx) %lluB%s\n",
                  i,
                  static_cast<unsigned long long>(e.recordFileOffset),
                  e.name().c_str(), e.imageOffset,
                  static_cast<unsigned long long>(e.payloadFileOffset),
                  static_cast<unsigned long long>(e.payloadFileOffset),
                  static_cast<unsigned long long>(e.payloadEnd),
                  static_cast<unsigned long long>(e.payloadSize()),
                  e.nameHasTerminator ? "" : " [no NUL]");
    }
    return 0;
  }

  if (family == mdk::MdkFileFamily::kBni) {
    const auto dir = mdk::inspectBniDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   BNI directory (length envelope; count u32 "
                "@0x04, records 16 bytes @0x08: name[12] + imgOff)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::bniDirectoryStatusName(dir.status))
                    .c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::BniDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("offsets:   sorted=%s unique=%s first-at-dir-end=%s\n",
                dir.offsetsSortedAscending ? "yes" : "NO",
                dir.offsetsUnique ? "yes" : "NO",
                dir.firstPayloadAtDirectoryEnd ? "yes" : "NO");

    for (std::size_t i = 0; i < dir.records.size(); ++i) {
      const auto& e = dir.records[i];
      std::printf("  [%3zu] @0x%06llx %-12s imgOff=0x%08x "
                  "->file=0x%08llx span=[0x%08llx,0x%08llx) %lluB%s\n",
                  i,
                  static_cast<unsigned long long>(e.recordFileOffset),
                  e.name().c_str(), e.imageOffset,
                  static_cast<unsigned long long>(e.payloadFileOffset),
                  static_cast<unsigned long long>(e.payloadFileOffset),
                  static_cast<unsigned long long>(e.payloadEnd),
                  static_cast<unsigned long long>(e.payloadSize()),
                  e.nameHasTerminator ? "" : " [no NUL]");
    }
    return 0;
  }

  const auto dir = mdk::inspectSniDirectory(
      std::span<const std::byte>(file->data(), file->size()));
  std::printf("entries:   SNI directory (count u32 @0x14, records "
              "24 bytes @0x18)\n");
  std::printf("status:    %s%s%s\n",
              std::string(mdk::sniDirectoryStatusName(dir.status)).c_str(),
              dir.detail.empty() ? "" : " — ",
              dir.detail.empty() ? "" : dir.detail.c_str());
  if (dir.status != mdk::SniDirectoryStatus::kOk) {
    return 1;
  }
  std::printf("count:     %u\n", dir.count);
  std::printf("dir-end:   0x%llx\n",
              static_cast<unsigned long long>(dir.directoryEnd));
  std::printf("trailer:   name[12] @ size-12 %s\n",
              dir.trailerPresent ? "present" : "ABSENT");
  std::printf("u32@0x10:  %u — %s trailer offset\n",
              dir.secondaryLength,
              dir.secondaryEqualsTrailerOffset ? "equals" : "does not equal");

  for (std::size_t i = 0; i < dir.entries.size(); ++i) {
    const auto& e = dir.entries[i];
    if (e.isSentinel()) {
      std::printf("  [%3zu] %-12s SENTINEL (field0x0c=size=0xffffffff) "
                  "marker fileOff=0x%08llx\n",
                  i, e.name().c_str(),
                  static_cast<unsigned long long>(e.payloadFileOffset()));
    } else {
      std::printf("  [%3zu] %-12s field0x0c=0x%08x blobOff=0x%08x "
                  "fileOff=0x%08llx size=%u\n",
                  i, e.name().c_str(), e.fieldAt0x0C, e.blobOffset,
                  static_cast<unsigned long long>(e.payloadFileOffset()),
                  e.payloadSize);
    }
  }
  return 0;
}
