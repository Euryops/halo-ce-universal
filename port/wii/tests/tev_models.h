/*
TEV_MODELS.H

The two models the TEV translation (nv2a_tev.c) is checked between, for
the host test (tev_test.c) and the Dolphin sheet's checker
(tev_sheet_check.c):

- the NV2A, in floats, with the Linux port's GLSL semantics
  (port/linux/src/nv2a_psh.c) and the texels and colors as bytes;
- TEV running a translation, in TEV's integers, as Dolphin computes them
  (its software renderer, Source/Core/VideoBackends/Software/Tev.cpp).
*/

#ifndef __TEV_MODELS_H
#define __TEV_MODELS_H

#include "nv2a_tev.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* prints each stage's registers as the models run (the test's -v and -s) */
static int trace;

/* ---------- what a pixel is given */

struct pixel_inputs
{
	/* [4]: the fog ramp's texel, the fog factor */
	uint8_t texels[5][4];
	uint8_t colors[2][4];
	uint32_t c0[8], c1[8], final_c0, final_c1, fog_color;
	uint8_t fog_factor;
};

/* ---------- the NV2A, in floats */

struct vec4
{
	float v[4];
};

static float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : x > hi ? hi : x;
}

static struct vec4 color_vec4(uint32_t color)
{
	struct vec4 r;

	r.v[0] = ((color >> 16) & 0xff) / 255.0f;
	r.v[1] = ((color >> 8) & 0xff) / 255.0f;
	r.v[2] = (color & 0xff) / 255.0f;
	r.v[3] = ((color >> 24) & 0xff) / 255.0f;
	return r;
}

static struct vec4 bytes_vec4(const uint8_t *b)
{
	struct vec4 r;
	int c;

	for (c = 0; c < 4; c++)
		r.v[c] = b[c] / 255.0f;
	return r;
}

struct nv2a_state
{
	struct vec4 reg[16];
	const struct nv2a_combiners *combiners;
	const struct pixel_inputs *inputs;
	int unique_c0, unique_c1;
};

static struct vec4 register_read(struct nv2a_state *s, int reg, int stage)
{
	struct vec4 zero = { { 0, 0, 0, 0 } };

	switch (reg)
	{
	case 1:
		if (stage < 0)
			return color_vec4(s->inputs->final_c0);
		return color_vec4(s->inputs->c0[s->unique_c0 ? stage : 0]);
	case 2:
		if (stage < 0)
			return color_vec4(s->inputs->final_c1);
		return color_vec4(s->inputs->c1[s->unique_c1 ? stage : 0]);
	case 14: case 15:
		return stage < 0 ? s->reg[reg] : zero;
	default:
		return reg < 16 ? s->reg[reg] : zero;
	}
}

static float mapped(float x, int mapping)
{
	switch (mapping)
	{
	case 0x00: return fmaxf(x, 0.0f);
	case 0x20: return 1.0f - clampf(x, 0.0f, 1.0f);
	case 0x40: return 2.0f * fmaxf(x, 0.0f) - 1.0f;
	case 0x60: return 1.0f - 2.0f * fmaxf(x, 0.0f);
	case 0x80: return fmaxf(x, 0.0f) - 0.5f;
	case 0xa0: return 0.5f - fmaxf(x, 0.0f);
	case 0xc0: return x;
	default: return -x;
	}
}

/* an input as three channels (rgb) or one (alpha portion, in [0]) */
static void combiner_input(struct nv2a_state *s, unsigned input, int alpha, int stage, float out[3])
{
	struct vec4 source = register_read(s, input & 0x0f, stage);
	int alpha_channel = (input & 0x10) != 0;
	int c;

	for (c = 0; c < 3; c++)
	{
		float x = alpha ? source.v[alpha_channel ? 3 : 2] : alpha_channel ? source.v[3] : source.v[c];

		out[c] = mapped(x, input & 0xe0);
	}
}

static float output_mapped(float x, unsigned flags)
{
	switch (flags & 0x38)
	{
	case 0x08: x = x - 0.5f; break;
	case 0x10: x = x * 2.0f; break;
	case 0x18: x = (x - 0.5f) * 2.0f; break;
	case 0x20: x = x * 4.0f; break;
	case 0x30: x = x * 0.5f; break;
	}
	return clampf(x, -1.0f, 1.0f);
}

static int writable(int reg)
{
	return reg == 4 || reg == 5 || (reg >= 8 && reg <= 13);
}

