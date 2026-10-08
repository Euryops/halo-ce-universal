/*
TEV_TEST.C

The combiners-to-TEV translation (nv2a_tev.c) checked on the host. Each of
the game's pixel shaders (psh_corpus.h), and as many random ones, is run
two ways on the same random inputs (texels, vertex colors, constants, fog):

- as the NV2A runs it, in floats, with the Linux port's GLSL semantics
  (port/linux/src/nv2a_psh.c) and the texels and colors as bytes;
- as TEV runs the translation, in TEV's integers, as Dolphin computes them
  (its software renderer, Source/Core/VideoBackends/Software/Tev.cpp).

and the two results compared, in 255ths. A program the translation refuses
is reported with its reason.

	tev_test            the corpus, then random programs; fails on a
	                    corpus program off by more than the tolerance
	tev_test -v N       the corpus entry N's TEV stages, and its worst inputs
*/

#include "nv2a_tev.h"
#include "psh_corpus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- inputs */

struct pixel_inputs
{
	/* [4]: the fog ramp's texel, the fog factor */
	uint8_t texels[5][4];
	uint8_t colors[2][4];
	uint32_t c0[8], c1[8], final_c0, final_c1, fog_color;
	uint8_t fog_factor;
};

static uint32_t random_state = 12345;
static int trace;

