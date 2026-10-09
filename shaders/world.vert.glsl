#version 450

/*
 * world.vert.glsl — Task 7 textured-quad vertex stage (GLSL -> SPIR-V).
 *
 * SDL_gpu SPIR-V resource-set convention (SDL_gpu.h, SDL_CreateGPUShader):
 *   vertex shaders:   set 0 = sampled/storage textures, set 1 = uniform buffers
 *   fragment shaders: set 2 = sampled/storage textures, set 3 = uniform buffers
 * so the view-projection block is bound at set 1, binding 0 — the slot
 * SDL_PushGPUVertexUniformData(command_buffer, 0, ...) writes.
 *
 * The mat4 is column-major, matching Mat4 in math3d.h byte for byte.
 */

layout(location = 0) in vec3 inPosition;	/* world space */
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;		/* vertex tint */
layout(location = 3) in float inAlphaMode;	/* 1.0 = cutout discard */

layout(set = 1, binding = 0, std140) uniform UBO {
	mat4 uViewProj;
} ubo;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out float vAlphaMode;

void main() {
	gl_Position = ubo.uViewProj * vec4(inPosition, 1.0);
	vUV = inUV;
	vColor = inColor;
	vAlphaMode = inAlphaMode;
}
