// Phase 7 (G1) — translucent arm for the original's LUT-remap class.
//
// Poly material indices -1027..-1024 / <=-1029 dispatch to
// FUN_00412970 (DAT_00540b20 + idx*256) — a screen-space framebuffer
// remap that produces the energy-barrier / force-field / shimmer
// surfaces the player sees through. The exact LUT target is UNKNOWN
// (docs/reverse-engineering/ARENA_RENDER_PIPELINE.md); the faithful
// observable property is that the surface is TRANSLUCENT (geometry
// behind shows through, tinted). We reproduce that as an alpha-blend
// overlay sampling the per-class LUT placeholder tint, at a partial
// alpha so the occluded geometry remains visible.
//
// Same texel fetch as arena_unshaded.gdshader (the flat tris sample
// one LUT-strip cell). ALPHA<1 routes this surface to the transparent
// pass; depth_draw_never keeps it from occluding the painter-ordered
// scene it must blend over. cull_disabled matches the unbanked view.
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;

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
	ALPHA = 0.45;
}
