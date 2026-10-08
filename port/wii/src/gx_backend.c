/*
GX_BACKEND.C

The Wii's GPU for the Direct3D device (gx_backend.h). This file sees libogc
and not the XDK.

Vertices go to GX in immediate mode (GX_Begin, then each vertex's position,
color and texture coordinates through the write-gather pipe), the way
libogc's own examples draw: the device has already run the game's vertex
program for each vertex, so there is no buffer GX could read them from as
they are. GX's position matrix stays the identity; the projection matrix is
loaded for each draw from the fit d3d8_gx.c made (struct gxb_projection).

The picture is drawn in the embedded frame buffer, 640x480 with 24-bit color
and depth, and copied at presentation into one of two external frame buffers
that the video interface shows in turn. The text console keeps its own
frame buffer (wii_main.c), which comes back when the game halts.
*/

#include <malloc.h>
#include <string.h>
#include <gccore.h>

#include "gx_backend.h"

/* wii_main.c */
void *wii_video_mode(void);

#define FIFO_SIZE (256 * 1024)

static struct
{
	int ready;
	GXRModeObj *mode;
	void *fifo;
	void *frame_buffers[2];
	int current;

	/* what the device last set, so that a clear (which draws) can put it
	back */
	struct gxb_raster_state raster;
	float viewport[6];
	int scissor[4];
	enum gxb_combine combine;
	int texcoord_count;
	struct gxb_projection projection;
	int projection_valid;
} gx;

int gxb_ready(void)
{
	return gx.ready;
}

void *gxb_allocate(unsigned long size)
{
	return memalign(32, (size + 31) & ~31UL);
}

void gxb_free(void *memory)
{
	free(memory);
}

void gxb_flush(const void *address, unsigned long size)
{
	if (address && size)
		DCFlushRange((void *)address, size);
}

/* ---------- start-up */

void gxb_initialize(void)
{
	Mtx identity;
	f32 y_scale;
	u32 copy_height;
	int index;

	if (gx.ready)
		return;
	gx.mode = wii_video_mode();
	for (index = 0; index < 2; index++)
	{
		gx.frame_buffers[index] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(gx.mode));
		VIDEO_ClearFrameBuffer(gx.mode, gx.frame_buffers[index], COLOR_BLACK);
	}
	gx.fifo = memalign(32, FIFO_SIZE);
	memset(gx.fifo, 0, FIFO_SIZE);
	GX_Init(gx.fifo, FIFO_SIZE);

	GX_SetCopyClear((GXColor){ 0, 0, 0, 0xff }, GX_MAX_Z24);
	y_scale = GX_GetYScaleFactor(gx.mode->efbHeight, gx.mode->xfbHeight);
	copy_height = GX_SetDispCopyYScale(y_scale);
	GX_SetDispCopySrc(0, 0, gx.mode->fbWidth, gx.mode->efbHeight);
	GX_SetDispCopyDst(gx.mode->fbWidth, copy_height);
	GX_SetCopyFilter(gx.mode->aa, gx.mode->sample_pattern, GX_TRUE, gx.mode->vfilter);
	GX_SetFieldMode(gx.mode->field_rendering,
		gx.mode->viHeight == 2 * gx.mode->xfbHeight ? GX_ENABLE : GX_DISABLE);
	GX_SetPixelFmt(gx.mode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetDispCopyGamma(GX_GM_1_0);

	/* one color channel, from the vertex; no lighting */
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_ClearVtxDesc();
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	for (index = 0; index < GXB_MAXIMUM_TEXTURES; index++)
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0 + index, GX_TEX_ST, GX_F32, 0);
	/* (by hand: libogc's guMtxIdentity is in the unit of its paired-single
	matrix code, whose small-data constants the game's link misaligns) */
	memset(identity, 0, sizeof(identity));
	identity[0][0] = identity[1][1] = identity[2][2] = 1.0f;
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_SetZCompLoc(GX_FALSE);
	GX_SetDither(GX_DISABLE);

	/* the console's last words before the picture takes the screen */
	VIDEO_SetNextFramebuffer(gx.frame_buffers[0]);
	VIDEO_Flush();
	gx.current = 1;
	gx.ready = 1;

	gxb_set_viewport(0, 0, GXB_SCREEN_WIDTH, GXB_SCREEN_HEIGHT, 0.0f, 1.0f);
	gxb_set_scissor(0, 0, GXB_SCREEN_WIDTH, GXB_SCREEN_HEIGHT);
	gxb_set_combine(_gxb_combine_color, 0);
	{
		struct gxb_raster_state state = { 0 };

		state.z_function = 7;
		state.alpha_function = 7;
		state.color_write = 1;
		gxb_set_raster_state(&state);
	}
}

