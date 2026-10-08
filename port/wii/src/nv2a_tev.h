/*
NV2A_TEV.H

The Xbox's pixel shaders (the NV2A's register combiners) as TEV stages, the
Wii GPU's own combiners. The GX device (d3d8_gx.c) hands each draw's
combiner state to nv2a_tev_compile and loads what comes back into GX
(gx_backend.c); the Linux port does the same job in GLSL (nv2a_psh.c), and
this file's semantics are that one's.

Plain C, with GX's numbers written out rather than libogc's header included,
so that the translation can also be built and checked on the host
(port/wii/tests/tev_test.c), against an NV2A reference and a TEV that works
as Dolphin's does.

How it is done (nv2a_tev.c says more):
- Each combiner stage's outputs are written out as sums of terms: products of
  two inputs, single inputs and a constant, each with a power of two for its
  weight. A TEV stage computes (d +- lerp(a, b, c) + bias) * scale, so the
  terms are added one per stage, the largest weight first, and the scales
  put the weights back.
- The rgb portion runs in TEV's color pipe and the alpha portion in its alpha
  pipe, as they do on the NV2A. An rgb value whose three channels are equal
  (a dot product, or anything made of alpha inputs) is computed once, in the
  alpha pipe, and read back by the color pipe as an alpha input; dot products
  are summed channel by channel, the texture's channels picked by TEV's swap
  tables. That is also how the alpha portion reads a register's blue.
- A register that can go below zero is kept as (x + 1) / 2, since TEV's
  inputs other than d are unsigned bytes; NV2A clamps to [-1, 1] after each
  stage, and so does that form.
- TEV has four registers (one is PREV, where the result ends) and four
  constant registers; the combiner constants go into the constant registers,
  and into the plain ones as initial values when there are more than four.
*/

#ifndef __NV2A_TEV_H
#define __NV2A_TEV_H

#include <stdint.h>

#define NV2A_TEV_MAXIMUM_STAGES 16
/* the texture (map and coordinates) that carries the fog factor */
#define NV2A_TEV_FOG_TEXTURE 4

/* the combiner state, as the D3DRS_PS* render states hold it */
struct nv2a_combiners
{
	uint32_t rgb_inputs[8], rgb_outputs[8], alpha_inputs[8], alpha_outputs[8];
	uint32_t final_inputs_abcd, final_inputs_efg;
	uint32_t combiner_count;
	uint32_t texture_modes;
};

/* what else about the draw changes the program */
struct nv2a_tev_options
{
	/* texture stages that have a texture bound and a mode that samples it:
	the others read as zero */
	uint8_t textures_sampled;
	/* D3DRS_FOGENABLE: the fog register's alpha is the fog factor; without
	it, 1 */
	uint8_t fog;
	/* the constants (D3DCOLOR), of which only the channels that are 0 or 255
	are looked at: those are folded into the program as numbers (the game
	picks channels with constants such as 0x00ff0000), the others are read
	from TEV's constant registers. nv2a_tev_constant_classes says which a
	program depends on */
	uint32_t c0[8], c1[8], final_c0, final_c1;
};

/* where a constant register's value comes from, for the device to fill in
at each draw */
enum
{
	_nv2a_constant_none,
	/* PSConstant0/1 of a stage (index), the final combiner's two */
	_nv2a_constant_c0,
	_nv2a_constant_c1,
	_nv2a_constant_final_c0,
	_nv2a_constant_final_c1,
	/* D3DRS_FOGCOLOR */
	_nv2a_constant_fog,
	/* a number of the program's own (value) */
	_nv2a_constant_literal,
};

struct nv2a_tev_constant
{
	uint8_t source;
	/* the stage, for c0 and c1 */
	uint8_t index;
	/* the literal's RGBA */
	uint8_t value[4];
	/* as a register's initial value: one channel of it in all four
	(GX_CH_*), or 4 for the color as it is; and 1 - it */
	uint8_t lane;
	uint8_t invert;
};

/* one TEV stage, in GX's numbers (GX_CC_*, GX_CA_*, GX_TEV_*, GX_TB_*,
GX_CS_*, GX_TEVPREV/REGn, GX_TEV_KCSEL_*, GX_TEV_KASEL_*) */
struct nv2a_tev_combine
{
	uint8_t a, b, c, d;
	uint8_t op, bias, scale, clamp, dest;
};

struct nv2a_tev_stage
{
	/* the texture stage sampled (texture coordinates and map alike), or 0xff */
	uint8_t texture;
	/* 0 the diffuse color (GX_COLOR0A0), 1 the specular (GX_COLOR1A1), or 0xff */
	uint8_t color;
	uint8_t texture_swap, color_swap;
	uint8_t konst_color, konst_alpha;
	struct nv2a_tev_combine rgb, alpha;
};

struct nv2a_tev_program
{
	int stage_count;
	struct nv2a_tev_stage stages[NV2A_TEV_MAXIMUM_STAGES];
	/* GX_SetTevSwapModeTable: each table's red, green, blue and alpha
	(GX_CH_*) */
	uint8_t swap_tables[4][4];
	/* the constant registers KCOLOR0-3 */
	struct nv2a_tev_constant konst[4];
	/* the registers' values at the first stage (PREV, REG0-2), when they hold
	constants; each register's color and alpha are separate */
	struct nv2a_tev_constant initial_color[4], initial_alpha[4];
	/* the program reads the fog factor, as texture stage
	NV2A_TEV_FOG_TEXTURE's alpha: a ramp (each texel's value its coordinate)
	looked up at the vertex's fog factor */
	uint8_t fog_texture;
	/* what the translation could only come near: each bit an
	_nv2a_tev_approximation */
	uint32_t approximations;
};

enum
{
	/* a texture mode TEV has no equivalent for (cube maps are sampled as 2D,
	dot product modes as their last lookup) */
	_nv2a_tev_approximation_texture_mode = 1 << 0,
	/* the multiplexer on bit 0 of r0's alpha, taken as bit 7 */
	_nv2a_tev_approximation_mux_lsb = 1 << 1,
	/* the alpha portion reads the blue of a register the rgb portion wrote in
	three different channels: read as zero */
	_nv2a_tev_approximation_blue = 1 << 3,
};

/* the translation; 0 when the program does not fit TEV (more stages,
registers or swap tables than there are), with the reason in *failure */
int nv2a_tev_compile(const struct nv2a_combiners *combiners, const struct nv2a_tev_options *options,
	struct nv2a_tev_program *program, const char **failure);

/* which channels of the constants are 0 and which 255, as the translation
folds them: two bits a channel (1 zero, 2 one), c0[0..7], c1[0..7], final
c0 and c1, in five words; two draws whose classes and combiner state are
the same share a translation */
void nv2a_tev_constant_classes(const struct nv2a_tev_options *options, uint32_t classes[5]);

/* the constant's RGBA as TEV's registers take it (its lane and inversion
applied), from the combiner constants (D3DCOLOR, alpha in the top byte, as the render states
hold them) and the fog color */
void nv2a_tev_constant_value(const struct nv2a_tev_constant *constant, const uint32_t c0[8], const uint32_t c1[8],
	uint32_t final_c0, uint32_t final_c1, uint32_t fog_color, uint8_t rgba[4]);

#endif /* __NV2A_TEV_H */
