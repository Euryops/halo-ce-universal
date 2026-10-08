/*
D3D8_GX.H

Shared by the Wii's Direct3D device (d3d8_gx.c) and its resources and
textures (d3d8_gx_resources.c). XDK side: these units see the XDK's
headers, and reach GX only through gx_backend.h.
*/

#ifndef __D3D8_GX_H
#define __D3D8_GX_H

#include "platform.h"
#include "gx_backend.h"
#include "../halo_wii_map.h"

/* a Direct3D format code for a texture already in one of GX's formats
(HALO_WII_D3DFMT_GX + GX_TF_*), as the map converter leaves the maps'
bitmaps (halo_wii_map.h); no Xbox format has a code this high */
#define D3DFMT_WII_GX(gx_format) ((D3DFORMAT)(HALO_WII_D3DFMT_GX + (gx_format)))
#define D3DFMT_IS_WII_GX(format) ((unsigned long)(format) >= HALO_WII_D3DFMT_GX && \
	(unsigned long)(format) < HALO_WII_D3DFMT_GX + 0x10)

/* GX's texture formats the device reads (libogc's GX_TF_*) */
enum
{
	_gx_tf_i8 = 0x1,
	_gx_tf_rgb565 = 0x4,
	_gx_tf_rgb5a3 = 0x5,
	_gx_tf_rgba8 = 0x6,
	_gx_tf_cmpr = 0xE,
};

/* what a texture header says (d3d8_gx_resources.c) */
struct gx_texture_description
{
	unsigned long format;   /* D3DFMT_* */
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;            /* an Xbox linear texture: rows of pitch bytes */
	BOOL gx;                /* in a GX format: tiles */
	unsigned long pitch;
};

void gx_texture_describe(DWORD format_word, DWORD size_word, struct gx_texture_description *description);
unsigned long gx_texture_level_offset(const struct gx_texture_description *description, unsigned long level);
unsigned long gx_texture_level_pitch(const struct gx_texture_description *description, unsigned long level);
unsigned long gx_texture_face_size(const struct gx_texture_description *description);

/* the texture GX samples for a texture header (with its palette, for P8):
a GX texture used as it is, or an Xbox one converted to GX's RGBA8, once a
frame at most. FALSE when there is nothing to sample (no data, a render
target, a format not handled) */
BOOL gx_texture_get(const D3DBaseTexture *texture, const D3DPalette *palette, struct gxb_texture *result);
/* the start of a frame: textures are flushed or converted again as they
are first used in it */
void gx_texture_cache_begin_frame(void);

/* the device's own surfaces have no pixels the CPU can reach: the back
buffer and the depth buffer are GX's embedded frame buffer */
BOOL gx_surface_is_device_owned(const D3DSurface *surface);

/* surfaces the device makes for itself (d3d8_gx_resources.c) */
void gx_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

#endif /* __D3D8_GX_H */