/* ---------- state */

void gxb_set_viewport(float x, float y, float width, float height, float minimum_z, float maximum_z)
{
	gx.viewport[0] = x; gx.viewport[1] = y;
	gx.viewport[2] = width; gx.viewport[3] = height;
	gx.viewport[4] = minimum_z; gx.viewport[5] = maximum_z;
	if (gx.ready)
		GX_SetViewport(x, y, width, height, minimum_z, maximum_z);
}

void gxb_set_scissor(int x, int y, int width, int height)
{
	gx.scissor[0] = x; gx.scissor[1] = y;
	gx.scissor[2] = width; gx.scissor[3] = height;
	if (gx.ready)
		GX_SetScissor(x < 0 ? 0 : x, y < 0 ? 0 : y, width > 0 ? width : 0, height > 0 ? height : 0);
}

void gxb_set_raster_state(const struct gxb_raster_state *state)
{
	gx.raster = *state;
	if (!gx.ready)
		return;
	GX_SetZMode(state->z_test || state->z_write ? GX_TRUE : GX_FALSE,
		state->z_test ? state->z_function : GX_ALWAYS, state->z_write ? GX_TRUE : GX_FALSE);
	switch (state->blend_mode)
	{
	case 1:
		GX_SetBlendMode(GX_BM_BLEND, state->blend_source, state->blend_destination, GX_LO_CLEAR);
		break;
	case 2:
		GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
		break;
	default:
		GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
		break;
	}
	if (state->alpha_test)
	{
		GX_SetAlphaCompare(state->alpha_function, state->alpha_reference, GX_AOP_AND, GX_ALWAYS, 0);
		/* the depth test after the texture, so that a rejected texel writes no depth */
		GX_SetZCompLoc(GX_FALSE);
	}
	else
	{
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
	}
	/* GX's front faces are the clockwise ones on screen */
	GX_SetCullMode(state->cull == 1 ? GX_CULL_FRONT : state->cull == 2 ? GX_CULL_BACK : GX_CULL_NONE);
	GX_SetColorUpdate(state->color_write ? GX_TRUE : GX_FALSE);
	GX_SetAlphaUpdate(state->alpha_write ? GX_TRUE : GX_FALSE);
}

void gxb_set_textures(const struct gxb_texture *textures, int count)
{
	int index;

	if (!gx.ready)
		return;
	for (index = 0; index < count && index < GXB_MAXIMUM_TEXTURES; index++)
	{
		const struct gxb_texture *texture = &textures[index];
		GXTexObj object;
		u8 filter_min, filter_mag;

		if (!texture->data)
			continue;
		GX_InitTexObj(&object, (void *)texture->data, texture->width, texture->height, texture->format,
			texture->wrap_s, texture->wrap_t, texture->levels > 1 ? GX_TRUE : GX_FALSE);
		filter_mag = texture->linear ? GX_LINEAR : GX_NEAR;
		filter_min = texture->levels > 1 ?
			(texture->linear ? GX_LIN_MIP_LIN : GX_NEAR_MIP_NEAR) : filter_mag;
		GX_InitTexObjLOD(&object, filter_min, filter_mag, 0.0f, (f32)(texture->levels - 1), 0.0f,
			GX_FALSE, GX_FALSE, GX_ANISO_1);
		GX_LoadTexObj(&object, GX_TEXMAP0 + index);
	}
}

