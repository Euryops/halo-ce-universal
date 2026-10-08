/*
D3D8_GX_RESOURCES.C

The Xbox Direct3D resources on the Wii: creation, locking, registration and
release of textures, surfaces, vertex and index buffers and palettes, the
few D3DX helpers the game uses, and the textures GX samples (d3d8_gx.h).

A resource's Data field holds the virtual address of its data (the Xbox
held a physical one; the Wii's GX takes virtual addresses and converts them
itself). Textures are of two kinds:

- In one of GX's formats (D3DFMT_WII_GX): the maps' bitmaps, which the map
  converter leaves in GX tiles (halo_wii_map.h: each level after the one
  before, each padded to whole tiles). GX samples them where they are; the
  CPU's cache is written back the first time each is used in a frame, since
  the CPU wrote them as the map was read.
- In an Xbox format: the textures the game makes as it runs (the font
  cache, the HUD's, the menus'), swizzled or linear as on the Xbox and
  written by the Wii's CPU in its own byte order. Each is converted to GX's
  RGBA8 tiles the first time it is used in a frame, its first level only.
*/

#include "d3d8_gx.h"

#include <stdlib.h>
#include <string.h>

/* ---------- formats */

enum texel_kind
{
	_texel_unknown,
	_texel_a8r8g8b8, _texel_x8r8g8b8, _texel_r5g6b5, _texel_a1r5g5b5, _texel_x1r5g5b5, _texel_a4r4g4b4,
	_texel_l8, _texel_al8, _texel_a8, _texel_a8l8, _texel_p8, _texel_l16,
	_texel_dxt1, _texel_dxt3, _texel_dxt5,
	_texel_depth,
};

struct format_information
{
	unsigned char kind;
	unsigned char bytes; /* per texel; per 4x4 block for DXT */
	unsigned char linear;
};

static struct format_information format_information(unsigned long format)
{
	static const struct format_information table[0x42] =
	{
		[0x00] = { _texel_l8, 1, 0 },
		[0x01] = { _texel_al8, 1, 0 },
		[0x02] = { _texel_a1r5g5b5, 2, 0 },
		[0x03] = { _texel_x1r5g5b5, 2, 0 },
		[0x04] = { _texel_a4r4g4b4, 2, 0 },
		[0x05] = { _texel_r5g6b5, 2, 0 },
		[0x06] = { _texel_a8r8g8b8, 4, 0 },
		[0x07] = { _texel_x8r8g8b8, 4, 0 },
		[0x0b] = { _texel_p8, 1, 0 },
		[0x0c] = { _texel_dxt1, 8, 0 },
		[0x0e] = { _texel_dxt3, 16, 0 },
		[0x0f] = { _texel_dxt5, 16, 0 },
		[0x10] = { _texel_a1r5g5b5, 2, 1 },
		[0x11] = { _texel_r5g6b5, 2, 1 },
		[0x12] = { _texel_a8r8g8b8, 4, 1 },
		[0x13] = { _texel_l8, 1, 1 },
		[0x19] = { _texel_a8, 1, 0 },
		[0x1a] = { _texel_a8l8, 2, 0 },
		[0x1b] = { _texel_al8, 1, 1 },
		[0x1c] = { _texel_x1r5g5b5, 2, 1 },
		[0x1d] = { _texel_a4r4g4b4, 2, 1 },
		[0x1e] = { _texel_x8r8g8b8, 4, 1 },
		[0x1f] = { _texel_a8, 1, 1 },
		[0x20] = { _texel_a8l8, 2, 1 },
		[0x2a] = { _texel_depth, 4, 0 },
		[0x2b] = { _texel_depth, 4, 0 },
		[0x2c] = { _texel_depth, 2, 0 },
		[0x2d] = { _texel_depth, 2, 0 },
		[0x2e] = { _texel_depth, 4, 1 },
		[0x2f] = { _texel_depth, 4, 1 },
		[0x30] = { _texel_depth, 2, 1 },
		[0x31] = { _texel_depth, 2, 1 },
		[0x32] = { _texel_l16, 2, 0 },
		[0x35] = { _texel_l16, 2, 1 },
	};
	struct format_information unknown = { _texel_unknown, 4, 0 };

	if (format < sizeof(table) / sizeof(table[0]) && table[format].kind != _texel_unknown)
		return table[format];
	return unknown;
}

