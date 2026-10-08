/*
VSH_TEST.C

The vertex program interpreter (port/wii/src/nv2a_vsh_run.c) on the host,
against the game's own 67 programs (rasterizer_xbox_vertex_shaders_data.inc):
every program decodes to its header's instruction count and ends with its
last instruction; the widget program (56) and the environment program (49)
put a vertex where the matrices the game gives them say, through the
screen-space conversion and back; and NORMPACKED3 unpacks.

    port/wii/tests/run.sh
*/

#include "nv2a_vsh_run.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const uint32_t code[] =
{
#include "rasterizer_xbox_vertex_shaders_data.inc"
};

/* rasterizer_xbox_vertex_shaders.c: each program's byte offset */
static const unsigned long offsets[67] =
{
	0x0000, 0x0078, 0x0140, 0x0308, 0x0470, 0x04F8, 0x0670, 0x0708, 0x0890, 0x0918, 0x0BF0, 0x1018,
	0x1210, 0x13E8, 0x1540, 0x1808, 0x1B70, 0x1D08, 0x2200, 0x2478, 0x2710, 0x27D8, 0x2910, 0x2A98,
	0x2BF0, 0x2E48, 0x31C0, 0x3308, 0x3490, 0x36B8, 0x3770, 0x39C8, 0x3F30, 0x4198, 0x4380, 0x4658,
	0x4900, 0x4CB8, 0x4DA0, 0x4E58, 0x4FB0, 0x5088, 0x51E0, 0x52E8, 0x54E0, 0x5648, 0x5A00, 0x5B88,
	0x5F00, 0x6158, 0x6280, 0x6708, 0x6830, 0x6938, 0x6A40, 0x6C78, 0x6DD0, 0x6E68, 0x7260, 0x72D8,
	0x7410, 0x7788, 0x7970, 0x7D38, 0x80D0, 0x8368, 0x8510,
};

static int failures;

#define CHECK(condition, ...) do { if (!(condition)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static float constants[NV2A_VSH_CONSTANT_COUNT][4];
static float inputs[NV2A_VSH_ATTRIBUTE_COUNT][4];

static void set_constant(int reg, float x, float y, float z, float w)
{
	float *c = constants[reg + NV2A_VSH_CONSTANT_BIAS];

	c[0] = x; c[1] = y; c[2] = z; c[3] = w;
}

/* what Direct3D keeps in c[-38] and c[-37] for a 640x480 viewport (d3d8_gx.c) */
static void set_viewport_constants(void)
{
	set_constant(-38, 320.0f, -240.0f, 16777215.0f, 0.0f);
	set_constant(-37, 320.0f, 240.0f, 0.0f, 0.0f);
}

static int near(float a, float b, float tolerance)
{
	return fabsf(a - b) <= tolerance * (1.0f + fabsf(b));
}

static void test_decode_all(void)
{
	int index;

	for (index = 0; index < 67; index++)
	{
		const uint32_t *words = code + offsets[index] / 4;
		unsigned long count = words[0] >> 16;
		struct nv2a_vsh_program program;

		CHECK(nv2a_vsh_decode(words + 1, count, &program), "program %d decodes", index);
		CHECK(program.count == count, "program %d: %lu instructions decoded of %lu", index, program.count, count);
		CHECK(words[1 + (count - 1) * 4 + 3] & 1, "program %d: its last instruction ends it", index);
	}
}