void gxb_set_combine(enum gxb_combine combine, int texcoord_count)
{
	int index;

	gx.combine = combine;
	gx.texcoord_count = texcoord_count;
	if (!gx.ready)
		return;
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	for (index = 0; index < texcoord_count; index++)
	{
		GX_SetVtxDesc(GX_VA_TEX0 + index, GX_DIRECT);
		GX_SetTexCoordGen(GX_TEXCOORD0 + index, GX_TG_MTX2x4, GX_TG_TEX0 + index, GX_IDENTITY);
	}
	GX_SetNumTexGens(texcoord_count);
	GX_SetNumTevStages(1);
	if (combine == _gxb_combine_color || !texcoord_count)
	{
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	}
	else
	{
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, combine == _gxb_combine_modulate ? GX_MODULATE : GX_REPLACE);
	}
}

static void load_projection(const struct gxb_projection *projection)
{
	Mtx44 matrix;

	if (gx.projection_valid && !memcmp(projection, &gx.projection, sizeof(*projection)))
		return;
	gx.projection = *projection;
	gx.projection_valid = 1;
	memset(matrix, 0, sizeof(matrix));
	matrix[0][0] = 1.0f;
	matrix[1][1] = 1.0f;
	if (projection->perspective)
	{
		/* clip = (x, y, p22 * -w + p23, w) */
		matrix[2][2] = projection->p22;
		matrix[2][3] = projection->p23;
		matrix[3][2] = -1.0f;
		GX_LoadProjectionMtx(matrix, GX_PERSPECTIVE);
	}
	else
	{
		/* GX's depth is -1 (near) to 0 (far): z - 1 */
		matrix[2][2] = 1.0f;
		matrix[2][3] = -1.0f;
		matrix[3][3] = 1.0f;
		GX_LoadProjectionMtx(matrix, GX_ORTHOGRAPHIC);
	}
}

/* ---------- drawing */

static u8 gx_primitive(enum gxb_primitive primitive, unsigned *group)
{
	switch (primitive)
	{
	case _gxb_points: *group = 1; return GX_POINTS;
	case _gxb_lines: *group = 2; return GX_LINES;
	case _gxb_line_strip: *group = 0; return GX_LINESTRIP;
	case _gxb_triangle_strip: *group = 0; return GX_TRIANGLESTRIP;
	case _gxb_triangle_fan: *group = 0; return GX_TRIANGLEFAN;
	case _gxb_quads: *group = 4; return GX_QUADS;
	default: *group = 3; return GX_TRIANGLES;
	}
}

static void emit(const struct gxb_vertex *vertex, int texcoord_count)
{
	int index;

	GX_Position3f32(vertex->position[0], vertex->position[1], vertex->position[2]);
	GX_Color1u32(vertex->color);
	for (index = 0; index < texcoord_count; index++)
		GX_TexCoord2f32(vertex->texcoords[index][0], vertex->texcoords[index][1]);
}

void gxb_draw(enum gxb_primitive primitive, const struct gxb_projection *projection,
	const struct gxb_vertex *vertices, const uint16_t *indices, unsigned long count)
{
	unsigned group;
	u8 type = gx_primitive(primitive, &group);
	unsigned long first = 0, batch_limit;

	if (!gx.ready || !count)
		return;
	load_projection(projection);
	/* GX_Begin counts vertices in 16 bits: lists go in batches of whole
	primitives; a strip or fan that long is cut short */
	batch_limit = group ? 65535 / group * group : 65535;
	while (first < count)
	{
		unsigned long batch = count - first < batch_limit ? count - first : batch_limit;
		unsigned long index;

		GX_Begin(type, GX_VTXFMT0, (u16)batch);
		for (index = first; index < first + batch; index++)
			emit(&vertices[indices ? indices[index] : index], gx.texcoord_count);
		GX_End();
		if (!group)
			break;
		first += batch;
	}
}

