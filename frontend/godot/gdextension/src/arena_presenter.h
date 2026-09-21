// Phase 7 (G1) — ArenaMeshTextures/ArenaMeshTri -> Godot objects.
//
// Builds the palette-expanded RGBA atlas (texture regions + the
// flat-color LUT strip) and the ordered triangle-soup ArrayMesh.
// Vertex positions arrive in MDK space and are converted here via
// mdk_convert.h; material descriptors travel per-vertex in CUSTOM0
// ({atlasX, atlasY, pitch, bucketH} texel units — the shader does
// mod() fetches so the original mask/wrap semantics hold per pixel).
#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <vector>

#include "core/arena_mesh.h"

namespace godot {

// Palette strip + the documented placeholder colors for the effect
// dispatch classes (NOT original colors — stand-ins until the fx
// drawers are reconstructed; see docs/GODOT_FRONTEND.md).
Ref<Image> arenaAtlasImage(const mdk::ArenaMeshTextures& texs,
                           const std::array<std::uint8_t, 768>& palette,
                           Ref<ImageTexture>* outTexture);

// Ordered triangle soup (non-indexed; the vertex order IS the
// painter's submission order — no depth reliance). Arrays are also
// written to the optional out-params for snapshot verification.
Ref<ArrayMesh> arenaArrayMesh(const mdk::ArenaMeshTextures& texs,
                              const std::vector<mdk::ArenaMeshTri>& tris,
                              PackedVector3Array* outPositions,
                              PackedVector2Array* outUvs,
                              PackedFloat32Array* outMatDesc);

}  // namespace godot