static void test_widget(void)
{
	const uint32_t *words = code + offsets[56] / 4;
	struct nv2a_vsh_program program;
	struct nv2a_vsh_result result;
	const float width = 640.0f, height = 480.0f;

	nv2a_vsh_decode(words + 1, words[0] >> 16, &program);
	memset(constants, 0, sizeof(constants));
	set_viewport_constants();
	/* rasterizer_xbox_widgets.c: pixels to clip space */
	set_constant(-68, 2.0f / width, 0.0f, 0.0f, -1.0f - 1.0f / width);
	set_constant(-67, 0.0f, -2.0f / height, 0.0f, 1.0f / height + 1.0f);
	set_constant(-66, 0.0f, 0.0f, 1.0f, 0.0f);
	set_constant(-65, 0.0f, 0.0f, 0.0f, 1.0f);
	memset(inputs, 0, sizeof(inputs));
	inputs[0][0] = 160.0f; inputs[0][1] = 120.0f; inputs[0][2] = 0.5f; inputs[0][3] = 1.0f;
	inputs[4][0] = 0.25f; inputs[4][1] = 0.75f;
	inputs[9][0] = 1.0f; inputs[9][1] = 0.5f; inputs[9][2] = 0.25f; inputs[9][3] = 0.125f;
	nv2a_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, &result);
	CHECK(result.clip_captured, "the widget program's clip position is kept");
	CHECK(near(result.clip[0], 2.0f * 160.0f / width - 1.0f - 1.0f / width, 1e-5f), "clip x %g", result.clip[0]);
	CHECK(near(result.clip[1], -2.0f * 120.0f / height + 1.0f + 1.0f / height, 1e-5f), "clip y %g", result.clip[1]);
	CHECK(near(result.clip[3], 1.0f, 1e-6f), "clip w %g", result.clip[3]);
	/* in screen space: the pixel, less the half pixel the constants add */
	CHECK(near(result.position[0], 159.5f, 1e-4f), "screen x %g", result.position[0]);
	CHECK(near(result.position[1], 119.5f, 1e-4f), "screen y %g", result.position[1]);
	CHECK(near(result.texcoords[0][0], 0.25f, 1e-6f) && near(result.texcoords[0][1], 0.75f, 1e-6f), "texcoord");
	CHECK(near(result.diffuse[0], 1.0f, 1e-6f) && near(result.diffuse[3], 0.125f, 1e-6f), "diffuse");
}

static void test_environment(void)
{
	const uint32_t *words = code + offsets[49] / 4;
	struct nv2a_vsh_program program;
	struct nv2a_vsh_result result;
	/* a perspective view: rows of the world-view-projection the program
	reads at c[-96] to c[-93] */
	const float m[4][4] =
	{
		{ 1.2f, 0.1f, 0.0f, 0.3f },
		{ 0.0f, 1.6f, 0.2f, -0.4f },
		{ 0.0f, 0.0f, 1.001f, -0.1001f },
		{ 0.0f, 0.0f, 1.0f, 0.0f },
	};
	float v[4] = { 0.5f, -0.25f, 4.0f, 1.0f };
	int row;

	nv2a_vsh_decode(words + 1, words[0] >> 16, &program);
	memset(constants, 0, sizeof(constants));
	set_viewport_constants();
	for (row = 0; row < 4; row++)
		set_constant(-96 + row, m[row][0], m[row][1], m[row][2], m[row][3]);
	/* the texture transform: u and v as they are, times 2 */
	set_constant(-83, 1.0f, 0.0f, 0.0f, 0.0f);
	set_constant(-82, 0.0f, 1.0f, 0.0f, 0.0f);
	set_constant(-84, 2.0f, 2.0f, 0.0f, 0.0f);
	memset(inputs, 0, sizeof(inputs));
	memcpy(inputs[0], v, sizeof(v));
	inputs[4][0] = 0.5f; inputs[4][1] = 0.125f; inputs[4][3] = 1.0f;
	nv2a_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, &result);
	CHECK(result.clip_captured, "the environment program's clip position is kept");
	for (row = 0; row < 4; row++)
	{
		float expected = m[row][0] * v[0] + m[row][1] * v[1] + m[row][2] * v[2] + m[row][3];

		CHECK(near(result.clip[row], expected, 1e-5f), "clip[%d] %g, not %g", row, result.clip[row], expected);
	}
	CHECK(near(result.texcoords[0][0], 1.0f, 1e-6f) && near(result.texcoords[0][1], 0.25f, 1e-6f),
		"texcoord %g %g", result.texcoords[0][0], result.texcoords[0][1]);
	/* the screen position is the clip position divided and scaled */
	CHECK(near(result.position[0], result.clip[0] / result.clip[3] * 320.0f + 320.0f, 1e-4f), "screen x %g", result.position[0]);
}

static void test_normpacked3(void)
{
	float out[4];

	/* x 1023 (11 bits), y -1023, z 511 (10 bits) */
	nv2a_unpack_normpacked3(0x3ffU | (0x401U << 11) | (0x1ffU << 22), out);
	CHECK(near(out[0], 1.0f, 1e-6f) && near(out[1], -1.0f, 1e-6f) && near(out[2], 1.0f, 1e-6f) && out[3] == 1.0f,
		"normpacked3 %g %g %g", out[0], out[1], out[2]);
}

int main(void)
{
	test_decode_all();
	test_widget();
	test_environment();
	test_normpacked3();
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
