shader_type spatial;
// FUN_00412970/FUN_0040c860 veil — dst = lut[row][dst] applied to the
// already-composited screen pixels. The rgb565 lookup is NOT proven
// lossless: 256 palette entries map into 65536 keys, later entries
// overwrite earlier same-key entries, and uninitialized keys read 0.
// Recovery is exact only where the sampled screen color is
// palette-exact (unshaded, nearest-filtered presentation) AND every
// colliding index yields an equivalent LUT output — the §4B audit
// tracks both conditions per course palette. lut_tex then applies the
// remap and pal_tex resolves the result. Depth-tested like ordinary
// geometry, so veil sections composite at their own depth instead of
// sitting under every body (the indexed-compositor seam this replaces).
// Residual seam: screen_texture holds only the opaque pass — veil
// pixels under an earlier veil apply one remap, not the serial chain.
// No depth_draw mode: ALPHA puts the surface in the transparent pass,
// which depth-tests against the opaque buffer without writing — the
// overlap contract (veil sections composite at their own depth).
render_mode unshaded, cull_disabled, fog_disabled;

uniform sampler2D screen_tex : hint_screen_texture, filter_nearest;
uniform sampler2D idx_tex : filter_nearest, repeat_disable; // 256x256 R8: rgb565 -> index
uniform sampler2D lut_tex : filter_nearest, repeat_disable; // 256x64 R8: index -> remapped
uniform sampler2D pal_tex : filter_nearest, repeat_disable; // 256x1 RGB8: index -> color
uniform float lut_row = 0.0;
uniform bool row_from_color = false;   // trail: per-tri row via vertex COLOR.r

void fragment() {
	vec4 dst = texture(screen_tex, SCREEN_UV);
	ivec3 c = ivec3(dst.rgb * 255.0 + 0.5);
	int k = ((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3);
	float ix = texelFetch(idx_tex, ivec2(k & 255, k >> 8), 0).r;
	float row = row_from_color ? floor(COLOR.r * 255.0 + 0.5) : lut_row;
	float ni = texelFetch(lut_tex,
		ivec2(int(ix * 255.0 + 0.5), int(row)), 0).r;
	vec3 rgb = texture(pal_tex,
		vec2((floor(ni * 255.0 + 0.5) + 0.5) / 256.0, 0.5)).rgb;
	ALBEDO = rgb;
	ALPHA = 1.0;
}
