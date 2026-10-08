/*
SKINTEST_SCENE.C

Skinned characters and the vertex-program effects drawn by the GX device
(port/wii/src/d3d8_gx.c), whose vertex programs run on the CPU
(nv2a_vsh_run.c), where there are no maps to load. Everything goes through
the XDK header's inline functions with the game's own programs, the
declaration they are made with, and the constants as the rasterizer sets
them:

- the camera as rasterizer_set_frustum_z puts it at c[-96] to c[-89] (the
  game's z-up world, its OpenGL-style depth, c[-89].w = 255.9375);
- three walking characters of eleven nodes, made of the game's compressed
  model vertices (struct model_vertex_compressed: position, three
  NORMPACKED3 vectors, NORMSHORT2 texture coordinates, two node index bytes
  that are node numbers times three, a node weight). Their node matrices go
  to c[-36] as rasterizer_set_model_skinning packs them (real_matrix4x3:
  forward, left, up, position, scale), and their lights to c[-79] to c[-69]
  as rasterizer_set_model_lighting does. Each is drawn with one of the
  model shader's skinned programs, rasterizer_set_vertex_shader_permutation
  (10, model vertex, permutation): 10 (point lights), 9 (reflection) and 17
  (planar fog);
- a rifle in the first one's right hand, a part of one node: program 27
  (reflection, one node), its node matrix the hand's;
- the second one in a plasma shell, as rasterizer_plasma_energy_draw draws
  an overshield: program 15 pushes each skinned vertex out along its normal
  and takes its noise coordinates from the vertex's own position, with the
  game's constants and pixel shader;
- grass: detail objects (rasterizer_xbox_detail_objects.c), sprites that
  program 33 builds on the CPU, one quad from four vertices of packed bytes,
  with the game's sprite, type and corner constants and the cell's origin
  and ground plane in v2 and v3.

There are two phases. The first (HERO_FRAMES) is the scene above, full
screen. The second (CHECK_FRAMES) is a check: the screen is split, and each
half is drawn from the same camera and the same poses. The left half is as
above: the bind-pose vertices skinned by the game's programs on the Wii.
The right half is skinned here instead, on the CPU, by the formula the
game's own debug code uses (rasterizer.c, rasterizer_debug_model_vertices:
node index byte / 3, weight / 32767, the node matrices' transform of
position and normal), written each frame to a dynamic vertex buffer and
drawn with one node, the identity. Everything after the skinning is the
same program on both sides, so the two halves must agree to the pixel,
less the rounding of the reference's normals to NORMPACKED3 and the odd
pixel on a triangle's edge. The plasma shell is drawn there with its
colors alone (the noise is placed by the unskinned position, which the
reference has no longer). skintest/check.py compares them.

The frames carry a mark in their top left corner (two cleared rectangles,
the same in both halves) that says which phase they are.

Timings go to the log (skintest_main.c writes it to the SD card): the
ticks the device spends on each draw, which in Dolphin are its emulated
time base, not a real Wii's.
*/

#include "d3d8_gx.h"
#include "nv2a_vsh_run.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* skintest_main.c */
unsigned long long skintest_ticks(void);
double skintest_ticks_to_microseconds(unsigned long long ticks);

/* ---------- the game's vertex programs */

static const unsigned long vertex_shader_code[] =
{
#include "rasterizer_xbox_vertex_shaders_data.inc"
};

/* rasterizer_xbox_vertex_shaders.c's offsets */
static const unsigned long program_offsets[67] =
{
	0x0000, 0x0078, 0x0140, 0x0308, 0x0470, 0x04F8, 0x0670, 0x0708, 0x0890, 0x0918, 0x0BF0, 0x1018,
	0x1210, 0x13E8, 0x1540, 0x1808, 0x1B70, 0x1D08, 0x2200, 0x2478, 0x2710, 0x27D8, 0x2910, 0x2A98,
	0x2BF0, 0x2E48, 0x31C0, 0x3308, 0x3490, 0x36B8, 0x3770, 0x39C8, 0x3F30, 0x4198, 0x4380, 0x4658,
	0x4900, 0x4CB8, 0x4DA0, 0x4E58, 0x4FB0, 0x5088, 0x51E0, 0x52E8, 0x54E0, 0x5648, 0x5A00, 0x5B88,
	0x5F00, 0x6158, 0x6280, 0x6708, 0x6830, 0x6938, 0x6A40, 0x6C78, 0x6DD0, 0x6E68, 0x7260, 0x72D8,
	0x7410, 0x7788, 0x7970, 0x7D38, 0x80D0, 0x8368, 0x8510,
};

/* rasterizer_xbox_vertex_shaders_initialize.c's declarations: the model
vertex (vertex_shader_declarations at 0x78) and the detail object's (0x20) */
static const DWORD model_declaration[] =
{
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40210004, 0x40240005, 0x40110006, 0xFFFFFFFF,
};
static const DWORD detail_object_declaration[] =
{
	0x20000000, 0x40340000, 0x40340009, 0x40150008, 0xFFFFFFFF,
};

enum
{
	_program_model_planar_fog = 17,
	_program_model_lights = 10,
	_program_model_reflection = 9,
	_program_model_reflection_one_node = 27,
	_program_plasma = 15,
	_program_detail_objects = 33,
};

static D3DDevice *device;
static DWORD shaders[67];

static DWORD shader(int program)
{
	if (!shaders[program])
	{
		const DWORD *declaration = program == _program_detail_objects ? detail_object_declaration : model_declaration;

		IDirect3DDevice8_CreateVertexShader(device, declaration,
			(const DWORD *)vertex_shader_code + program_offsets[program] / 4, &shaders[program], 0);
	}
	return shaders[program];
}

/* ---------- vectors and the game's matrices */

typedef float vector3[3];

/* real_matrix4x3: a point goes to scale * (forward x + left y + up z) + position */
struct matrix4x3
{
	float scale;
	vector3 forward, left, up, position;
};

static float dot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross(const float *a, const float *b, float *out)
{
	float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];

	out[0] = x; out[1] = y; out[2] = z;
}

static void normalize(float *v)
{
	float length = sqrtf(dot(v, v));

	if (length > 0.0f)
	{
		v[0] /= length; v[1] /= length; v[2] /= length;
	}
}

static void set3(float *v, float x, float y, float z)
{
	v[0] = x; v[1] = y; v[2] = z;
}

/* matrix4x3_transform_point and _vector, as the game's */
static void transform_point(const struct matrix4x3 *m, const float *p, float *out)
{
	int axis;
	float result[3];

	for (axis = 0; axis < 3; axis++)
		result[axis] = m->scale * (m->forward[axis] * p[0] + m->left[axis] * p[1] + m->up[axis] * p[2]) + m->position[axis];
	memcpy(out, result, sizeof(result));
}

static void transform_vector(const struct matrix4x3 *m, const float *v, float *out)
{
	int axis;
	float result[3];

	for (axis = 0; axis < 3; axis++)
		result[axis] = m->scale * (m->forward[axis] * v[0] + m->left[axis] * v[1] + m->up[axis] * v[2]);
	memcpy(out, result, sizeof(result));
}

static void matrix_identity(struct matrix4x3 *m)
{
	memset(m, 0, sizeof(*m));
	m->scale = 1.0f;
	m->forward[0] = m->left[1] = m->up[2] = 1.0f;
}

/* a * b: b's transform first */
static void matrix_multiply(const struct matrix4x3 *a, const struct matrix4x3 *b, struct matrix4x3 *out)
{
	struct matrix4x3 result;

	result.scale = a->scale * b->scale;
	transform_vector(a, b->forward, result.forward);
	transform_vector(a, b->left, result.left);
	transform_vector(a, b->up, result.up);
	/* the scale is kept apart, as the game keeps it */
	result.forward[0] /= a->scale; result.forward[1] /= a->scale; result.forward[2] /= a->scale;
	result.left[0] /= a->scale; result.left[1] /= a->scale; result.left[2] /= a->scale;
	result.up[0] /= a->scale; result.up[1] /= a->scale; result.up[2] /= a->scale;
	transform_point(a, b->position, result.position);
	*out = result;
}

/* a rotation by angle about a unit axis, around a point */
static void matrix_rotation_about(const float *axis, float angle, const float *center, struct matrix4x3 *out)
{
	float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
	float x = axis[0], y = axis[1], z = axis[2];
	float rotated[3];

	out->scale = 1.0f;
	/* columns: where x, y and z go */
	set3(out->forward, t * x * x + c, t * x * y + s * z, t * x * z - s * y);
	set3(out->left, t * x * y - s * z, t * y * y + c, t * y * z + s * x);
	set3(out->up, t * x * z + s * y, t * y * z - s * x, t * z * z + c);
	transform_vector(out, center, rotated);
	set3(out->position, center[0] - rotated[0], center[1] - rotated[1], center[2] - rotated[2]);
}

/* ---------- vertex formats */

/* struct model_vertex_compressed (rasterizer_model_types.h), in the Wii's
byte order as the map converter leaves it */
struct model_vertex
{
	float position[3];
	DWORD normal, binormal, tangent;
	short texcoord[2];
	BYTE nodes[2];
	short node_weight;
};

