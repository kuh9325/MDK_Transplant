shader_type spatial;
// Kind-5 launch FLARE — 0x46d6d1: texel 0 transparent, else
// dst = lut[6 + count + srcPx][dst] where srcPx is the FLARE4 texel
// index (0..8). Same rgb565 index recovery as ff_veil.gdshader (NOT
// proven lossless — palette-key collisions keep whichever index was
// written last; the §4B audit tracks whether any collision changes
// a used row's output); the row rides on the source texel instead
// of a uniform.
// §4C — mask_tex's ordered records replay the serial chain (the
// flare's own {row,z'} elements are recorded per srcPx, so the
// per-texel rows still apply inside the chain); empty px fall back
// to this texel's own remap.
// No depth_draw mode — the transparent pass depth-tests against the
// opaque buffer (the 10u camera-offset handles the front-of-body
// layering; anything nearer still occludes it, per the depth seam).
render_mode unshaded, cull_disabled, fog_disabled;

uniform sampler2D screen_tex : hint_screen_texture, filter_nearest;
uniform sampler2D src_tex : filter_nearest, repeat_disable; // R8 FLARE4 indices
uniform sampler2D idx_tex : filter_nearest, repeat_disable;
uniform sampler2D lut_tex : filter_nearest, repeat_disable;
uniform sampler2D pal_tex : filter_nearest, repeat_disable;
uniform sampler2D mask_tex : filter_nearest, repeat_disable;
uniform sampler2D depth_tex : hint_depth_texture, filter_nearest;
uniform float base_row = 6.0;   // 6 + count (obj+0x108 launch ramp)

void fragment() {
	float sp = texture(src_tex, UV).r;
	int spi = int(sp * 255.0 + 0.5);
	if (spi == 0) {
		discard;
	}
	vec4 dst = texture(screen_tex, SCREEN_UV);
	ivec3 c = ivec3(dst.rgb * 255.0 + 0.5);
	int k = ((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3);
	float ix = texelFetch(idx_tex, ivec2(k & 255, k >> 8), 0).r;
	float d_raw = texture(depth_tex, SCREEN_UV).r;
	vec3 ndc = vec3(SCREEN_UV * 2.0 - 1.0, d_raw);
	vec4 vp = INV_PROJECTION_MATRIX * vec4(ndc, 1.0);
	float win_z = -vp.z / vp.w;
	ivec2 mp = clamp(ivec2(SCREEN_UV * vec2(600.0, 360.0)),
		ivec2(0), ivec2(599, 359));
	// Records exist -> replay the gated chain (all-buried records
	// leave the winner's color standing); no records -> this texel's
	// own remap as the coverage fallback.
	bool any_rec = false;
	for (int j = 0; j < 4; ++j) {
		vec4 rec = texelFetch(mask_tex,
			ivec2(mp.x * 4 + j, mp.y), 0);
		if (rec.x >= 0.0) {
			any_rec = true;
			if (rec.y < win_z)
				ix = texelFetch(lut_tex, ivec2(
					int(ix * 255.0 + 0.5), int(rec.x)), 0).r;
		}
		if (rec.z >= 0.0) {
			any_rec = true;
			if (rec.w < win_z)
				ix = texelFetch(lut_tex, ivec2(
					int(ix * 255.0 + 0.5), int(rec.z)), 0).r;
		}
	}
	if (!any_rec) {
		ix = texelFetch(lut_tex, ivec2(
			int(ix * 255.0 + 0.5), int(base_row) + spi), 0).r;
	}
	vec3 rgb = texture(pal_tex,
		vec2((floor(ix * 255.0 + 0.5) + 0.5) / 256.0, 0.5)).rgb;
	ALBEDO = rgb;
	ALPHA = 1.0;
}
