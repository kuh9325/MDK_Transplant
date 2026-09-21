// Phase 7 (G1) — rasterizer-replacement arena mesh bundle.
// See arena_mesh.h for the evidence summary and field semantics.

#include "core/arena_mesh.h"

#include <cstring>

namespace mdk {

namespace {

std::uint64_t fnv1a(std::uint64_t h, const void* p, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    h = (h ^ b[i]) * 0x100000001b3ull;
  }
  return h;
}

}  // namespace

bool arenaPaletteCompose(std::span<const std::uint8_t> sysPalHead,
                         std::span<const std::uint8_t> levelPal,
                         std::span<const std::uint8_t> regionB,
                         std::uint32_t paletteCount,
                         std::uint8_t outRgb[768]) {
  if (!outRgb) return false;
  if (!levelPal.empty() && levelPal.size() < 768) return false;
  if (!sysPalHead.empty() && sysPalHead.size() < 192) return false;

  // Entries [64,256): the DTI s3 tail first (FUN_004346e8 stages it
  // before region B lands), else black.
  std::memset(outRgb, 0, 768);
  if (!levelPal.empty()) {
    std::memcpy(outRgb + 192, levelPal.data() + 192, 576);
  }
  // Entries [0,64): the SYS_PAL head; entry 0 forced black
  // (FUN_0040163c — the file's entry 0 is magenta and never shown).
  if (!sysPalHead.empty()) {
    std::memcpy(outRgb, sysPalHead.data(), 192);
    outRgb[0] = outRgb[1] = outRgb[2] = 0;
  }
  // Entries [64,64+count): the arena region-B overlay
  // (FUN_004321dc -> FUN_0046d490(0x40, count, regionB)). Clamped to
  // the supplied triplets and the [64,256) window.
  std::size_t n = paletteCount;
  const std::size_t avail = regionB.size() / 3;
  if (n > avail) n = avail;
  if (n > 192) n = 192;
  std::memcpy(outRgb + 192, regionB.data(), n * 3);
  return true;
}

std::uint64_t arenaOrderDigest(std::span<const std::uint32_t> order) {
  return fnv1a(0xcbf29ce484222325ull, order.data(),
               order.size() * sizeof(std::uint32_t));
}

bool arenaMeshTexturesBuild(const ArenaRenderData& rd,
                            std::span<const std::uint8_t> palette768,
                            ArenaMeshTextures* out) {
  if (!out || palette768.size() < 768) return false;
  *out = ArenaMeshTextures{};

  // Expand every resolved bank slot a poly actually references —
  // the set is camera-independent (referenced, not submitted).
  const std::size_t slotCount = rd.bankA.size() + rd.bankB.size();
  out->texOfSlot.assign(slotCount, -1);
  std::vector<bool> used(slotCount, false);
  for (std::size_t p = 0; p < rd.polys.size(); ++p) {
    const std::int16_t mi = rd.polys[p].material;
    if (mi < 0 || std::size_t(mi) >= rd.materialOfName.size()) continue;
    const int slot = rd.materialOfName[mi];
    if (slot >= 0 && std::size_t(slot) < slotCount) used[slot] = true;
  }
  const auto recordOf = [&](int slot) -> const ArenaRenderMaterial* {
    if (slot < 0) return nullptr;
    if (std::size_t(slot) < rd.bankA.size()) {
      return &rd.bankA[slot];
    }
    const std::size_t bi = std::size_t(slot) - rd.bankA.size();
    return bi < rd.bankB.size() ? &rd.bankB[bi] : nullptr;
  };

  for (std::size_t slot = 0; slot < slotCount; ++slot) {
    if (!used[slot]) continue;
    const ArenaRenderMaterial* m = recordOf(static_cast<int>(slot));
    if (!m || m->pixels.empty()) continue;
    const std::size_t pitch = m->uMask + 1;
    const std::size_t bucketH = (m->vMask >> m->shift) + 1;
    const std::size_t addrBytes = pitch * bucketH;
    // Hardening: pitch is 2^shift with shift < 12, so cap the
    // addressable region rather than trusting corrupt fields.
    if (pitch > 4096 || addrBytes > 16u * 1024 * 1024) continue;
    ArenaMeshTexture t;
    t.name = m->name;
    t.pitch = static_cast<std::uint32_t>(pitch);
    t.bucketH = static_cast<std::uint32_t>(bucketH);
    t.rgba.resize(addrBytes * 4);
    for (std::size_t i = 0; i < addrBytes; ++i) {
      // The linear masked index IS the payload offset — idx ==
      // v'*pitch + u' reads payload[idx] (pitch == width for all
      // shipped content; for a hypothetical non-pow2 width this
      // pitch-strided layout still matches the original fetch).
      const std::uint8_t idx = i < m->pixels.size() ? m->pixels[i] : 0;
      t.rgba[i * 4 + 0] = palette768[idx * 3 + 0];
      t.rgba[i * 4 + 1] = palette768[idx * 3 + 1];
      t.rgba[i * 4 + 2] = palette768[idx * 3 + 2];
      t.rgba[i * 4 + 3] = 255;
    }
    out->texOfSlot[slot] =
        static_cast<std::int32_t>(out->textures.size());
    out->textures.push_back(std::move(t));
  }

  // Shelf-pack the texture regions into a fixed-width atlas, then
  // append the flat-color LUT strip as a final row.
  constexpr std::uint32_t kAtlasW = 2048;
  std::uint32_t x = 0, y = 0, shelfH = 0;
  for (auto& t : out->textures) {
    if (t.pitch > kAtlasW) return false;
    if (x + t.pitch > kAtlasW) {
      x = 0;
      y += shelfH;
      shelfH = 0;
    }
    t.atlasX = x;
    t.atlasY = y;
    x += t.pitch;
    if (t.bucketH > shelfH) shelfH = t.bucketH;
  }
  y += shelfH;
  out->lutX = 0;
  out->lutY = y;
  out->atlasW = kAtlasW;
  out->atlasH = y + 1;
  return true;
}