/* struct detail_object_vertex (rasterizer_xbox_detail_objects.c) */
struct detail_object_vertex
{
	BYTE position[3];
	BYTE color[3];
	WORD sprite;
};

static DWORD normpacked3(const float *v)
{
	long x = lrintf(v[0] * 1023.0f), y = lrintf(v[1] * 1023.0f), z = lrintf(v[2] * 511.0f);

	return ((DWORD)x & 0x7ff) | (((DWORD)y & 0x7ff) << 11) | (((DWORD)z & 0x3ff) << 22);
}

static void unpack3(DWORD packed, float *out)
{
	long x = (long)(packed << 21) >> 21, y = (long)(packed << 10) >> 21, z = (long)packed >> 22;

	out[0] = x / 1023.0f; out[1] = y / 1023.0f; out[2] = z / 511.0f;
}

/* ---------- the character */

enum
{
	_node_pelvis, _node_chest, _node_head,
	_node_left_upper_arm, _node_left_forearm, _node_right_upper_arm, _node_right_forearm,
	_node_left_thigh, _node_left_shin, _node_right_thigh, _node_right_shin,
	NODE_COUNT
};

/* each node's parent and the joint it turns about, in the bind pose (the
model's space: x forward, y left, z up, feet at 0) */
static const struct
{
	int parent;
	float joint[3];
} nodes[NODE_COUNT] =
{
	{ -1, { 0.0f, 0.0f, 0.98f } },
	{ _node_pelvis, { 0.0f, 0.0f, 1.08f } },
	{ _node_chest, { 0.0f, 0.0f, 1.56f } },
	{ _node_chest, { 0.0f, 0.27f, 1.5f } },
	{ _node_left_upper_arm, { 0.0f, 0.27f, 1.2f } },
	{ _node_chest, { 0.0f, -0.27f, 1.5f } },
	{ _node_right_upper_arm, { 0.0f, -0.27f, 1.2f } },
	{ _node_pelvis, { 0.0f, 0.11f, 0.95f } },
	{ _node_left_thigh, { 0.0f, 0.11f, 0.52f } },
	{ _node_pelvis, { 0.0f, -0.11f, 0.95f } },
	{ _node_right_thigh, { 0.0f, -0.11f, 0.52f } },
};

/* the limbs: a tube from one point to another, its radius at each end,
which node it follows and which it blends into at its start */
static const struct
{
	int node, blend;
	float from[3], to[3];
	float radius_from, radius_to;
} segments[] =
{
	{ _node_pelvis, -1, { 0.0f, 0.0f, 0.86f }, { 0.0f, 0.0f, 1.08f }, 0.17f, 0.16f },
	{ _node_chest, _node_pelvis, { 0.0f, 0.0f, 1.08f }, { 0.0f, 0.0f, 1.52f }, 0.16f, 0.21f },
	{ _node_head, _node_chest, { 0.0f, 0.0f, 1.52f }, { 0.0f, 0.0f, 1.84f }, 0.08f, 0.12f },
	{ _node_left_upper_arm, _node_chest, { 0.0f, 0.27f, 1.52f }, { 0.0f, 0.27f, 1.2f }, 0.075f, 0.06f },
	{ _node_left_forearm, _node_left_upper_arm, { 0.0f, 0.27f, 1.2f }, { 0.0f, 0.27f, 0.9f }, 0.06f, 0.045f },
	{ _node_right_upper_arm, _node_chest, { 0.0f, -0.27f, 1.52f }, { 0.0f, -0.27f, 1.2f }, 0.075f, 0.06f },
	{ _node_right_forearm, _node_right_upper_arm, { 0.0f, -0.27f, 1.2f }, { 0.0f, -0.27f, 0.9f }, 0.06f, 0.045f },
	{ _node_left_thigh, _node_pelvis, { 0.0f, 0.11f, 0.95f }, { 0.0f, 0.11f, 0.52f }, 0.095f, 0.07f },
	{ _node_left_shin, _node_left_thigh, { 0.0f, 0.11f, 0.52f }, { 0.0f, 0.11f, 0.06f }, 0.07f, 0.055f },
	{ _node_right_thigh, _node_pelvis, { 0.0f, -0.11f, 0.95f }, { 0.0f, -0.11f, 0.52f }, 0.095f, 0.07f },
	{ _node_right_shin, _node_right_thigh, { 0.0f, -0.11f, 0.52f }, { 0.0f, -0.11f, 0.06f }, 0.07f, 0.055f },
};

#define SEGMENT_COUNT (sizeof(segments) / sizeof(segments[0]))
#define SIDES 12
/* rings along a tube; the first and last close it */
#define RINGS 10
#define CHARACTER_VERTICES (SEGMENT_COUNT * (RINGS + 1) * (SIDES + 1))
#define CHARACTER_INDICES (SEGMENT_COUNT * RINGS * SIDES * 6)

/* the rifle, a part of one node: boxes in the right hand's space */
#define RIFLE_BOXES 4
#define RIFLE_VERTICES (RIFLE_BOXES * 24)
#define RIFLE_INDICES (RIFLE_BOXES * 36)

static struct model_vertex character_vertices[CHARACTER_VERTICES];
static WORD character_indices[CHARACTER_INDICES];
static struct model_vertex rifle_vertices[RIFLE_VERTICES];
static WORD rifle_indices[RIFLE_INDICES];

static D3DVertexBuffer *character_buffer, *rifle_buffer;
static D3DIndexBuffer *character_index_buffer, *rifle_index_buffer;
/* the reference's vertices, skinned on the CPU each draw, as the game
fills its dynamic vertex buffers */
static D3DVertexBuffer *reference_buffer;

static short texcoord_short(float value)
{
	return (short)lrintf((value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value) * 32767.0f);
}

static void make_character(void)
{
	unsigned long segment, vertex = 0, index = 0;

	for (segment = 0; segment < SEGMENT_COUNT; segment++)
	{
		float axis[3], side_x[3], side_y[3], length;
		const float up[3] = { 0.0f, 0.0f, 1.0f }, across[3] = { 1.0f, 0.0f, 0.0f };
		unsigned long first = vertex;
		int ring, side;

		set3(axis, segments[segment].to[0] - segments[segment].from[0], segments[segment].to[1] - segments[segment].from[1],
			segments[segment].to[2] - segments[segment].from[2]);
		length = sqrtf(dot(axis, axis));
		normalize(axis);
		cross(axis, fabsf(axis[2]) > 0.9f ? across : up, side_x);
		normalize(side_x);
		cross(axis, side_x, side_y);
		for (ring = 0; ring <= RINGS; ring++)
		{
			/* the first and last ring have no radius: the ends are closed */
			float t = ring == 0 ? 0.0f : ring == RINGS ? 1.0f : (ring - 0.5f) / (RINGS - 1);
			float radius = ring == 0 || ring == RINGS ? 0.0f :
				segments[segment].radius_from + (segments[segment].radius_to - segments[segment].radius_from) * t;
			/* a bulge along the tube, so it does not read as a pipe */
			float bulge = 1.0f + 0.18f * sinf(t * 3.14159265f);
			/* at the start, half the parent's; all its own from a quarter of
			the way along */
			float weight = segments[segment].blend < 0 ? 1.0f : t >= 0.25f ? 1.0f : 0.5f + 2.0f * t;

			for (side = 0; side <= SIDES; side++)
			{
				struct model_vertex *out = &character_vertices[vertex++];
				float angle = 2.0f * 3.14159265f * side / SIDES;
				float radial[3], normal[3], tangent[3], binormal[3];
				int k;

				for (k = 0; k < 3; k++)
				{
					radial[k] = cosf(angle) * side_x[k] + sinf(angle) * side_y[k];
					out->position[k] = segments[segment].from[k] + axis[k] * length * t + radial[k] * radius * bulge;
				}
				/* the ends' normals point along the tube */
				if (ring == 0)
					set3(normal, -axis[0], -axis[1], -axis[2]);
				else if (ring == RINGS)
					memcpy(normal, axis, sizeof(normal));
				else
					memcpy(normal, radial, sizeof(normal));
				memcpy(tangent, axis, sizeof(tangent));
				cross(normal, tangent, binormal);
				normalize(binormal);
				out->normal = normpacked3(normal);
				out->binormal = normpacked3(binormal);
				out->tangent = normpacked3(tangent);
				out->texcoord[0] = texcoord_short((float)side / SIDES);
				out->texcoord[1] = texcoord_short(((float)segment + t) / SEGMENT_COUNT);
				/* node numbers times three, as the game's model vertices hold
				them: c[-89].w = 255.9375 turns the byte into a row */
				out->nodes[0] = (BYTE)(3 * segments[segment].node);
				out->nodes[1] = (BYTE)(3 * (weight < 1.0f ? segments[segment].blend : segments[segment].node));
				out->node_weight = (short)lrintf(weight * 32767.0f);
			}
		}
		/* clockwise seen from outside, as the game's models are wound */
		for (ring = 0; ring < RINGS; ring++)
		{
			for (side = 0; side < SIDES; side++)
			{
				WORD a = (WORD)(first + ring * (SIDES + 1) + side), b = (WORD)(a + 1);
				WORD c = (WORD)(a + SIDES + 1), d = (WORD)(c + 1);

				character_indices[index++] = a; character_indices[index++] = b; character_indices[index++] = c;
				character_indices[index++] = b; character_indices[index++] = d; character_indices[index++] = c;
			}
		}
	}
}

