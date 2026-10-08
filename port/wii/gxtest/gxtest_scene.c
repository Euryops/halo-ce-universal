/*
GXTEST_SCENE.C

A scene for the GX device (port/wii/src/d3d8_gx.c) to draw where there are
no maps to load: the engine cannot reach its menu without ui.map from a Halo
disc, so this drives the device the way the game does, through the XDK
header's inline functions, with the game's own vertex programs (the
microcode in rasterizer_xbox_vertex_shaders_data.inc and the declarations of
rasterizer_xbox_vertex_shaders_initialize.c) and the constants the game
gives them:

- a level: a hilly ground and pillars, drawn with the environment program
  (49) from the compressed BSP vertex layout (position, NORMPACKED3 normal,
  binormal and tangent, texture coordinates), its world-view-projection at
  c[-96] and its texture transform at c[-84] to c[-82]. The ground is an
  indexed draw from an index buffer; the pillars are not indexed. Their
  textures are in GX formats as the converted maps' are: the ground's CMPR
  registered at an address as the texture cache does it
  (xbox_texture_cache.c), the pillars' RGB5A3 made by CreateTexture.
- a menu over it: a panel, a title and three buttons, drawn with the widget
  program (56) and the constants rasterizer_xbox_widgets.c sets at c[-68].
  The text is a linear A8R8G8B8 texture and the buttons' icon a swizzled
  A4R4G4B4 one, both written by the CPU as the game's run-time textures are,
  and converted by the device. The buttons are drawn in immediate mode
  (Begin, SetVertexData, End).
- checks in the corner: two triangles of opposite winding with
  counter-clockwise culling (only the clockwise one may show), a cleared
  rectangle, and the icon drawn with the alpha test.

Each draw sets a pixel shader of the game's kind (set_pixel_shader), which
the device translates to TEV; the game's own programs are drawn on
tevtest.dol's sheet.

The camera turns for a few seconds and the scene then holds its last frame.
*/

#include "d3d8_gx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the game's vertex programs */

static const unsigned long vertex_shader_code[] =
{
#include "rasterizer_xbox_vertex_shaders_data.inc"
};

/* rasterizer_xbox_vertex_shaders.c's offsets of the two programs */
#define ENVIRONMENT_PROGRAM_OFFSET 0x6158 /* 49 */
#define WIDGET_PROGRAM_OFFSET 0x6DD0      /* 56 */

/* rasterizer_xbox_vertex_shaders_initialize.c's declarations of them
(vertex_shader_declarations at 0xC4 and 0x34) */
static const DWORD environment_declaration[] =
{
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0xFFFFFFFF,
};
static const DWORD widget_declaration[] =
{
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0xFFFFFFFF,
};

struct environment_vertex
{
	float position[3];
	DWORD normal, binormal, tangent;
	float texcoord[2];
};

struct widget_vertex
{
	float position[3];
	float texcoord[2];
	D3DCOLOR color;
};

#define SCREEN_WIDTH 640.0f
#define SCREEN_HEIGHT 480.0f

static D3DDevice *device;
static DWORD environment_shader, widget_shader;

/* ---------- small helpers */

static DWORD normpacked3(float x, float y, float z)
{
	long ix = (long)(x * 1023.0f), iy = (long)(y * 1023.0f), iz = (long)(z * 511.0f);

	return ((DWORD)ix & 0x7ff) | (((DWORD)iy & 0x7ff) << 11) | (((DWORD)iz & 0x3ff) << 22);
}

static void cross(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static void normalize(float v[3])
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	v[0] /= length; v[1] /= length; v[2] /= length;
}

static float dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void *aligned_memory(unsigned long size)
{
	void *memory = gxb_allocate(size);

	memset(memory, 0, size);
	return memory;
}

/* ---------- textures */

