#ifndef ISOMATA_RENDER_MATH3D_H
#define ISOMATA_RENDER_MATH3D_H

/*
 * Minimal 3D math for the render tier: Vec3/Vec4 and a 4x4 matrix.
 *
 * MEMORY CONVENTION (pinned end to end):
 *   Mat4.m is COLUMN-MAJOR: m[col * 4 + row]. Column 0 is m[0..3], the
 *   translation column is m[12..14], and m[15] is the homogeneous 1.
 *   This is the layout a GLSL `mat4` occupies in a std140 uniform block,
 *   so a Mat4 can be pushed to the GPU byte-for-byte and used as
 *   `uViewProj * vec4(pos, 1.0)` with no transpose. mat4Multiply computes
 *   the true column-major product; the ortho/look-at builders write
 *   entries in that same layout (tests assert exact entries).
 *
 * Handedness / clip space: right-handed world, VULKAN-style clip space
 * (x right, y down after the viewport flip, z in [0, 1] from near to far).
 * SDL_gpu's only shader format on this host is SPIR-V for Vulkan, whose
 * clip volume is 0 <= z <= w; glslangValidator does NOT remap gl_Position.z,
 * so the projection must already produce [0, 1] (the static-quad render
 * proved this empirically: a GL-style [-1, 1] ortho is depth-clipped).
 * Mat4 is plain data (no padding, 64 bytes) so it is safe to memcpy into a
 * GPU uniform.
 *
 * Pure module: no SDL, no allocation, no hidden state.
 */

typedef struct Vec3 {
	float x;
	float y;
	float z;
} Vec3;

typedef struct Vec4 {
	float x;
	float y;
	float z;
	float w;
} Vec4;

typedef struct Mat4 {
	float m[16];	/* column-major: m[col * 4 + row] */
} Mat4;

/* Identity. */
Mat4 mat4Identity(void);

/* a * b (column-major product; applies b first when transforming a
 * column vector). */
Mat4 mat4Multiply(const Mat4 *a, const Mat4 *b);

/* Translation by (x, y, z): identity with the translation in column 3. */
Mat4 mat4Translate(float x, float y, float z);

/* Orthographic projection mapping the box [l,r]x[b,t]x[n,f] (n, f are
 * distances along -Z) to VULKAN clip space: x/y in [-1, 1] and z in [0, 1]
 * (near -> 0, far -> 1). Exact entries are pinned by the tests. */
Mat4 mat4Ortho(float left, float right, float bottom, float top, float near,
	       float far);

/* Right-handed look-at view matrix: eye maps to the origin, target lands
 * on the -Z axis at minus the eye->target distance. `up` must not be
 * parallel to the view direction (the isometric camera's fixed pitch keeps
 * this satisfied). */
Mat4 mat4LookAt(Vec3 eye, Vec3 target, Vec3 up);

/* Transform a point (w = 1) by m; returns the homogeneous result. */
Vec4 mat4TransformPoint(const Mat4 *m, Vec3 p);

#endif /* ISOMATA_RENDER_MATH3D_H */