static void rifle_box(unsigned long *vertex, unsigned long *index, const float *center, const float *half,
	float u0, float u1)
{
	static const float normals[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	int face;

	for (face = 0; face < 6; face++)
	{
		const float *n = normals[face];
		float a[3], b[3];
		static const float signs[4][2] = { { -1, -1 }, { -1, 1 }, { 1, 1 }, { 1, -1 } };
		unsigned long first = *vertex;
		int corner;

		set3(a, n[2] != 0.0f ? 1.0f : 0.0f, 0.0f, n[2] != 0.0f ? 0.0f : 1.0f);
		cross(n, a, b);
		for (corner = 0; corner < 4; corner++)
		{
			struct model_vertex *out = &rifle_vertices[(*vertex)++];
			int k;

			for (k = 0; k < 3; k++)
				out->position[k] = center[k] + (n[k] + signs[corner][0] * a[k] + signs[corner][1] * b[k]) * half[k];
			out->normal = normpacked3(n);
			out->binormal = normpacked3(b);
			out->tangent = normpacked3(a);
			out->texcoord[0] = texcoord_short(signs[corner][0] < 0 ? u0 : u1);
			out->texcoord[1] = texcoord_short(signs[corner][1] < 0 ? 0.2f : 0.3f);
			out->nodes[0] = out->nodes[1] = 0;
			out->node_weight = 32767;
		}
		/* -a-b, -a+b, +a+b, +a-b: with a x b = n these turn clockwise
		seen from outside */
		rifle_indices[(*index)++] = (WORD)first; rifle_indices[(*index)++] = (WORD)(first + 2);
		rifle_indices[(*index)++] = (WORD)(first + 1);
		rifle_indices[(*index)++] = (WORD)first; rifle_indices[(*index)++] = (WORD)(first + 3);
		rifle_indices[(*index)++] = (WORD)(first + 2);
	}
}

static void make_rifle(void)
{
	/* in the hand's space: the hand at the origin, the barrel along x */
	static const float boxes[RIFLE_BOXES][6] =
	{
		{ 0.12f, 0.0f, 0.0f, 0.28f, 0.035f, 0.06f },
		{ 0.48f, 0.0f, 0.02f, 0.14f, 0.02f, 0.02f },
		{ -0.02f, 0.0f, -0.08f, 0.03f, 0.025f, 0.07f },
		{ 0.1f, 0.0f, 0.09f, 0.1f, 0.02f, 0.03f },
	};
	unsigned long vertex = 0, index = 0;
	int box;

	for (box = 0; box < RIFLE_BOXES; box++)
		rifle_box(&vertex, &index, boxes[box], boxes[box] + 3, 0.05f + 0.2f * box, 0.15f + 0.2f * box);
}

/* ---------- poses */

struct pose
{
	/* each node's matrix: bind-pose model space to the world */
	struct matrix4x3 nodes[NODE_COUNT];
};

/* a walk: the legs and arms swing about the model's y axis, the knees and
elbows bend, the chest turns against the hips; placed at (x, y), facing
heading */
static void pose_walk(float phase, float x, float y, float heading, struct pose *pose)
{
	const float y_axis[3] = { 0.0f, 1.0f, 0.0f }, z_axis[3] = { 0.0f, 0.0f, 1.0f };
	float swing = 0.55f * sinf(phase), bob = 0.03f * cosf(2.0f * phase);
	float local_angle[NODE_COUNT];
	const float *local_axis[NODE_COUNT];
	struct matrix4x3 root, heading_matrix, lift;
	int node;
	const float origin[3] = { 0.0f, 0.0f, 0.0f };

	for (node = 0; node < NODE_COUNT; node++)
	{
		local_angle[node] = 0.0f;
		local_axis[node] = y_axis;
	}
	local_axis[_node_chest] = z_axis;
	local_angle[_node_chest] = 0.25f * sinf(phase);
	local_angle[_node_head] = 0.12f * sinf(2.0f * phase);
	local_angle[_node_left_thigh] = swing;
	local_angle[_node_right_thigh] = -swing;
	local_angle[_node_left_shin] = -0.45f - 0.45f * cosf(phase);
	local_angle[_node_right_shin] = -0.45f + 0.45f * cosf(phase);
	local_angle[_node_left_upper_arm] = -0.6f * sinf(phase);
	local_angle[_node_right_upper_arm] = 0.6f * sinf(phase) - 0.9f;
	local_angle[_node_left_forearm] = 0.5f;
	local_angle[_node_right_forearm] = 0.7f;

	matrix_rotation_about(z_axis, heading, origin, &heading_matrix);
	matrix_identity(&lift);
	set3(lift.position, x, y, bob);
	matrix_multiply(&lift, &heading_matrix, &root);
	for (node = 0; node < NODE_COUNT; node++)
	{
		struct matrix4x3 local;

		/* the parent is always earlier in the list */
		matrix_rotation_about(local_axis[node], -local_angle[node], nodes[node].joint, &local);
		matrix_multiply(nodes[node].parent < 0 ? &root : &pose->nodes[nodes[node].parent], &local, &pose->nodes[node]);
	}
}

/* the rifle's node: in the right hand, along the forearm's forward */
static void rifle_node(const struct pose *pose, struct matrix4x3 *out)
{
	struct matrix4x3 grip;
	const float x_axis[3] = { 0.0f, 1.0f, 0.0f };
	const float hand[3] = { 0.0f, -0.27f, 0.9f };

	/* tipped up towards the forearm, and moved to the hand */
	matrix_rotation_about(x_axis, -0.9f, (const float[3]){ 0.0f, 0.0f, 0.0f }, &grip);
	memcpy(grip.position, hand, sizeof(grip.position));
	matrix_multiply(&pose->nodes[_node_right_forearm], &grip, out);
}

/* ---------- constants, as the rasterizer sets them */

static void set_constant(int reg, float x, float y, float z, float w)
{
	float value[4] = { x, y, z, w };

	IDirect3DDevice8_SetVertexShaderConstant(device, reg, value, 1);
}

/* rasterizer_set_model_skinning */
static void set_skinning(const struct matrix4x3 *matrices, int count)
{
	static float constants[44][3][4];
	int node;

	for (node = 0; node < count; node++)
	{
		const struct matrix4x3 *matrix = &matrices[node];
		float scale = matrix->scale;

		constants[node][0][0] = scale * matrix->forward[0];
		constants[node][0][1] = scale * matrix->left[0];
		constants[node][0][2] = scale * matrix->up[0];
		constants[node][0][3] = matrix->position[0];
		constants[node][1][0] = scale * matrix->forward[1];
		constants[node][1][1] = scale * matrix->left[1];
		constants[node][1][2] = scale * matrix->up[1];
		constants[node][1][3] = matrix->position[1];
		constants[node][2][0] = scale * matrix->forward[2];
		constants[node][2][1] = scale * matrix->left[2];
		constants[node][2][2] = scale * matrix->up[2];
		constants[node][2][3] = matrix->position[2];
	}
	IDirect3DDevice8_SetVertexShaderConstant(device, -36, constants, count * 3);
}

struct camera
{
	float position[3], forward[3], up[3];
	float field_of_view, aspect, z_near, z_far;
};

/* render_camera_build_frustum's view and projection, and
rasterizer_set_frustum_z's constants from them */
static void set_camera(const struct camera *camera)
{
	float x_axis[3], y_axis[3], z_axis[3];
	float world_to_view[3][4], projection[4][4], constants[8][4];
	float x_scale, y_scale;
	int row, column;

	cross(camera->forward, camera->up, x_axis);
	cross(x_axis, camera->forward, y_axis);
	set3(z_axis, -camera->forward[0], -camera->forward[1], -camera->forward[2]);
	normalize(x_axis);
	normalize(y_axis);
	normalize(z_axis);
	/* view_to_world's inverse: its rows are the axes */
	for (column = 0; column < 3; column++)
	{
		world_to_view[0][column] = x_axis[column];
		world_to_view[1][column] = y_axis[column];
		world_to_view[2][column] = z_axis[column];
	}
	world_to_view[0][3] = -dot(x_axis, camera->position);
	world_to_view[1][3] = -dot(y_axis, camera->position);
	world_to_view[2][3] = -dot(z_axis, camera->position);

	y_scale = 1.0f / tanf(camera->field_of_view * 0.5f);
	x_scale = y_scale / camera->aspect;
	memset(projection, 0, sizeof(projection));
	projection[0][0] = x_scale;
	projection[1][1] = y_scale;
	projection[2][3] = -1.0f;
	/* render_camera_hack_frustum_z */
	projection[2][2] = -((camera->z_near + camera->z_far) / (camera->z_far - camera->z_near));
	projection[3][2] = (camera->z_near * camera->z_far * -2.0f) / (camera->z_far - camera->z_near);

	/* rasterizer_set_frustum_z: world_to_view.n[row] is the matrix4x3's
	forward, left, up and position (the columns above) */
	for (column = 0; column < 4; column++)
	{
		for (row = 0; row < 4; row++)
		{
			float n[3] = { world_to_view[0][row], world_to_view[1][row], world_to_view[2][row] };

			constants[column][row] = n[0] * projection[0][column] + n[1] * projection[1][column] +
				n[2] * projection[2][column];
		}
		constants[column][3] += projection[3][column];
	}
	set3(constants[4], camera->position[0], camera->position[1], camera->position[2]);
	constants[4][3] = 2.0f;
	set3(constants[5], camera->forward[0], camera->forward[1], camera->forward[2]);
	constants[5][3] = 0.5f;
	memcpy(constants[6], x_axis, sizeof(x_axis));
	constants[6][3] = 1.0f;
	memcpy(constants[7], y_axis, sizeof(y_axis));
	constants[7][3] = 255.9375f;
	IDirect3DDevice8_SetVertexShaderConstant(device, -96, constants, 8);
}

/* rasterizer_window_begin's fog constants at c[-88] to c[-85]: atmospheric
fog from 12 to 70 units at most half dense, no planar fog (its plane put
at the camera, as the game does when there is none) */
static void set_fog(const struct camera *camera)
{
	const float minimum = 12.0f, maximum = 70.0f, density = 0.5f;
	float inverse_range = 1.0f / (maximum - minimum);
	float view_distance = dot(camera->position, camera->forward);

	set_constant(-88, camera->forward[0] * inverse_range, camera->forward[1] * inverse_range,
		camera->forward[2] * inverse_range, -((minimum + view_distance) * inverse_range));
	/* the planar fog's plane through the camera along its forward; depth
	and distance 1 */
	set_constant(-87, -camera->forward[0], -camera->forward[1], -camera->forward[2], view_distance);
	set_constant(-86, camera->forward[0], camera->forward[1], camera->forward[2], -view_distance);
	set_constant(-85, density, 0.0f, 0.0f, 0.0f);
}

/* rasterizer_set_model_lighting: two point lights (position and 1/r^2,
forward and the spot's falloff scale, color and falloff offset), two
distant lights (direction, color) and the ambient color */
static void set_lighting(float time)
{
	float lighting[11][4];

	memset(lighting, 0, sizeof(lighting));
	/* an orange point light circling the characters, a white one above */
	set3(lighting[0], 3.0f * cosf(time * 0.9f), 3.0f * sinf(time * 0.9f), 1.4f);
	lighting[0][3] = 1.0f / (4.5f * 4.5f);
	lighting[1][3] = 0.0f;
	set3(lighting[2], 1.0f, 0.55f, 0.2f);
	lighting[2][3] = 1.0f;
	set3(lighting[3], 0.0f, 0.0f, 4.0f);
	lighting[3][3] = 1.0f / (5.0f * 5.0f);
	lighting[4][3] = 0.0f;
	set3(lighting[5], 0.35f, 0.35f, 0.4f);
	lighting[5][3] = 1.0f;
	/* the sun, and a blue fill from below */
	set3(lighting[6], -0.45f, -0.3f, -0.84f);
	set3(lighting[7], 0.95f, 0.9f, 0.78f);
	set3(lighting[8], 0.2f, 0.3f, 0.93f);
	set3(lighting[9], 0.1f, 0.14f, 0.25f);
	set3(lighting[10], 0.3f, 0.31f, 0.34f);
	IDirect3DDevice8_SetVertexShaderConstant(device, -79, lighting, 11);
}

/* rasterizer_xbox_models.c's texture transform at c[-84] to c[-82] (the
detail map's scale, the base map's, the translucency) and the specular
colors at c[-81] and c[-80] (perpendicular less parallel, parallel) */
static void set_model_constants(void)
{
	set_constant(-84, 4.0f, 4.0f, 1.0f, 1.0f);
	set_constant(-83, 1.0f, 0.0f, 0.0f, 0.0f);
	set_constant(-82, 0.0f, 1.0f, 0.0f, 0.0f);
	set_constant(-81, -0.25f, -0.2f, -0.1f, -0.5f);
	set_constant(-80, 0.35f, 0.38f, 0.45f, 0.6f);
}

/* ---------- pixel shaders */

enum
{
	/* the base map by the vertex lighting */
	_shader_lit,
	/* the same, and the reflection's specular color added */
	_shader_lit_specular,
	/* the vertex specular color alone, its alpha the second color's */
	_shader_specular,
	/* rasterizer_plasma_energy_draw's */
	_shader_plasma,
	/* rasterizer_detail_objects's */
	_shader_detail_objects,
};

static void set_pixel_shader(int which)
{
	D3DPIXELSHADERDEF definition;

	memset(&definition, 0, sizeof(definition));
	definition.PSCombinerCount = 1;
	definition.PSTextureModes = PS_TEXTUREMODES(PS_TEXTUREMODES_PROJECT2D, 0, 0, 0);
	switch (which)
	{
	case _shader_lit:
		definition.PSFinalCombinerInputsABCD = PS_COMBINERINPUTS(PS_REGISTER_T0, PS_REGISTER_V0, 0, 0);
		definition.PSFinalCombinerInputsEFG = PS_COMBINERINPUTS(0, 0, PS_REGISTER_T0 | PS_CHANNEL_ALPHA, 0);
		break;
	case _shader_lit_specular:
		definition.PSFinalCombinerInputsABCD = PS_COMBINERINPUTS(PS_REGISTER_T0, PS_REGISTER_V0, 0, PS_REGISTER_V1);
		definition.PSFinalCombinerInputsEFG = PS_COMBINERINPUTS(0, 0, PS_REGISTER_T0 | PS_CHANNEL_ALPHA, 0);
		break;
	case _shader_specular:
		definition.PSTextureModes = 0;
		definition.PSFinalCombinerInputsABCD = PS_COMBINERINPUTS(0, 0, 0, PS_REGISTER_V1);
		definition.PSFinalCombinerInputsEFG = PS_COMBINERINPUTS(0, 0, PS_REGISTER_V1 | PS_CHANNEL_ALPHA, 0);
		break;
	case _shader_plasma:
		/* rasterizer_plasma_energy_draw's, its noise maps sampled in 2D */
		definition.PSTextureModes = PS_TEXTUREMODES(PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_PROJECT2D, 0, 0);
		definition.PSCombinerCount = 0x104;
		definition.PSAlphaInputs[0] = 0x0820A920;
		definition.PSAlphaOutputs[0] = 0xC00;
		definition.PSRGBInputs[0] = 0x1920B820;
		definition.PSRGBOutputs[0] = 0xC00;
		definition.PSAlphaInputs[1] = 0x1C1C0C0C;
		definition.PSAlphaOutputs[1] = 0x24C00;
		definition.PSAlphaInputs[2] = 0x5C5C;
		definition.PSAlphaOutputs[2] = 0x4D00;
		definition.PSAlphaInputs[3] = 0x14150000;
		definition.PSAlphaOutputs[3] = 0x40;
		definition.PSRGBInputs[3] = 0x1C051DA0;
		definition.PSRGBOutputs[3] = 0xC00;
		definition.PSFinalCombinerInputsABCD = 0x0C0F0000;
		definition.PSFinalCombinerInputsEFG = 0x1C1C1400;
		break;
	case _shader_detail_objects:
		definition.PSRGBInputs[0] = 0x08040000;
		definition.PSRGBOutputs[0] = 0xC0;
		definition.PSAlphaInputs[0] = 0x18140000;
		definition.PSAlphaOutputs[0] = 0xC0;
		definition.PSFinalCombinerInputsABCD = 0xC;
		definition.PSFinalCombinerInputsEFG = 0x1C00;
		break;
	}
	IDirect3DDevice8_SetPixelShaderProgram(device, &definition);
}

/* ---------- textures: GX's RGB5A3, made by CreateTexture */

static void rgb5a3_put(D3DLOCKED_RECT *locked, int size, int x, int y, unsigned r, unsigned g, unsigned b, unsigned a)
{
	unsigned short texel;
	unsigned char *tile = (unsigned char *)locked->pBits + ((y / 4) * (size / 4) + x / 4) * 32;

	if (a >= 0xe0)
		texel = (unsigned short)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
	else
		texel = (unsigned short)(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
	memcpy(tile + ((y % 4) * 4 + x % 4) * 2, &texel, sizeof(texel));
}

/* armour: a base color in panels with dark seams, a light band on each
limb, and a visor on the head (the head is the third of eleven bands) */
static D3DTexture *armour_texture(unsigned r, unsigned g, unsigned b)
{
	D3DTexture *texture;
	D3DLOCKED_RECT locked;
	const int size = 64;
	int x, y;

	IDirect3DDevice8_CreateTexture(device, size, size, 1, 0, D3DFMT_WII_GX(_gx_tf_rgb5a3), D3DPOOL_MANAGED, &texture);
	D3DTexture_LockRect(texture, 0, &locked, NULL, 0);
	for (y = 0; y < size; y++)
	{
		for (x = 0; x < size; x++)
		{
			float v = (float)y / size * SEGMENT_COUNT, band = v - floorf(v);
			int segment = (int)v;
			BOOL seam = (x % 16) == 0 || band < 0.06f;
			BOOL stripe = band > 0.55f && band < 0.68f;
			BOOL visor = segment == 2 && band > 0.45f && band < 0.75f && x > 36 && x < 60;
			unsigned shade = 200 + (unsigned)(((x * 13 + y * 7) % 11));

			if (visor)
				rgb5a3_put(&locked, size, x, y, 230, 170, 40, 255);
			else if (seam)
				rgb5a3_put(&locked, size, x, y, r / 3, g / 3, b / 3, 255);
			else if (stripe)
				rgb5a3_put(&locked, size, x, y, 210, 210, 200, 255);
			else
				rgb5a3_put(&locked, size, x, y, r * shade / 255, g * shade / 255, b * shade / 255, 255);
		}
	}
	return texture;
}

/* noise for the plasma: a few octaves of a hashed lattice, gray */
static float lattice(int x, int y, int period)
{
	unsigned h = (unsigned)((x % period + period) % period) * 374761393u + (unsigned)((y % period + period) % period) * 668265263u;

	h = (h ^ (h >> 13)) * 1274126177u;
	return ((h >> 8) & 0xffff) / 65535.0f;
}

static float smooth_noise(float x, float y, int period)
{
	int ix = (int)floorf(x), iy = (int)floorf(y);
	float fx = x - ix, fy = y - iy;
	float a = lattice(ix, iy, period), b = lattice(ix + 1, iy, period);
	float c = lattice(ix, iy + 1, period), d = lattice(ix + 1, iy + 1, period);

	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static D3DTexture *noise_texture(int seed)
{
	D3DTexture *texture;
	D3DLOCKED_RECT locked;
	const int size = 64;
	int x, y;

	IDirect3DDevice8_CreateTexture(device, size, size, 1, 0, D3DFMT_WII_GX(_gx_tf_rgb5a3), D3DPOOL_MANAGED, &texture);
	D3DTexture_LockRect(texture, 0, &locked, NULL, 0);
	for (y = 0; y < size; y++)
	{
		for (x = 0; x < size; x++)
		{
			float n = 0.5f * smooth_noise(x / 8.0f + seed, y / 8.0f, 8) + 0.3f * smooth_noise(x / 4.0f, y / 4.0f + seed, 16) +
				0.2f * smooth_noise(x / 2.0f + seed, y / 2.0f + seed, 32);
			unsigned value = (unsigned)(n * 255.0f);

			rgb5a3_put(&locked, size, x, y, value, value, value, value < 224 ? value : 224);
		}
	}
	return texture;
}

/* the grass's sprites: four blade clumps side by side, alpha cut out */
#define GRASS_SPRITES 4

static D3DTexture *grass_texture(void)
{
	D3DTexture *texture;
	D3DLOCKED_RECT locked;
	const int size = 64;
	int x, y;

	IDirect3DDevice8_CreateTexture(device, size, size, 1, 0, D3DFMT_WII_GX(_gx_tf_rgb5a3), D3DPOOL_MANAGED, &texture);
	D3DTexture_LockRect(texture, 0, &locked, NULL, 0);
	for (y = 0; y < size; y++)
	{
		for (x = 0; x < size; x++)
		{
			int sprite = x / 16, local = x % 16;
			/* v grows down the texture: the blades' tips at the top */
			float height = 1.0f - (float)y / size;
			BOOL blade = FALSE;
			int k;

			for (k = 0; k < 4; k++)
			{
				float base = 2.5f + k * 3.6f + (sprite & 1);
				float lean = (k - 1.5f) * 2.5f * height * height;
				float width = 2.2f * (1.0f - height) + 0.3f;
				float tall = 0.45f + 0.15f * ((k + sprite * 3) % 4);

				if (height < tall && fabsf(local - base - lean) < width + 0.4f)
					blade = TRUE;
			}
			if (blade)
				rgb5a3_put(&locked, size, x, y, 120 + (unsigned)(height * 100), 180 + (unsigned)(height * 60), 60, 255);
			else
				rgb5a3_put(&locked, size, x, y, 0, 0, 0, 0);
		}
	}
	return texture;
}

/* ---------- the ground and the grass */

#define GRASS_CELLS 3
#define GRASS_PER_CELL 90
#define CELL_SIZE 8.0f

static D3DVertexBuffer *grass_buffer;

/* detail_object_build_vertices: four vertices for each object, the same
but for the corner in the sprite word's low bits; (type << 4) | (sprite << 8) */
static void make_grass(void)
{
	struct detail_object_vertex *vertices;
	BYTE *data;
	int cell, object;
	unsigned seed = 12345;

	IDirect3DDevice8_CreateVertexBuffer(device, GRASS_CELLS * GRASS_CELLS * GRASS_PER_CELL * 4 * sizeof(*vertices),
		D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &grass_buffer);
	D3DVertexBuffer_Lock(grass_buffer, 0, 0, &data, 0);
	vertices = (struct detail_object_vertex *)data;
	for (cell = 0; cell < GRASS_CELLS * GRASS_CELLS; cell++)
	{
		for (object = 0; object < GRASS_PER_CELL; object++)
		{
			struct detail_object_vertex vertex;
			int corner, type, sprite;

			seed = seed * 1103515245u + 12345u;
			vertex.position[0] = (BYTE)(seed >> 24);
			seed = seed * 1103515245u + 12345u;
			vertex.position[1] = (BYTE)(seed >> 24);
			vertex.position[2] = 0;
			seed = seed * 1103515245u + 12345u;
			vertex.color[0] = (BYTE)(150 + ((seed >> 24) & 63));
			vertex.color[1] = (BYTE)(170 + ((seed >> 20) & 63));
			vertex.color[2] = (BYTE)(110 + ((seed >> 16) & 31));
			type = (seed >> 8) & 1;
			sprite = (seed >> 12) % GRASS_SPRITES;
			for (corner = 0; corner < 4; corner++)
			{
				vertex.sprite = (WORD)((type << 4) | (sprite << 8) | corner);
				*vertices++ = vertex;
			}
		}
	}
}

/* the constants rasterizer_detail_objects_draw sets: at c[-81] the sprite
word's unpacking, the cell size and the screen-facing offset, then the four
corners (texture coordinate, offset across, offset up); at c[-75] each
type's fade (near and far) and size; at c[-36] each sprite's place in the
texture */
static void set_grass_constants(void)
{
	const float corners[24] =
	{
		0.00390625f, 16.0f, 541.0f, 659.0f,
		CELL_SIZE, CELL_SIZE, CELL_SIZE, 0.0f,
		1.0f, 1.0f, 0.5f, 0.0f,
		0.0f, 1.0f, -0.5f, 0.0f,
		0.0f, 0.0f, -0.5f, 1.0f,
		1.0f, 0.0f, 0.5f, 1.0f,
	};
	float types[2][4], frames[GRASS_SPRITES][4];
	int type, sprite;

	for (type = 0; type < 2; type++)
	{
		const float near_fade = 18.0f, far_fade = 30.0f, size = type ? 0.009f : 0.006f;
		float range = 1.0f / (far_fade - near_fade);

		types[type][0] = range * far_fade;
		types[type][1] = -range;
		/* a sprite is 16 by 64 texels of the bitmap, drawn half as tall
		again as it is wide over its own */
		types[type][2] = 32.0f * size;
		types[type][3] = 64.0f * size;
	}
	for (sprite = 0; sprite < GRASS_SPRITES; sprite++)
	{
		frames[sprite][0] = sprite * 0.25f;
		frames[sprite][1] = 0.0f;
		frames[sprite][2] = 0.25f;
		frames[sprite][3] = 1.0f;
	}
	IDirect3DDevice8_SetVertexShaderConstant(device, -81, corners, 6);
	IDirect3DDevice8_SetVertexShaderConstant(device, -75, types, 2);
	IDirect3DDevice8_SetVertexShaderConstant(device, -36, frames, GRASS_SPRITES);
}

/* ---------- timing */

struct timing
{
	const char *name;
	unsigned long long ticks;
	unsigned long vertices, draws;
};

enum
{
	_timing_skinned,
	_timing_rifle,
	_timing_plasma,
	_timing_grass,
	_timing_reference,
	NUMBER_OF_TIMINGS
};

static struct timing timings[NUMBER_OF_TIMINGS] =
{
	{ "skinned characters (programs 10, 9, 17)", 0, 0, 0 },
	{ "rifle and ground, one node (program 27)", 0, 0, 0 },
	{ "plasma shell (program 15)", 0, 0, 0 },
	{ "grass (program 33)", 0, 0, 0 },
	{ "reference (CPU skinning, then the same programs)", 0, 0, 0 },
};

static unsigned long long timing_start;

static void timing_begin(void)
{
	timing_start = skintest_ticks();
}

static void timing_end(int which, unsigned long vertices)
{
	timings[which].ticks += skintest_ticks() - timing_start;
	timings[which].vertices += vertices;
	timings[which].draws++;
}

/* ---------- drawing */

struct character
{
	D3DTexture *armour;
	int program;
	float phase_offset, radius, speed, angle_offset;
	struct pose pose;
};

static struct character characters[3];
static D3DTexture *noise[2], *grass;

static void model_state(void)
{
	IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ZWRITEENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
	IDirect3DDevice8_SetRenderState(device, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALL);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
}

/* the reference: what the game's debug code computes for a vertex
(rasterizer_debug_model_vertices), written as a vertex of one node */
static void reference_skin(const struct model_vertex *in, const struct matrix4x3 *matrices,
	struct model_vertex *out)
{
	short node0 = in->nodes[0] / 3, node1 = in->nodes[1] / 3;
	float weight0 = (float)in->node_weight * (1.0f / 32767.0f), weight1 = 1.0f - weight0;
	float point0[3], point1[3], vector[3], vector0[3], vector1[3];
	float position[3], normal[3], binormal[3], tangent[3];
	int k;

	transform_point(&matrices[node0], in->position, point0);
	transform_point(&matrices[node1], in->position, point1);
	for (k = 0; k < 3; k++)
		position[k] = point0[k] * weight0 + point1[k] * weight1;
#define BLEND(field, result) \
	unpack3(in->field, vector); \
	transform_vector(&matrices[node0], vector, vector0); \
	transform_vector(&matrices[node1], vector, vector1); \
	for (k = 0; k < 3; k++) result[k] = vector0[k] * weight0 + vector1[k] * weight1;
	BLEND(normal, normal)
	BLEND(binormal, binormal)
	BLEND(tangent, tangent)
#undef BLEND
	memcpy(out->position, position, sizeof(position));
	out->normal = normpacked3(normal);
	out->binormal = normpacked3(binormal);
	out->tangent = normpacked3(tangent);
	out->texcoord[0] = in->texcoord[0];
	out->texcoord[1] = in->texcoord[1];
	out->nodes[0] = out->nodes[1] = 0;
	out->node_weight = 32767;
}

/* draws a model's indexed triangles with a program: skinned by it from the
bind pose, or (reference) skinned here and drawn with the identity node */
static void draw_model(D3DVertexBuffer *buffer, const struct model_vertex *vertices, unsigned long vertex_count,
	D3DIndexBuffer *indices, unsigned long index_count, const struct matrix4x3 *matrices, int matrix_count,
	int program, BOOL reference, int timing)
{
	IDirect3DDevice8_SetVertexShader(device, shader(program));
	if (reference)
	{
		struct model_vertex *out;
		struct matrix4x3 identity;
		BYTE *data;
		unsigned long index;

		timing_begin();
		D3DVertexBuffer_Lock(reference_buffer, 0, 0, &data, 0);
		out = (struct model_vertex *)data;
		for (index = 0; index < vertex_count; index++)
			reference_skin(&vertices[index], matrices, &out[index]);
		matrix_identity(&identity);
		set_skinning(&identity, 1);
		IDirect3DDevice8_SetStreamSource(device, 0, reference_buffer, sizeof(struct model_vertex));
		IDirect3DDevice8_SetIndices(device, indices, 0);
		IDirect3DDevice8_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, vertex_count, 0, index_count / 3);
		timing_end(_timing_reference, vertex_count);
		return;
	}
	timing_begin();
	set_skinning(matrices, matrix_count);
	IDirect3DDevice8_SetStreamSource(device, 0, buffer, sizeof(struct model_vertex));
	IDirect3DDevice8_SetIndices(device, indices, 0);
	IDirect3DDevice8_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, vertex_count, 0, index_count / 3);
	timing_end(timing, vertex_count);
}

/* rasterizer_plasma_energy_draw's constants: the noise maps' transforms
from the vertex's own position at c[-81] (and the shell's offset along the
normal in c[-81].z), its colors at c[-84] */
static void set_plasma_constants(float time)
{
	const float primary_scale = 1.6f, secondary_scale = 2.7f, offset = 0.05f;
	const float primary_time = time / 2.0f, secondary_time = time / 3.1f;
	const float perpendicular[4] = { 0.2f, 0.45f, 0.9f, 0.55f }, parallel[4] = { 0.02f, 0.05f, 0.15f, 0.05f };
	float vertex_constants[6][4], colors[3][4];
	int k;

	memset(vertex_constants, 0, sizeof(vertex_constants));
	vertex_constants[0][0] = primary_scale;
	vertex_constants[0][2] = offset;
	vertex_constants[0][3] = primary_time * 0.3f;
	vertex_constants[1][1] = primary_scale;
	vertex_constants[1][3] = primary_time * 0.7f;
	vertex_constants[2][2] = primary_scale;
	vertex_constants[2][3] = primary_time * 0.2f;
	vertex_constants[3][0] = secondary_scale;
	vertex_constants[3][3] = secondary_time * -0.5f;
	vertex_constants[4][1] = secondary_scale;
	vertex_constants[4][3] = secondary_time * 0.4f;
	vertex_constants[5][2] = secondary_scale;
	vertex_constants[5][3] = secondary_time * 0.6f;
	for (k = 0; k < 4; k++)
	{
		colors[0][k] = 1.0f;
		colors[1][k] = perpendicular[k] - parallel[k];
		colors[2][k] = parallel[k];
	}
	IDirect3DDevice8_SetVertexShaderConstant(device, -81, vertex_constants, 6);
	IDirect3DDevice8_SetVertexShaderConstant(device, -84, colors, 3);
}

static void draw_plasma(const struct character *character, float time, BOOL check, BOOL reference)
{
	IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_COLORWRITEENABLE,
		D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice8_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_ONE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
	IDirect3DDevice8_SetTexture(device, 0, (D3DBaseTexture *)noise[0]);
	IDirect3DDevice8_SetTexture(device, 1, (D3DBaseTexture *)noise[1]);
	IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
	set_plasma_constants(time);
	/* in the check, its colors alone: its noise is placed by the unskinned
	position, which the reference's vertices no longer have */
	set_pixel_shader(check ? _shader_specular : _shader_plasma);
	draw_model(character_buffer, character_vertices, CHARACTER_VERTICES, character_index_buffer, CHARACTER_INDICES,
		character->pose.nodes, NODE_COUNT, _program_plasma, reference, _timing_plasma);
	IDirect3DDevice8_SetTexture(device, 1, NULL);
	model_state();
}

static void draw_grass(void)
{
	int cell;

	IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHATESTENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHAREF, 0x60);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	IDirect3DDevice8_SetTexture(device, 0, (D3DBaseTexture *)grass);
	IDirect3DDevice8_SetVertexShader(device, shader(_program_detail_objects));
	set_pixel_shader(_shader_detail_objects);
	set_grass_constants();
	IDirect3DDevice8_SetStreamSource(device, 0, grass_buffer, sizeof(struct detail_object_vertex));
	for (cell = 0; cell < GRASS_CELLS * GRASS_CELLS; cell++)
	{
		float x = (cell % GRASS_CELLS - GRASS_CELLS / 2.0f) * CELL_SIZE;
		float y = (cell / GRASS_CELLS - GRASS_CELLS / 2.0f) * CELL_SIZE;

		/* the cell's origin, and its ground plane: z = 0 */
		IDirect3DDevice8_SetVertexData4f(device, 2, x, y, 0.0f, 1.0f);
		IDirect3DDevice8_SetVertexData4f(device, 3, 0.0f, 0.0f, 0.0f, 0.0f);
		timing_begin();
		IDirect3DDevice8_DrawVertices(device, D3DPT_QUADLIST, cell * GRASS_PER_CELL * 4, GRASS_PER_CELL * 4);
		timing_end(_timing_grass, GRASS_PER_CELL * 4);
	}
	model_state();
}