static void nv2a_run(const struct nv2a_combiners *combiners, const struct nv2a_tev_options *options,
	const struct pixel_inputs *inputs, float result[4])
{
	struct nv2a_state s;
	int stage, i, c, portion;
	unsigned count = combiners->combiner_count & 0xff;
	uint32_t abcd = combiners->final_inputs_abcd, efg = combiners->final_inputs_efg;
	struct vec4 final;

	memset(&s, 0, sizeof(s));
	s.combiners = combiners;
	s.inputs = inputs;
	s.unique_c0 = (combiners->combiner_count & 0x1000) != 0;
	s.unique_c1 = (combiners->combiner_count & 0x10000) != 0;
	for (i = 0; i < 4; i++)
	{
		unsigned mode = (combiners->texture_modes >> (5 * i)) & 0x1f;
		int sampled = mode != 0 && mode != 0x04 && mode != 0x05 && mode != 0x0a && mode != 0x11 &&
			(options->textures_sampled & (1 << i));

		if (sampled)
			s.reg[8 + i] = bytes_vec4(inputs->texels[i]);
	}
	s.reg[4] = bytes_vec4(inputs->colors[0]);
	s.reg[5] = bytes_vec4(inputs->colors[1]);
	{
		struct vec4 fog = color_vec4(inputs->fog_color);

		fog.v[3] = options->fog ? inputs->fog_factor / 255.0f : 1.0f;
		s.reg[3] = fog;
	}
	s.reg[12].v[3] = s.reg[8].v[3];
	if (count > 8)
		count = 8;
	for (stage = 0; stage < (int)count; stage++)
	{
		unsigned mux_msb = (combiners->combiner_count & 0x100) != 0;
		float ab[2][3], cd[2][3], sum[2][3];
		unsigned outputs[2];

		for (portion = 0; portion < 2; portion++)
		{
			uint32_t inputs_word = portion ? combiners->alpha_inputs[stage] : combiners->rgb_inputs[stage];
			unsigned flags;
			float a[3], b[3], cc[3], d[3];
			int alpha = portion;

			outputs[portion] = portion ? combiners->alpha_outputs[stage] : combiners->rgb_outputs[stage];
			flags = outputs[portion] >> 12;
			combiner_input(&s, (inputs_word >> 24) & 0xff, alpha, stage, a);
			combiner_input(&s, (inputs_word >> 16) & 0xff, alpha, stage, b);
			combiner_input(&s, (inputs_word >> 8) & 0xff, alpha, stage, cc);
			combiner_input(&s, inputs_word & 0xff, alpha, stage, d);
			for (c = 0; c < 3; c++)
			{
				ab[portion][c] = a[c] * b[c];
				cd[portion][c] = cc[c] * d[c];
			}
			if (!alpha && (flags & 0x02))
				ab[portion][0] = ab[portion][1] = ab[portion][2] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
			if (!alpha && (flags & 0x01))
				cd[portion][0] = cd[portion][1] = cd[portion][2] = cc[0] * d[0] + cc[1] * d[1] + cc[2] * d[2];
			for (c = 0; c < 3; c++)
			{
				if (flags & 0x04)
				{
					int pick = mux_msb ? s.reg[12].v[3] >= 0.5f : ((int)(s.reg[12].v[3] * 255.0f + 0.5f) & 1);

					sum[portion][c] = pick ? cd[portion][c] : ab[portion][c];
				}
				else
				{
					sum[portion][c] = ab[portion][c] + cd[portion][c];
				}
				ab[portion][c] = output_mapped(ab[portion][c], flags);
				cd[portion][c] = output_mapped(cd[portion][c], flags);
				sum[portion][c] = output_mapped(sum[portion][c], flags);
			}
		}
		if (trace)
			printf("  nv2a stage %d in: r0 %.3f %.3f %.3f %.3f r1 %.3f %.3f %.3f %.3f | sum rgb %.3f %.3f %.3f a %.3f ab %.3f cd %.3f\n", stage,
				s.reg[12].v[0], s.reg[12].v[1], s.reg[12].v[2], s.reg[12].v[3], s.reg[13].v[0], s.reg[13].v[1], s.reg[13].v[2],
				s.reg[13].v[3], sum[0][0], sum[0][1], sum[0][2], sum[1][0], ab[1][0], cd[1][0]);
		for (portion = 0; portion < 2; portion++)
		{
			unsigned flags = outputs[portion] >> 12;
			int ab_reg = (outputs[portion] >> 4) & 0x0f, cd_reg = outputs[portion] & 0x0f, sum_reg = (outputs[portion] >> 8) & 0x0f;

			if (writable(ab_reg))
			{
				if (portion)
					s.reg[ab_reg].v[3] = ab[1][0];
				else
				{
					for (c = 0; c < 3; c++)
						s.reg[ab_reg].v[c] = ab[0][c];
					if (flags & 0x80)
						s.reg[ab_reg].v[3] = ab[0][2];
				}
			}
			if (writable(cd_reg))
			{
				if (portion)
					s.reg[cd_reg].v[3] = cd[1][0];
				else
				{
					for (c = 0; c < 3; c++)
						s.reg[cd_reg].v[c] = cd[0][c];
					if (flags & 0x40)
						s.reg[cd_reg].v[3] = cd[0][2];
				}
			}
			if (writable(sum_reg))
			{
				if (portion)
					s.reg[sum_reg].v[3] = sum[1][0];
				else
					for (c = 0; c < 3; c++)
						s.reg[sum_reg].v[c] = sum[0][c];
			}
		}
	}
	if (!abcd && !efg)
	{
		final = s.reg[12];
	}
	else
	{
		unsigned settings = efg & 0xff;
		float fa[3], fb[3], fc[3], fd[3], e[3], f[3], g;
		unsigned bytes[7];

		for (i = 0; i < 4; i++)
			bytes[i] = (abcd >> (24 - 8 * i)) & 0xff;
		for (i = 0; i < 3; i++)
			bytes[4 + i] = (efg >> (24 - 8 * i)) & 0xff;
		memset(&s.reg[14], 0, sizeof(s.reg[14]));
		memset(&s.reg[15], 0, sizeof(s.reg[15]));
		/* E and F first: they cannot read EF or v1r0_sum usefully */
		for (i = 0; i < 2; i++)
		{
			unsigned input = bytes[4 + i];
			struct vec4 source = register_read(&s, input & 0x0f, -1);
			float *out = i ? f : e;

			for (c = 0; c < 3; c++)
			{
				float x = clampf((input & 0x10) ? source.v[3] : source.v[c], 0.0f, 1.0f);

				out[c] = (input & 0x20) ? 1.0f - x : x;
			}
		}
		for (c = 0; c < 3; c++)
		{
			float v1 = clampf(s.reg[5].v[c], 0.0f, 1.0f), r0 = clampf(s.reg[12].v[c], 0.0f, 1.0f);

			s.reg[15].v[c] = e[c] * f[c];
			s.reg[14].v[c] = ((settings & 0x40) ? 1.0f - v1 : v1) + ((settings & 0x20) ? 1.0f - r0 : r0);
			if (settings & 0x80)
				s.reg[14].v[c] = clampf(s.reg[14].v[c], 0.0f, 1.0f);
		}
		for (i = 0; i < 4; i++)
		{
			unsigned input = bytes[i];
			struct vec4 source = register_read(&s, input & 0x0f, -1);
			float *out = i == 0 ? fa : i == 1 ? fb : i == 2 ? fc : fd;

			for (c = 0; c < 3; c++)
			{
				float x = clampf((input & 0x10) ? source.v[3] : source.v[c], 0.0f, 1.0f);

				out[c] = (input & 0x20) ? 1.0f - x : x;
			}
		}
		{
			unsigned input = bytes[6];
			struct vec4 source = register_read(&s, input & 0x0f, -1);
			float x = clampf((input & 0x10) ? source.v[3] : source.v[2], 0.0f, 1.0f);

			g = (input & 0x20) ? 1.0f - x : x;
		}
		for (c = 0; c < 3; c++)
			final.v[c] = fa[c] * fb[c] + (1.0f - fa[c]) * fc[c] + fd[c];
		final.v[3] = g;
	}
	for (c = 0; c < 4; c++)
		result[c] = clampf(final.v[c], 0.0f, 1.0f);
}