static unsigned short rgb565(unsigned long r, unsigned long g, unsigned long b)
{
	return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* the ground: grass in cells, with darker patches and lighter seams */
static void ground_color(int x, int y, unsigned long rgb[3])
{
	int cell = ((x / 16) + (y / 16)) & 1;
	unsigned long noise = ((unsigned long)(x * 73 + y * 151) * 2654435761UL) >> 27;

	rgb[0] = cell ? 70 : 96;
	rgb[1] = cell ? 120 : 150;
	rgb[2] = cell ? 40 : 56;
	if (noise < 5)
	{
		rgb[0] = 90; rgb[1] = 76; rgb[2] = 50;
	}
	if ((x & 15) == 0 || (y & 15) == 0)
	{
		rgb[0] = 170; rgb[1] = 190; rgb[2] = 120;
	}
}

/* GX's CMPR: 8x8 tiles of four DXT1 blocks (top left, top right, bottom
left, bottom right), each with big-endian colors and two bits a texel,
the leftmost texel of each row in the top bits */
static void encode_cmpr(int width, int height, void (*color)(int, int, unsigned long[3]), unsigned char *out)
{
	int tile_x, tile_y, block;

	for (tile_y = 0; tile_y < height; tile_y += 8)
	{
		for (tile_x = 0; tile_x < width; tile_x += 8)
		{
			for (block = 0; block < 4; block++)
			{
				int bx = tile_x + (block & 1) * 4, by = tile_y + (block >> 1) * 4;
				unsigned long texels[16][3], low[3] = { 255, 255, 255 }, high[3] = { 0, 0, 0 };
				unsigned long palette[4][3];
				unsigned short c0, c1;
				int index, channel, row;

				for (index = 0; index < 16; index++)
				{
					color(bx + (index & 3), by + (index >> 2), texels[index]);
					for (channel = 0; channel < 3; channel++)
					{
						if (texels[index][channel] < low[channel]) low[channel] = texels[index][channel];
						if (texels[index][channel] > high[channel]) high[channel] = texels[index][channel];
					}
				}
				c0 = rgb565(high[0], high[1], high[2]);
				c1 = rgb565(low[0], low[1], low[2]);
				if (c0 == c1)
					c1 = c0 > 0 ? c0 - 1 : 1;
				if (c0 < c1)
				{
					unsigned short swap = c0;
					unsigned long t[3];

					c0 = c1; c1 = swap;
					memcpy(t, high, sizeof(t)); memcpy(high, low, sizeof(t)); memcpy(low, t, sizeof(t));
				}
				for (channel = 0; channel < 3; channel++)
				{
					palette[0][channel] = high[channel];
					palette[1][channel] = low[channel];
					palette[2][channel] = (2 * high[channel] + low[channel]) / 3;
					palette[3][channel] = (high[channel] + 2 * low[channel]) / 3;
				}
				out[0] = (unsigned char)(c0 >> 8); out[1] = (unsigned char)c0;
				out[2] = (unsigned char)(c1 >> 8); out[3] = (unsigned char)c1;
				for (row = 0; row < 4; row++)
				{
					unsigned char bits = 0;
					int column;

					for (column = 0; column < 4; column++)
					{
						const unsigned long *texel = texels[row * 4 + column];
						long best = -1, best_distance = 0x7fffffff;
						int candidate;

						for (candidate = 0; candidate < 4; candidate++)
						{
							long distance = 0;

							for (channel = 0; channel < 3; channel++)
							{
								long delta = (long)texel[channel] - (long)palette[candidate][channel];

								distance += delta * delta;
							}
							if (distance < best_distance)
							{
								best_distance = distance;
								best = candidate;
							}
						}
						bits |= (unsigned char)(best << (6 - column * 2));
					}
					out[4 + row] = bits;
				}
				out += 8;
			}
		}
	}
}

/* registered at an address, as texture_cache_initialize_hardware_format
does it for a bitmap the cache has read */
static D3DBaseTexture *ground_texture(void)
{
	static D3DBaseTexture texture;
	const int size = 64;
	unsigned char *pixels = aligned_memory(size * size / 2);

	encode_cmpr(size, size, ground_color, pixels);
	texture.Data = 0;
	texture.Lock = 0;
	texture.Common = D3DCOMMON_TYPE_TEXTURE | 1;
	texture.Format = (6 << D3DFORMAT_VSIZE_SHIFT) | (6 << D3DFORMAT_USIZE_SHIFT) |
		((DWORD)D3DFMT_WII_GX(_gx_tf_cmpr) << D3DFORMAT_FORMAT_SHIFT) |
		(2 << D3DFORMAT_DIMENSION_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT) |
		D3DFORMAT_BORDERSOURCE_COLOR | D3DFORMAT_DMACHANNEL_A;
	texture.Size = 0;
	IDirect3DBaseTexture8_Register(&texture, pixels);
	return &texture;
}

/* GX's RGB5A3 (opaque: the top bit set and RGB555), 4x4 tiles of 32 bytes,
made by CreateTexture and written through LockRect */
static D3DTexture *brick_texture(void)
{
	D3DTexture *texture;
	D3DLOCKED_RECT locked;
	const int size = 64;
	int x, y;

	IDirect3DDevice8_CreateTexture(device, size, size, 1, 0, D3DFMT_WII_GX(_gx_tf_rgb5a3), D3DPOOL_MANAGED, &texture);
	D3DTexture_LockRect(texture, 0, &locked, NULL, 0);
	for (y = 0; y < size; y++)
	{
		for (x = 0; x < size; x++)
		{
			int row = y / 16, shifted = (x + (row & 1) * 16) & 31;
			BOOL mortar = (y % 16) < 2 || shifted < 2;
			unsigned long r = mortar ? 180 : 150 + (row * 37 % 40), g = mortar ? 175 : 70 + (row * 17 % 20), b = mortar ? 160 : 50;
			unsigned short texel = (unsigned short)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
			unsigned char *tile = (unsigned char *)locked.pBits + ((y / 4) * (size / 4) + x / 4) * 32;

			memcpy(tile + ((y % 4) * 4 + x % 4) * 2, &texel, sizeof(texel));
		}
	}
	return texture;
}

/* a 5x7 pixel font, enough for the menu */
static const struct
{
	char letter;
	unsigned char rows[7];
} glyphs[] =
{
	{ 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
	{ 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
	{ 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
	{ 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
	{ 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
	{ 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11 } },
	{ 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
};

#define TEXT_WIDTH 256
#define TEXT_ROW_HEIGHT 16
#define TEXT_ROWS 4

static const char *text_rows[TEXT_ROWS] = { "HALO  WII  GX", "CAMPAIGN", "SPLIT SCREEN", "SETTINGS" };

/* a linear A8R8G8B8 texture, written as the game writes its own: white
letters, two texels to the font's pixel, on clear */
static D3DTexture *text_texture(void)
{
	D3DTexture *texture;
	D3DLOCKED_RECT locked;
	int row;

	IDirect3DDevice8_CreateTexture(device, TEXT_WIDTH, TEXT_ROW_HEIGHT * TEXT_ROWS, 1, 0, D3DFMT_LIN_A8R8G8B8,
		D3DPOOL_MANAGED, &texture);
	D3DTexture_LockRect(texture, 0, &locked, NULL, 0);
	for (row = 0; row < TEXT_ROWS; row++)
	{
		const char *text = text_rows[row];
		int character;

		for (character = 0; text[character]; character++)
		{
			unsigned glyph;

			for (glyph = 0; glyph < sizeof(glyphs) / sizeof(glyphs[0]); glyph++)
			{
				int gx, gy;

				if (glyphs[glyph].letter != text[character])
					continue;
				for (gy = 0; gy < 7; gy++)
				{
					for (gx = 0; gx < 5; gx++)
					{
						int px, py;

						if (!(glyphs[glyph].rows[gy] & (0x10 >> gx)))
							continue;
						for (py = 0; py < 2; py++)
						{
							for (px = 0; px < 2; px++)
							{
								int x = 2 + character * 12 + gx * 2 + px, y = row * TEXT_ROW_HEIGHT + 1 + gy * 2 + py;
								DWORD *texel = (DWORD *)((unsigned char *)locked.pBits + y * locked.Pitch) + x;

								if (x < TEXT_WIDTH)
									*texel = 0xffffffffUL;
							}
						}
					}
				}
			}
		}
	}
	return texture;
}

/* an Xbox swizzled texel offset, for a square texture: x in the even bits */
static unsigned long swizzle(unsigned long x, unsigned long y)
{
	unsigned long offset = 0;
	int bit;

	for (bit = 0; bit < 8; bit++)
		offset |= (((x >> bit) & 1) << (bit * 2)) | (((y >> bit) & 1) << (bit * 2 + 1));
	return offset;
}

/* a swizzled A4R4G4B4 icon: an orange disc, its edge fading out */
static D3DTexture *icon_texture(void)
{
	D3DTexture *texture;
	D3DLOCKED_RECT locked;
	const int size = 64;
	int x, y;

	IDirect3DDevice8_CreateTexture(device, size, size, 1, 0, D3DFMT_A4R4G4B4, D3DPOOL_MANAGED, &texture);
	D3DTexture_LockRect(texture, 0, &locked, NULL, 0);
	for (y = 0; y < size; y++)
	{
		for (x = 0; x < size; x++)
		{
			float dx = x - 31.5f, dy = y - 31.5f, distance = sqrtf(dx * dx + dy * dy);
			unsigned alpha = distance < 22.0f ? 15 : distance < 30.0f ? (unsigned)(15.0f * (30.0f - distance) / 8.0f) : 0;
			BOOL ring = distance > 12.0f && distance < 16.0f;
			unsigned short texel = (unsigned short)((alpha << 12) | ((ring ? 0xf : 0xf) << 8) |
				((ring ? 0xf : 0x8) << 4) | (ring ? 0xf : 0x1));

			((unsigned short *)locked.pBits)[swizzle(x, y)] = texel;
		}
	}
	return texture;
}

/* ---------- geometry */

#define GRID 24
#define GROUND_EXTENT 30.0f

static float ground_height(float x, float z)
{
	return 1.6f * sinf(x * 0.25f) * cosf(z * 0.2f) + 0.6f * sinf(z * 0.5f + 1.0f);
}

static D3DVertexBuffer *ground_vertices;
static D3DIndexBuffer *ground_indices;
static D3DVertexBuffer *pillar_vertices;
static unsigned long pillar_vertex_count;

static void make_ground(void)
{
	struct environment_vertex *vertices;
	WORD *indices;
	BYTE *data;
	int x, z;

	IDirect3DDevice8_CreateVertexBuffer(device, (GRID + 1) * (GRID + 1) * sizeof(*vertices), 0, 0, D3DPOOL_MANAGED,
		&ground_vertices);
	D3DVertexBuffer_Lock(ground_vertices, 0, 0, &data, 0);
	vertices = (struct environment_vertex *)data;
	for (z = 0; z <= GRID; z++)
	{
		for (x = 0; x <= GRID; x++)
		{
			struct environment_vertex *vertex = &vertices[z * (GRID + 1) + x];
			float wx = -GROUND_EXTENT + 2.0f * GROUND_EXTENT * x / GRID;
			float wz = -GROUND_EXTENT + 2.0f * GROUND_EXTENT * z / GRID;
			float normal[3] = { -(ground_height(wx + 0.1f, wz) - ground_height(wx - 0.1f, wz)) / 0.2f, 1.0f,
				-(ground_height(wx, wz + 0.1f) - ground_height(wx, wz - 0.1f)) / 0.2f };

			normalize(normal);
			vertex->position[0] = wx;
			vertex->position[1] = ground_height(wx, wz);
			vertex->position[2] = wz;
			vertex->normal = normpacked3(normal[0], normal[1], normal[2]);
			vertex->binormal = normpacked3(0.0f, 0.0f, 1.0f);
			vertex->tangent = normpacked3(1.0f, 0.0f, 0.0f);
			vertex->texcoord[0] = wx / 6.0f;
			vertex->texcoord[1] = wz / 6.0f;
		}
	}
	IDirect3DDevice8_CreateIndexBuffer(device, GRID * GRID * 6 * sizeof(WORD), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
		&ground_indices);
	indices = (WORD *)ground_indices->Data;
	/* clockwise from above: (x0, z0), (x0, z1), (x1, z1) and (x0, z0), (x1, z1), (x1, z0) */
	for (z = 0; z < GRID; z++)
	{
		for (x = 0; x < GRID; x++)
		{
			WORD p0 = (WORD)(z * (GRID + 1) + x), p1 = (WORD)((z + 1) * (GRID + 1) + x);
			WORD p2 = (WORD)(p1 + 1), p3 = (WORD)(p0 + 1);
			WORD *quad = indices + (z * GRID + x) * 6;

			quad[0] = p0; quad[1] = p1; quad[2] = p2;
			quad[3] = p0; quad[4] = p2; quad[5] = p3;
		}
	}
}

/* a box's six faces, each clockwise from outside: with a x b = -n, the
corners -a-b, -a+b, +a+b, +a-b */
static void box(struct environment_vertex **out, const float center[3], const float half[3])
{
	static const float normals[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	int face;

	for (face = 0; face < 6; face++)
	{
		const float *n = normals[face];
		float a[3], b[3], corners[4][3];
		static const int order[6] = { 0, 1, 2, 0, 2, 3 };
		static const float signs[4][2] = { { -1, -1 }, { -1, 1 }, { 1, 1 }, { 1, -1 } };
		int corner, index;

		/* a: along y for the sides, along x for the top and bottom */
		a[0] = n[1] != 0.0f ? 1.0f : 0.0f;
		a[1] = n[1] != 0.0f ? 0.0f : 1.0f;
		a[2] = 0.0f;
		cross(a, n, b);
		for (corner = 0; corner < 4; corner++)
		{
			int axis;

			for (axis = 0; axis < 3; axis++)
			{
				corners[corner][axis] = center[axis] +
					(n[axis] + signs[corner][0] * a[axis] + signs[corner][1] * b[axis]) * half[axis];
			}
		}
		for (index = 0; index < 6; index++)
		{
			struct environment_vertex *vertex = (*out)++;
			const float *p = corners[order[index]];
			float u = dot(p, a), v = dot(p, b);

			memcpy(vertex->position, p, sizeof(vertex->position));
			vertex->normal = normpacked3(n[0], n[1], n[2]);
			vertex->binormal = normpacked3(b[0], b[1], b[2]);
			vertex->tangent = normpacked3(a[0], a[1], a[2]);
			vertex->texcoord[0] = v / 2.0f;
			vertex->texcoord[1] = -u / 2.0f;
		}
	}
}

static void make_pillars(void)
{
	static const float places[][2] = { { 6, 4 }, { -8, 7 }, { -4, -9 }, { 10, -6 }, { 0, 14 }, { -14, -2 } };
	const int count = sizeof(places) / sizeof(places[0]);
	struct environment_vertex *vertices;
	BYTE *data;
	int index;

	pillar_vertex_count = count * 36 + 36;
	IDirect3DDevice8_CreateVertexBuffer(device, pillar_vertex_count * sizeof(*vertices), 0, 0, D3DPOOL_MANAGED,
		&pillar_vertices);
	D3DVertexBuffer_Lock(pillar_vertices, 0, 0, &data, 0);
	vertices = (struct environment_vertex *)data;
	for (index = 0; index < count; index++)
	{
		float center[3] = { places[index][0], ground_height(places[index][0], places[index][1]) + 3.0f, places[index][1] };
		float half[3] = { 1.0f, 3.5f, 1.0f };

		box(&vertices, center, half);
	}
	{
		/* a low wall across the middle */
		float center[3] = { 0.0f, ground_height(0.0f, 0.0f) + 0.8f, 0.0f };
		float half[3] = { 5.0f, 1.0f, 0.6f };

		box(&vertices, center, half);
	}
}

/* ---------- drawing */

static void set_constant(int reg, float x, float y, float z, float w)
{
	float value[4] = { x, y, z, w };

	IDirect3DDevice8_SetVertexShaderConstant(device, reg, value, 1);
}

/* the world-view-projection the environment program reads, as rows at
c[-96]: a left-handed view from the eye to the target, and a perspective
projection as D3DXMatrixPerspectiveFovLH makes it */
static void set_camera(float eye_x, float eye_y, float eye_z)
{
	float eye[3] = { eye_x, eye_y, eye_z }, target[3] = { 0.0f, 1.0f, 0.0f }, up[3] = { 0.0f, 1.0f, 0.0f };
	float x_axis[3], y_axis[3], z_axis[3];
	float view[4][4], projection[4][4], matrix[4][4];
	const float near_plane = 0.1f, far_plane = 200.0f, y_scale = 1.0f / tanf(35.0f * 3.14159265f / 180.0f);
	const float q = far_plane / (far_plane - near_plane);
	int row, column, k;

	z_axis[0] = target[0] - eye[0]; z_axis[1] = target[1] - eye[1]; z_axis[2] = target[2] - eye[2];
	normalize(z_axis);
	cross(up, z_axis, x_axis);
	normalize(x_axis);
	cross(z_axis, x_axis, y_axis);
	memset(view, 0, sizeof(view));
	memcpy(view[0], x_axis, sizeof(x_axis)); view[0][3] = -dot(x_axis, eye);
	memcpy(view[1], y_axis, sizeof(y_axis)); view[1][3] = -dot(y_axis, eye);
	memcpy(view[2], z_axis, sizeof(z_axis)); view[2][3] = -dot(z_axis, eye);
	view[3][3] = 1.0f;
	memset(projection, 0, sizeof(projection));
	projection[0][0] = y_scale * (SCREEN_HEIGHT / SCREEN_WIDTH);
	projection[1][1] = y_scale;
	projection[2][2] = q;
	projection[2][3] = -q * near_plane;
	projection[3][2] = 1.0f;
	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
		{
			matrix[row][column] = 0.0f;
			for (k = 0; k < 4; k++)
				matrix[row][column] += projection[row][k] * view[k][column];
		}
	}
	IDirect3DDevice8_SetVertexShaderConstant(device, -96, matrix, 4);
}

/* the game's pixel shaders for these draws: a texture alone, as
rasterizer_xbox.c sets one where it only copies a texture (texture stage 0
to the final combiner's D), and the widgets' texture by the vertex color,
or the color alone */
enum
{
	_shader_texture,
	_shader_modulate,
	_shader_color,
};

static void set_pixel_shader(int shader)
{
	D3DPIXELSHADERDEF definition;

	memset(&definition, 0, sizeof(definition));
	definition.PSCombinerCount = 1;
	switch (shader)
	{
	case _shader_texture:
		definition.PSTextureModes = PS_TEXTUREMODES(PS_TEXTUREMODES_PROJECT2D, 0, 0, 0);
		definition.PSFinalCombinerInputsABCD = PS_COMBINERINPUTS(0, 0, 0, PS_REGISTER_T0);
		break;
	case _shader_modulate:
		/* r0.a = t0.a * v0.a; rgb = t0 * v0, alpha r0.a */
		definition.PSTextureModes = PS_TEXTUREMODES(PS_TEXTUREMODES_PROJECT2D, 0, 0, 0);
		definition.PSAlphaInputs[0] = PS_COMBINERINPUTS(PS_REGISTER_T0 | PS_CHANNEL_ALPHA,
			PS_REGISTER_V0 | PS_CHANNEL_ALPHA, 0, 0);
		definition.PSAlphaOutputs[0] = PS_COMBINEROUTPUTS(PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
		definition.PSFinalCombinerInputsABCD = PS_COMBINERINPUTS(PS_REGISTER_T0, PS_REGISTER_V0, 0, 0);
		definition.PSFinalCombinerInputsEFG = PS_COMBINERINPUTS(0, 0, PS_REGISTER_R0 | PS_CHANNEL_ALPHA, 0);
		break;
	default:
		definition.PSFinalCombinerInputsABCD = PS_COMBINERINPUTS(0, 0, 0, PS_REGISTER_V0);
		definition.PSFinalCombinerInputsEFG = PS_COMBINERINPUTS(0, 0, PS_REGISTER_V0 | PS_CHANNEL_ALPHA, 0);
		break;
	}
	IDirect3DDevice8_SetPixelShaderProgram(device, &definition);
}

/* a widget's texture, and the shader that goes with it */
static void set_widget_texture(D3DBaseTexture *texture)
{
	IDirect3DDevice8_SetTexture(device, 0, texture);
	set_pixel_shader(texture ? _shader_modulate : _shader_color);
}

static void draw_level(D3DBaseTexture *ground, D3DTexture *bricks, float angle)
{
	IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ZWRITEENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
	IDirect3DDevice8_SetVertexShader(device, environment_shader);
	set_pixel_shader(_shader_texture);
	set_camera(22.0f * cosf(angle), 9.0f, 22.0f * sinf(angle));
	/* rasterizer_xbox_environment.c's texture transform: u and v as they are */
	set_constant(-84, 1.0f, 1.0f, 0.0f, 0.0f);
	set_constant(-83, 1.0f, 0.0f, 0.0f, 0.0f);
	set_constant(-82, 0.0f, 1.0f, 0.0f, 0.0f);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);

	IDirect3DDevice8_SetTexture(device, 0, ground);
	IDirect3DDevice8_SetStreamSource(device, 0, ground_vertices, sizeof(struct environment_vertex));
	IDirect3DDevice8_SetIndices(device, ground_indices, 0);
	IDirect3DDevice8_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, (GRID + 1) * (GRID + 1), 0, GRID * GRID * 2);

	IDirect3DDevice8_SetTexture(device, 0, (D3DBaseTexture *)bricks);
	IDirect3DDevice8_SetStreamSource(device, 0, pillar_vertices, sizeof(struct environment_vertex));
	IDirect3DDevice8_DrawVertices(device, D3DPT_TRIANGLELIST, 0, pillar_vertex_count);
}

static void set_widget_state(void)
{
	IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice8_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
	IDirect3DDevice8_SetVertexShader(device, widget_shader);
	/* rasterizer_xbox_widgets.c: screen pixels to clip space */
	set_constant(-68, 1.0f / SCREEN_WIDTH * 2.0f, 0.0f, 0.0f, -1.0f - 1.0f / SCREEN_WIDTH);
	set_constant(-67, 0.0f, 1.0f / SCREEN_HEIGHT * -2.0f, 0.0f, 1.0f / SCREEN_HEIGHT + 1.0f);
	set_constant(-66, 0.0f, 0.0f, 1.0f, 0.0f);
	set_constant(-65, 0.0f, 0.0f, 0.0f, 1.0f);
	set_constant(-64, 0.0f, 0.0f, 0.0f, 1.0f);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
}

/* a quad from a vertex buffer the game fills each frame, as its dynamic
widgets are */
static D3DVertexBuffer *widget_vertices;

static void widget_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	D3DCOLOR top, D3DCOLOR bottom)
{
	struct widget_vertex *vertices;
	BYTE *data;
	const struct widget_vertex quad[4] =
	{
		{ { x0, y0, 0.0f }, { u0, v0 }, top },
		{ { x1, y0, 0.0f }, { u1, v0 }, top },
		{ { x1, y1, 0.0f }, { u1, v1 }, bottom },
		{ { x0, y1, 0.0f }, { u0, v1 }, bottom },
	};

	D3DVertexBuffer_Lock(widget_vertices, 0, 0, &data, 0);
	vertices = (struct widget_vertex *)data;
	memcpy(vertices, quad, sizeof(quad));
	IDirect3DDevice8_SetStreamSource(device, 0, widget_vertices, sizeof(struct widget_vertex));
	IDirect3DDevice8_DrawVertices(device, D3DPT_QUADLIST, 0, 4);
}

/* a quad in immediate mode: the texture coordinates (v4) and color (v9),
then the position (v0), which completes each vertex */
static void immediate_quad(float x0, float y0, float x1, float y1, D3DCOLOR color)
{
	const float corners[4][4] = { { x0, y0, 0, 0 }, { x1, y0, 1, 0 }, { x1, y1, 1, 1 }, { x0, y1, 0, 1 } };
	int corner;

	IDirect3DDevice8_Begin(device, D3DPT_QUADLIST);
	for (corner = 0; corner < 4; corner++)
	{
		D3DDevice_SetVertexData2f(4, corners[corner][2], corners[corner][3]);
		IDirect3DDevice8_SetVertexDataColor(device, 9, color);
		D3DDevice_SetVertexData4f(0, corners[corner][0], corners[corner][1], 0.0f, 1.0f);
	}
	IDirect3DDevice8_End(device);
}

static void text_row(D3DTexture *text, int row, float x, float y, float scale, D3DCOLOR top, D3DCOLOR bottom)
{
	float v0 = (float)(row * TEXT_ROW_HEIGHT) / (TEXT_ROW_HEIGHT * TEXT_ROWS);
	float v1 = (float)((row + 1) * TEXT_ROW_HEIGHT) / (TEXT_ROW_HEIGHT * TEXT_ROWS);

	set_widget_texture((D3DBaseTexture *)text);
	widget_quad(x, y, x + TEXT_WIDTH * scale, y + TEXT_ROW_HEIGHT * scale, 0.0f, v0, 1.0f, v1, top, bottom);
}

static void draw_menu(D3DTexture *text, D3DTexture *icon, int selected)
{
	int index;

	set_widget_state();
	IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	/* the panel, untextured */
	set_widget_texture(NULL);
	widget_quad(40, 36, 600, 108, 0, 0, 0, 0, 0xb0000000UL, 0x60000000UL);
	widget_quad(40, 300, 420, 452, 0, 0, 0, 0, 0xa0101820UL, 0xa0101820UL);
	/* the title */
	text_row(text, 0, 64, 46, 2.0f, 0xffffffffUL, 0xffffd040UL);

	for (index = 0; index < 3; index++)
	{
		float y = 312.0f + index * 46.0f;

		set_widget_texture(NULL);
		if (index == selected)
			immediate_quad(48, y - 4, 412, y + 38, 0x803080ffUL);
		set_widget_texture((D3DBaseTexture *)icon);
		IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		immediate_quad(54, y, 88, y + 34, 0xffffffffUL);
		IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
		text_row(text, index + 1, 96, y + 2, 2.0f, 0xffffffffUL, index == selected ? 0xffa0e0ffUL : 0xffc0c0c0UL);
	}
}

/* the corner's checks: culling, a cleared rectangle, the alpha test */
static void draw_checks(D3DTexture *icon)
{
	struct widget_vertex *vertices;
	BYTE *data;
	D3DRECT bar = { 470, 440, 610, 452 };
	/* clockwise on screen (shown), then counter-clockwise (culled) */
	const struct widget_vertex triangles[6] =
	{
		{ { 480, 300, 0 }, { 0, 0 }, 0xff40ff40UL }, { { 540, 360, 0 }, { 0, 0 }, 0xff40ff40UL },
		{ { 480, 360, 0 }, { 0, 0 }, 0xff40ff40UL },
		{ { 550, 300, 0 }, { 0, 0 }, 0xffff4040UL }, { { 550, 360, 0 }, { 0, 0 }, 0xffff4040UL },
		{ { 610, 360, 0 }, { 0, 0 }, 0xffff4040UL },
	};

	set_widget_state();
	IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
	set_widget_texture(NULL);
	D3DVertexBuffer_Lock(widget_vertices, 0, 0, &data, 0);
	vertices = (struct widget_vertex *)data;
	memcpy(vertices, triangles, sizeof(triangles));
	IDirect3DDevice8_SetStreamSource(device, 0, widget_vertices, sizeof(struct widget_vertex));
	IDirect3DDevice8_DrawVertices(device, D3DPT_TRIANGLELIST, 0, 6);
	IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);

	/* the icon, its faded edge cut by the alpha test */
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHATESTENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHAREF, 0x80);
	set_widget_texture((D3DBaseTexture *)icon);
	widget_quad(480, 372, 544, 436, 0, 0, 1, 1, 0xffffffffUL, 0xffffffffUL);
	IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);

	/* a cleared bar */
	IDirect3DDevice8_Clear(device, 1, &bar, D3DCLEAR_TARGET, 0xffe02020UL, 1.0f, 0);
}

#define TURNING_FRAMES 120

void gxtest_run(void)
{
	D3DPRESENT_PARAMETERS parameters;
	D3DBaseTexture *ground;
	D3DTexture *bricks, *text, *icon;
	int frame;

	memset(&parameters, 0, sizeof(parameters));
	parameters.BackBufferWidth = 640;
	parameters.BackBufferHeight = 480;
	Direct3D_CreateDevice(0, D3DDEVTYPE_HAL, NULL, 0, &parameters, &device);
	IDirect3DDevice8_CreateVertexShader(device, environment_declaration,
		(const DWORD *)vertex_shader_code + ENVIRONMENT_PROGRAM_OFFSET / 4, &environment_shader, 0);
	IDirect3DDevice8_CreateVertexShader(device, widget_declaration,
		(const DWORD *)vertex_shader_code + WIDGET_PROGRAM_OFFSET / 4, &widget_shader, 0);
	IDirect3DDevice8_CreateVertexBuffer(device, 64 * sizeof(struct widget_vertex), 0, 0, D3DPOOL_DEFAULT,
		&widget_vertices);
	ground = ground_texture();
	bricks = brick_texture();
	text = text_texture();
	icon = icon_texture();
	make_ground();
	make_pillars();
	platform_log("the scene is made: drawing %d frames", TURNING_FRAMES);

	for (frame = 0; frame < TURNING_FRAMES; frame++)
	{
		IDirect3DDevice8_SetViewport(device, &(D3DVIEWPORT8){ 0, 0, 640, 480, 0.0f, 1.0f });
		IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff5a7ea8UL, 1.0f, 0);
		draw_level(ground, bricks, 0.6f + frame * 0.012f);
		draw_menu(text, icon, (frame / 40) % 3);
		draw_checks(icon);
		IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
	}
	platform_log("done");
}
