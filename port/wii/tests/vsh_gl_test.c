/*
VSH_GL_TEST.C

The Wii's vertex program interpreter (port/wii/src/nv2a_vsh_run.c) against
the Linux port's, which is the reference: the Linux port's translator
(port/linux/src/nv2a_vsh.c) turns each of the game's 67 programs into the
GLSL the PC build draws with, Mesa's software GL runs it with no display
(EGL's surfaceless platform), and transform feedback hands back every output
of every vertex. The same vertices go through the Wii's path: the vertex
bytes read by the program's own declaration (nv2a_vsh_fetch, where the PC
build lets GL read them with the formats d3d8_gl.c gives it), the
interpreter, and the clip position recovered (nv2a_vsh_clip_position).

Each program runs on random vertices of its declaration's layout with
random constants, as well as the shapes the game gives them where they
matter: Direct3D's viewport constants at c[-38] and c[-37], and for the
skinned programs node indices that are node numbers times three with
c[-89].w at 255.9375 (rasterizer_set_frustum_z), so their relative
addressing reaches the node matrices at c[-36] as the game's does. The
outputs compared are the clip position, both colors and both back colors
(clamped, as both devices clamp them), the four texture coordinates and the
fog. A program fails when any output of any vertex differs by more than
1e-3 of its size (plus 1e-3).

    port/wii/tests/run.sh                 (builds and runs it after the others)
    vsh_gl_test -v N                      program N's GLSL and its first vertex, side by side

It needs Mesa's EGL and GL libraries (libEGL.so.1, llvmpipe), and nothing
else: GL is reached through eglGetProcAddress. Where there is no EGL it says
so and is skipped.
*/

#include "nv2a_vsh_run.h"
#include "vsh_programs.h"
#include "xgpu_host.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- EGL and GL, by hand: the box has the libraries, not the headers */

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef int EGLint;
typedef unsigned int EGLenum, EGLBoolean;

#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_OPENGL_API 0x30A2
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_NONE 0x3038

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef long GLsizeiptr, GLintptr;
typedef char GLchar;

#define GL_FLOAT 0x1406
#define GL_SHORT 0x1402
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_BGRA 0x80E1
#define GL_POINTS 0x0000
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TRANSFORM_FEEDBACK_BUFFER 0x8C8E
#define GL_INTERLEAVED_ATTRIBS 0x8C8C
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_MAP_READ_BIT 0x0001
#define GL_RENDERER 0x1F01
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_RGBA8 0x8058
#define GL_COLOR_ATTACHMENT0 0x8CE0

static struct
{
	EGLDisplay (*GetPlatformDisplay)(EGLenum, void *, const intptr_t *);
	EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
	EGLBoolean (*BindAPI)(EGLenum);
	EGLContext (*CreateContext)(EGLDisplay, void *, EGLContext, const EGLint *);
	EGLBoolean (*MakeCurrent)(EGLDisplay, void *, void *, EGLContext);
	void *(*GetProcAddress)(const char *);
} egl;