/* the ground: one quad, drawn with the one-node program and a plain
texture, so that it is lit as the characters are */
static D3DVertexBuffer *ground_buffer;
static D3DIndexBuffer *ground_index_buffer;
static struct model_vertex ground_vertices[4];
static WORD ground_indices[6] = { 0, 2, 1, 0, 3, 2 };
static D3DTexture *ground_texture;

static void make_ground(void)
{
	const float extent = 14.0f;
	const float up[3] = { 0.0f, 0.0f, 1.0f }, x_axis[3] = { 1.0f, 0.0f, 0.0f }, y_axis[3] = { 0.0f, 1.0f, 0.0f };
	int corner;

	for (corner = 0; corner < 4; corner++)
	{
		struct model_vertex *vertex = &ground_vertices[corner];
		float sx = (corner == 1 || corner == 2) ? 1.0f : -1.0f, sy = corner >= 2 ? 1.0f : -1.0f;

		set3(vertex->position, sx * extent, sy * extent, 0.0f);
		vertex->normal = normpacked3(up);
		vertex->binormal = normpacked3(y_axis);
		vertex->tangent = normpacked3(x_axis);
		vertex->texcoord[0] = texcoord_short(sx);
		vertex->texcoord[1] = texcoord_short(sy);
		vertex->nodes[0] = vertex->nodes[1] = 0;
		vertex->node_weight = 32767;
	}
}

