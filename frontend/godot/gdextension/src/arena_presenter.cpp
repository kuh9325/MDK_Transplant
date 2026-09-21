// Phase 7 (G1) — ArenaMeshTextures/ArenaMeshTri -> Godot objects.
// See arena_presenter.h for the design summary.

#include "arena_presenter.h"

#include <cstring>

#include "mdk_convert.h"

using namespace godot;

namespace {

// Documented placeholder colors for the fx dispatch classes (tagged,
// not original). Slots match kArenaLutFx770/E94/12970.
constexpr std::uint8_t kFxPlaceholderRgb[3][3] = {
    {64, 128, 224},   // fx770    — cool blue
    {224, 160, 48},   // fxe94    — amber
    {96, 200, 128},   // fx12970  — pale green
};

}  // namespace

Ref<Image> godot::arenaAtlasImage(const mdk::ArenaMeshTextures& texs,
                                  const std::array<std::uint8_t, 768>& palette,
                                  Ref<ImageTexture>* outTexture) {
  Ref<Image> img;
  if (texs.atlasW == 0 || texs.atlasH == 0) {
    return img;
  }
  const std::size_t w = texs.atlasW, h = texs.atlasH;
  PackedByteArray bytes;
  bytes.resize(static_cast<int64_t>(w * h * 4));
  std::uint8_t* px = bytes.ptrw();
  std::memset(px, 0, w * h * 4);  // RGB = 0
  for (std::size_t i = 0; i < w * h; ++i) px[i * 4 + 3] = 255;

  for (const mdk::ArenaMeshTexture& t : texs.textures) {
    for (std::uint32_t row = 0; row < t.bucketH; ++row) {
      std::memcpy(px + ((std::size_t(t.atlasY) + row) * w + t.atlasX) * 4,
                  t.rgba.data() + std::size_t(row) * t.pitch * 4,
                  std::size_t(t.pitch) * 4);
    }
  }

  // LUT strip: [0,256) palette entries, then the fx placeholders.
  std::uint8_t* lut = px + (std::size_t(texs.lutY) * w + texs.lutX) * 4;
  for (std::uint32_t i = 0; i < mdk::kArenaLutPaletteSlots; ++i) {
    lut[i * 4 + 0] = palette[i * 3 + 0];
    lut[i * 4 + 1] = palette[i * 3 + 1];
    lut[i * 4 + 2] = palette[i * 3 + 2];
    lut[i * 4 + 3] = 255;
  }
  for (std::uint32_t k = 0; k < 3; ++k) {
    std::uint8_t* d = lut + (mdk::kArenaLutFx770 + k) * 4;
    d[0] = kFxPlaceholderRgb[k][0];
    d[1] = kFxPlaceholderRgb[k][1];
    d[2] = kFxPlaceholderRgb[k][2];
    d[3] = 255;
  }

  img = Image::create_from_data(static_cast<int>(w), static_cast<int>(h),
                                false, Image::FORMAT_RGBA8, bytes);
  if (outTexture) {
    *outTexture = ImageTexture::create_from_image(img);
  }
  return img;
}

Ref<ArrayMesh> godot::arenaArrayMesh(
    const mdk::ArenaMeshTextures& texs,
    const std::vector<mdk::ArenaMeshTri>& tris,
    PackedVector3Array* outPositions, PackedVector2Array* outUvs,
    PackedFloat32Array* outMatDesc) {
  PackedVector3Array positions;
  PackedVector2Array uvs;
  // ARRAY_CUSTOM_RGBA_FLOAT takes a flat PackedFloat32Array —
  // 4 floats per vertex (RenderingServer rejects Vector4 arrays).
  PackedFloat32Array matDesc;
  const std::size_t n = tris.size() * 3;
  positions.resize(static_cast<int64_t>(n));
  uvs.resize(static_cast<int64_t>(n));
  matDesc.resize(static_cast<int64_t>(n * 4));

  for (std::size_t i = 0; i < tris.size(); ++i) {
    const mdk::ArenaMeshTri& t = tris[i];
    float desc[4];
    if (t.tex >= 0 &&
        std::size_t(t.tex) < texs.textures.size()) {
      const mdk::ArenaMeshTexture& tx = texs.textures[t.tex];
      desc[0] = static_cast<float>(tx.atlasX);
      desc[1] = static_cast<float>(tx.atlasY);
      desc[2] = static_cast<float>(tx.pitch);
      desc[3] = static_cast<float>(tx.bucketH);
    } else {
      // Flat classes sample a single LUT-strip texel.
      desc[0] = static_cast<float>(texs.lutX + t.flatSlot);
      desc[1] = static_cast<float>(texs.lutY);
      desc[2] = 1.0f;
      desc[3] = 1.0f;
    }
    for (int k = 0; k < 3; ++k) {
      const std::size_t vi = i * 3 + k;
      positions.set(static_cast<int64_t>(vi),
                    mdkToGodotVec(t.pos[k]));
      uvs.set(static_cast<int64_t>(vi),
              Vector2(t.uv[k][0], t.uv[k][1]));
      for (int c = 0; c < 4; ++c) {
        matDesc.set(static_cast<int64_t>(vi * 4 + c), desc[c]);
      }
    }
  }

  if (outPositions) *outPositions = positions;
  if (outUvs) *outUvs = uvs;
  if (outMatDesc) *outMatDesc = matDesc;

  Ref<ArrayMesh> mesh;
  mesh.instantiate();
  if (tris.empty()) {
    return mesh;
  }
  Array arrays;
  arrays.resize(Mesh::ARRAY_MAX);
  arrays[Mesh::ARRAY_VERTEX] = positions;
  arrays[Mesh::ARRAY_TEX_UV] = uvs;
  arrays[Mesh::ARRAY_CUSTOM0] = matDesc;
  const uint64_t flags =
      Mesh::ARRAY_FORMAT_VERTEX | Mesh::ARRAY_FORMAT_TEX_UV |
      Mesh::ARRAY_FORMAT_CUSTOM0 |
      (uint64_t(Mesh::ARRAY_CUSTOM_RGBA_FLOAT)
       << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT);
  mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays,
                                Array(), Dictionary(),
                                BitField<Mesh::ArrayFormat>(
                                    static_cast<int64_t>(flags)));
  return mesh;
}