static struct
{
	const unsigned char *(*GetString)(GLenum);
	GLuint (*CreateShader)(GLenum);
	void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
	void (*CompileShader)(GLuint);
	void (*GetShaderiv)(GLuint, GLenum, GLint *);
	void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
	GLuint (*CreateProgram)(void);
	void (*AttachShader)(GLuint, GLuint);
	void (*TransformFeedbackVaryings)(GLuint, GLsizei, const GLchar *const *, GLenum);
	void (*LinkProgram)(GLuint);
	void (*GetProgramiv)(GLuint, GLenum, GLint *);
	void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
	void (*UseProgram)(GLuint);
	void (*DeleteProgram)(GLuint);
	void (*DeleteShader)(GLuint);
	GLint (*GetUniformLocation)(GLuint, const GLchar *);
	void (*Uniform4fv)(GLint, GLsizei, const GLfloat *);
	void (*Uniform1f)(GLint, GLfloat);
	void (*GenVertexArrays)(GLsizei, GLuint *);
	void (*BindVertexArray)(GLuint);
	void (*GenBuffers)(GLsizei, GLuint *);
	void (*BindBuffer)(GLenum, GLuint);
	void (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
	void (*BindBufferBase)(GLenum, GLuint, GLuint);
	void (*EnableVertexAttribArray)(GLuint);
	void (*DisableVertexAttribArray)(GLuint);
	void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
	void (*VertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void *);
	void (*VertexAttrib4f)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
	void (*VertexAttribI4ui)(GLuint, GLuint, GLuint, GLuint, GLuint);
	void (*Enable)(GLenum);
	void (*BeginTransformFeedback)(GLenum);
	void (*EndTransformFeedback)(void);
	void (*DrawArrays)(GLenum, GLint, GLsizei);
	void *(*MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
	GLboolean (*UnmapBuffer)(GLenum);
	void (*GenFramebuffers)(GLsizei, GLuint *);
	void (*BindFramebuffer)(GLenum, GLuint);
	void (*GenRenderbuffers)(GLsizei, GLuint *);
	void (*BindRenderbuffer)(GLenum, GLuint);
	void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
	void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
	GLenum (*GetError)(void);
} gl;

static int gl_start(void)
{
	void *library = dlopen("libEGL.so.1", RTLD_NOW);
	static const EGLint attributes[] =
	{
		EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, 1, EGL_NONE,
	};
	EGLDisplay display;
	EGLContext context;
	EGLint major, minor;

	if (!library)
		return 0;
	*(void **)&egl.GetPlatformDisplay = dlsym(library, "eglGetPlatformDisplay");
	*(void **)&egl.Initialize = dlsym(library, "eglInitialize");
	*(void **)&egl.BindAPI = dlsym(library, "eglBindAPI");
	*(void **)&egl.CreateContext = dlsym(library, "eglCreateContext");
	*(void **)&egl.MakeCurrent = dlsym(library, "eglMakeCurrent");
	*(void **)&egl.GetProcAddress = dlsym(library, "eglGetProcAddress");
	if (!egl.GetPlatformDisplay || !egl.GetProcAddress)
		return 0;
	display = egl.GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
	if (!display || !egl.Initialize(display, &major, &minor) || !egl.BindAPI(EGL_OPENGL_API))
		return 0;
	context = egl.CreateContext(display, NULL, NULL, attributes);
	if (!context || !egl.MakeCurrent(display, NULL, NULL, context))
		return 0;
#define LOAD(name) if (!(*(void **)&gl.name = egl.GetProcAddress("gl" #name))) return 0
	LOAD(GetString); LOAD(CreateShader); LOAD(ShaderSource); LOAD(CompileShader); LOAD(GetShaderiv);
	LOAD(GetShaderInfoLog); LOAD(CreateProgram); LOAD(AttachShader); LOAD(TransformFeedbackVaryings);
	LOAD(LinkProgram); LOAD(GetProgramiv); LOAD(GetProgramInfoLog); LOAD(UseProgram); LOAD(DeleteProgram);
	LOAD(DeleteShader); LOAD(GetUniformLocation); LOAD(Uniform4fv); LOAD(Uniform1f); LOAD(GenVertexArrays);
	LOAD(BindVertexArray); LOAD(GenBuffers); LOAD(BindBuffer); LOAD(BufferData); LOAD(BindBufferBase);
	LOAD(EnableVertexAttribArray); LOAD(DisableVertexAttribArray); LOAD(VertexAttribPointer);
	LOAD(VertexAttribIPointer); LOAD(VertexAttrib4f); LOAD(VertexAttribI4ui); LOAD(Enable);
	LOAD(BeginTransformFeedback); LOAD(EndTransformFeedback); LOAD(DrawArrays); LOAD(MapBufferRange);
	LOAD(UnmapBuffer); LOAD(GenFramebuffers); LOAD(BindFramebuffer); LOAD(GenRenderbuffers);
	LOAD(BindRenderbuffer); LOAD(RenderbufferStorage); LOAD(FramebufferRenderbuffer); LOAD(GetError);
#undef LOAD
	/* a surfaceless context has no framebuffer, and GL draws nothing, not
	even with the rasterizer off, until it has a complete one */
	{
		GLuint framebuffer, renderbuffer;

		gl.GenFramebuffers(1, &framebuffer);
		gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
		gl.GenRenderbuffers(1, &renderbuffer);
		gl.BindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
		gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
		gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
	}
	printf("reference: the Linux port's GLSL on %s\n", gl.GetString(GL_RENDERER));
	return 1;
}

/* ---------- a program's declaration */

struct element
{
	int reg, stream, type, bytes, offset;
};

struct layout
{
	struct element elements[NV2A_VSH_ATTRIBUTE_COUNT];
	int count;
	int strides[2];
	unsigned long packed_mask;
};

static void parse(const uint32_t *declaration, struct layout *layout)
{
	int stream = 0;

	memset(layout, 0, sizeof(*layout));
	for (; *declaration != 0xFFFFFFFF; declaration++)
	{
		uint32_t token = *declaration;

		if ((token >> 29) == 1)
			stream = token & 0xf;
		else if ((token >> 29) == 2 && !(token & 0x10000000))
		{
			struct element *element = &layout->elements[layout->count++];

			element->reg = token & 0x1f;
			element->stream = stream;
			element->type = (token >> 16) & 0xff;
			element->bytes = (int)nv2a_vsh_type_bytes(element->type);
			element->offset = layout->strides[stream];
			layout->strides[stream] += element->bytes;
			if (element->type == NV2A_VSDT_NORMPACKED3)
				layout->packed_mask |= 1UL << element->reg;
		}
	}
}

/* ---------- random data */

static unsigned long long random_state = 0x9E3779B97F4A7C15ULL;

static uint32_t random_word(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return (uint32_t)(random_state >> 16);
}

static float random_float(float low, float high)
{
	return low + (high - low) * (random_word() & 0xffffff) / (float)0x1000000;
}

#define VERTEX_COUNT 256
#define NODE_COUNT 44

static float constants[NV2A_VSH_CONSTANT_COUNT][4];
static unsigned char streams[2][VERTEX_COUNT * 64];

static float *constant(int reg)
{
	return constants[reg + NV2A_VSH_CONSTANT_BIAS];
}

static void random_constants(void)
{
	int index, component;

	for (index = 0; index < NV2A_VSH_CONSTANT_COUNT; index++)
		for (component = 0; component < 4; component++)
			constants[index][component] = random_float(-2.0f, 2.0f);
	/* Direct3D's viewport constants for 640x480 (d3d8_gx.c, d3d8_gl.c) */
	memcpy(constant(-38), (float[4]){ 320.0f, -240.0f, 16777215.0f, 0.0f }, sizeof(float[4]));
	memcpy(constant(-37), (float[4]){ 320.0f, 240.0f, 0.0f, 0.0f }, sizeof(float[4]));
	/* a node index byte times this is the node's first row */
	constant(-89)[3] = 255.9375f;
}

static void random_vertices(const struct layout *layout)
{
	int vertex, which, index;

	for (vertex = 0; vertex < VERTEX_COUNT; vertex++)
	{
		for (which = 0; which < layout->count; which++)
		{
			const struct element *element = &layout->elements[which];
			unsigned char *data = streams[element->stream] + vertex * layout->strides[element->stream] + element->offset;

			switch (element->type)
			{
			case NV2A_VSDT_FLOAT1: case NV2A_VSDT_FLOAT2: case NV2A_VSDT_FLOAT3: case NV2A_VSDT_FLOAT4:
			case NV2A_VSDT_FLOAT2H:
				for (index = 0; index < element->bytes / 4; index++)
				{
					float value = random_float(-2.0f, 2.0f);

					memcpy(data + index * 4, &value, 4);
				}
				break;
			case NV2A_VSDT_PBYTE1: case NV2A_VSDT_PBYTE2: case NV2A_VSDT_PBYTE3: case NV2A_VSDT_PBYTE4:
				for (index = 0; index < element->bytes; index++)
				{
					/* the model vertex's node indices (v5): node numbers times
					three, as the game's models store them */
					data[index] = element->reg == 5 ?
						(unsigned char)(3 * (random_word() % NODE_COUNT)) : (unsigned char)random_word();
				}
				break;
			default:
				for (index = 0; index < element->bytes; index++)
					data[index] = (unsigned char)random_word();
				break;
			}
		}
	}
}

/* ---------- the reference: the Linux port's GLSL, run by GL */

/* each vertex's outputs as both sides give them: the clip position, the
colors and back colors, the four texture coordinates, the fog */
#define OUTPUT_FLOATS 37

static const char *const output_names[] = { "gl_Position", "xD0", "xD1", "xB0", "xB1", "xT0", "xT1", "xT2", "xT3", "xFog" };
static const char *const output_labels[OUTPUT_FLOATS / 4 + 1] = { "clip", "D0", "D1", "B0", "B1", "T0", "T1", "T2", "T3", "fog" };

static int run_reference(int program_index, const struct layout *layout, float (*out)[OUTPUT_FLOATS], int verbose)
{
	const uint32_t *words = vsh_program_words(program_index);
	char *code = nv2a_vertex_shader_to_glsl((const DWORD *)words + 1, words[0] >> 16, layout->packed_mask, NULL);
	const char *source = code;
	GLuint shader, program, vertex_array, buffers[2], feedback;
	GLint status;
	char log[4096];
	int which, index;
	void *mapped;

	if (verbose)
		printf("%s\n", code);
	shader = gl.CreateShader(GL_VERTEX_SHADER);
	gl.ShaderSource(shader, 1, &source, NULL);
	gl.CompileShader(shader);
	gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		gl.GetShaderInfoLog(shader, sizeof(log), NULL, log);
		printf("FAIL program %d: the reference GLSL does not compile:\n%s\n", program_index, log);
		free(code);
		return 0;
	}
	program = gl.CreateProgram();
	gl.AttachShader(program, shader);
	gl.TransformFeedbackVaryings(program, 10, output_names, GL_INTERLEAVED_ATTRIBS);
	gl.LinkProgram(program);
	gl.GetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		gl.GetProgramInfoLog(program, sizeof(log), NULL, log);
		printf("FAIL program %d: the reference does not link:\n%s\n", program_index, log);
		free(code);
		return 0;
	}
	gl.UseProgram(program);
	gl.Uniform4fv(gl.GetUniformLocation(program, "c"), NV2A_VSH_CONSTANT_COUNT, &constants[0][0]);
	/* the viewport the constants at c[-38] and c[-37] are made from */
	gl.Uniform4fv(gl.GetUniformLocation(program, "viewport_scale"), 1, constant(-38));
	gl.Uniform4fv(gl.GetUniformLocation(program, "viewport_offset"), 1, constant(-37));
	gl.Uniform1f(gl.GetUniformLocation(program, "point_size"), 1.0f);
	gl.Uniform1f(gl.GetUniformLocation(program, "screen_offset"), 0.0f);

