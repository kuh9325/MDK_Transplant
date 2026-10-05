shader_type spatial;
// The radar wedge — same dst = lut[row][dst] op as ff_veil.gdshader.
// Its object key is the smallest obj.z (the radar sits at pz = 0),
// so the original's far-to-near walk draws it FIRST; nearer bodies
// then overwrite it. §4C — mask_tex's ordered records replay that
// painter semantics: wedge elements apply only where they pass the
// depth gate against the opaque winner, and the shader still needs
// no depth test + highest priority so its fragments reach px where
// the chain must write (records buried under a body write the
// winner's color back — an identity write — so covered px stay raw).
render_mode unshaded, cull_disabled, depth_test_disabled,
	depth_draw_never, fog_disabled;

uniform sampler2D screen_tex : hint_screen_texture, filter_nearest;
uniform sampler2D idx_tex : filter_nearest, repeat_disable;
uniform sampler2D lut_tex : filter_nearest, repeat_disable;
uniform sampler2D pal_tex : filter_nearest, repeat_disable;
uniform sampler2D mask_tex : filter_nearest, repeat_disable;
uniform sampler2D depth_tex : hint_depth_texture, filter_nearest;
uniform float lut_row = 0.0;

void fragment() {
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
	// Same three cases as ff_veil: chain applied, records all buried
	// (winner color stands), or no records (coverage-mismatch
	// fallback to this quad's own remap).
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
		ix = texelFetch(lut_tex,
			ivec2(int(ix * 255.0 + 0.5), int(lut_row)), 0).r;
	}
	vec3 rgb = texture(pal_tex,
		vec2((floor(ix * 255.0 + 0.5) + 0.5) / 256.0, 0.5)).rgb;
	ALBEDO = rgb;
	ALPHA = 1.0;
}
