// Phase 6A (G1-RE) — arena render-data boundary. See the header for
// the full evidence chain (MDK95.EXE BUILD_A, instruction-level).

#include "core/arena_render.h"

#include <cstring>

#include "core/mti_directory.h"

namespace mdk {

namespace {

std::uint16_t rdU16(const std::uint8_t* p) {
  std::uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}

float rdF32(const std::uint8_t* p) {
  float v;
  std::memcpy(&v, p, 4);
  return v;
}

std::string trimName(const std::byte* p, std::size_t n) {
  std::string s;
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const char c = static_cast<char>(p[i]);
    if (c == '\0') break;
    s.push_back(c);
  }
  return s;
}

// FUN_0041a1e0 — the height-bucket vMask table (OBSERVED): for
// width != height the v field mask is the largest (2^k)-1 that the
// height range implies, shifted left by the u shift.
std::uint32_t vBucketMask(std::uint32_t h) {
  if (h < 0x11) return 0x0f;
  if (h < 0x21) return 0x1f;
  if (h < 0x41) return 0x3f;
  if (h < 0x81) return 0x7f;
  if (h < 0x101) return 0xff;
  if (h < 0x201) return 0x1ff;
  return 0x3ff;
}

// Decode one material payload at `fileOff` (absolute byte position of
// the payload's first byte) into `out`.
bool decodePayload(std::span<const std::byte> fileBytes,
                   std::uint64_t fileOff, std::uint32_t classWord,
                   std::uint32_t raw0c, std::uint32_t raw10,
                   std::span<const std::byte> nameField,
                   ArenaRenderMaterial* out) {
  ArenaRenderMaterial m;
  m.name = trimName(nameField.data(), nameField.size());
  m.isIndexRecord = (classWord == kMtiIndexRecordFlag);
  if (m.isIndexRecord) {
    // Index record (FUN_0041a1e0, CODE-CORROBORATED): only +0x08 gets
    // the index value (record +0x0c) and +0x0c is stored -1; the
    // payload offset is never dereferenced, and every other field
    // stays zero because the table buffer is memset(0) before the
    // record loop (FUN_0047d20a fill). +0x24 == 0 sends the
    // dispatcher down the flat 0xff path.
    m.height = raw0c;
    m.flags = classWord;
    *out = m;
    return true;
  }

  if (fileOff + 4 > fileBytes.size()) return false;
  const std::uint8_t* const p =
      reinterpret_cast<const std::uint8_t*>(fileBytes.data()) + fileOff;
  m.param0c = raw0c;
  m.param10 = raw10;
  const bool ext = (classWord & kMtiExtendedHeaderMask) != 0;
  std::uint64_t dataOff;
  if (ext) {
    if (fileOff + 8 > fileBytes.size()) return false;
    m.frameCount = rdU16(p);          // u16 @payload+0
    m.width = rdU16(p + 4);           // u16 @payload+4
    m.height = rdU16(p + 6);          // u16 @payload+6
    dataOff = fileOff + 8;
    m.flags = (classWord & 0xffffu) | (std::uint32_t(m.frameCount) << 16);
  } else {
    m.frameCount = 0;
    m.width = rdU16(p);               // u16 @payload+0
    m.height = rdU16(p + 2);          // u16 @payload+2
    dataOff = fileOff + 4;
    m.flags = classWord;
  }

  // shift = smallest k < 12 with (1<<k) >= width (OBSERVED loop).
  std::uint32_t shift = 0;
  while (shift < 12 && (int)m.width > (1 << shift)) ++shift;
  m.shift = shift;
  m.uMask = (1u << shift) - 1u;
  m.invUMask = ~m.uMask;
  m.vMask = (m.width == m.height ? m.uMask : vBucketMask(m.height))
            << shift;

  const std::uint64_t texelBytes =
      std::uint64_t(m.width) * m.height * (ext ? m.frameCount : 1u);
  if (dataOff + texelBytes > fileBytes.size()) return false;
  m.pixels = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(fileBytes.data()) + dataOff,
      static_cast<std::size_t>(texelBytes));
  *out = m;
  return true;
}

// FUN_0042fa50 — the name compare: 8-byte MTI field vs the region-C
// char[10] material name. Case-sensitive byte equality up to the
// shorter NUL run (OBSERVED compare shape; both fields are ASCII).
bool nameMatch(const std::string& a, const std::string& b) {
  return a == b;
}

} // namespace

