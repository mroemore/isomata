#version 450

/*
 * grid.frag.glsl — infinite ground-grid fragment stage (GLSL -> SPIR-V).
 *
 * Reuses world.vert.spv verbatim: that vertex stage outputs vUV and vColor,
 * and buildGridQuad sets UV = world XZ, so vUV carries world coordinates here.
 *
 * SDL_gpu SPIR-V resource-set convention (SDL_gpu.h, SDL_CreateGPUShader):
 * fragment uniform buffers live at set 3, so the fade block is bound at set 3
 * binding 0 — the slot SDL_PushGPUFragmentUniformData(command_buffer, 0, ...)
 * writes. (world.vert.glsl documents the same convention for the vertex UBO.)
 *
 * Lines every GRID_MINOR_SPACING world units (subtle) and every
 * GRID_MAJOR_SPACING (brighter), anti-aliased with fwidth, with a radial
 * alpha fade from fadeStart to fadeEnd so the quad edge is never a hard cut.
 * The line colour comes from vColor.
 */

layout(location = 0) in vec2 vUV;	/* world XZ */
layout(location = 1) in vec4 vColor;	/* line colour */

layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0, std140) uniform GridUBO {
	vec4 uCenterFade;	/* xy = centre XZ, z = fadeStart, w = fadeEnd */
} ubo;

#define GRID_MINOR_SPACING 1.0
#define GRID_MAJOR_SPACING 8.0
#define GRID_MINOR_ALPHA 0.45	/* minor lines are subtle */
#define GRID_MAJOR_BOOST 0.9	/* major lines are ~1.9x brighter */

/* 1 at a line centre, falling to 0 one screen pixel away (fwidth is the world
 * units per pixel). */
float lineMask(vec2 world, float spacing) {
	vec2 c = world / spacing;
	vec2 d = abs(fract(c - 0.5) - 0.5) / max(fwidth(c), vec2(1e-6));
	return 1.0 - min(min(d.x, d.y), 1.0);
}

void main() {
	vec2 world = vUV;
	float minor = lineMask(world, GRID_MINOR_SPACING);
	float major = lineMask(world, GRID_MAJOR_SPACING);
	float dist = distance(world, ubo.uCenterFade.xy);
	float fade = 1.0 - smoothstep(ubo.uCenterFade.z, ubo.uCenterFade.w, dist);
	float alpha = max(minor * GRID_MINOR_ALPHA, major) * fade;
	vec3 rgb = vColor.rgb * (1.0 + major * GRID_MAJOR_BOOST);
	outColor = vec4(rgb, alpha * vColor.a);
}