static D3DTexture *dirt_texture(void)
{
	D3DTexture *texture;
	D3DLOCKED_RECT locked;
	const int size = 64;
	int x, y;

	IDirect3DDevice8_CreateTexture(device, size, size, 1, 0, D3DFMT_WII_GX(_gx_tf_rgb5a3), D3DPOOL_MANAGED, &texture);
	D3DTexture_LockRect(texture, 0, &locked, NULL, 0);
	for (y = 0; y < size; y++)
	{
		for (x = 0; x < size; x++)
		{
			float n = smooth_noise(x / 6.0f, y / 6.0f, 11) * 0.6f + smooth_noise(x / 2.0f, y / 2.0f, 32) * 0.4f;
			BOOL line = (x % 32) == 0 || (y % 32) == 0;

			rgb5a3_put(&locked, size, x, y, line ? 70 : 80 + (unsigned)(n * 50), line ? 90 : 105 + (unsigned)(n * 60),
				line ? 50 : 50 + (unsigned)(n * 25), 255);
		}
	}
	return texture;
}

static void draw_ground(BOOL reference)
{
	struct matrix4x3 identity;

	matrix_identity(&identity);
	/* the ground's texture repeats: four times across */
	set_constant(-83, 4.0f, 0.0f, 0.0f, 0.0f);
	set_constant(-82, 0.0f, 4.0f, 0.0f, 0.0f);
	IDirect3DDevice8_SetTexture(device, 0, (D3DBaseTexture *)ground_texture);
	set_pixel_shader(_shader_lit);
	/* the ground is not skinned: one node either way */
	draw_model(ground_buffer, ground_vertices, 4, ground_index_buffer, 6, &identity, 1,
		_program_model_reflection_one_node, reference, _timing_rifle);
	set_model_constants();
}