ArenaRenderPoly arenaRenderPolyDecode(const CollisionPoly& poly) {
  ArenaRenderPoly r;
  const std::uint8_t* const p = reinterpret_cast<const std::uint8_t*>(&poly);
  r.v[0] = rdU16(p);
  r.v[1] = rdU16(p + 2);
  r.v[2] = rdU16(p + 4);
  std::memcpy(&r.material, p + 6, 2);
  for (int i = 0; i < 3; ++i) {
    r.uv[i][0] = rdF32(p + 8 + i * 8);
    r.uv[i][1] = rdF32(p + 8 + i * 8 + 4);
  }
  r.flags = p[0x20];
  r.aux21 = p[0x21];
  r.aux22 = p[0x22];
  r.surface = p[0x23];
  return r;
}

bool arenaRenderMaterialDecode(std::span<const std::byte> fileBytes,
                               std::uint64_t payloadOffset,
                               std::uint32_t classWord,
                               std::uint32_t raw0c, std::uint32_t raw10,
                               std::span<const std::byte> nameField,
                               ArenaRenderMaterial* out) {
  if (!out) return false;
  return decodePayload(fileBytes, payloadOffset, classWord, raw0c, raw10,
                       nameField, out);
}

ArenaMatClass arenaMatClassFor(std::int16_t material) {
  if (material >= 0) return ArenaMatClass::kTextured;
  const int m = material;
  if (m >= -1023 && m <= -1) {
    if (m >= -1010 && m <= -990) return ArenaMatClass::kEffect770;
    return ArenaMatClass::kPen;
  }
  if (m >= -1027 && m <= -1024) return ArenaMatClass::kEffect12970;
  if (m == -1028) return ArenaMatClass::kEffectE94;
  return ArenaMatClass::kEffect12970;  // <= -1029
}

std::uint8_t arenaPenIndex(std::int16_t material) {
  return static_cast<std::uint8_t>(-material & 0xff);
}

const ArenaRenderMaterial* ArenaRenderData::materialFor(
    std::size_t polyIdx) const {
  if (polyIdx >= polys.size()) return nullptr;
  const std::int16_t mi = polys[polyIdx].material;
  if (mi < 0) return nullptr;
  if (static_cast<std::size_t>(mi) >= materialOfName.size()) return nullptr;
  const int slot = materialOfName[mi];
  if (slot < 0) return nullptr;
  if (slot < (int)bankA.size()) return &bankA[slot];
  const int j = slot - (int)bankA.size();
  if (j < (int)bankB.size()) return &bankB[j];
  return nullptr;
}

ArenaMatClass ArenaRenderData::polyMaterialClass(std::size_t polyIdx) const {
  if (polyIdx >= polys.size()) return ArenaMatClass::kUnresolved;
  const std::int16_t mi = polys[polyIdx].material;
  if (mi < 0) return arenaMatClassFor(mi);
  const ArenaRenderMaterial* m = materialFor(polyIdx);
  if (!m || m->pixels.empty()) return ArenaMatClass::kUnresolved;
  return ArenaMatClass::kTextured;
}

