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

/* NORMPACKED3, the compressed normals of the game's models and BSPs: x and y
11 bits, z 10, signed, in one 32-bit word */
void nv2a_unpack_normpacked3(uint32_t packed, float out[4]);

#endif /* __NV2A_VSH_RUN_H */