bool arenaMeshTrisEmit(const ArenaRenderData& rd,
                       const ArenaMeshTextures& texs,
                       std::span<const std::uint32_t> order,
                       std::vector<ArenaMeshTri>* outTris) {
  if (!outTris || !rd.verts || rd.vertCount == 0) return false;
  outTris->clear();
  outTris->reserve(order.size());
  // The order list already excludes +0x20-bit4 (render-skip) polys —
  // see arenaRenderOrder.
  for (const std::uint32_t pi : order) {
    if (pi >= rd.polys.size()) return false;
    const ArenaRenderPoly& p = rd.polys[pi];
    ArenaMeshTri t;
    t.poly = pi;
    t.cls = rd.polyMaterialClass(pi);
    t.flags = p.flags;
    t.aux22 = p.aux22;
    for (int k = 0; k < 3; ++k) {
      const std::uint16_t vi = p.v[k];
      if (vi >= rd.vertCount) return false;
      t.pos[k][0] = rd.verts[vi * 3 + 0];
      t.pos[k][1] = rd.verts[vi * 3 + 1];
      t.pos[k][2] = rd.verts[vi * 3 + 2];
      t.uv[k][0] = p.uv[k][0];
      t.uv[k][1] = p.uv[k][1];
    }
    switch (t.cls) {
    case ArenaMatClass::kTextured: {
      const int slot =
          std::size_t(p.material) < rd.materialOfName.size()
              ? rd.materialOfName[p.material]
              : -1;
      if (slot < 0 || std::size_t(slot) >= texs.texOfSlot.size() ||
          texs.texOfSlot[slot] < 0) {
        // Resolved-name decode gap — same flat 0xff as unresolved.
        t.cls = ArenaMatClass::kUnresolved;
        t.flatSlot = 0xff;
        break;
      }
      t.tex = texs.texOfSlot[slot];
      break;
    }
    case ArenaMatClass::kUnresolved:
      t.flatSlot = 0xff;
      break;
    case ArenaMatClass::kPen:
      t.flatSlot = arenaPenIndex(p.material);
      break;
    case ArenaMatClass::kEffect770:
      t.flatSlot = kArenaLutFx770;
      break;
    case ArenaMatClass::kEffectE94:
      t.flatSlot = kArenaLutFxE94;
      break;
    case ArenaMatClass::kEffect12970:
      t.flatSlot = kArenaLutFx12970;
      break;
    }
    outTris->push_back(t);
  }
  return true;
}

bool arenaMeshBuild(const ArenaRenderData& rd,
                    std::span<const std::uint32_t> order,
                    std::span<const std::uint8_t> palette768,
                    ArenaMeshBundle* out) {
  if (!out || !rd.verts || rd.vertCount == 0 || palette768.size() < 768) {
    return false;
  }
  *out = ArenaMeshBundle{};
  std::memcpy(out->palette.data(), palette768.data(), 768);

  // Census over all polys + geometry digest over the decoded view —
  // the same folds mdk-inspect --arena-render prints.
  for (std::size_t p = 0; p < rd.polys.size(); ++p) {
    ++out->clsCount[static_cast<int>(rd.polyMaterialClass(p))];
  }
  std::uint64_t h = 0xcbf29ce484222325ull;
  h = fnv1a(h, rd.verts, std::size_t(rd.vertCount) * 12);
  h = fnv1a(h, rd.polys.data(),
            rd.polys.size() * sizeof(ArenaRenderPoly));
  out->geomDigest = h;
  out->orderDigest = arenaOrderDigest(order);

  if (!arenaMeshTexturesBuild(rd, palette768, &out->texs)) {
    return false;
  }
  if (!arenaMeshTrisEmit(rd, out->texs, order, &out->tris)) {
    return false;
  }
  out->submitted = static_cast<std::uint32_t>(out->tris.size());
  return true;
}

}  // namespace mdk