bool arenaRenderDataBuild(std::span<const std::byte> fileBytes,
                          const MtoBlock& block,
                          const CollisionArena& arena,
                          std::uint32_t nodeCount, std::uint32_t polyCount,
                          std::uint32_t vertCount,
                          std::span<const std::byte> sharedBankFile,
                          ArenaRenderData* out) {
  if (!out || !arena.verts || !arena.nodes || !arena.polys) return false;
  if (block.regionCOffset == 0 || block.regionCOffset >= fileBytes.size()) {
    return false;
  }
  ArenaRenderData d;
  d.verts = arena.verts;
  d.nodes = arena.nodes;
  d.vertCount = vertCount;
  d.nodeCount = nodeCount;
  d.polys.reserve(polyCount);
  for (std::uint32_t i = 0; i < polyCount; ++i) {
    d.polys.push_back(arenaRenderPolyDecode(arena.polys[i]));
  }
  d.materialNames.reserve(block.regionCNames.size());
  for (const MtoRegionCName& n : block.regionCNames) {
    d.materialNames.push_back(n.name());
  }

  // Bank A — the shared level bank (LEVELnS.MTI), searched FIRST.
  if (!sharedBankFile.empty()) {
    const MtiDirectory mti = inspectMtiDirectory(sharedBankFile);
    if (mti.status == MtiDirectoryStatus::kOk) {
      d.bankA.reserve(mti.entries.size());
      for (const MtiEntry& e : mti.entries) {
        ArenaRenderMaterial m;
        if (arenaRenderMaterialDecode(sharedBankFile,
                                      e.payloadFileOffset(),
                                      e.fieldAt0x08, e.fieldAt0x0C,
                                      e.fieldAt0x10, e.nameField, &m)) {
          d.bankA.push_back(std::move(m));
        }
      }
    }
  }

  // Bank B — the arena's embedded ".MAT" file. Payload offsets are
  // img-relative where img = the embedded name field at block+0x14.
  const std::uint64_t imgBase =
      block.fileOffset + kMtoInnerFileOffset + kMtoInnerBlobOffset;
  d.bankB.reserve(block.innerRecords.size());
  for (const MtoInnerRecord& e : block.innerRecords) {
    ArenaRenderMaterial m;
    if (arenaRenderMaterialDecode(fileBytes, imgBase + e.fieldAt0x14,
                                  e.fieldAt0x08, e.fieldAt0x0C,
                                  e.fieldAt0x10, e.nameField, &m)) {
      d.bankB.push_back(std::move(m));
    }
  }

  // FUN_0041a694 — matlkup: per name slot, bank A first then bank B;
  // -1 = the original's fallback record.
  d.materialOfName.assign(d.materialNames.size(), -1);
  for (std::size_t i = 0; i < d.materialNames.size(); ++i) {
    const std::string& want = d.materialNames[i];
    for (std::size_t j = 0; j < d.bankA.size(); ++j) {
      if (nameMatch(d.bankA[j].name, want)) {
        d.materialOfName[i] = (int)j;
        break;
      }
    }
    if (d.materialOfName[i] >= 0) continue;
    for (std::size_t j = 0; j < d.bankB.size(); ++j) {
      if (nameMatch(d.bankB[j].name, want)) {
        d.materialOfName[i] = (int)(d.bankA.size() + j);
        break;
      }
    }
  }

  // Palette — region B RGB triplets (aliases the file).
  if (block.regionBOffset + block.regionBSize <= fileBytes.size()) {
    d.paletteRgb = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(fileBytes.data()) +
            block.regionBOffset,
        static_cast<std::size_t>(block.regionBSize));
  }

  *out = std::move(d);
  return true;
}

void arenaRenderOrder(const CollisionArena& arena,
                      const float camPos[3], bool mirror,
                      std::vector<std::uint32_t>* outOrder) {
  if (!outOrder || !arena.nodes || !arena.polys || !camPos) return;
  outOrder->clear();

  // Submit a node's poly span: span dword = [lo16 count | hi16 first].
  // Polys with +0x20 bit4 (0x10) are skipped (FUN_00409860, OBSERVED).
  const auto submitSpan = [&](std::uint32_t span) {
    const std::uint32_t count = span & 0xffff;
    const std::uint32_t first = span >> 16;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t pi = first + i;
      if (arena.polys[pi].flags & 0x10) continue;
      outOrder->push_back(pi);
    }
  };

  const auto dist = [&](const CollisionNode* n) {
    return camPos[1] * n->ny + camPos[0] * n->nx + camPos[2] * n->nz +
           n->d;
  };

  // Per dist sign (and the dead mirror flag), which child is entered
  // first, which span is submitted, and which child is descended last.
  const auto firstChild = [&](const CollisionNode* n, bool pos) {
    return pos ? (mirror ? n->childFar : n->childNear)
               : (mirror ? n->childNear : n->childFar);
  };
  const auto lastChild = [&](const CollisionNode* n, bool pos) {
    return pos ? (mirror ? n->childNear : n->childFar)
               : (mirror ? n->childFar : n->childNear);
  };
  const auto span = [&](const CollisionNode* n, bool pos) {
    return pos ? n->polysPos : n->polysNeg;
  };

  // Iterative equivalent of the original's recurse + tail-descend:
  // `pending` holds nodes whose span-submit + lastChild-descend is
  // owed once their first-subtree traversal returns.
  std::vector<std::int32_t> pending;
  std::int32_t ni = 0;
  for (;;) {
    while (ni >= 0) {
      const CollisionNode* node = arena.nodes + ni;
      const bool pos = dist(node) > 0.0f;
      const std::int16_t fc = firstChild(node, pos);
      if (fc >= 0) {
        pending.push_back(ni);
        ni = fc;
        continue;
      }
      submitSpan(span(node, pos));
      ni = lastChild(node, pos);
    }
    if (pending.empty()) break;
    ni = pending.back();
    pending.pop_back();
    const CollisionNode* node = arena.nodes + ni;
    const bool pos = dist(node) > 0.0f;
    submitSpan(span(node, pos));
    ni = lastChild(node, pos);
  }
}

} // namespace mdk