	gl.GenVertexArrays(1, &vertex_array);
	gl.BindVertexArray(vertex_array);
	gl.GenBuffers(2, buffers);
	for (index = 0; index < 2; index++)
	{
		gl.BindBuffer(GL_ARRAY_BUFFER, buffers[index]);
		gl.BufferData(GL_ARRAY_BUFFER, sizeof(streams[index]), streams[index], GL_STATIC_DRAW);
	}
	/* the inputs no element feeds: (0, 0, 0, 1), as both devices start them */
	for (index = 0; index < NV2A_VSH_ATTRIBUTE_COUNT; index++)
	{
		gl.DisableVertexAttribArray(index);
		if (layout->packed_mask & (1UL << index))
			gl.VertexAttribI4ui(index, 0, 0, 0, 0);
		else
			gl.VertexAttrib4f(index, 0.0f, 0.0f, 0.0f, 1.0f);
	}
	/* the formats d3d8_gl.c's attribute_format gives GL */
	for (which = 0; which < layout->count; which++)
	{
		const struct element *element = &layout->elements[which];
		const void *offset = (const void *)(intptr_t)element->offset;
		GLsizei stride = layout->strides[element->stream];
		GLint size = 4;
		GLenum type = GL_FLOAT;
		GLboolean normalized = 0;

		gl.BindBuffer(GL_ARRAY_BUFFER, buffers[element->stream]);
		gl.EnableVertexAttribArray(element->reg);
		if (element->type == NV2A_VSDT_NORMPACKED3)
		{
			gl.VertexAttribIPointer(element->reg, 1, GL_UNSIGNED_INT, stride, offset);
			continue;
		}
		switch (element->type)
		{
		case NV2A_VSDT_FLOAT1: size = 1; break;
		case NV2A_VSDT_FLOAT2: size = 2; break;
		case NV2A_VSDT_FLOAT3: case NV2A_VSDT_FLOAT2H: size = 3; break;
		case NV2A_VSDT_FLOAT4: size = 4; break;
		case NV2A_VSDT_D3DCOLOR: size = GL_BGRA; type = GL_UNSIGNED_BYTE; normalized = 1; break;
		case NV2A_VSDT_SHORT1: case NV2A_VSDT_SHORT2: case NV2A_VSDT_SHORT3: case NV2A_VSDT_SHORT4:
			size = element->bytes / 2; type = GL_SHORT; break;
		case NV2A_VSDT_NORMSHORT1: case NV2A_VSDT_NORMSHORT2: case NV2A_VSDT_NORMSHORT3: case NV2A_VSDT_NORMSHORT4:
			size = element->bytes / 2; type = GL_SHORT; normalized = 1; break;
		case NV2A_VSDT_PBYTE1: case NV2A_VSDT_PBYTE2: case NV2A_VSDT_PBYTE3: case NV2A_VSDT_PBYTE4:
			size = element->bytes; type = GL_UNSIGNED_BYTE; normalized = 1; break;
		}
		gl.VertexAttribPointer(element->reg, size, type, normalized, stride, offset);
	}