static BOOL kind_compressed(unsigned char kind)
{
	return kind == _texel_dxt1 || kind == _texel_dxt3 || kind == _texel_dxt5;
}

/* a GX format's tile: its width and height in texels and its bytes */
static void gx_tile(unsigned long gx_format, unsigned long *width, unsigned long *height, unsigned long *bytes)
{
	switch (gx_format)
	{
	case _gx_tf_cmpr: *width = 8; *height = 8; *bytes = 32; break;
	case _gx_tf_rgba8: *width = 4; *height = 4; *bytes = 64; break;
	case _gx_tf_i8: *width = 8; *height = 4; *bytes = 32; break;
	default: *width = 4; *height = 4; *bytes = 32; break; /* 16-bit formats */
	}
}

/* ---------- geometry of a texture in memory */

static unsigned long floor_log2(unsigned long value)
{
	unsigned long result = 0;

	while (value > 1)
	{
		value >>= 1;
		result++;
	}
	return result;
}

static unsigned long level_dimension(unsigned long base, unsigned long level)
{
	unsigned long value = base >> level;

	return value ? value : 1;
}

void gx_texture_describe(DWORD format_word, DWORD size_word, struct gx_texture_description *description)
{
	struct format_information information;

	memset(description, 0, sizeof(*description));
	description->format = (format_word & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT;
	information = format_information(description->format);
	description->gx = D3DFMT_IS_WII_GX(description->format);
	description->cube_map = (format_word & D3DFORMAT_CUBEMAP) != 0;
	if (size_word && !description->gx)
	{
		description->width = (size_word & D3DSIZE_WIDTH_MASK) + 1;
		description->height = ((size_word & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;
		description->depth = 1;
		description->levels = 1;
		description->pitch = (((size_word & D3DSIZE_PITCH_MASK) >> D3DSIZE_PITCH_SHIFT) + 1) * D3DTEXTURE_PITCH_ALIGNMENT;
		description->linear = TRUE;
	}
	else
	{
		description->width = 1UL << ((format_word & D3DFORMAT_USIZE_MASK) >> D3DFORMAT_USIZE_SHIFT);
		description->height = 1UL << ((format_word & D3DFORMAT_VSIZE_MASK) >> D3DFORMAT_VSIZE_SHIFT);
		description->depth = 1UL << ((format_word & D3DFORMAT_PSIZE_MASK) >> D3DFORMAT_PSIZE_SHIFT);
		description->levels = (format_word & D3DFORMAT_MIPMAP_MASK) >> D3DFORMAT_MIPMAP_SHIFT;
		if (!description->levels)
			description->levels = 1;
		description->linear = !description->gx && information.linear;
		description->pitch = description->width * information.bytes;
	}
	if ((format_word & D3DFORMAT_DIMENSION_MASK) >> D3DFORMAT_DIMENSION_SHIFT != 3)
		description->depth = 1;
}

static unsigned long level_bytes(const struct gx_texture_description *description, unsigned long level)
{
	unsigned long width = level_dimension(description->width, level);
	unsigned long height = level_dimension(description->height, level);
	unsigned long depth = level_dimension(description->depth, level);

	if (description->gx)
	{
		unsigned long tile_width, tile_height, tile_bytes;

		gx_tile(description->format - HALO_WII_D3DFMT_GX, &tile_width, &tile_height, &tile_bytes);
		/* a 3D texture's slices are each a 2D image; a cube map's faces
		are in each level */
		return ((width + tile_width - 1) / tile_width) * ((height + tile_height - 1) / tile_height) * tile_bytes *
			depth * (description->cube_map ? 6 : 1);
	}
	else
	{
		struct format_information information = format_information(description->format);

		if (kind_compressed(information.kind))
			return ((width + 3) / 4) * ((height + 3) / 4) * information.bytes * depth;
		if (description->linear)
			return description->pitch * height;
		return width * height * depth * information.bytes;
	}
}

unsigned long gx_texture_level_offset(const struct gx_texture_description *description, unsigned long level)
{
	unsigned long offset = 0;
	unsigned long index;

	for (index = 0; index < level && index < description->levels; index++)
		offset += level_bytes(description, index);
	return offset;
}

unsigned long gx_texture_face_size(const struct gx_texture_description *description)
{
	unsigned long size = gx_texture_level_offset(description, description->levels);

	/* (a GX cube map's faces are in each level: it is one "face") */
	if (description->cube_map && !description->gx)
		size = (size + D3DTEXTURE_CUBEFACE_ALIGNMENT - 1) & ~(unsigned long)(D3DTEXTURE_CUBEFACE_ALIGNMENT - 1);
	return size;
}

unsigned long gx_texture_level_pitch(const struct gx_texture_description *description, unsigned long level)
{
	struct format_information information = format_information(description->format);

	if (description->gx)
	{
		unsigned long tile_width, tile_height, tile_bytes;

		/* a row of tiles */
		gx_tile(description->format - HALO_WII_D3DFMT_GX, &tile_width, &tile_height, &tile_bytes);
		return ((level_dimension(description->width, level) + tile_width - 1) / tile_width) * tile_bytes;
	}
	if (description->linear)
		return description->pitch;
	if (kind_compressed(information.kind))
		return ((level_dimension(description->width, level) + 3) / 4) * information.bytes;
	return level_dimension(description->width, level) * information.bytes;
}

/* ---------- swizzling (the Xbox's Morton order) */

struct swizzle_masks
{
	unsigned long x, y;
};

static struct swizzle_masks swizzle_masks(unsigned long width, unsigned long height)
{
	struct swizzle_masks masks = { 0, 0 };
	unsigned long bit = 1, mask_bit = 1;
	BOOL done;

	/* bits of x and y alternate until each dimension runs out */
	do
	{
		done = TRUE;
		if (bit < width)
		{
			masks.x |= mask_bit;
			mask_bit <<= 1;
			done = FALSE;
		}
		if (bit < height)
		{
			masks.y |= mask_bit;
			mask_bit <<= 1;
			done = FALSE;
		}
		bit <<= 1;
	} while (!done);
	return masks;
}

static unsigned long spread(unsigned long mask, unsigned long value)
{
	unsigned long result = 0, bit = 1;

	while (value && bit)
	{
		if (mask & bit)
		{
			if (value & 1)
				result |= bit;
			value >>= 1;
		}
		bit <<= 1;
	}
	return result;
}

/* ---------- texel conversion, to ARGB */

static unsigned long expand5(unsigned long v) { return (v << 3) | (v >> 2); }
static unsigned long expand6(unsigned long v) { return (v << 2) | (v >> 4); }
static unsigned long expand4(unsigned long v) { return v * 0x11; }

static unsigned long argb(unsigned long a, unsigned long r, unsigned long g, unsigned long b)
{
	return (a << 24) | (r << 16) | (g << 8) | b;
}

/* 16- and 32-bit texels are read as the CPU wrote them, in its own order */
static unsigned long texel16(const unsigned char *source)
{
	uint16_t value;

	memcpy(&value, source, sizeof(value));
	return value;
}

static unsigned long texel32(const unsigned char *source)
{
	uint32_t value;

	memcpy(&value, source, sizeof(value));
	return value;
}

static unsigned long convert_texel(unsigned char kind, const unsigned char *source, const D3DCOLOR *palette)
{
	unsigned long value;

	switch (kind)
	{
	case _texel_a8r8g8b8: return texel32(source);
	case _texel_x8r8g8b8: return texel32(source) | 0xff000000UL;
	case _texel_r5g6b5:
		value = texel16(source);
		return argb(255, expand5(value >> 11), expand6((value >> 5) & 0x3f), expand5(value & 0x1f));
	case _texel_a1r5g5b5:
		value = texel16(source);
		return argb((value & 0x8000) ? 255 : 0, expand5((value >> 10) & 0x1f), expand5((value >> 5) & 0x1f), expand5(value & 0x1f));
	case _texel_x1r5g5b5:
		value = texel16(source);
		return argb(255, expand5((value >> 10) & 0x1f), expand5((value >> 5) & 0x1f), expand5(value & 0x1f));
	case _texel_a4r4g4b4:
		value = texel16(source);
		return argb(expand4(value >> 12), expand4((value >> 8) & 0xf), expand4((value >> 4) & 0xf), expand4(value & 0xf));
	case _texel_l8: return argb(255, source[0], source[0], source[0]);
	case _texel_al8: return argb(source[0], source[0], source[0], source[0]);
	case _texel_a8: return argb(source[0], 255, 255, 255);
	case _texel_a8l8:
		value = texel16(source);
		return argb(value >> 8, value & 0xff, value & 0xff, value & 0xff);
	case _texel_p8: return palette ? palette[source[0]] : argb(255, source[0], source[0], source[0]);
	case _texel_l16:
		value = texel16(source) >> 8;
		return argb(255, value, value, value);
	default: return 0xffff00ffUL;
	}
}

/* ---------- GX's RGBA8 tiles

4x4 texels a tile, 64 bytes: the alpha and red of its 16 texels, row by
row, then their green and blue */

static void store_rgba8(unsigned char *tiles, unsigned long width, unsigned long x, unsigned long y, unsigned long color)
{
	unsigned long tiles_across = (width + 3) / 4;
	unsigned char *tile = tiles + ((y / 4) * tiles_across + x / 4) * 64;
	unsigned long texel = (y & 3) * 4 + (x & 3);

	tile[texel * 2] = (unsigned char)(color >> 24);
	tile[texel * 2 + 1] = (unsigned char)(color >> 16);
	tile[32 + texel * 2] = (unsigned char)(color >> 8);
	tile[32 + texel * 2 + 1] = (unsigned char)color;
}

static unsigned long rgba8_size(unsigned long width, unsigned long height)
{
	return ((width + 3) / 4) * ((height + 3) / 4) * 64;
}

/* level 0 of an uncompressed Xbox texture into GX RGBA8 tiles */
static void convert_level(const struct gx_texture_description *description, const unsigned char *source,
	const D3DCOLOR *palette, unsigned char *tiles)
{
	struct format_information information = format_information(description->format);
	unsigned long width = description->width, height = description->height, x, y;

	if (description->linear)
	{
		unsigned long row_texels = description->pitch / information.bytes;

		for (y = 0; y < height; y++)
		{
			const unsigned char *row = source + y * description->pitch;

			for (x = 0; x < width; x++)
				store_rgba8(tiles, width, x, y, x < row_texels ?
					convert_texel(information.kind, row + x * information.bytes, palette) : 0);
		}
		return;
	}
	{
		struct swizzle_masks masks = swizzle_masks(width, height);

		for (y = 0; y < height; y++)
		{
			unsigned long y_offset = spread(masks.y, y);

			for (x = 0; x < width; x++)
			{
				const unsigned char *texel = source + (spread(masks.x, x) | y_offset) * information.bytes;

				store_rgba8(tiles, width, x, y, convert_texel(information.kind, texel, palette));
			}
		}
	}
}

/* ---------- the textures GX samples */

/* a texture seen this frame or before: the GX textures by their address,
when they were last flushed; the Xbox ones with their converted copy */
#define TEXTURE_CACHE_SIZE 512

struct texture_entry
{
	DWORD data, format, size;
	unsigned long frame;
	void *converted;
	unsigned long converted_size;
};

static struct texture_entry texture_cache[TEXTURE_CACHE_SIZE];
/* 0 is no frame: every entry starts out of date */
static unsigned long texture_frame = 1;

void gx_texture_cache_begin_frame(void)
{
	texture_frame++;
}

static struct texture_entry *texture_entry_get(DWORD data, DWORD format, DWORD size)
{
	unsigned long hash = ((data >> 5) ^ (data >> 13) ^ format) % TEXTURE_CACHE_SIZE;
	unsigned long probe;
	struct texture_entry *oldest = NULL;

	for (probe = 0; probe < 8; probe++)
	{
		struct texture_entry *entry = &texture_cache[(hash + probe) % TEXTURE_CACHE_SIZE];

		if (entry->data == data && entry->format == format && entry->size == size)
			return entry;
		if (!oldest || entry->frame < oldest->frame)
			oldest = entry;
	}
	/* the least recently used of the probed entries makes way */
	if (oldest->converted)
		gxb_free(oldest->converted);
	memset(oldest, 0, sizeof(*oldest));
	oldest->data = data;
	oldest->format = format;
	oldest->size = size;
	return oldest;
}

BOOL gx_texture_get(const D3DBaseTexture *texture, const D3DPalette *palette, struct gxb_texture *result)
{
	struct gx_texture_description description;
	struct texture_entry *entry;

	if (!texture || !texture->Data || gx_surface_is_device_owned((const D3DSurface *)texture))
		return FALSE;
	gx_texture_describe(texture->Format, texture->Size, &description);
	memset(result, 0, sizeof(*result));
	entry = texture_entry_get(texture->Data, texture->Format, texture->Size);
	if (description.gx)
	{
		unsigned long gx_format = description.format - HALO_WII_D3DFMT_GX;

		/* a cube map's first face, a 3D texture's first slice, alone; a
		2D texture with its levels */
		result->levels = description.cube_map || description.depth > 1 ? 1 : (unsigned char)description.levels;
		if (entry->frame != texture_frame)
		{
			gxb_flush((const void *)texture->Data, gx_texture_level_offset(&description, result->levels));
			entry->frame = texture_frame;
		}
		result->data = (const void *)texture->Data;
		result->width = (uint16_t)description.width;
		result->height = (uint16_t)description.height;
		result->format = (uint8_t)gx_format;
		return TRUE;
	}
	{
		struct format_information information = format_information(description.format);
		unsigned long size = rgba8_size(description.width, description.height);

		if (information.kind == _texel_unknown || information.kind == _texel_depth || kind_compressed(information.kind) ||
			description.width > 1024 || description.height > 1024)
		{
			return FALSE;
		}
		if (entry->frame != texture_frame)
		{
			if (entry->converted_size != size)
			{
				gxb_free(entry->converted);
				entry->converted = gxb_allocate(size);
				entry->converted_size = entry->converted ? size : 0;
			}
			if (!entry->converted)
				return FALSE;
			convert_level(&description, (const unsigned char *)texture->Data,
				palette ? (const D3DCOLOR *)palette->Data : NULL, entry->converted);
			gxb_flush(entry->converted, size);
			entry->frame = texture_frame;
		}
		result->data = entry->converted;
		result->width = (uint16_t)description.width;
		result->height = (uint16_t)description.height;
		result->format = _gx_tf_rgba8;
		result->levels = 1;
		return TRUE;
	}
}

/* ---------- surfaces the device makes for itself */

/* The back buffer and the depth buffer are GX's embedded frame buffer, out
of the CPU's reach: each surface's Data is a word of its own that names it
(gx_surface_is_device_owned) and holds no pixels */
void gx_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height)
{
	static DWORD names[2][8] __attribute__((aligned(32)));
	static int next;
	unsigned long pitch = (width * 4 + D3DTEXTURE_PITCH_ALIGNMENT - 1) & ~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);

	memset(surface, 0, sizeof(*surface));
	/* owned by the device: without D3DCOMMON_D3DCREATED, Release never frees it */
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = (DWORD)names[next++ & 1];
	surface->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
	surface->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
		((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
}

/* ---------- registration and release */

void WINAPI D3DResource_Register(D3DResource *resource, void *base)
{
	/* D3DResource is opaque in C; every resource starts Common, Data, Lock */
	DWORD *fields = (DWORD *)resource;

	fields[1] = (DWORD)base + fields[1];
}

ULONG WINAPI D3DResource_Release(D3DResource *resource)
{
	DWORD *fields = (DWORD *)resource;
	ULONG count = fields[0] & D3DCOMMON_REFCOUNT_MASK;

	if (count)
	{
		count--;
		fields[0] = (fields[0] & ~D3DCOMMON_REFCOUNT_MASK) | count;
	}
	/* resources the game built itself (not D3DCOMMON_D3DCREATED) are never freed here */
	if (!count && (fields[0] & D3DCOMMON_D3DCREATED))
	{
		if (fields[1])
			gxb_free((void *)fields[1]);
		free(resource);
	}
	return count;
}

BOOL WINAPI D3DResource_IsBusy(D3DResource *resource)
{
	(void)resource;
	return FALSE;
}

void WINAPI D3DResource_BlockUntilNotBusy(D3DResource *resource)
{
	(void)resource;
}

/* ---------- textures */

static HRESULT create_texture(unsigned long width, unsigned long height, unsigned long depth, unsigned long levels,
	D3DFORMAT format, BOOL cube_map, D3DBaseTexture **result)
{
	D3DBaseTexture *texture = calloc(1, sizeof(*texture));
	struct gx_texture_description description;
	unsigned long maximum_levels = floor_log2(width > height ? width : height) + 1;
	unsigned long size;
	void *memory;

	if (!texture)
		return E_OUTOFMEMORY;
	if (!levels || levels > maximum_levels)
		levels = maximum_levels;
	texture->Common = D3DCOMMON_TYPE_TEXTURE | D3DCOMMON_D3DCREATED | 1;
	if (!D3DFMT_IS_WII_GX(format) && format_information(format).linear)
	{
		unsigned long pitch = (width * format_information(format).bytes + D3DTEXTURE_PITCH_ALIGNMENT - 1) &
			~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);

		texture->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT) |
			(2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
		texture->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
			((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
	}
	else
	{
		texture->Format = (floor_log2(depth) << D3DFORMAT_PSIZE_SHIFT) |
			(floor_log2(height) << D3DFORMAT_VSIZE_SHIFT) |
			(floor_log2(width) << D3DFORMAT_USIZE_SHIFT) |
			((DWORD)format << D3DFORMAT_FORMAT_SHIFT) |
			(levels << D3DFORMAT_MIPMAP_SHIFT) |
			((depth > 1 ? 3 : 2) << D3DFORMAT_DIMENSION_SHIFT) |
			(cube_map ? D3DFORMAT_CUBEMAP : 0) |
			D3DFORMAT_DMACHANNEL_A;
		texture->Size = 0;
	}
	gx_texture_describe(texture->Format, texture->Size, &description);
	size = gx_texture_face_size(&description) * (cube_map && !description.gx ? 6 : 1);
	memory = gxb_allocate(size ? size : 1);
	if (!memory)
	{
		platform_log("Direct3D: no memory for a %lu byte texture", size);
		free(texture);
		return E_OUTOFMEMORY;
	}
	memset(memory, 0, size);
	texture->Data = (DWORD)memory;
	*result = texture;
	return S_OK;
}

HRESULT WINAPI D3DDevice_CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
	D3DPOOL pool, D3DTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(width, height, 1, levels, format, FALSE, (D3DBaseTexture **)texture);
}

HRESULT WINAPI D3DDevice_CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
	D3DFORMAT format, D3DPOOL pool, D3DVolumeTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(width, height, depth, levels, format, FALSE, (D3DBaseTexture **)texture);
}

HRESULT WINAPI D3DDevice_CreateCubeTexture(UINT edge_length, UINT levels, DWORD usage, D3DFORMAT format,
	D3DPOOL pool, D3DCubeTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(edge_length, edge_length, 1, levels, format, TRUE, (D3DBaseTexture **)texture);
}

static void lock_level(const DWORD *resource, unsigned long face, unsigned long level,
	D3DLOCKED_RECT *locked, CONST RECT *rectangle)
{
	struct gx_texture_description description;
	unsigned long pitch;
	char *bits;

	gx_texture_describe(resource[3], resource[4], &description);
	pitch = gx_texture_level_pitch(&description, level);
	bits = (char *)resource[1];
	if (gx_surface_is_device_owned((const D3DSurface *)resource))
	{
		platform_log("Direct3D: the back buffer and depth buffer cannot be locked on the Wii");
		bits = NULL;
	}
	if (bits)
		bits += face * gx_texture_face_size(&description) + gx_texture_level_offset(&description, level);
	if (rectangle && bits && !description.gx)
	{
		/* swizzled textures cannot be addressed by rectangle; only linear
		and compressed layouts have a meaningful row pitch */
		unsigned long bytes = pitch / level_dimension(description.width, level);

		if (kind_compressed(format_information(description.format).kind))
			bits += (rectangle->top / 4) * pitch + (rectangle->left / 4) * (pitch / ((level_dimension(description.width, level) + 3) / 4));
		else
			bits += rectangle->top * pitch + rectangle->left * bytes;
	}
	locked->Pitch = (INT)pitch;
	locked->pBits = bits;
}

void WINAPI D3DTexture_LockRect(D3DTexture *texture, UINT level, D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((const DWORD *)texture, 0, level, locked, rectangle);
}

void WINAPI D3DCubeTexture_LockRect(D3DCubeTexture *texture, D3DCUBEMAP_FACES face, UINT level,
	D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((const DWORD *)texture, (unsigned long)face, level, locked, rectangle);
}

void WINAPI D3DVolumeTexture_LockBox(D3DVolumeTexture *texture, UINT level, D3DLOCKED_BOX *locked,
	CONST D3DBOX *box, DWORD flags)
{
	const DWORD *resource = (const DWORD *)texture;
	struct gx_texture_description description;
	unsigned long row_pitch, slice;
	char *bits;

	(void)flags;
	gx_texture_describe(resource[3], resource[4], &description);
	row_pitch = gx_texture_level_pitch(&description, level);
	slice = row_pitch * level_dimension(description.height, level);
	bits = (char *)resource[1];
	if (bits)
		bits += gx_texture_level_offset(&description, level);
	if (box && bits)
		bits += box->Front * slice + box->Top * row_pitch + box->Left * (row_pitch / level_dimension(description.width, level));
	locked->RowPitch = (INT)row_pitch;
	locked->SlicePitch = (INT)slice;
	locked->pBits = bits;
}

static void describe_level(const DWORD *resource, unsigned long level, D3DSURFACE_DESC *description)
{
	struct gx_texture_description texture;
	unsigned long width, height;

	gx_texture_describe(resource[3], resource[4], &texture);
	width = level_dimension(texture.width, level);
	height = level_dimension(texture.height, level);
	memset(description, 0, sizeof(*description));
	description->Format = (D3DFORMAT)texture.format;
	description->Type = D3DRTYPE_SURFACE;
	description->Width = width;
	description->Height = height;
	description->Size = level_bytes(&texture, level);
	description->MultiSampleType = D3DMULTISAMPLE_NONE;
}

void WINAPI D3DTexture_GetLevelDesc(D3DTexture *texture, UINT level, D3DSURFACE_DESC *description)
{
	describe_level((const DWORD *)texture, level, description);
}

HRESULT WINAPI D3DTexture_GetSurfaceLevel(D3DTexture *texture, UINT level, D3DSurface **result)
{
	D3DBaseTexture *base = (D3DBaseTexture *)texture;
	struct gx_texture_description description;
	D3DSurface *surface = calloc(1, sizeof(*surface));
	unsigned long width, height;

	if (!surface)
		return E_OUTOFMEMORY;
	gx_texture_describe(base->Format, base->Size, &description);
	width = level_dimension(description.width, level);
	height = level_dimension(description.height, level);
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = base->Data + gx_texture_level_offset(&description, level);
	surface->Format = (base->Format & ~(D3DFORMAT_USIZE_MASK | D3DFORMAT_VSIZE_MASK | D3DFORMAT_PSIZE_MASK | D3DFORMAT_MIPMAP_MASK)) |
		(floor_log2(width) << D3DFORMAT_USIZE_SHIFT) |
		(floor_log2(height) << D3DFORMAT_VSIZE_SHIFT) |
		(1 << D3DFORMAT_MIPMAP_SHIFT);
	surface->Size = base->Size;
	surface->Parent = base;
	*result = surface;
	return S_OK;
}

void WINAPI D3DSurface_GetDesc(D3DSurface *surface, D3DSURFACE_DESC *description)
{
	describe_level((const DWORD *)surface, 0, description);
}

void WINAPI D3DSurface_LockRect(D3DSurface *surface, D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((const DWORD *)surface, 0, 0, locked, rectangle);
}

/* ---------- vertex and index buffers */

HRESULT WINAPI D3DDevice_CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, D3DVertexBuffer **result)
{
	D3DVertexBuffer *buffer = calloc(1, sizeof(*buffer));
	void *memory;

	(void)usage;
	(void)fvf;
	(void)pool;
	if (!buffer)
		return E_OUTOFMEMORY;
	memory = gxb_allocate(length ? length : 1);
	if (!memory)
	{
		platform_log("Direct3D: no memory for a %lu byte vertex buffer", (unsigned long)length);
		free(buffer);
		return E_OUTOFMEMORY;
	}
	buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | D3DCOMMON_D3DCREATED | 1;
	buffer->Data = (DWORD)memory;
	*result = buffer;
	return S_OK;
}

void WINAPI D3DVertexBuffer_Lock(D3DVertexBuffer *buffer, UINT offset, UINT size, BYTE **data, DWORD flags)
{
	(void)size;
	(void)flags;
	*data = buffer->Data ? (BYTE *)buffer->Data + offset : NULL;
}

HRESULT WINAPI D3DDevice_CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, D3DIndexBuffer **result)
{
	D3DIndexBuffer *buffer = calloc(1, sizeof(*buffer));
	void *memory;

	(void)usage;
	(void)format;
	(void)pool;
	if (!buffer)
		return E_OUTOFMEMORY;
	memory = gxb_allocate(length ? length : 1);
	if (!memory)
	{
		free(buffer);
		return E_OUTOFMEMORY;
	}
	memset(memory, 0, length);
	buffer->Common = D3DCOMMON_TYPE_INDEXBUFFER | D3DCOMMON_D3DCREATED | 1;
	buffer->Data = (DWORD)memory;
	*result = buffer;
	return S_OK;
}

