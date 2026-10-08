/*
GX_BACKEND.H

The Wii's GPU (GX, through libogc), as the Direct3D device (d3d8_gx.c) uses
it. The device sees the XDK's headers and gx_backend.c sees libogc's, which
cannot be read by one unit (wii_os.h says why), so this is the line between
them, in plain C types.

The device hands each draw over already transformed: the game's vertex
program has run on the CPU (nv2a_vsh_run.c) and given each vertex a
clip-space position in Direct3D's terms (x and y in -w..w, z in 0..w). GX's
own transform then does the projection, the clipping and the
perspective-correct interpolation from that (gx_projection says how).
*/

#ifndef __GX_BACKEND_H
#define __GX_BACKEND_H

#include <stdint.h>

/* the screen the game draws, as the Xbox's: 640 by 480 */
#define GXB_SCREEN_WIDTH 640
#define GXB_SCREEN_HEIGHT 480
#define GXB_MAXIMUM_TEXTURES 4

/* one vertex as GX is given it */
struct gxb_vertex
{
	float position[3];
	/* RGBA, red in the top byte */
	uint32_t color;
	float texcoords[GXB_MAXIMUM_TEXTURES][2];
};

enum gxb_primitive
{
	_gxb_points,
	_gxb_lines,
	_gxb_line_strip,
	_gxb_triangles,
	_gxb_triangle_strip,
	_gxb_triangle_fan,
	_gxb_quads,
};

/* how a draw's positions are to be read. Perspective: a vertex's position
is (x, y, -w) of its clip position, and z = (1 - p22) * w + p23 holds for
all of the draw's vertices (d3d8_gx.c fits p22 and p23), so GX's
perspective projection gives back x, y, z and w. Orthographic: the position
is the clip position divided by w; z is in 0..1. */
struct gxb_projection
{
	int perspective;
	float p22, p23;
};

/* comparison functions are GX's (0 never ... 7 always), which are in
Direct3D's order */
struct gxb_raster_state
{
	uint8_t z_test, z_write, z_function;
	/* 0 none, 1 blend (with source and destination factors, GX_BL_*),
	2 subtract (destination - source) */
	uint8_t blend_mode, blend_source, blend_destination;
	uint8_t alpha_test, alpha_function, alpha_reference;
	/* 0 none, 1 cull clockwise triangles, 2 counter-clockwise (on screen) */
	uint8_t cull;
	uint8_t color_write, alpha_write;
};

/* a texture in one of GX's formats (GX_TF_*), its levels one after the
other, 32-byte aligned and flushed from the CPU's cache */
struct gxb_texture
{
	const void *data;
	uint16_t width, height;
	uint8_t format;
	uint8_t levels;
	/* GX_CLAMP, GX_REPEAT or GX_MIRROR */
	uint8_t wrap_s, wrap_t;
	uint8_t linear;
};

/* how the texture and the vertex color are combined (TEV) */
enum gxb_combine
{
	/* the vertex color */
	_gxb_combine_color,
	/* texture 0 by the vertex color */
	_gxb_combine_modulate,
	/* texture 0 */
	_gxb_combine_texture,
};

/* takes the screen from the text console (wii_main.c): GX, two frame
buffers of the console's video mode */
void gxb_initialize(void);
int gxb_ready(void);

void gxb_set_viewport(float x, float y, float width, float height, float minimum_z, float maximum_z);
void gxb_set_scissor(int x, int y, int width, int height);
void gxb_set_raster_state(const struct gxb_raster_state *state);
/* textures[0..count), NULL entries unbound */
void gxb_set_textures(const struct gxb_texture *textures, int count);
void gxb_set_combine(enum gxb_combine combine, int texcoord_count);

/* a draw: with indices, the vertices are vertices[indices[0..count)], else
vertices[0..count) */
void gxb_draw(enum gxb_primitive primitive, const struct gxb_projection *projection,
	const struct gxb_vertex *vertices, const uint16_t *indices, unsigned long count);

/* fills the rectangle (in screen pixels, the right and bottom edges
excluded) with the color (RGBA) and depth (0..1) where asked */
void gxb_clear(int x0, int y0, int x1, int y1, int color, int alpha, int depth, uint32_t rgba, float z);

/* the picture so far to the screen, at the next vertical blank */
void gxb_present(void);
void gxb_wait_vertical_blank(void);
/* called at every vertical blank from a thread of its own, which runs
ahead of the game's threads (as the Xbox's DPC did) */
void gxb_set_vertical_blank_handler(void (*handler)(void));

/* writes the CPU's cache of [address, address + size) to memory, for GX to
read (textures the CPU made) */
void gxb_flush(const void *address, unsigned long size);
/* 32-byte aligned memory for converted textures */
void *gxb_allocate(unsigned long size);
void gxb_free(void *memory);

#endif /* __GX_BACKEND_H */
