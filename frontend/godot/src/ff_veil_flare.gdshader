shader_type spatial;
// Kind-5 launch FLARE — 0x46d6d1: texel 0 transparent, else
// dst = lut[6 + count + srcPx][dst] where srcPx is the FLARE4 texel
// index (0..8). Same rgb565 index recovery as ff_veil.gdshader; the
// row rides on the source texel instead of a uniform.
// No depth_draw mode — the transparent pass depth-tests against the
// opaque buffer (the 10u camera-offset handles the front-of-body
// layering; anything nearer still occludes it, per the depth seam).
render_mode unshaded, cull_disabled, fog_disabled;

uniform sampler2D screen_tex : hint_screen_texture, filter_nearest;
uniform sampler2D src_tex : filter_nearest, repeat_disable; // R8 FLARE4 indices
uniform sampler2D idx_tex : filter_nearest, repeat_disable;
uniform sampler2D lut_tex : filter_nearest, repeat_disable;
uniform sampler2D pal_tex : filter_nearest, repeat_disable;
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
	float ni = texelFetch(lut_tex,
		ivec2(int(ix * 255.0 + 0.5), int(base_row) + spi), 0).r;
	vec3 rgb = texture(pal_tex,
		vec2((floor(ni * 255.0 + 0.5) + 0.5) / 256.0, 0.5)).rgb;
	ALBEDO = rgb;
	ALPHA = 1.0;
}
