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

  Ref<ArrayMesh> mesh;
  mesh.instantiate();

  for (std::size_t e = 0; e < m.elems.size(); ++e) {
    g.elemNames.set(static_cast<int64_t>(e),
                    String(m.elemName(e).c_str()));
    const auto& src = m.elemVerts[e];
    const auto& tris = m.elemTris[e];
    const std::size_t vertCount = src.size() / 3;
    const std::size_t triCount = tris.size() / 0x24;
    g.vertCount += static_cast<int64_t>(vertCount);
    g.triCount += static_cast<int64_t>(triCount);
    if (vertCount == 0 || triCount == 0) continue;

    PackedVector3Array verts;
    verts.resize(static_cast<int64_t>(vertCount));
    for (std::size_t v = 0; v < vertCount; ++v) {
      verts.set(static_cast<int64_t>(v), mdkToGodotVec(&src[v * 3]));
    }
    PackedInt32Array indices;
    indices.resize(static_cast<int64_t>(triCount) * 3);
    std::int64_t bad = 0;
    for (std::size_t t = 0; t < triCount; ++t) {
      const std::uint8_t* rec = tris.data() + t * 0x24;
      for (int k = 0; k < 3; ++k) {
        std::uint16_t idx;
        std::memcpy(&idx, rec + k * 2, 2);
        if (idx >= vertCount) {
          idx = 0;   // malformed index — clamp, count below
          ++bad;
        }
        indices.set(static_cast<int64_t>(t) * 3 + k, idx);
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
    arrays[Mesh::ARRAY_INDEX] = indices;
    mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
    g.surfaceElems.push_back(static_cast<int32_t>(e));
  }
  g.mesh = mesh;
  return g;
}