static uint32_t random_next(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* bytes, with 0, 255 and the middle more often than chance */
static uint8_t random_byte(void)
{
	switch (random_next() % 8)
	{
	case 0: return 0;
	case 1: return 255;
	case 2: return (uint8_t)(127 + random_next() % 3);
	default: return (uint8_t)random_next();
	}
}

static uint32_t random_color(void)
{
	return ((uint32_t)random_byte() << 24) | ((uint32_t)random_byte() << 16) | ((uint32_t)random_byte() << 8) | random_byte();
}

/* the texels, colors and fog at random; the constants are the program's
(the translation folds those of their channels that are 0 or 255) */
static void random_inputs(struct pixel_inputs *inputs, const struct nv2a_tev_options *options)
{
	int i, c;

	for (i = 0; i < 4; i++)
		for (c = 0; c < 4; c++)
			inputs->texels[i][c] = random_byte();
	for (i = 0; i < 2; i++)
		for (c = 0; c < 4; c++)
			inputs->colors[i][c] = random_byte();
	memcpy(inputs->c0, options->c0, sizeof(inputs->c0));
	memcpy(inputs->c1, options->c1, sizeof(inputs->c1));
	inputs->final_c0 = options->final_c0;
	inputs->final_c1 = options->final_c1;
	inputs->fog_color = random_color() & 0xffffff;
	inputs->fog_factor = random_byte();
	memset(inputs->texels[4], inputs->fog_factor, 4);
}

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

/* ---------- running both */

static void program_from_case(const struct psh_case *k, struct nv2a_combiners *combiners)
{
	int i;

	memset(combiners, 0, sizeof(*combiners));
	for (i = 0; i < 8; i++)
	{
		combiners->rgb_inputs[i] = (uint32_t)k->rgb_inputs[i];
		combiners->rgb_outputs[i] = (uint32_t)k->rgb_outputs[i];
		combiners->alpha_inputs[i] = (uint32_t)k->alpha_inputs[i];
		combiners->alpha_outputs[i] = (uint32_t)k->alpha_outputs[i];
	}
	combiners->final_inputs_abcd = (uint32_t)k->final_combiner_inputs_abcd;
	combiners->final_inputs_efg = (uint32_t)k->final_combiner_inputs_efg;
	combiners->combiner_count = (uint32_t)k->combiner_count;
	combiners->texture_modes = (uint32_t)k->texture_modes;
}

struct comparison
{
	int worst;
	double mean;
	int worst_trial;
	/* the share of inputs off by more than the tolerance */
	double off;
};

/* off by more than this, in 255ths, is wrong; a program is wrong when more
than one input in a hundred is (a multiplexer on r0's alpha, or an alpha test,
flips on a rounding at its edge, and that is right as rarely as it is
wrong) */
static const int tolerance = 6;

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

static struct comparison compare(const struct nv2a_combiners *combiners, const struct nv2a_tev_options *options,
	const struct nv2a_tev_program *program, int trials, int verbose_trial)
{
	struct comparison result = { 0, 0.0, 0 };
	int trial, c;
	double total = 0.0;

	for (trial = 0; trial < trials; trial++)
	{
		struct pixel_inputs inputs;
		float expected[4];
		uint8_t konst[4][4], colors[2][4], out[4];
		int initial[4][4], worst = 0;

		random_inputs(&inputs, options);
		nv2a_run(combiners, options, &inputs, expected);
		tev_inputs(program, &inputs, konst, initial, colors);
		tev_run(program, (const uint8_t (*)[4])konst, (const int (*)[4])initial, (const uint8_t (*)[4])inputs.texels,
			(const uint8_t (*)[4])colors, out);
		for (c = 0; c < 4; c++)
		{
			int difference = abs((int)out[c] - (int)floorf(expected[c] * 255.0f + 0.5f));

			if (difference > worst)
				worst = difference;
			total += difference;
		}
		if (worst > tolerance)
			result.off += 1.0;
		if (worst > result.worst)
		{
			result.worst = worst;
			result.worst_trial = trial;
		}
		if (trial == verbose_trial)
		{
			trace = 1;
			nv2a_run(combiners, options, &inputs, expected);
			tev_run(program, (const uint8_t (*)[4])konst, (const int (*)[4])initial, (const uint8_t (*)[4])inputs.texels,
				(const uint8_t (*)[4])colors, out);
			trace = 0;
			printf("  texels");
			for (c = 0; c < 4; c++)
				printf(" %02x%02x%02x%02x", inputs.texels[c][0], inputs.texels[c][1], inputs.texels[c][2], inputs.texels[c][3]);
			printf("  colors %02x%02x%02x%02x %02x%02x%02x%02x  fog %02x\n", colors[0][0], colors[0][1], colors[0][2],
				colors[0][3], colors[1][0], colors[1][1], colors[1][2], colors[1][3], inputs.fog_factor);
			printf("  konst");
			for (c = 0; c < 4; c++)
				printf(" %02x%02x%02x%02x", konst[c][0], konst[c][1], konst[c][2], konst[c][3]);
			printf("\n  nv2a %3d %3d %3d %3d   tev %3d %3d %3d %3d\n",
				(int)(expected[0] * 255 + 0.5f), (int)(expected[1] * 255 + 0.5f), (int)(expected[2] * 255 + 0.5f),
				(int)(expected[3] * 255 + 0.5f), out[0], out[1], out[2], out[3]);
		}
	}
	result.mean = total / (trials * 4.0);
	result.off /= trials;
	return result;
}

static void print_program(const struct nv2a_tev_program *program)
{
	int s, k;

	for (k = 0; k < 4; k++)
	{
		if (program->konst[k].source)
			printf("  K%d: source %d index %d\n", k, program->konst[k].source, program->konst[k].index);
	}
	for (s = 0; s < program->stage_count; s++)
	{
		const struct nv2a_tev_stage *st = &program->stages[s];

		printf("  %2d tex %3d/%d ras %3d/%d k %02x/%02x | c %2d %2d %2d %2d op%d b%d s%d c%d ->%d | a %d %d %d %d op%d b%d s%d c%d ->%d\n",
			s, st->texture == 0xff ? -1 : st->texture, st->texture_swap, st->color == 0xff ? -1 : st->color, st->color_swap,
			st->konst_color, st->konst_alpha,
			st->rgb.a, st->rgb.b, st->rgb.c, st->rgb.d, st->rgb.op, st->rgb.bias, st->rgb.scale, st->rgb.clamp, st->rgb.dest,
			st->alpha.a, st->alpha.b, st->alpha.c, st->alpha.d, st->alpha.op, st->alpha.bias, st->alpha.scale,
			st->alpha.clamp, st->alpha.dest);
	}
}

/* the options for a program: the textures its modes sample, fog, and its
own constants (the corpus entry's) or random ones */
static void options_for(const struct nv2a_combiners *combiners, int fog, const struct psh_case *k,
	struct nv2a_tev_options *options)
{
	int i;

	memset(options, 0, sizeof(*options));
	for (i = 0; i < 4; i++)
	{
		if ((combiners->texture_modes >> (5 * i)) & 0x1f)
			options->textures_sampled |= (uint8_t)(1 << i);
	}
	options->fog = (uint8_t)fog;
	for (i = 0; i < 8; i++)
	{
		options->c0[i] = k ? (uint32_t)k->constant_0[i] : random_color();
		options->c1[i] = k ? (uint32_t)k->constant_1[i] : random_color();
	}
	options->final_c0 = k ? (uint32_t)k->final_combiner_constant_0 : random_color();
	options->final_c1 = k ? (uint32_t)k->final_combiner_constant_1 : random_color();
}

/* a random program: any register, mapping and output, as the hardware
takes them */
static void random_program(struct nv2a_combiners *combiners)
{
	static const unsigned readable[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13 };
	static const unsigned writable_registers[] = { 0, 4, 5, 8, 9, 10, 11, 12, 13 };
	int stage, count = 1 + random_next() % 4;

	memset(combiners, 0, sizeof(*combiners));
	combiners->combiner_count = (uint32_t)count | 0x100 | ((random_next() & 1) ? 0x1000 : 0) | ((random_next() & 1) ? 0x10000 : 0);
	combiners->texture_modes = 0x21 | (1 << 10) | (1 << 15);
	for (stage = 0; stage < count; stage++)
	{
		int portion, i;

		for (portion = 0; portion < 2; portion++)
		{
			uint32_t inputs = 0, outputs;
			unsigned flags = (unsigned)(random_next() % 7) << 3;

			for (i = 0; i < 4; i++)
			{
				unsigned input = readable[random_next() % 12] | ((random_next() % 3 == 0) ? 0x10 : 0) |
					((random_next() % 8) << 5);

				inputs = (inputs << 8) | input;
			}
			if (flags == 0x28)
				flags = 0;
			if (!portion && random_next() % 4 == 0)
				flags |= 0x02;
			if (random_next() % 8 == 0)
				flags |= 0x04;
			outputs = (flags << 12) | (writable_registers[random_next() % 9] << 4) | writable_registers[random_next() % 9] |
				(writable_registers[random_next() % 9] << 8);
			if (portion)
			{
				combiners->alpha_inputs[stage] = inputs;
				combiners->alpha_outputs[stage] = outputs;
			}
			else
			{
				combiners->rgb_inputs[stage] = inputs;
				combiners->rgb_outputs[stage] = outputs;
			}
		}
	}
	if (random_next() % 4)
	{
		uint32_t abcd = 0, efg = 0;
		int i;
		static const unsigned final_readable[] = { 0, 1, 2, 3, 4, 5, 8, 9, 12, 13, 14, 15 };

		for (i = 0; i < 4; i++)
			abcd = (abcd << 8) | final_readable[random_next() % 12] | ((random_next() % 4 == 0) ? 0x10 : 0) | ((random_next() & 1) ? 0x20 : 0);
		for (i = 0; i < 3; i++)
			efg = (efg << 8) | (i < 2 ? (final_readable[random_next() % 10] | ((random_next() & 1) ? 0x20 : 0)) :
				(final_readable[random_next() % 12] | ((random_next() & 1) ? 0x10 : 0)));
		efg = (efg << 8) | ((random_next() & 7) << 5);
		combiners->final_inputs_abcd = abcd;
		combiners->final_inputs_efg = efg;
	}
}

int main(int argc, char **argv)
{
	int count = (int)(sizeof(psh_corpus) / sizeof(psh_corpus[0]));
	int index, refused = 0, off = 0, approximate = 0, fog;
	int total_stages = 0, compiled = 0, maximum_stages = 0;
	int verbose = argc > 2 && !strcmp(argv[1], "-v") ? atoi(argv[2]) : -1;

	if (argc > 2 && !strcmp(argv[1], "-s"))
	{
		/* one random program again, by the seed it was reported with */
		struct nv2a_combiners combiners;
		struct nv2a_tev_options options;
		struct nv2a_tev_program program;
		const char *failure;
		struct comparison result;

		random_state = (uint32_t)strtoul(argv[2], NULL, 10);
		random_program(&combiners);
		options_for(&combiners, (int)(random_next() & 1), NULL, &options);
		if (!nv2a_tev_compile(&combiners, &options, &program, &failure))
		{
			printf("refused: %s\n", failure);
			return 1;
		}
		result = compare(&combiners, &options, &program, 200, -1);
		printf("worst %d, mean %.3f, %.1f%% off, %d stages, fog %d\n", result.worst, result.mean, 100.0 * result.off,
			program.stage_count, options.fog);
		print_program(&program);
		random_state = (uint32_t)strtoul(argv[2], NULL, 10);
		random_program(&combiners);
		options_for(&combiners, (int)(random_next() & 1), NULL, &options);
		compare(&combiners, &options, &program, result.worst_trial + 1, result.worst_trial);
		return 0;
	}
	struct
	{
		const char *reason;
		int count;
	} reasons[32];
	int reason_count = 0;

	for (index = 0; index < count; index++)
	{
		const struct psh_case *k = &psh_corpus[index];
		struct nv2a_combiners combiners;

		if (verbose >= 0 && index != verbose)
			continue;
		program_from_case(k, &combiners);
		/* fog off and on, with the game's constants and then random ones */
		for (fog = 0; fog < 4; fog++)
		{
			struct nv2a_tev_options options;
			struct nv2a_tev_program program;
			const char *failure;
			struct comparison result;

			random_state = 777u + (uint32_t)index * 4u + (uint32_t)fog;
			options_for(&combiners, fog & 1, fog < 2 ? k : NULL, &options);
			if (!nv2a_tev_compile(&combiners, &options, &program, &failure))
			{
				printf("REFUSED %3d %s (%s)%s: %s\n", index, k->name, k->source,
					fog == 0 ? "" : fog == 1 ? " with fog" : " with random constants", failure);
				refused++;
				break;
			}
			result = compare(&combiners, &options, &program, 3000, -1);
			if (!fog)
			{
				compiled++;
				total_stages += program.stage_count;
				if (program.stage_count > maximum_stages)
					maximum_stages = program.stage_count;
				if (program.approximations)
					approximate++;
			}
			if (verbose >= 0)
			{
				printf("%3d %s (%s) case %d: %d stages, approximations %x, worst %d, mean %.3f\n", index, k->name,
					k->source, fog, program.stage_count, program.approximations, result.worst, result.mean);
				print_program(&program);
				random_state = 777u + (uint32_t)index * 4u + (uint32_t)fog;
				options_for(&combiners, fog & 1, fog < 2 ? k : NULL, &options);
				compare(&combiners, &options, &program, result.worst_trial + 1, result.worst_trial);
			}
			if (result.off > 0.01)
			{
				printf("OFF     %3d %s (%s) case %d: worst %d/255, mean %.3f, %.1f%% off, %d stages\n", index, k->name,
					k->source, fog, result.worst, result.mean, 100.0 * result.off, program.stage_count);
				off++;
				break;
			}
		}
	}
	printf("corpus: %d programs, %d translated (%.1f TEV stages on average, %d at most, %d with an approximated "
		"texture mode or fog), %d refused, %d off by more than %d/255 on more than 1%% of inputs\n",
		count, compiled, compiled ? (double)total_stages / compiled : 0.0, maximum_stages, approximate, refused, off,
		tolerance);
	if (verbose >= 0)
		return 0;

	/* random programs */
	{
		int trials = argc > 1 ? atoi(argv[1]) : 3000, random_refused = 0, random_off = 0, worst_seen = 0, random_approximated = 0;

		for (index = 0; index < trials; index++)
		{
			struct nv2a_combiners combiners;
			struct nv2a_tev_options options;
			struct nv2a_tev_program program;
			const char *failure;
			struct comparison result;
			uint32_t seed;
			int r;

			seed = random_state;
			random_program(&combiners);
			options_for(&combiners, (int)(random_next() & 1), NULL, &options);
			if (!nv2a_tev_compile(&combiners, &options, &program, &failure))
			{
				random_refused++;
				for (r = 0; r < reason_count && strcmp(reasons[r].reason, failure); r++)
					;
				if (r == reason_count && reason_count < 32)
				{
					reasons[r].reason = failure;
					reasons[r].count = 0;
					reason_count++;
				}
				if (r < 32)
					reasons[r].count++;
				continue;
			}
			result = compare(&combiners, &options, &program, 200, -1);
			if (result.worst > worst_seen)
				worst_seen = result.worst;
			if (program.approximations & ~(uint32_t)_nv2a_tev_approximation_texture_mode)
			{
				random_approximated++;
				continue;
			}
			if (result.off > 0.01)
			{
				if (random_off < 5 || result.worst > 20)
				{
					int s;

					printf("random OFF seed %u worst %d: count %x modes %x", seed, result.worst, combiners.combiner_count,
						combiners.texture_modes);
					for (s = 0; s < (int)(combiners.combiner_count & 0xff); s++)
						printf(" [%08x %08x %08x %08x]", combiners.rgb_inputs[s], combiners.rgb_outputs[s],
							combiners.alpha_inputs[s], combiners.alpha_outputs[s]);
					printf(" final %08x %08x\n", combiners.final_inputs_abcd, combiners.final_inputs_efg);
				}
				random_off++;
			}
		}
		printf("random: %d programs, %d refused, %d with an approximation declared, %d off by more than %d/255 "
			"(worst %d)\n", trials, random_refused, random_approximated, random_off, tolerance, worst_seen);
		for (index = 0; index < reason_count; index++)
			printf("  refused %4d: %s\n", reasons[index].count, reasons[index].reason);
	}
	/* refusals are expected (the longest of the environment's specular
	programs do not fit 16 stages) and so is the odd random program off at
	a multiplexer's edge or past TEV's range; a game program off is not */
	return off ? 1 : 0;
}
