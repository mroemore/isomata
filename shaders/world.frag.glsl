#version 450

/*
 * world.frag.glsl — Task 7 textured-quad fragment stage (GLSL -> SPIR-V).
 *
 * The sampler is bound at set 2, binding 0 per the SDL_gpu SPIR-V fragment
 * resource-set convention (SDL_gpu.h, SDL_CreateGPUShader): set 2 holds
 * sampled textures. The pipeline's sampler is nearest/nearest/clamp for
 * pixel art. Output is texture * vertex color (tint).
 */

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vAlphaMode;

layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D uTexture;

void main() {
	vec4 tex = texture(uTexture, vUV) * vColor;
	/* Alpha modes: opaque and blend composite through the pipeline's fixed
	 * alpha blend; cutout discards low-alpha texels (foliage-style). */
	if (vAlphaMode > 0.5 && tex.a < 0.5)
		discard;
	outColor = tex;
}
