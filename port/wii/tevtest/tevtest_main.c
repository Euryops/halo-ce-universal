/*
TEVTEST_MAIN.C

tevtest.dol: the shader sheet (tev_sheet.h). Each of the game's pixel
shaders (psh_corpus.h) is translated to TEV by nv2a_tev.c, as the GX device
does it for each draw, and drawn by the GX backend on a tile of its own,
with known textures, vertex colors, fog and the program's own constants. A
program the translation refuses is drawn magenta.

The sheet is drawn twice over, in turn, for a second and a half each: as it
comes out (rgb), then with a stage added that shows its alpha as gray (a
program that already has 16 stages shows its rgb again there). The copy to
the screen does not filter, so that each frame dump's pixels are TEV's.

This file sees libogc; the backend and the translation are plain C.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>

#include "gx_backend.h"
#include "nv2a_tev.h"
#include "psh_corpus.h"
#include "tev_sheet.h"

static void *console_buffer;
static GXRModeObj *video_mode;

void *wii_video_mode(void)
{
	return video_mode;
}

#define CORPUS_COUNT ((int)(sizeof(psh_corpus) / sizeof(psh_corpus[0])))

static struct nv2a_tev_program programs[CORPUS_COUNT];
static int translated[CORPUS_COUNT];

/* the texture as GX's RGBA8 tiles: 4 by 4 texels, their alpha and red, then
their green and blue */
static void *make_texture(int texture)
{
	unsigned char *tiles = memalign(32, SHEET_TEXTURE * SHEET_TEXTURE * 4);
	int x, y;

	for (y = 0; y < SHEET_TEXTURE; y++)
	{
		for (x = 0; x < SHEET_TEXTURE; x++)
		{
			int block = (y / 4) * (SHEET_TEXTURE / 4) + x / 4;
			int texel = (y % 4) * 4 + x % 4;
			unsigned char *base = tiles + block * 64;
			uint8_t rgba[4];

			sheet_texel(texture, x, y, rgba);
			base[texel * 2] = rgba[3];
			base[texel * 2 + 1] = rgba[0];
			base[32 + texel * 2] = rgba[1];
			base[32 + texel * 2 + 1] = rgba[2];
		}
	}
	DCFlushRange(tiles, SHEET_TEXTURE * SHEET_TEXTURE * 4);
	return tiles;
}

static void options_for(const struct psh_case *k, struct nv2a_tev_options *options)
{
	int i;

	memset(options, 0, sizeof(*options));
	for (i = 0; i < 4; i++)
	{
		if ((k->texture_modes >> (5 * i)) & 0x1f)
			options->textures_sampled |= (uint8_t)(1 << i);
		options->c0[i] = (uint32_t)k->constant_0[i];
		options->c1[i] = (uint32_t)k->constant_1[i];
		options->c0[i + 4] = (uint32_t)k->constant_0[i + 4];
		options->c1[i + 4] = (uint32_t)k->constant_1[i + 4];
	}
	options->final_c0 = (uint32_t)k->final_combiner_constant_0;
	options->final_c1 = (uint32_t)k->final_combiner_constant_1;
	options->fog = 1;
}

static void combiners_for(const struct psh_case *k, struct nv2a_combiners *combiners)
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

/* a quad over the screen rectangle, in GX's orthographic terms */
static void quad(int x, int y, int size, uint32_t color, uint32_t specular, float fog)
{
	static const struct gxb_projection orthographic = { 0, 0.0f, 0.0f };
	struct gxb_vertex corners[4];
	int corner, texture;

	memset(corners, 0, sizeof(corners));
	for (corner = 0; corner < 4; corner++)
	{
		int right = corner == 1 || corner == 2, bottom = corner >= 2;

		corners[corner].position[0] = (float)(x + right * size) / (GXB_SCREEN_WIDTH / 2) - 1.0f;
		corners[corner].position[1] = 1.0f - (float)(y + bottom * size) / (GXB_SCREEN_HEIGHT / 2);
		corners[corner].position[2] = 0.5f;
		corners[corner].color = color;
		corners[corner].specular = specular;
		corners[corner].fog = fog;
		for (texture = 0; texture < GXB_MAXIMUM_TEXTURES; texture++)
		{
			corners[corner].texcoords[texture][0] = (float)right;
			corners[corner].texcoords[texture][1] = (float)bottom;
		}
	}
	gxb_draw(_gxb_quads, &orthographic, corners, NULL, 4);
}

static uint32_t rgba_word(const uint8_t *rgba)
{
	return ((uint32_t)rgba[0] << 24) | ((uint32_t)rgba[1] << 16) | ((uint32_t)rgba[2] << 8) | rgba[3];
}