static void draw_characters(float time, BOOL check, BOOL reference)
{
	int index;

	set_model_constants();
	for (index = 0; index < 3; index++)
	{
		struct character *character = &characters[index];
		int program = character->program;

		IDirect3DDevice8_SetTexture(device, 0, (D3DBaseTexture *)character->armour);
		set_pixel_shader(program == _program_model_reflection ? _shader_lit_specular : _shader_lit);
		draw_model(character_buffer, character_vertices, CHARACTER_VERTICES, character_index_buffer, CHARACTER_INDICES,
			character->pose.nodes, NODE_COUNT, program, reference, _timing_skinned);
		if (index == 0)
		{
			struct matrix4x3 rifle;

			rifle_node(&character->pose, &rifle);
			IDirect3DDevice8_SetTexture(device, 0, (D3DBaseTexture *)characters[2].armour);
			draw_model(rifle_buffer, rifle_vertices, RIFLE_VERTICES, rifle_index_buffer, RIFLE_INDICES, &rifle, 1,
				_program_model_reflection_one_node, reference, _timing_rifle);
		}
	}
	draw_plasma(&characters[1], time, check, reference);
}

static void pose_characters(float time)
{
	int index;

	for (index = 0; index < 3; index++)
	{
		struct character *character = &characters[index];
		float angle = character->angle_offset + time * character->speed;
		float x = character->radius * cosf(angle), y = character->radius * sinf(angle);
		/* walking round the circle: facing along it */
		float heading = angle + (character->speed > 0.0f ? 1.5707963f : -1.5707963f);

		pose_walk(time * 5.0f + character->phase_offset, x, y, heading, &character->pose);
	}
}

