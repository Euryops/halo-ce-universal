/*
XGPU_HOST.H

What the Linux port's vertex program translator (port/linux/src/nv2a_vsh.c)
and its string helper (xgpu_text.c) need of xgpu.h, for building them on
the host without the XDK or GL headers: run.sh compiles copies of the two
with their include of xgpu.h pointed here, so vsh_gl_test.c can run the
translator's GLSL as the reference for the Wii's interpreter.
*/

#ifndef __XGPU_HOST_H
#define __XGPU_HOST_H

#include <stdint.h>

/* 32 bits, as on the Xbox and the Linux port's i386 build: the translator
reads the instructions as DWORDs */
typedef uint32_t DWORD;
typedef int BOOL;
#define TRUE 1
#define FALSE 0

struct xgpu_text
{
	char *buffer;
	unsigned long length;
	unsigned long capacity;
};

void xgpu_text_append(struct xgpu_text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
#define XGPU_VERTEX_CONSTANT_COUNT 192
#define XGPU_VERTEX_CONSTANT_BIAS 96

struct nv2a_vertex_lighting
{
	int lights;
	unsigned long normal_instruction, normal_register;
	unsigned long position_instruction, position_register;
};

BOOL nv2a_vertex_shader_lighting(const DWORD *instructions, unsigned long instruction_count,
	struct nv2a_vertex_lighting *lighting);
char *nv2a_vertex_shader_to_glsl(const DWORD *instructions, unsigned long instruction_count,
	unsigned long packed_attribute_mask, const struct nv2a_vertex_lighting *lighting);

#endif /* __XGPU_HOST_H */