static void draw_sheet(const struct gxb_texture textures[4], int alpha_view)
{
	int tile;

	gxb_clear(0, 0, GXB_SCREEN_WIDTH, GXB_SCREEN_HEIGHT, 1, 1, 1, 0x202020ffUL, 1.0f);
	for (tile = 0; tile < CORPUS_COUNT; tile++)
	{
		const struct psh_case *k = &psh_corpus[tile];
		uint8_t colors[2][4], fog, konst[4][4], initial_color[4][4], initial_alpha[4][4];
		uint32_t c0[8], c1[8];
		struct nv2a_tev_program program;
		int x, y, index;

		sheet_tile_origin(tile, &x, &y);
		sheet_tile_inputs(tile, colors, &fog);
		if (!translated[tile])
		{
			gxb_set_combine(_gxb_combine_color, 0);
			quad(x, y, SHEET_TILE, 0xff00ffffUL, 0, 1.0f);
			continue;
		}
		program = programs[tile];
		if (alpha_view && program.stage_count < NV2A_TEV_MAXIMUM_STAGES)
		{
			/* rgb = alpha, as gray */
			struct nv2a_tev_stage *stage = &program.stages[program.stage_count++];

			memset(stage, 0, sizeof(*stage));
			stage->texture = stage->color = 0xff;
			stage->rgb.a = stage->rgb.b = stage->rgb.c = 15;
			stage->rgb.d = 1;
			stage->rgb.clamp = 1;
			stage->alpha.a = stage->alpha.b = stage->alpha.c = 7;
			stage->alpha.d = 0;
			stage->alpha.clamp = 1;
		}
		for (index = 0; index < 8; index++)
		{
			c0[index] = (uint32_t)k->constant_0[index];
			c1[index] = (uint32_t)k->constant_1[index];
		}
		memset(initial_color, 0, sizeof(initial_color));
		memset(initial_alpha, 0, sizeof(initial_alpha));
		for (index = 0; index < 4; index++)
		{
			nv2a_tev_constant_value(&program.konst[index], c0, c1, (uint32_t)k->final_combiner_constant_0,
				(uint32_t)k->final_combiner_constant_1, SHEET_FOG_COLOR, konst[index]);
			if (program.initial_color[index].source)
				nv2a_tev_constant_value(&program.initial_color[index], c0, c1, (uint32_t)k->final_combiner_constant_0,
					(uint32_t)k->final_combiner_constant_1, SHEET_FOG_COLOR, initial_color[index]);
			if (program.initial_alpha[index].source)
				nv2a_tev_constant_value(&program.initial_alpha[index], c0, c1, (uint32_t)k->final_combiner_constant_0,
					(uint32_t)k->final_combiner_constant_1, SHEET_FOG_COLOR, initial_alpha[index]);
		}
		gxb_set_textures(textures, 4);
		gxb_set_program(&program, (const uint8_t (*)[4])konst, (const uint8_t (*)[4])initial_color,
			(const uint8_t (*)[4])initial_alpha);
		quad(x, y, SHEET_TILE, rgba_word(colors[0]), rgba_word(colors[1]), fog / 255.0f);
	}
}

int main(int argc, char **argv)
{
	struct gxb_texture textures[4];
	struct gxb_raster_state state;
	int tile, refused = 0, frame;

	(void)argc;
	(void)argv;
	VIDEO_Init();
	video_mode = VIDEO_GetPreferredMode(NULL);
	console_buffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
	console_init(console_buffer, 16, 16, video_mode->fbWidth, video_mode->xfbHeight,
		video_mode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(video_mode);
	VIDEO_SetNextFramebuffer(console_buffer);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (video_mode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	printf("\n\n  tevtest: translating %d pixel shaders\n", CORPUS_COUNT);

	for (tile = 0; tile < CORPUS_COUNT; tile++)
	{
		struct nv2a_combiners combiners;
		struct nv2a_tev_options options;
		const char *failure;

		combiners_for(&psh_corpus[tile], &combiners);
		options_for(&psh_corpus[tile], &options);
		translated[tile] = nv2a_tev_compile(&combiners, &options, &programs[tile], &failure);
		if (!translated[tile])
			refused++;
	}
	printf("  tevtest: %d programs, %d refused\n", CORPUS_COUNT, refused);

	gxb_initialize();
	gxb_set_display_filter(0);
	memset(textures, 0, sizeof(textures));
	for (tile = 0; tile < 4; tile++)
	{
		textures[tile].data = make_texture(tile);
		textures[tile].width = textures[tile].height = SHEET_TEXTURE;
		textures[tile].format = GX_TF_RGBA8;
		textures[tile].levels = 1;
	}
	memset(&state, 0, sizeof(state));
	state.alpha_function = 7;
	state.z_function = 7;
	state.color_write = 1;
	gxb_set_raster_state(&state);
	for (frame = 0;; frame++)
	{
		draw_sheet(textures, (frame / 90) & 1);
		gxb_present();
	}
	return 0;
}