static void view_camera(float time, float aspect, struct camera *camera)
{
	float angle = 0.4f + time * 0.15f;
	float target[3] = { 0.0f, 0.0f, 0.9f };

	set3(camera->position, 6.2f * cosf(angle), 6.2f * sinf(angle), 2.6f);
	set3(camera->forward, target[0] - camera->position[0], target[1] - camera->position[1],
		target[2] - camera->position[2]);
	normalize(camera->forward);
	set3(camera->up, 0.0f, 0.0f, 1.0f);
	camera->field_of_view = 1.0f;
	camera->aspect = aspect;
	camera->z_near = 0.06f;
	camera->z_far = 200.0f;
}

/* the phase mark: two small rectangles in the viewport's top left corner,
white then black for the scene, black then white for the check */
static void mark(DWORD x, BOOL check)
{
	D3DRECT first = { x + 2, 2, x + 8, 8 }, second = { x + 8, 2, x + 14, 8 };

	IDirect3DDevice8_Clear(device, 1, &first, D3DCLEAR_TARGET, check ? 0xff000000UL : 0xffffffffUL, 1.0f, 0);
	IDirect3DDevice8_Clear(device, 1, &second, D3DCLEAR_TARGET, check ? 0xffffffffUL : 0xff000000UL, 1.0f, 0);
}

static void draw_view(float time, DWORD x, DWORD width, BOOL check, BOOL reference)
{
	D3DVIEWPORT8 viewport = { x, 0, width, 480, 0.0f, 1.0f };
	struct camera camera;

	IDirect3DDevice8_SetViewport(device, &viewport);
	IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff6a8cb4UL, 1.0f, 0);
	view_camera(time, (float)width / 480.0f, &camera);
	set_camera(&camera);
	set_fog(&camera);
	set_lighting(time);
	model_state();
	draw_ground(reference);
	draw_characters(time, check, reference);
	draw_grass();
	mark(x, check);
}

static void log_timings(const char *phase, unsigned long frames)
{
	int index;

	platform_log("%s: %lu frames", phase, frames);
	for (index = 0; index < NUMBER_OF_TIMINGS; index++)
	{
		struct timing *timing = &timings[index];

		if (!timing->vertices)
			continue;
		platform_log("  %-48s %6lu vertices a frame, %7.2f us a vertex, %7.2f ms a frame", timing->name,
			timing->vertices / frames, skintest_ticks_to_microseconds(timing->ticks) / timing->vertices,
			skintest_ticks_to_microseconds(timing->ticks) / 1000.0 / frames);
		timing->ticks = 0;
		timing->vertices = timing->draws = 0;
	}
}

/* the batch executor against the interpreter on the Wii's CPU, which the
host's tests cannot be (their compiler, their square root, their floor
are the host's): each of the game's 67 programs on random vertices and
constants, compiled for every output, every result compared to the bit */
static void self_check(void)
{
	static const uint8_t all_outputs[16] =
	{
		0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
	};
	static struct nv2a_vsh_program program;
	static struct nv2a_vsh_compiled compiled;
	static struct nv2a_vsh_lanes lanes;
	static float constants[NV2A_VSH_CONSTANT_COUNT][4];
	unsigned long seed = 2463534242UL;
	int index, matched = 0, first_failure = -1;

#define RANDOM() (seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5, seed)
#define RANDOM_FLOAT() ((float)(RANDOM() & 0xffff) / 16384.0f - 2.0f)
	for (index = 0; index < 67; index++)
	{
		const DWORD *words = (const DWORD *)vertex_shader_code + program_offsets[index] / 4;
		float inputs[NV2A_VSH_BATCH][NV2A_VSH_ATTRIBUTE_COUNT][4];
		unsigned long lane;
		int row, component, same = 1, round;

		nv2a_vsh_decode((const uint32_t *)words + 1, words[0] >> 16, &program);
		nv2a_vsh_compile(&program, all_outputs, &compiled);
		for (round = 0; round < 4 && same; round++)
		{
			for (row = 0; row < NV2A_VSH_CONSTANT_COUNT; row++)
				for (component = 0; component < 4; component++)
					constants[row][component] = RANDOM_FLOAT();
			constants[NV2A_VSH_CONSTANT_BIAS - 89][3] = 255.9375f;
			for (lane = 0; lane < NV2A_VSH_BATCH; lane++)
			{
				for (row = 0; row < NV2A_VSH_ATTRIBUTE_COUNT; row++)
				{
					for (component = 0; component < 4; component++)
					{
						inputs[lane][row][component] = RANDOM_FLOAT();
						lanes.rows[NV2A_VSH_FILE_INPUTS + row][component][lane] = inputs[lane][row][component];
					}
				}
				/* node indices as the models have them */
				inputs[lane][5][0] = lanes.rows[NV2A_VSH_FILE_INPUTS + 5][0][lane] = (float)(3 * (RANDOM() % 44)) / 255.0f;
				inputs[lane][5][1] = lanes.rows[NV2A_VSH_FILE_INPUTS + 5][1][lane] = (float)(3 * (RANDOM() % 44)) / 255.0f;
			}
			nv2a_vsh_run_batch(&compiled, (const float (*)[4])constants, &lanes, NV2A_VSH_BATCH);
			for (lane = 0; lane < NV2A_VSH_BATCH && same; lane++)
			{
				struct nv2a_vsh_result expected, got;

				memset(&expected, 0, sizeof(expected));
				memset(&got, 0, sizeof(got));
				nv2a_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs[lane], &expected);
				nv2a_vsh_lanes_result(&compiled, &lanes, lane, &got);
				if (!expected.clip_captured)
					memset(expected.clip, 0, sizeof(expected.clip)), memset(got.clip, 0, sizeof(got.clip));
				/* (a NaN may have other bits; none of these programs makes one
				from these inputs) */
				if (memcmp(&expected, &got, sizeof(expected)))
					same = 0;
			}
		}
		if (same)
			matched++;
		else if (first_failure < 0)
			first_failure = index;
	}
#undef RANDOM
#undef RANDOM_FLOAT
	platform_log("self check: %d of 67 programs run by the batch executor as by the interpreter, to the bit, on this"
		" CPU%s", matched, first_failure < 0 ? "" : " (NOT ALL)");
	if (first_failure >= 0)
		platform_log("self check: the first that differs is program %d", first_failure);
}