	gl.GenBuffers(1, &feedback);
	gl.BindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback);
	gl.BufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof(float) * OUTPUT_FLOATS * VERTEX_COUNT, NULL, GL_STATIC_DRAW);
	gl.BindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback);
	gl.Enable(GL_RASTERIZER_DISCARD);
	gl.BeginTransformFeedback(GL_POINTS);
	gl.DrawArrays(GL_POINTS, 0, VERTEX_COUNT);
	gl.EndTransformFeedback();
	mapped = gl.MapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(float) * OUTPUT_FLOATS * VERTEX_COUNT,
		GL_MAP_READ_BIT);
	if (!mapped || gl.GetError())
	{
		printf("FAIL program %d: the reference's transform feedback failed\n", program_index);
		free(code);
		return 0;
	}
	memcpy(out, mapped, sizeof(float) * OUTPUT_FLOATS * VERTEX_COUNT);
	gl.UnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
	gl.UseProgram(0);
	gl.DeleteProgram(program);
	gl.DeleteShader(shader);
	free(code);
	return 1;
}

/* ---------- the Wii's: fetch, interpreter, clip position */

static float clamp01(float x)
{
	return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

static void run_wii(int program_index, const struct layout *layout, float (*out)[OUTPUT_FLOATS])
{
	const uint32_t *words = vsh_program_words(program_index);
	struct nv2a_vsh_program program;
	int vertex;

	nv2a_vsh_decode(words + 1, words[0] >> 16, &program);
	for (vertex = 0; vertex < VERTEX_COUNT; vertex++)
	{
		float inputs[NV2A_VSH_ATTRIBUTE_COUNT][4];
		struct nv2a_vsh_result result;
		float *o = out[vertex];
		int which, index;

		memset(inputs, 0, sizeof(inputs));
		for (index = 0; index < NV2A_VSH_ATTRIBUTE_COUNT; index++)
			inputs[index][3] = 1.0f;
		for (which = 0; which < layout->count; which++)
		{
			const struct element *element = &layout->elements[which];

			nv2a_vsh_fetch(element->type, streams[element->stream] + vertex * layout->strides[element->stream] +
				element->offset, inputs[element->reg]);
		}
		nv2a_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, &result);
		nv2a_vsh_clip_position(&result, constant(-38), constant(-37), constant(-38), constant(-37), o);
		for (index = 0; index < 4; index++)
		{
			o[4 + index] = clamp01(result.diffuse[index]);
			o[8 + index] = clamp01(result.specular[index]);
			o[12 + index] = clamp01(result.back_diffuse[index]);
			o[16 + index] = clamp01(result.back_specular[index]);
			o[20 + index] = result.texcoords[0][index];
			o[24 + index] = result.texcoords[1][index];
			o[28 + index] = result.texcoords[2][index];
			o[32 + index] = result.texcoords[3][index];
		}
		o[36] = result.fog;
	}
}

