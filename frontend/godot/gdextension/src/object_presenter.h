// Phase 9 (G3) — RuntimeModel -> Godot mesh presentation.
//
// DynamicObject models parse through the proven FUN_00428400
// geometry record: per-element local f32 triples + 0x24-byte
// triangle records whose ONLY established field is u16 v[3] at +0
// (docs/GAMEPLAY_RECONSTRUCTION.md §42-49). The remaining record
// bytes — material index / UV semantics — are NOT evidenced for
// model records (the proven interior layout belongs to arena
// region-C polys), so this presenter emits geometry only: one
// indexed surface per element carrying the verbatim local verts
// (P-converted) and triangle indices. GDScript assigns the
// deterministic debug materials — see docs/GODOT_FRONTEND.md.
//
// `surfaceElems` aligns mesh surface index -> element index so the
// caller can hide surfaces the core's element-disable mask masks
// (connMaskLock/HC door masks, SW_DUMMY bits, +0x2c8).
//
// geomKey is an FNV-1a identity digest over the immutable geometry
// content (flag, name table, element names, verts, tris). Two
// objects whose deep-copied models hold byte-identical geometry
// share the same key and may share the built mesh; any future
// deformation path that mutates verts changes the key and forces a
// rebuild — sharing is a presentation cache, never ownership.
#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <cstdint>

#include "core/dynamic_objects.h"

namespace godot {

struct ObjectGeometry {
  Ref<ArrayMesh> mesh;             // one surface per element w/ tris
  PackedInt32Array surfaceElems;   // surface -> element index
  PackedStringArray elemNames;     // all elements (size = elem count)
  int64_t vertCount = 0;           // total local verts
  int64_t triCount = 0;            // total parsed tris
  std::uint64_t geomKey = 0;
};

// Identity digest over the model's immutable geometry content.
std::uint64_t objectGeomKey(const mdk::RuntimeModel& m);

// Build the indexed per-element mesh. Malformed triangle indices
// (>= element vert count) are dropped individually — the core's
// hardening contract — never fatal.
ObjectGeometry objectGeometryFromModel(const mdk::RuntimeModel& m);

}  // namespace godot
