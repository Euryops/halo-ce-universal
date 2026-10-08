/*
NV2A_VSH_RUN.H

The Xbox's vertex programs (NV2A microcode), run on the Wii's CPU. The Wii's
GPU has a fixed transform and no vertex programs, so the GX device
(d3d8_gx.c) runs each draw's program here, vertex by vertex, and hands GX
what the program wrote: its position, colors and texture coordinates.

The instruction fields are those the Linux port's GLSL translator reads
(port/linux/src/nv2a_vsh.c); a program is decoded once, when the game
creates its shader, and run from the decoded form.

Plain C: this file sees neither the XDK nor libogc, so the interpreter can
also be built and checked on the host (port/wii/tests/vsh_test.c).
*/

#ifndef __NV2A_VSH_RUN_H
#define __NV2A_VSH_RUN_H

#include <stdint.h>

#define NV2A_VSH_MAXIMUM_INSTRUCTIONS 136
#define NV2A_VSH_ATTRIBUTE_COUNT 16
#define NV2A_VSH_CONSTANT_COUNT 192
/* Direct3D's constant register -96 is the hardware's register 0 */
#define NV2A_VSH_CONSTANT_BIAS 96

struct nv2a_vsh_operand
{
	uint8_t mux;        /* 1 temporary, 2 input, 3 constant */
	uint8_t index;      /* the temporary; 12 reads the position output */
	uint8_t negate;
	uint8_t swizzle[4];
};

struct nv2a_vsh_instruction
{
	uint8_t mac, ilu;
	/* write masks, bit 3 x ... bit 0 w */
	uint8_t mac_mask, ilu_mask, output_mask;
	uint8_t temporary;
	uint8_t output_address, output_is_register, output_from_ilu;
	uint8_t relative;
	/* the input register (v) and the constant (c) the operands that read
	one read: there is one of each per instruction */
	uint8_t input;
	uint8_t constant;
	/* this instruction is the screen-space conversion's rcc of the clip
	position's w: the clip position is kept before it runs */
	uint8_t captures_clip;
	struct nv2a_vsh_operand operands[3];
};

struct nv2a_vsh_program
{
	unsigned long count;
	struct nv2a_vsh_instruction instructions[NV2A_VSH_MAXIMUM_INSTRUCTIONS];
};

/* what a program wrote for one vertex */
struct nv2a_vsh_result
{
	/* oPos: in screen space, as the Xbox's programs end */
	float position[4];
	/* the clip-space position the screen conversion started from, when the
	program has that conversion (captured is then 1) */
	float clip[4];
	int clip_captured;
	float diffuse[4], specular[4];
	float back_diffuse[4], back_specular[4];
	float fog, point_size;
	float texcoords[4][4];
};

/* decodes count instructions (four words each, after the program's header
word); 0 if there are more than the hardware has */
int nv2a_vsh_decode(const uint32_t *words, unsigned long count, struct nv2a_vsh_program *program);

/* runs the program for one vertex: constants[NV2A_VSH_CONSTANT_COUNT] in
hardware order, inputs[NV2A_VSH_ATTRIBUTE_COUNT] */
void nv2a_vsh_run(const struct nv2a_vsh_program *program, const float (*constants)[4],
	const float (*inputs)[4], struct nv2a_vsh_result *result);

/* ---------- the batch executor

The interpreter above is the reference; this is what the device runs. A
program is compiled once for the outputs its caller reads: each
instruction's operands resolved, and every component nothing reads
afterwards (and every instruction left with none) dropped. It then runs each
instruction over a batch of up to NV2A_VSH_BATCH vertices at a time, the
registers laid out by component and then by vertex, so that the decoding
and the dispatch are paid once for the batch and each component's work is
a short loop. The results are the interpreter's, bit for bit, in every
component the caller asked for. */

#define NV2A_VSH_BATCH 16

/* the register file's rows: r0 to r11 and r12 (oPos), the inputs v0 to
v15, the outputs o0 to o15 (o0 is not used: the position is r12) */
#define NV2A_VSH_FILE_TEMPORARIES 0
#define NV2A_VSH_FILE_POSITION 12
#define NV2A_VSH_FILE_INPUTS 13
#define NV2A_VSH_FILE_OUTPUTS 29
#define NV2A_VSH_FILE_COUNT 45