/* ---------- palettes */

HRESULT WINAPI D3DDevice_CreatePalette(D3DPALETTESIZE size, D3DPalette **result)
{
	D3DPalette *palette = calloc(1, sizeof(*palette));
	void *memory;

	if (!palette)
		return E_OUTOFMEMORY;
	/* always room for 256 entries, which the texture conversion may read */
	memory = gxb_allocate(256 * sizeof(D3DCOLOR));
	if (!memory)
	{
		free(palette);
		return E_OUTOFMEMORY;
	}
	memset(memory, 0, 256 * sizeof(D3DCOLOR));
	palette->Common = D3DCOMMON_TYPE_PALETTE | D3DCOMMON_D3DCREATED | 1 | ((DWORD)size << D3DPALETTE_COMMON_PALETTESIZE_SHIFT);
	palette->Data = (DWORD)memory;
	*result = palette;
	return S_OK;
}

void WINAPI D3DPalette_Lock(D3DPalette *palette, D3DCOLOR **colors, DWORD flags)
{
	(void)flags;
	*colors = (D3DCOLOR *)palette->Data;
}

/* ---------- D3DX */

D3DXMATRIX *WINAPI D3DXMatrixPerspectiveLH(D3DXMATRIX *out, FLOAT width, FLOAT height, FLOAT near_plane, FLOAT far_plane)
{
	memset(out, 0, sizeof(*out));
	out->_11 = 2.0f * near_plane / width;
	out->_22 = 2.0f * near_plane / height;
	out->_33 = far_plane / (far_plane - near_plane);
	out->_34 = 1.0f;
	out->_43 = near_plane * far_plane / (near_plane - far_plane);
	return out;
}

