// Phase 9 (G3) — RuntimeModel -> Godot mesh presentation.
// See object_presenter.h for the format/evidence contract.

#include "object_presenter.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

#include "mdk_convert.h"

using namespace godot;

namespace {

std::uint64_t fnvAppend(std::uint64_t h, const void* p,
                        std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    h = (h ^ b[i]) * 0x100000001b3ull;
  }
  return h;
}

std::uint64_t fnvU32(std::uint64_t h, std::uint32_t v) {
  return fnvAppend(h, &v, sizeof(v));
}

// Shared surface emit — one surface per (element, material index)
// group in record encounter order; verts are expanded per-tri
// (non-indexed) so a vertex shared across materials keeps each
// surface's UV set. UVs stay in texture-pixel space — the surface
// material's uv1_scale folds in the material's {w,h} at bind time.
struct SurfEmit {
  PackedInt32Array elems;
  PackedInt32Array matIdx;
  PackedStringArray mats;
  PackedInt32Array penIdx;
  int64_t vertCount = 0;
  int64_t triCount = 0;
};

void emitMaterialSurfaces(const mdk::RuntimeModel& m,
                          const Ref<ArrayMesh>& mesh,
                          SurfEmit* out) {
  for (std::size_t e = 0; e < m.elems.size(); ++e) {
    const auto& src = m.elemVerts[e];
    const auto& tris = m.elemTris[e];
    const std::size_t vertCount = src.size() / 3;
    const std::size_t triCount = tris.size() / 0x24;
    out->triCount += static_cast<int64_t>(triCount);
    if (vertCount == 0 || triCount == 0) continue;

    // Group the element's tris by raw material index in encounter
    // order — the software path rasterized tris in record order;
    // the surface split is the same stream minus interleaving.
    std::vector<std::int16_t> mats;
    for (std::size_t t = 0; t < triCount; ++t) {
      const std::uint8_t* rec = tris.data() + t * 0x24;
      std::int16_t mi;
      std::memcpy(&mi, rec + 6, 2);
      bool seen = false;
      for (std::int16_t x : mats) seen |= (x == mi);
      if (!seen) mats.push_back(mi);
    }

    for (std::int16_t mi : mats) {
      std::size_t n = 0;
      for (std::size_t t = 0; t < triCount; ++t) {
        std::int16_t x;
        std::memcpy(&x, tris.data() + t * 0x24 + 6, 2);
        n += (x == mi);
      }
      PackedVector3Array verts;
      PackedVector2Array uvs;
      verts.resize(static_cast<int64_t>(n) * 3);
      uvs.resize(static_cast<int64_t>(n) * 3);
      std::size_t w = 0, bad = 0;
      for (std::size_t t = 0; t < triCount; ++t) {
        const std::uint8_t* rec = tris.data() + t * 0x24;
        std::int16_t x;
        std::memcpy(&x, rec + 6, 2);
        if (x != mi) continue;
        for (int k = 0; k < 3; ++k) {
          std::uint16_t idx;
          std::memcpy(&idx, rec + k * 2, 2);
          if (idx >= vertCount) { idx = 0; ++bad; }
          verts.set(static_cast<int64_t>(w),
                    mdkToGodotVec(&src[std::size_t(idx) * 3]));
          float uv[2];
          std::memcpy(uv, rec + 8 + std::size_t(k) * 8, 8);
          uvs.set(static_cast<int64_t>(w), Vector2(uv[0], uv[1]));
          ++w;
        }
      }
      if (bad != 0) {
        UtilityFunctions::printerr(
            "MdkBridge: model '", m.modelName().c_str(), "' elem ",
            int64_t(e), " has ", int64_t(bad),
            " out-of-range tri indices (clamped)");
      }

      Array arrays;
      arrays.resize(Mesh::ARRAY_MAX);
      arrays[Mesh::ARRAY_VERTEX] = verts;
      arrays[Mesh::ARRAY_TEX_UV] = uvs;
      mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
      out->vertCount += static_cast<int64_t>(w);
      out->elems.push_back(static_cast<int32_t>(e));
      out->matIdx.push_back(static_cast<int32_t>(mi));
      if (mi >= 0 &&
          std::size_t(mi) < m.names.size()) {
        std::string nm(m.names[std::size_t(mi)].name.data(),
                       strnlen(m.names[std::size_t(mi)].name.data(),
                               m.names[std::size_t(mi)].name.size()));
        out->mats.push_back(String(nm.c_str()));
        out->penIdx.push_back(-1);
      } else if (mi < 0) {
        out->mats.push_back(String());
        out->penIdx.push_back(
            static_cast<int32_t>((-mi) & 0xff));
      } else {
        out->mats.push_back(String());   // OOB — unresolved
        out->penIdx.push_back(-1);
      }
    }
  }
}

}  // namespace

std::uint64_t godot::objectGeomKey(const mdk::RuntimeModel& m) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  h = fnvU32(h, m.flag);
  h = fnvU32(h, std::uint32_t(m.names.size()));
  for (const auto& n : m.names) {
    h = fnvAppend(h, n.name.data(), n.name.size());
    h = fnvU32(h, n.tag);
  }
  h = fnvU32(h, std::uint32_t(m.elems.size()));
  for (std::size_t i = 0; i < m.elems.size(); ++i) {
    h = fnvAppend(h, m.elemNames[i].data(), m.elemNames[i].size());
    const auto& verts = m.elemVerts[i];
    const auto& tris = m.elemTris[i];
    h = fnvU32(h, std::uint32_t(verts.size()));
    if (!verts.empty()) {
      h = fnvAppend(h, verts.data(), verts.size() * sizeof(float));
    }
    h = fnvU32(h, std::uint32_t(tris.size()));
    if (!tris.empty()) {
      h = fnvAppend(h, tris.data(), tris.size());
    }
  }
  return h;
}

ObjectGeometry godot::objectGeometryFromModel(
    const mdk::RuntimeModel& m) {
  ObjectGeometry g;
  g.geomKey = objectGeomKey(m);
  g.elemNames.resize(static_cast<int64_t>(m.elems.size()));
  for (std::size_t e = 0; e < m.elems.size(); ++e) {
    g.elemNames.set(static_cast<int64_t>(e),
                    String(m.elemName(e).c_str()));
  }

  Ref<ArrayMesh> mesh;
  mesh.instantiate();
  SurfEmit emit;
  emitMaterialSurfaces(m, mesh, &emit);
  g.mesh = mesh;
  g.surfaceElems = emit.elems;
  g.surfaceMatIdx = emit.matIdx;
  g.surfaceMats = emit.mats;
  g.surfacePenIdx = emit.penIdx;
  g.vertCount = emit.vertCount;
  g.triCount = emit.triCount;
  return g;
}

FreefallGeometry godot::freefallGeometryFromModel(
    const mdk::RuntimeModel& m) {
  FreefallGeometry g;
  g.geomKey = objectGeomKey(m);
  g.elemNames.resize(static_cast<int64_t>(m.elems.size()));
  for (std::size_t e = 0; e < m.elems.size(); ++e) {
    g.elemNames.set(static_cast<int64_t>(e),
                    String(m.elemName(e).c_str()));
  }

  Ref<ArrayMesh> mesh;
  mesh.instantiate();
  SurfEmit emit;
  emitMaterialSurfaces(m, mesh, &emit);
  g.mesh = mesh;
  g.surfaceElems = emit.elems;
  g.surfaceMatIdx = emit.matIdx;
  g.surfaceMats = emit.mats;
  g.surfacePenIdx = emit.penIdx;
  g.vertCount = emit.vertCount;
  g.triCount = emit.triCount;
  return g;
}
