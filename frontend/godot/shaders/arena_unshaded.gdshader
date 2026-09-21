// Phase 7 (G1) — original-mapped unshaded arena fetch.
//
// The original mapper indexes pixels[(v & vMask) | (u & uMask)] over
// an address space pitch x bucketH (pitch = uMask+1, bucketH =
// bucketMask+1 — both powers of two). Masks over a pow2 range are
// plain mod(), so the shader reproduces the exact fetch — including
// negative-coordinate wrap — without bitwise ops:
//   texel = atlas[origin + (floor(u) mod pitch, floor(v) mod bucketH)]
// floor() matches the fixed-point truncating rasterizer; mod() maps
// negatives the same way two's-complement & does.
//
// Per-vertex CUSTOM0 carries the material descriptor
// {atlasOriginX, atlasOriginY, pitch, bucketH} in atlas texels.
// Flat-color classes (pen / unresolved 0xff / fx placeholders) share
// the same path: they point at the LUT strip row with pitch=1,
// bucketH=1 so every fragment samples the one LUT texel.
//
// Unshaded + nearest: the 1997 palette look, no PBR, no filtering.
shader_type spatial;
render_mode unshaded, cull_disabled;

uniform sampler2D atlas : filter_nearest, repeat_disable;

varying vec4 mat_desc;
varying vec2 tex_uv;

void vertex() {
	mat_desc = CUSTOM0;
	tex_uv = UV;
}

void fragment() {
	float u = mod(floor(tex_uv.x), mat_desc.z);
	float v = mod(floor(tex_uv.y), mat_desc.w);
	vec4 c = texelFetch(
		atlas,
		ivec2(int(mat_desc.x) + int(u), int(mat_desc.y) + int(v)),
		0);
	ALBEDO = c.rgb;
}