D3DXMATRIX *WINAPI D3DXMatrixOrthoLH(D3DXMATRIX *out, FLOAT width, FLOAT height, FLOAT near_plane, FLOAT far_plane)
{
	memset(out, 0, sizeof(*out));
	out->_11 = 2.0f / width;
	out->_22 = 2.0f / height;
	out->_33 = 1.0f / (far_plane - near_plane);
	out->_43 = near_plane / (near_plane - far_plane);
	out->_44 = 1.0f;
	return out;
}

D3DXVECTOR4 *WINAPI D3DXVec4Transform(D3DXVECTOR4 *out, CONST D3DXVECTOR4 *vector, CONST D3DXMATRIX *matrix)
{
	D3DXVECTOR4 result;

	result.x = vector->x * matrix->_11 + vector->y * matrix->_21 + vector->z * matrix->_31 + vector->w * matrix->_41;
	result.y = vector->x * matrix->_12 + vector->y * matrix->_22 + vector->z * matrix->_32 + vector->w * matrix->_42;
	result.z = vector->x * matrix->_13 + vector->y * matrix->_23 + vector->z * matrix->_33 + vector->w * matrix->_43;
	result.w = vector->x * matrix->_14 + vector->y * matrix->_24 + vector->z * matrix->_34 + vector->w * matrix->_44;
	*out = result;
	return out;
}

HRESULT WINAPI D3DXGetErrorStringA(HRESULT error, LPSTR buffer, UINT buffer_length)
{
	if (buffer && buffer_length)
	{
		const char *text = error == S_OK ? "S_OK" :
			error == E_OUTOFMEMORY ? "E_OUTOFMEMORY" :
			error == E_FAIL ? "E_FAIL" : "Direct3D error";

		strncpy(buffer, text, buffer_length - 1);
		buffer[buffer_length - 1] = '\0';
	}
	return S_OK;
}