/* ---------- TEV, in integers, as Dolphin's software renderer */

static const int konst_fractions[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };

static int konst_component(const uint8_t konst[4][4], int select, int channel, int alpha)
{
	if (select < 8)
		return konst_fractions[select];
	if (!alpha && select >= 0x0c && select <= 0x0f)
		return konst[select - 0x0c][channel];
	return konst[select & 3][(select - 0x10) >> 2];
}

static int sign_extend_11(int x)
{
	x &= 0x7ff;
	return x >= 0x400 ? x - 0x800 : x;
}

static int regular(int a, int b, int c, int d, int op, int bias, int scale)
{
	static const int bias_values[4] = { 0, 128, -128, 0 };
	static const int left[4] = { 0, 1, 2, 0 };
	static const int right[4] = { 0, 0, 0, 1 };
	int c2 = c + (c >> 7);
	int lerp = a * (256 - c2) + b * c2;
	int result;

	lerp <<= left[scale];
	lerp += scale == 3 ? 0 : op ? 127 : 128;
	lerp >>= 8;
	if (op)
		lerp = -lerp;
	result = ((d + bias_values[bias]) << left[scale]) + lerp;
	return result >> right[scale];
}

static void tev_run(const struct nv2a_tev_program *program, const uint8_t konst[4][4], const int initial[4][4],
	const uint8_t texels[5][4], const uint8_t colors[2][4], uint8_t out[4])
{
	int reg[4][4];
	int s, c;

	memcpy(reg, initial, sizeof(reg));
	for (s = 0; s < program->stage_count; s++)
	{
		const struct nv2a_tev_stage *stage = &program->stages[s];
		int tex[4] = { 0, 0, 0, 0 }, ras[4] = { 0, 0, 0, 0 };
		int result[4];
		int pipe;

		if (stage->texture != 0xff)
			for (c = 0; c < 4; c++)
				tex[c] = texels[stage->texture][program->swap_tables[stage->texture_swap][c]];
		if (stage->color != 0xff)
			for (c = 0; c < 4; c++)
				ras[c] = colors[stage->color][program->swap_tables[stage->color_swap][c]];
		for (pipe = 0; pipe < 2; pipe++)
		{
			const struct nv2a_tev_combine *combine = pipe ? &stage->alpha : &stage->rgb;
			int first = pipe ? 3 : 0, last = pipe ? 4 : 3;

			for (c = first; c < last; c++)
			{
				int in[4], i, value;

				for (i = 0; i < 4; i++)
				{
					int selector = (&combine->a)[i];

					if (!pipe)
					{
						switch (selector)
						{
						case 0: case 2: case 4: case 6: value = reg[selector / 2][c]; break;
						case 1: case 3: case 5: case 7: value = reg[selector / 2][3]; break;
						case 8: value = tex[c]; break;
						case 9: value = tex[3]; break;
						case 10: value = ras[c]; break;
						case 11: value = ras[3]; break;
						case 12: value = 255; break;
						case 13: value = 128; break;
						case 14: value = konst_component(konst, stage->konst_color, c, 0); break;
						default: value = 0; break;
						}
					}
					else
					{
						switch (selector)
						{
						case 0: case 1: case 2: case 3: value = reg[selector][3]; break;
						case 4: value = tex[3]; break;
						case 5: value = ras[3]; break;
						case 6: value = konst_component(konst, stage->konst_alpha, 3, 1); break;
						default: value = 0; break;
						}
					}
					in[i] = i < 3 ? value & 255 : sign_extend_11(value);
				}
				if (combine->op <= 1)
				{
					value = regular(in[0], in[1], in[2], in[3], combine->op, combine->bias, combine->scale);
				}
				else
				{
					/* comparisons: only the 8-bit ones are used */
					int greater = in[0] > in[1];
					int equal = in[0] == in[1];
					int pass = (combine->op & 1) ? equal : greater;

					value = in[3] + (pass ? in[2] : 0);
				}
				result[c] = combine->clamp ? (value < 0 ? 0 : value > 255 ? 255 : value) :
					(value < -1024 ? -1024 : value > 1023 ? 1023 : value);
			}
		}
		for (c = 0; c < 3; c++)
			reg[stage->rgb.dest][c] = result[c];
		reg[stage->alpha.dest][3] = result[3];
		if (trace)
			printf("  tev %2d: rgb %4d %4d %4d ->%d  a %4d ->%d\n", s, result[0], result[1], result[2], stage->rgb.dest,
				result[3], stage->alpha.dest);
	}
	for (c = 0; c < 4; c++)
		out[c] = (uint8_t)(reg[0][c] & 255);
}

static void tev_inputs(const struct nv2a_tev_program *program, const struct pixel_inputs *inputs,
	uint8_t konst[4][4], int initial[4][4], uint8_t colors[2][4])
{
	int k, c;

	for (k = 0; k < 4; k++)
	{
		nv2a_tev_constant_value(&program->konst[k], inputs->c0, inputs->c1, inputs->final_c0, inputs->final_c1,
			inputs->fog_color, konst[k]);
	}
	memset(initial, 0, sizeof(int) * 16);
	for (k = 0; k < 4; k++)
	{
		uint8_t value[4];

		if (program->initial_color[k].source)
		{
			nv2a_tev_constant_value(&program->initial_color[k], inputs->c0, inputs->c1, inputs->final_c0,
				inputs->final_c1, inputs->fog_color, value);
			for (c = 0; c < 3; c++)
				initial[k][c] = value[c];
		}
		if (program->initial_alpha[k].source)
		{
			nv2a_tev_constant_value(&program->initial_alpha[k], inputs->c0, inputs->c1, inputs->final_c0,
				inputs->final_c1, inputs->fog_color, value);
			initial[k][3] = value[3];
		}
	}
	memcpy(colors, inputs->colors, 8);

}

#endif /* __TEV_MODELS_H */