/* where a skinned vertex's time goes on the Wii's CPU, the device's steps
one at a time on the character's vertices: reading them into a batch
(nv2a_vsh_fetch_lanes), running the program (nv2a_vsh_run_batch, compiled
for what the device reads), the clip positions; and, for comparison, the
interpreter */
static void benchmark(void)
{
	static const uint8_t device_outputs[16] = { 0, 0, 0, 0xf, 0xf, 0x8, 0, 0, 0, 0xc, 0xc, 0xc, 0xc, 0, 0, 0 };
	static const int programs[] = { _program_model_lights, _program_model_reflection_one_node, _program_plasma };
	static struct nv2a_vsh_program program;
	static struct nv2a_vsh_compiled compiled;
	static struct nv2a_vsh_lanes lanes;
	static float constants[NV2A_VSH_CONSTANT_COUNT][4];
	static float clips[NV2A_VSH_BATCH][4];
	const float scale[3] = { 320.0f, -240.0f, 16777215.0f }, offset[3] = { 320.0f, 240.0f, 0.0f };
	unsigned long vertex, which;
	int index, row;

	for (row = 0; row < NV2A_VSH_CONSTANT_COUNT; row++)
		for (index = 0; index < 4; index++)
			constants[row][index] = 0.01f * ((row * 7 + index * 3) % 50) + 0.1f;
	constants[NV2A_VSH_CONSTANT_BIAS - 89][3] = 255.9375f;
	for (which = 0; which < sizeof(programs) / sizeof(programs[0]); which++)
	{
		const DWORD *words = (const DWORD *)vertex_shader_code + program_offsets[programs[which]] / 4;
		unsigned long long fetch = 0, run = 0, clip = 0, interpreter = 0, start;

		nv2a_vsh_decode((const uint32_t *)words + 1, words[0] >> 16, &program);
		nv2a_vsh_compile(&program, device_outputs, &compiled);
		for (vertex = 0; vertex + NV2A_VSH_BATCH <= CHARACTER_VERTICES; vertex += NV2A_VSH_BATCH)
		{
			const unsigned char *data = (const unsigned char *)&character_vertices[vertex];
			static const struct { int reg, type, offset; } elements[] =
			{
				{ 0, NV2A_VSDT_FLOAT3, 0 }, { 1, NV2A_VSDT_NORMPACKED3, 12 }, { 2, NV2A_VSDT_NORMPACKED3, 16 },
				{ 3, NV2A_VSDT_NORMPACKED3, 20 }, { 4, NV2A_VSDT_NORMSHORT2, 24 }, { 5, NV2A_VSDT_PBYTE2, 28 },
				{ 6, NV2A_VSDT_NORMSHORT1, 30 },
			};
			unsigned long element, lane;

			start = skintest_ticks();
			for (element = 0; element < sizeof(elements) / sizeof(elements[0]); element++)
				nv2a_vsh_fetch_lanes(elements[element].type, data + elements[element].offset,
					sizeof(struct model_vertex), NV2A_VSH_BATCH, lanes.rows[NV2A_VSH_FILE_INPUTS + elements[element].reg]);
			fetch += skintest_ticks() - start;
			start = skintest_ticks();
			nv2a_vsh_run_batch(&compiled, (const float (*)[4])constants, &lanes, NV2A_VSH_BATCH);
			run += skintest_ticks() - start;
			start = skintest_ticks();
			nv2a_vsh_lanes_clip_positions(&compiled, &lanes, NV2A_VSH_BATCH, constants[NV2A_VSH_CONSTANT_BIAS - 38],
				constants[NV2A_VSH_CONSTANT_BIAS - 37], scale, offset, clips);
			clip += skintest_ticks() - start;
			start = skintest_ticks();
			for (lane = 0; lane < NV2A_VSH_BATCH; lane++)
			{
				float inputs[NV2A_VSH_ATTRIBUTE_COUNT][4];
				struct nv2a_vsh_result result;
				int reg, component;

				for (reg = 0; reg < NV2A_VSH_ATTRIBUTE_COUNT; reg++)
					for (component = 0; component < 4; component++)
						inputs[reg][component] = lanes.rows[NV2A_VSH_FILE_INPUTS + reg][component][lane];
				nv2a_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, &result);
			}
			interpreter += skintest_ticks() - start;
		}
		platform_log("program %2d (%lu instructions, %lu kept): %.2f us a vertex reading it, %.2f running it, %.2f its"
			" clip position; the interpreter %.2f", programs[which], program.count, compiled.count,
			skintest_ticks_to_microseconds(fetch) / vertex, skintest_ticks_to_microseconds(run) / vertex,
			skintest_ticks_to_microseconds(clip) / vertex, skintest_ticks_to_microseconds(interpreter) / vertex);
	}
}

#define HERO_FRAMES 150
#define CHECK_FRAMES 90
/* the scene's clock: a frame is a thirtieth of a second of it, whatever
the time the frame took */
#define FRAME_TIME (1.0f / 30.0f)

void skintest_run(void)
{
	D3DPRESENT_PARAMETERS parameters;
	BYTE *data;
	int frame;

	memset(&parameters, 0, sizeof(parameters));
	parameters.BackBufferWidth = 640;
	parameters.BackBufferHeight = 480;
	Direct3D_CreateDevice(0, D3DDEVTYPE_HAL, NULL, 0, &parameters, &device);

	make_character();
	make_rifle();
	make_ground();
	IDirect3DDevice8_CreateVertexBuffer(device, sizeof(character_vertices), 0, 0, D3DPOOL_MANAGED, &character_buffer);
	D3DVertexBuffer_Lock(character_buffer, 0, 0, &data, 0);
	memcpy(data, character_vertices, sizeof(character_vertices));
	IDirect3DDevice8_CreateIndexBuffer(device, sizeof(character_indices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
		&character_index_buffer);
	memcpy((void *)character_index_buffer->Data, character_indices, sizeof(character_indices));
	IDirect3DDevice8_CreateVertexBuffer(device, sizeof(rifle_vertices), 0, 0, D3DPOOL_MANAGED, &rifle_buffer);
	D3DVertexBuffer_Lock(rifle_buffer, 0, 0, &data, 0);
	memcpy(data, rifle_vertices, sizeof(rifle_vertices));
	IDirect3DDevice8_CreateIndexBuffer(device, sizeof(rifle_indices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
		&rifle_index_buffer);
	memcpy((void *)rifle_index_buffer->Data, rifle_indices, sizeof(rifle_indices));
	IDirect3DDevice8_CreateVertexBuffer(device, sizeof(ground_vertices), 0, 0, D3DPOOL_MANAGED, &ground_buffer);
	D3DVertexBuffer_Lock(ground_buffer, 0, 0, &data, 0);
	memcpy(data, ground_vertices, sizeof(ground_vertices));
	IDirect3DDevice8_CreateIndexBuffer(device, sizeof(ground_indices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
		&ground_index_buffer);
	memcpy((void *)ground_index_buffer->Data, ground_indices, sizeof(ground_indices));
	IDirect3DDevice8_CreateVertexBuffer(device, sizeof(character_vertices), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
		D3DPOOL_DEFAULT, &reference_buffer);
	make_grass();

	characters[0].armour = armour_texture(80, 140, 70);
	characters[0].program = _program_model_lights;
	characters[0].radius = 1.6f; characters[0].speed = 0.32f; characters[0].angle_offset = 0.0f;
	characters[1].armour = armour_texture(70, 110, 190);
	characters[1].program = _program_model_reflection;
	characters[1].radius = 1.6f; characters[1].speed = 0.32f; characters[1].angle_offset = 2.1f;
	characters[1].phase_offset = 1.7f;
	characters[2].armour = armour_texture(190, 70, 60);
	characters[2].program = _program_model_planar_fog;
	characters[2].radius = 3.0f; characters[2].speed = -0.22f; characters[2].angle_offset = 0.9f;
	characters[2].phase_offset = 3.1f;
	noise[0] = noise_texture(3);
	noise[1] = noise_texture(11);
	grass = grass_texture();
	ground_texture = dirt_texture();
	platform_log("the scene is made: %lu vertices a character in %d nodes, %lu grass sprites",
		(unsigned long)CHARACTER_VERTICES, NODE_COUNT, (unsigned long)(GRASS_CELLS * GRASS_CELLS * GRASS_PER_CELL));
	self_check();
	benchmark();

	for (frame = 0; frame < HERO_FRAMES; frame++)
	{
		float time = frame * FRAME_TIME;

		pose_characters(time);
		draw_view(time, 0, 640, FALSE, FALSE);
		IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
	}
	log_timings("the scene, full screen", HERO_FRAMES);

	for (frame = 0; frame < CHECK_FRAMES; frame++)
	{
		float time = (HERO_FRAMES + frame * 2) * FRAME_TIME;

		pose_characters(time);
		draw_view(time, 0, 320, TRUE, FALSE);
		draw_view(time, 320, 320, TRUE, TRUE);
		IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
	}
	log_timings("the check, split screen: left the programs' skinning, right the reference's", CHECK_FRAMES);
	platform_log("done");
}
