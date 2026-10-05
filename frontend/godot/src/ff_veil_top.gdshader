shader_type spatial;
// The radar wedge — same dst = lut[row][dst] op as ff_veil.gdshader
// but painter-last like the original (its object key sorts z' ~= 0):
// no depth test and the highest render priority, so it always
// composites over every already-drawn surface.
render_mode unshaded, cull_disabled, depth_test_disabled,
	depth_draw_never, fog_disabled;

uniform sampler2D screen_tex : hint_screen_texture, filter_nearest;
uniform sampler2D idx_tex : filter_nearest, repeat_disable;
uniform sampler2D lut_tex : filter_nearest, repeat_disable;
uniform sampler2D pal_tex : filter_nearest, repeat_disable;
uniform float lut_row = 0.0;

void fragment() {
	vec4 dst = texture(screen_tex, SCREEN_UV);
	ivec3 c = ivec3(dst.rgb * 255.0 + 0.5);
	int k = ((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3);
	float ix = texelFetch(idx_tex, ivec2(k & 255, k >> 8), 0).r;
	float ni = texelFetch(lut_tex,
		ivec2(int(ix * 255.0 + 0.5), int(lut_row)), 0).r;
	vec3 rgb = texture(pal_tex,
		vec2((floor(ni * 255.0 + 0.5) + 0.5) / 256.0, 0.5)).rgb;
	ALBEDO = rgb;
	ALPHA = 1.0;
}
