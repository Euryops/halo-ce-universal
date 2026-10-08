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

#include "tev_models.h"
#include "psh_corpus.h"

static uint32_t random_state = 12345;

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