/* the output registers (o[]) */
enum
{
	NV2A_VSH_OUTPUT_DIFFUSE = 3,
	NV2A_VSH_OUTPUT_SPECULAR = 4,
	NV2A_VSH_OUTPUT_FOG = 5,
	NV2A_VSH_OUTPUT_POINT_SIZE = 6,
	NV2A_VSH_OUTPUT_BACK_DIFFUSE = 7,
	NV2A_VSH_OUTPUT_BACK_SPECULAR = 8,
	NV2A_VSH_OUTPUT_TEXCOORD0 = 9,
};

/* the constants' components a program reads (each with its sign) are
spread across a batch once, so that every operand is a row; the game's
programs read at most 97 (program 31) */
#define NV2A_VSH_CONSTANT_SLOTS 112

/* where one component of an operand comes from */
struct nv2a_vsh_source
{
	uint16_t offset;    /* the row's, in floats from the start of struct nv2a_vsh_lanes */
	uint8_t kind;       /* 0 that row; 1 a constant at a0 (constant, component, negate); 2 that row negated;
	                       3 a constant where it is, with no slot (constant, component, negate) */
	uint8_t constant, component, negate;
};

struct nv2a_vsh_compiled_instruction
{
	uint8_t mac, ilu;
	/* the components of each unit's result that are used (bit 3 x ...
	bit 0 w), and where they go: a temporary's row and an output's, and
	which of those components each takes */
	uint8_t mac_used, ilu_used;
	uint8_t mac_row, mac_mask, mac_output_row, mac_output_mask;
	uint8_t ilu_row, ilu_mask, ilu_output_row, ilu_output_mask;
	/* the MAC's result can be written straight to its one destination:
	nothing in this instruction reads what it overwrites */
	uint8_t mac_direct;
	uint8_t captures_clip;
	/* how many dot products (this and the ones after it) run as one: the
	same A, each its own B and a component of the one row they write */
	uint8_t group;
	/* the components of each operand, after its swizzle, that are used,
	and where each is read from */
	uint8_t needs[3];
	struct nv2a_vsh_source sources[3][4];
};

struct nv2a_vsh_compiled
{
	unsigned long count;
	int clip_captured;
	/* the components of each row that are read before the program writes
	them: the inputs the caller must fill, and the temporaries and outputs
	the executor starts as the hardware does */
	uint8_t live_in[NV2A_VSH_FILE_COUNT];
	/* the constants' components read where they are (not at a0), each slot
	a row of the batch: the constant, its component and its sign */
	unsigned long slot_count;
	uint8_t slot_constants[NV2A_VSH_CONSTANT_SLOTS];
	uint8_t slot_components[NV2A_VSH_CONSTANT_SLOTS];
	uint8_t slot_negates[NV2A_VSH_CONSTANT_SLOTS];
	struct nv2a_vsh_compiled_instruction instructions[NV2A_VSH_MAXIMUM_INSTRUCTIONS];
};

/* a batch's registers, by row, component and vertex; only floats, so that
a row's place is its offset in floats */
struct nv2a_vsh_lanes
{
	float rows[NV2A_VSH_FILE_COUNT][4][NV2A_VSH_BATCH];
	float constants[NV2A_VSH_CONSTANT_SLOTS][NV2A_VSH_BATCH];
	/* operands gathered at a0 or negated, and the units' results */
	float scratch[3][4][NV2A_VSH_BATCH];
	float results[3][4][NV2A_VSH_BATCH];
	/* the clip position the screen conversion started from, where the
	program has that conversion (clip_captured) */
	float clip[4][NV2A_VSH_BATCH];
	int a0[NV2A_VSH_BATCH];
};

/* compiles a decoded program for the outputs whose components are set in
outputs[16] (bit 3 x ... bit 0 w; outputs[0] is ignored: the position is
always kept) */
void nv2a_vsh_compile(const struct nv2a_vsh_program *program, const uint8_t outputs[16],
	struct nv2a_vsh_compiled *compiled);

