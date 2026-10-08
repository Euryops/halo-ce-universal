/*
VSH_PROGRAMS.H

The game's 67 vertex programs for the host tests and tools, from the same
data as rasterizer_xbox_vertex_shaders.c: vsh_program_words(index) is a
program's header word followed by its instructions, and
vsh_program_declaration(index) its vertex declaration.
*/

#ifndef __VSH_PROGRAMS_H
#define __VSH_PROGRAMS_H

#include <stdint.h>

#define VSH_PROGRAM_COUNT 67

static const uint32_t vsh_program_code[] =
{
#include "rasterizer_xbox_vertex_shaders_data.inc"
};

/* rasterizer_xbox_vertex_shaders.c: each program's byte offset */
static const unsigned long vsh_program_offsets[VSH_PROGRAM_COUNT] =
{
	0x0000, 0x0078, 0x0140, 0x0308, 0x0470, 0x04F8, 0x0670, 0x0708, 0x0890, 0x0918, 0x0BF0, 0x1018,
	0x1210, 0x13E8, 0x1540, 0x1808, 0x1B70, 0x1D08, 0x2200, 0x2478, 0x2710, 0x27D8, 0x2910, 0x2A98,
	0x2BF0, 0x2E48, 0x31C0, 0x3308, 0x3490, 0x36B8, 0x3770, 0x39C8, 0x3F30, 0x4198, 0x4380, 0x4658,
	0x4900, 0x4CB8, 0x4DA0, 0x4E58, 0x4FB0, 0x5088, 0x51E0, 0x52E8, 0x54E0, 0x5648, 0x5A00, 0x5B88,
	0x5F00, 0x6158, 0x6280, 0x6708, 0x6830, 0x6938, 0x6A40, 0x6C78, 0x6DD0, 0x6E68, 0x7260, 0x72D8,
	0x7410, 0x7788, 0x7970, 0x7D38, 0x80D0, 0x8368, 0x8510,
};

/* rasterizer_xbox_vertex_shaders_initialize.c: the declarations, and the
dword each program's starts at */
static const uint32_t vsh_declaration_words[] =
{
	0x20000000, 0x40320000, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40210004, 0xFFFFFFFF,
	0x20000000, 0x40340000, 0x40340009, 0x40150008, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0x20000001, 0x4024000B, 0xFFFFFFFF,
	0x20000000, 0x40220000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40210004, 0x40240005, 0x40110006, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0x20000001, 0x40160007, 0x40210008, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0xFFFFFFFF,
};

static const unsigned char vsh_program_declarations[VSH_PROGRAM_COUNT] =
{
	  0,   4,  13,  25,  25,  30,  49,  49,  49,  30,  30,   8,
	 13,  30,  30,  30,  39,  30,  30,  49,  49,  49,  49,  49,
	 49,  39,  49,  30,  49,  49,  30,  30,  30,   8,  49,  49,
	 30,  49,  25,  30,  49,  49,  49,  49,  39,  30,  49,  30,
	 49,  49,  30,  49,  49,  49,  49,  49,  13,  30,  39,  39,
	 30,  49,  30,  30,  30,  13,  18,
};

static inline const uint32_t *vsh_program_words(int index)
{
	return vsh_program_code + vsh_program_offsets[index] / 4;
}

static inline const uint32_t *vsh_program_declaration(int index)
{
	return vsh_declaration_words + vsh_program_declarations[index];
}

#endif /* __VSH_PROGRAMS_H */
