/*
TEV_SHEET_EXPECT.C

What the shader sheet (tev_sheet.h, drawn by tevtest.dol) should look like,
for check.py to compare Dolphin's frame dumps with. Four 640x480 RGB images
(raw, 8 bits a channel), each the sheet as tevtest.dol lays it out:

	nv2a_rgb.raw    each tile as the NV2A computes its pixel shader
	nv2a_alpha.raw  its alpha, as gray
	tev_rgb.raw     each tile as TEV computes its translation, in the
	                integers of Dolphin's software renderer (tev_models.h)
	tev_alpha.raw   its alpha, as gray

and tiles.txt, a line for each tile: its number, the stages of its
translation (0: refused) and its name.

	tev_sheet_expect <folder>
*/

#include "tev_models.h"
#include "psh_corpus.h"
#include "tev_sheet.h"

#define WIDTH 640
#define HEIGHT 480
#define CORPUS_COUNT ((int)(sizeof(psh_corpus) / sizeof(psh_corpus[0])))

static unsigned char images[4][HEIGHT][WIDTH][3];

static void put(int image, int x, int y, int r, int g, int b)
{
	images[image][y][x][0] = (unsigned char)r;
	images[image][y][x][1] = (unsigned char)g;
	images[image][y][x][2] = (unsigned char)b;
}

int main(int argc, char **argv)
{
	char path[1024];
	FILE *tiles;
	int image, tile, x, y;
	static const char *names[4] = { "nv2a_rgb", "nv2a_alpha", "tev_rgb", "tev_alpha" };

	if (argc != 2)
	{
		fprintf(stderr, "usage: tev_sheet_expect <folder>\n");
		return 2;
	}
	for (image = 0; image < 4; image++)
		for (y = 0; y < HEIGHT; y++)
			for (x = 0; x < WIDTH; x++)
				put(image, x, y, 0x20, 0x20, 0x20);
	snprintf(path, sizeof(path), "%s/tiles.txt", argv[1]);
	tiles = fopen(path, "w");
	if (!tiles)
	{
		perror(path);
		return 1;
	}
	for (tile = 0; tile < CORPUS_COUNT; tile++)
	{
		const struct psh_case *k = &psh_corpus[tile];
		struct nv2a_combiners combiners;
		struct nv2a_tev_options options;
		struct nv2a_tev_program program;
		const char *failure;
		uint8_t colors[2][4], fog;
		int origin_x, origin_y, translated, i;

		memset(&combiners, 0, sizeof(combiners));
		memset(&options, 0, sizeof(options));
		for (i = 0; i < 8; i++)
		{
			combiners.rgb_inputs[i] = (uint32_t)k->rgb_inputs[i];
			combiners.rgb_outputs[i] = (uint32_t)k->rgb_outputs[i];
			combiners.alpha_inputs[i] = (uint32_t)k->alpha_inputs[i];
			combiners.alpha_outputs[i] = (uint32_t)k->alpha_outputs[i];
			options.c0[i] = (uint32_t)k->constant_0[i];
			options.c1[i] = (uint32_t)k->constant_1[i];
		}
		combiners.final_inputs_abcd = (uint32_t)k->final_combiner_inputs_abcd;
		combiners.final_inputs_efg = (uint32_t)k->final_combiner_inputs_efg;
		combiners.combiner_count = (uint32_t)k->combiner_count;
		combiners.texture_modes = (uint32_t)k->texture_modes;
		for (i = 0; i < 4; i++)
		{
			if ((k->texture_modes >> (5 * i)) & 0x1f)
				options.textures_sampled |= (uint8_t)(1 << i);
		}
		options.final_c0 = (uint32_t)k->final_combiner_constant_0;
		options.final_c1 = (uint32_t)k->final_combiner_constant_1;
		options.fog = 1;
		translated = nv2a_tev_compile(&combiners, &options, &program, &failure);
		fprintf(tiles, "%d %d %s\n", tile, translated ? program.stage_count : 0, k->name);

		sheet_tile_origin(tile, &origin_x, &origin_y);
		sheet_tile_inputs(tile, colors, &fog);
		for (y = 0; y < SHEET_TILE; y++)
		{
			for (x = 0; x < SHEET_TILE; x++)
			{
				struct pixel_inputs inputs;
				float expected[4];
				uint8_t konst[4][4], tev_colors[2][4], out[4];
				int initial[4][4], c, nv2a[4];

				if (!translated)
				{
					for (image = 0; image < 4; image++)
						put(image, origin_x + x, origin_y + y, 255, 0, 255);
					continue;
				}
				memset(&inputs, 0, sizeof(inputs));
				for (i = 0; i < 4; i++)
					sheet_texel(i, x * SHEET_TEXTURE / SHEET_TILE, y * SHEET_TEXTURE / SHEET_TILE, inputs.texels[i]);
				memcpy(inputs.colors, colors, sizeof(colors));
				memcpy(inputs.c0, options.c0, sizeof(inputs.c0));
				memcpy(inputs.c1, options.c1, sizeof(inputs.c1));
				inputs.final_c0 = options.final_c0;
				inputs.final_c1 = options.final_c1;
				inputs.fog_color = SHEET_FOG_COLOR;
				inputs.fog_factor = fog;
				memset(inputs.texels[4], fog, 4);
				nv2a_run(&combiners, &options, &inputs, expected);
				for (c = 0; c < 4; c++)
					nv2a[c] = (int)floorf(expected[c] * 255.0f + 0.5f);
				tev_inputs(&program, &inputs, konst, initial, tev_colors);
				tev_run(&program, (const uint8_t (*)[4])konst, (const int (*)[4])initial,
					(const uint8_t (*)[4])inputs.texels, (const uint8_t (*)[4])tev_colors, out);
				put(0, origin_x + x, origin_y + y, nv2a[0], nv2a[1], nv2a[2]);
				put(2, origin_x + x, origin_y + y, out[0], out[1], out[2]);
				/* (a translation of 16 stages has no room for the alpha's stage:
				tevtest.dol shows its rgb again) */
				if (program.stage_count < NV2A_TEV_MAXIMUM_STAGES)
				{
					put(1, origin_x + x, origin_y + y, nv2a[3], nv2a[3], nv2a[3]);
					put(3, origin_x + x, origin_y + y, out[3], out[3], out[3]);
				}
				else
				{
					put(1, origin_x + x, origin_y + y, nv2a[0], nv2a[1], nv2a[2]);
					put(3, origin_x + x, origin_y + y, out[0], out[1], out[2]);
				}
			}
		}
	}
	fclose(tiles);
	for (image = 0; image < 4; image++)
	{
		FILE *file;

		snprintf(path, sizeof(path), "%s/%s.raw", argv[1], names[image]);
		file = fopen(path, "wb");
		if (!file || fwrite(images[image], sizeof(images[image]), 1, file) != 1)
		{
			perror(path);
			return 1;
		}
		fclose(file);
	}
	return 0;
}
