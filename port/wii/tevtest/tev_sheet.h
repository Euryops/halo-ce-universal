/*
TEV_SHEET.H

The shader sheet: each of the game's pixel shaders (port/wii/tests/
psh_corpus.h) translated to TEV (nv2a_tev.c) and drawn by the GX backend on
a tile of its own, with inputs both sides can make again: four textures of
ramps and noise, two vertex colors and a fog factor for each tile, and the
program's own constants. tevtest.dol draws it in Dolphin;
tev_sheet_check.c runs the same inputs through the NV2A reference and
compares the frame dump with it, pixel by pixel.

Plain C, read by both.
*/

#ifndef __TEV_SHEET_H
#define __TEV_SHEET_H

#include <stdint.h>

/* 16 tiles across, 10 down, 32 pixels square on a 40 pixel pitch, below a
40 pixel strip */
#define SHEET_COLUMNS 16
#define SHEET_ROWS 10
#define SHEET_TILE 32
#define SHEET_PITCH 40
#define SHEET_TOP 40
#define SHEET_TEXTURE 32
#define SHEET_FOG_COLOR 0x8040c0UL

static inline void sheet_tile_origin(int tile, int *x, int *y)
{
	*x = (tile % SHEET_COLUMNS) * SHEET_PITCH + (SHEET_PITCH - SHEET_TILE) / 2;
	*y = SHEET_TOP + (tile / SHEET_COLUMNS) * SHEET_PITCH + (SHEET_PITCH - SHEET_TILE) / 2;
}

static inline uint32_t sheet_hash(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/* texture n's texel (RGBA): a ramp across, a ramp down and two of noise,
each texture with its channels in another order */
static inline void sheet_texel(int texture, int x, int y, uint8_t rgba[4])
{
	int channel;

	for (channel = 0; channel < 4; channel++)
	{
		int role = (channel + texture) & 3;
		uint32_t noise = sheet_hash((uint32_t)(texture * 4096 + y * 64 + x) * 4u + (uint32_t)channel);

		rgba[channel] = (uint8_t)(role == 0 ? x * 255 / (SHEET_TEXTURE - 1) :
			role == 1 ? y * 255 / (SHEET_TEXTURE - 1) : noise & 0xff);
	}
}

/* a tile's vertex colors (RGBA) and fog factor (a byte) */
static inline void sheet_tile_inputs(int tile, uint8_t colors[2][4], uint8_t *fog)
{
	int color, channel;

	for (color = 0; color < 2; color++)
		for (channel = 0; channel < 4; channel++)
			colors[color][channel] = (uint8_t)sheet_hash((uint32_t)(tile * 16 + color * 4 + channel) + 0x5eed);
	*fog = (uint8_t)sheet_hash((uint32_t)tile + 0xf06);
}

#endif /* __TEV_SHEET_H */