/* ---------- the comparison */

static float reference[VERTEX_COUNT][OUTPUT_FLOATS];
static float wii[VERTEX_COUNT][OUTPUT_FLOATS];

static int same(float a, float b)
{
	if (isnan(a) || isnan(b))
		return isnan(a) && isnan(b);
	if (isinf(a) || isinf(b))
		return a == b;
	return fabsf(a - b) <= 1.0e-3f * (1.0f + fmaxf(fabsf(a), fabsf(b)));
}

static int compare(int program_index, int verbose)
{
	int vertex, index, wrong = 0, first_vertex = -1, first_index = -1;

	for (vertex = 0; vertex < VERTEX_COUNT; vertex++)
	{
		int vertex_wrong = 0;

		for (index = 0; index < OUTPUT_FLOATS; index++)
		{
			if (!same(reference[vertex][index], wii[vertex][index]))
			{
				if (first_vertex < 0)
				{
					first_vertex = vertex;
					first_index = index;
				}
				vertex_wrong = 1;
			}
		}
		wrong += vertex_wrong;
	}
	if (verbose)
	{
		printf("program %d, vertex 0:\n", program_index);
		for (index = 0; index < OUTPUT_FLOATS; index += 4)
		{
			int count = index == 36 ? 1 : 4, component;

			printf("  %-5s gl", output_labels[index / 4]);
			for (component = 0; component < count; component++)
				printf(" %12.6g", reference[0][index + component]);
			printf("\n        wii");
			for (component = 0; component < count; component++)
				printf(" %12.6g", wii[0][index + component]);
			printf("\n");
		}
	}
	if (wrong)
	{
		printf("FAIL program %d: %d of %d vertices differ; the first, vertex %d's %s.%c: %g, not %g\n",
			program_index, wrong, VERTEX_COUNT, first_vertex, output_labels[first_index / 4],
			"xyzw"[first_index % 4], wii[first_vertex][first_index], reference[first_vertex][first_index]);
	}
	return !wrong;
}

int main(int argc, char **argv)
{
	int verbose = -1, index, passed = 0, failed = 0;

	if (argc > 2 && !strcmp(argv[1], "-v"))
		verbose = atoi(argv[2]);
	if (!gl_start())
	{
		printf("vsh_gl_test: skipped, there is no EGL with a GL 4.5 context (Mesa's llvmpipe) here\n");
		return 0;
	}
	for (index = 0; index < VSH_PROGRAM_COUNT; index++)
	{
		struct layout layout;
		int round;

		if (verbose >= 0 && index != verbose)
			continue;
		parse(vsh_program_declaration(index), &layout);
		/* each program on four sets of constants, VERTEX_COUNT vertices each */
		for (round = 0; round < 4; round++)
		{
			random_constants();
			random_vertices(&layout);
			if (!run_reference(index, &layout, reference, verbose >= 0 && round == 0))
			{
				failed++;
				break;
			}
			run_wii(index, &layout, wii);
			if (!compare(index, verbose >= 0 && round == 0))
			{
				failed++;
				break;
			}
		}
		if (round == 4)
			passed++;
	}
	printf("vsh_gl_test: %d programs as the Linux port's GLSL computes them, %d not\n", passed, failed);
	return failed != 0;
}