/* runs it for count vertices (at most NV2A_VSH_BATCH), whose inputs the
caller has put in lanes->rows[NV2A_VSH_FILE_INPUTS + v] for every component
compiled->live_in asks for; the outputs asked for are then in the output
rows, the position in NV2A_VSH_FILE_POSITION */
void nv2a_vsh_run_batch(const struct nv2a_vsh_compiled *compiled, const float (*constants)[4],
	struct nv2a_vsh_lanes *lanes, unsigned long count);

/* nv2a_vsh_clip_position for the first count vertices of a batch */
void nv2a_vsh_lanes_clip_positions(const struct nv2a_vsh_compiled *compiled, const struct nv2a_vsh_lanes *lanes,
	unsigned long count, const float c38[4], const float c37[4], const float viewport_scale[3],
	const float viewport_offset[3], float (*clips)[4]);

/* nv2a_vsh_fetch for count vertices, stride bytes apart, into a batch's
input row (all four components) */
void nv2a_vsh_fetch_lanes(unsigned type, const unsigned char *data, unsigned long stride, unsigned long count,
	float (*row)[NV2A_VSH_BATCH]);

/* one vertex of a batch as the interpreter's result (the components not
compiled for are left as they were) */
void nv2a_vsh_lanes_result(const struct nv2a_vsh_compiled *compiled, const struct nv2a_vsh_lanes *lanes,
	unsigned long lane, struct nv2a_vsh_result *result);

/* NORMPACKED3, the compressed normals of the game's models and BSPs: x and y
11 bits, z 10, signed, in one 32-bit word */
void nv2a_unpack_normpacked3(uint32_t packed, float out[4]);

/* the data types of a vertex declaration's elements: the XDK's D3DVSDT_*,
which are the NV2A's own */
enum
{
	NV2A_VSDT_NORMSHORT1 = 0x11, NV2A_VSDT_FLOAT1 = 0x12, NV2A_VSDT_PBYTE1 = 0x14,
	NV2A_VSDT_SHORT1 = 0x15, NV2A_VSDT_NORMPACKED3 = 0x16,
	NV2A_VSDT_NORMSHORT2 = 0x21, NV2A_VSDT_FLOAT2 = 0x22, NV2A_VSDT_PBYTE2 = 0x24, NV2A_VSDT_SHORT2 = 0x25,
	NV2A_VSDT_NORMSHORT3 = 0x31, NV2A_VSDT_FLOAT3 = 0x32, NV2A_VSDT_PBYTE3 = 0x34, NV2A_VSDT_SHORT3 = 0x35,
	NV2A_VSDT_D3DCOLOR = 0x40, NV2A_VSDT_NORMSHORT4 = 0x41, NV2A_VSDT_FLOAT4 = 0x42, NV2A_VSDT_PBYTE4 = 0x44,
	NV2A_VSDT_SHORT4 = 0x45, NV2A_VSDT_FLOAT2H = 0x72,
};

/* the bytes one element of the type takes in a vertex; 0 for a type the
NV2A does not have */
unsigned long nv2a_vsh_type_bytes(unsigned type);

/* one element of a vertex as the NV2A reads it into its input register, in
the CPU's byte order (the Wii's: the map converter swaps the maps' vertices,
and the game writes its own); the components the type does not have are 0,
and w 1 */
void nv2a_vsh_fetch(unsigned type, const unsigned char *data, float out[4]);

/* the clip-space position, in Direct3D's terms, of what a program wrote: its
screen-space conversion with c[-38] and c[-37] undone, as the Linux port's
GLSL does it (port/linux/src/nv2a_vsh.c), from the clip position it kept
where it kept it. viewport_scale and viewport_offset are what Direct3D
derives the two constants from for the viewport (an axis whose scale is 0
counts as 1). A w of zero, or not a number, comes back as a point behind
the camera, which the clipper takes, as the Xbox's divide sent it to
infinity. */
void nv2a_vsh_clip_position(const struct nv2a_vsh_result *result, const float c38[4], const float c37[4],
	const float viewport_scale[3], const float viewport_offset[3], float clip[4]);

#endif /* __NV2A_VSH_RUN_H */
