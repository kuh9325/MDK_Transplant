// Phase 9 (G3) — RuntimeModel -> Godot mesh presentation.
//
// DynamicObject models parse through the proven FUN_00428400
// geometry record: per-element local f32 triples + 0x24-byte
// triangle records (docs/GAMEPLAY_RECONSTRUCTION.md §42-49). The
// record's material index (s16 @+6) and pixel-space UV pairs
// (f32[3][2] @+8) are evidenced on the FALL3D family which shares
// the traversal record — CORROBORATED for traversal models, and
// consistent with the documented model +0x10 material-pointer table
// the parse fills via FUN_0041a694 (matlkup, one slot per name-table
// record). Surfaces are grouped one per (element, material index)
// so GDScript can bind each group's resolved material; a material
// that fails the bank lookup takes the original's flat-0xff arm.
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
  Ref<ArrayMesh> mesh;             // one surface per (elem,mat) group
  PackedInt32Array surfaceElems;   // surface -> element index
  PackedInt32Array surfaceMatIdx;  // surface -> raw s16 at tri+6
  PackedStringArray surfaceMats;   // surface -> name-table string
                                   //   ("" for negative/OOB index)
  PackedInt32Array surfacePenIdx;  // surface -> palette index for
                                   //   raw-negative tris, else -1
  PackedStringArray elemNames;     // all elements (size = elem count)
  int64_t vertCount = 0;           // expanded output verts
  int64_t triCount = 0;            // total parsed tris
  std::uint64_t geomKey = 0;
};

// Identity digest over the model's immutable geometry content.
std::uint64_t objectGeomKey(const mdk::RuntimeModel& m);

// Build the per-(element, material) mesh. Malformed triangle
// indices (>= element vert count) are clamped individually — the
// core's hardening contract — never fatal.
ObjectGeometry objectGeometryFromModel(const mdk::RuntimeModel& m);

// Phase 16C — freefall model geometry. FALL3D records share the
// traversal 0x24-byte triangle record AND evidence the fields the
// G3 path skips: the model name table is the material-name list
// (KURT's {CB3,CF3} resolve in FALL3D_<course>.MTI), the s16 at
// tri+6 selects it (negative = palette pen, (-mat)&0xff), and
// f32 uv[3][2] at tri+8 holds pixel-space texcoords.
//
// Surfaces are grouped one per (element, material index) in record
// encounter order; verts are expanded per-tri (non-indexed) so a
// vertex shared across materials keeps each surface's UV set. UVs
// stay in texture-pixel space — the surface material's uv1_scale
// folds in the material's {w,h} at bind time.
//
// For animated models (the freefall twins) elemVerts mutate per
// frame — geomKey is objectGeomKey(m), so every mutated frame
// re-keys and the caller rebuilds (same contract as G3).
struct FreefallGeometry {
  Ref<ArrayMesh> mesh;
  PackedInt32Array surfaceElems;    // surface -> element index
  PackedInt32Array surfaceMatIdx;   // surface -> raw s16 at tri+6
  PackedStringArray surfaceMats;    // surface -> name-table string
                                    //   ("" for negative/OOB index)
  PackedInt32Array surfacePenIdx;   // surface -> palette index for
                                    //   raw-negative tris, else -1
  PackedStringArray elemNames;      // all elements
  int64_t vertCount = 0;            // expanded output verts
  int64_t triCount = 0;
  std::uint64_t geomKey = 0;
};

FreefallGeometry freefallGeometryFromModel(const mdk::RuntimeModel& m);

}  // namespace godot