void gxb_clear(int x0, int y0, int x1, int y1, int color, int alpha, int depth, uint32_t rgba, float z)
{
	struct gxb_raster_state saved_raster = gx.raster, clear = { 0 };
	enum gxb_combine saved_combine = gx.combine;
	int saved_texcoords = gx.texcoord_count;
	float saved_viewport[6];
	struct gxb_projection orthographic = { 0, 0.0f, 0.0f };
	struct gxb_vertex corners[4];
	float left, right, top, bottom;
	int index;

	if (!gx.ready || x1 <= x0 || y1 <= y0 || !(color || alpha || depth))
		return;
	memcpy(saved_viewport, gx.viewport, sizeof(saved_viewport));
	/* a quad over the rectangle, drawn with the depth test passing always */
	clear.z_test = depth;
	clear.z_function = 7;
	clear.z_write = depth;
	clear.alpha_function = 7;
	clear.color_write = color;
	clear.alpha_write = alpha;
	gxb_set_raster_state(&clear);
	gxb_set_combine(_gxb_combine_color, 0);
	gxb_set_viewport(0, 0, GXB_SCREEN_WIDTH, GXB_SCREEN_HEIGHT, 0.0f, 1.0f);
	left = (float)x0 / (GXB_SCREEN_WIDTH / 2) - 1.0f;
	right = (float)x1 / (GXB_SCREEN_WIDTH / 2) - 1.0f;
	top = 1.0f - (float)y0 / (GXB_SCREEN_HEIGHT / 2);
	bottom = 1.0f - (float)y1 / (GXB_SCREEN_HEIGHT / 2);
	memset(corners, 0, sizeof(corners));
	corners[0].position[0] = left; corners[0].position[1] = top;
	corners[1].position[0] = right; corners[1].position[1] = top;
	corners[2].position[0] = right; corners[2].position[1] = bottom;
	corners[3].position[0] = left; corners[3].position[1] = bottom;
	for (index = 0; index < 4; index++)
	{
		corners[index].position[2] = z;
		corners[index].color = rgba;
	}
	gxb_draw(_gxb_quads, &orthographic, corners, NULL, 4);

	gxb_set_raster_state(&saved_raster);
	gxb_set_combine(saved_combine, saved_texcoords);
	gxb_set_viewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3],
		saved_viewport[4], saved_viewport[5]);
}

/* ---------- presentation */

void gxb_present(void)
{
	if (!gx.ready)
		return;
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	GX_CopyDisp(gx.frame_buffers[gx.current], GX_FALSE);
	GX_DrawDone();
	VIDEO_SetNextFramebuffer(gx.frame_buffers[gx.current]);
	VIDEO_Flush();
	/* the other buffer is drawn into next: wait until it is off the screen */
	VIDEO_WaitVSync();
	gx.current ^= 1;
	gxb_set_raster_state(&gx.raster);
}

void gxb_wait_vertical_blank(void)
{
	VIDEO_WaitVSync();
}

/* ---------- the vertical blank thread */

/* above the game's threads (64, wii_main.c), so that a blank is seen while
the game spins waiting for one (main.c's frame throttle) */
#define VERTICAL_BLANK_PRIORITY 100

static void (*vertical_blank_handler)(void);
static lwp_t vertical_blank_thread;

static void *vertical_blank_loop(void *unused)
{
	(void)unused;
	for (;;)
	{
		VIDEO_WaitVSync();
		if (vertical_blank_handler)
			vertical_blank_handler();
	}
	return NULL;
}

void gxb_set_vertical_blank_handler(void (*handler)(void))
{
	vertical_blank_handler = handler;
	if (handler && !vertical_blank_thread)
		LWP_CreateThread(&vertical_blank_thread, vertical_blank_loop, NULL, NULL, 16 * 1024, VERTICAL_BLANK_PRIORITY);
}
