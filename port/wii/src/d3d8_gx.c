/*
D3D8_GX.C

The Xbox Direct3D 8 device on the Wii's GPU (GX), in the place the Linux
port has d3d8_gl.c, and modelled on Halo3DS's GPU layer (port/n3ds,
engine_gpu_*), which runs the same engine on another fixed-function GPU.

The game drives the device as on the Xbox: the XDK header's inline
functions keep the simple render states in D3D__RenderState and call in here
for the rest, and each draw reads the whole state back from there. What
differs from the Linux port is where the work is done:

- Vertex programs run on the CPU (nv2a_vsh_run.c): GX has no programmable
  vertex stage. Each draw's vertices are read from the game's buffers by the
  vertex shader's declaration, run through its program, and handed to GX
  with the program's position, diffuse color and texture coordinates.
- GX's fixed transform does the rest. The programs end in screen space
  (c[-38], c[-37]); that conversion is undone to a clip-space position, and
  the draw's projection is fitted: Halo's 3D draws are a perspective
  projection, whose depth is a fixed affine function of w across the draw,
  so GX's own perspective matrix can give back each vertex's depth while x,
  y and w are passed through (gx_backend.h, struct gxb_projection). GX then
  clips and interpolates perspective-correctly, as the NV2A did. A draw
  that no one projection fits (none of Halo's, as far as is known) is
  drawn with its positions divided by w, which keeps its depth exact and
  loses perspective-correct texturing.
- Textures in GX's formats (the maps' bitmaps, converted by
  port/wii/mapconv) are sampled where they lie; textures the game makes at
  run time in Xbox formats are converted (d3d8_gx_resources.c).
- The pixel shader, the NV2A's register combiners, is translated to TEV
  stages (nv2a_tev.c), with the vertices' two colors, up to four textures
  and the fog factor (a fifth texture, a ramp) as its inputs. A shader that
  does not fit TEV's 16 stages (the environment's bump-mapped specular
  passes) is not drawn. Cube maps and 3D textures are sampled as their first
  face or slice, with the first two texture coordinates.
- Only the back buffer is drawn into. Draws and clears into other targets
  (the shadow maps, the water's reflection, the screen effects' copies) are
  counted and skipped, so the effects that read them are missing.
- Visibility tests answer "all of it visible".
*/

#include "d3d8_gx.h"
#include "nv2a_vsh_run.h"
#include "nv2a_tev.h"
#include "halo_ui_pointer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- state the XDK header's inline functions read and write */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;

/* ---------- vertex shaders */

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS NV2A_VSH_MAXIMUM_INSTRUCTIONS

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	struct nv2a_vsh_program *program;
	struct vertex_element elements[NV2A_VSH_ATTRIBUTE_COUNT];
	unsigned long element_count;
};

/* ---------- the device */

static struct
{
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	D3DSHADERCONSTANTMODE shader_constant_mode;

	struct vertex_shader_object *vertex_shader;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	unsigned long program_address;
	float constants[NV2A_VSH_CONSTANT_COUNT][4];
	float viewport_scale[4];
	float viewport_offset[4];

	struct
	{
		DWORD data;
		UINT stride;
	} streams[16];
	/* SetIndices' base vertex, added to every index of an indexed draw */
	UINT base_vertex_index;

	/* the current value of each input register (SetVertexData) */
	float attributes[NV2A_VSH_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	float *immediate_vertices;
	unsigned long immediate_count;
	unsigned long immediate_capacity;

	/* each draw's vertices as GX takes them, their clip positions, and its
	indices rebased to the first vertex drawn */
	struct gxb_vertex *vertices;
	float (*clips)[4];
	unsigned long vertex_capacity;
	uint16_t *indices;
	unsigned long index_capacity;

	unsigned long frame;
	unsigned long next_vertex_shader_id;
	BOOL created;
} device;

/* counted for each frame, and logged for the first frames and then now
and again: on a Wii with no debugger this is how the device is seen */
static struct
{
	unsigned long draws, vertices, clears, skipped_target, skipped_program, skipped_fvf;
	unsigned long perspective, orthographic, divided;
	unsigned long textured, untextured_format;
	/* draws with the game's pixel shader as TEV stages, and those whose
	shader did not fit TEV (skipped) */
	unsigned long shaded, skipped_shader;
} stats;

/* frames shown: a presented frame is counted at the vertical blank that
shows it */
static volatile unsigned int flip_count;
static volatile unsigned int pending_flips;
static D3DCALLBACK vertical_blank_callback;

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&device;
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

/* D3DCOLOR (ARGB) to GX's RGBA */
static uint32_t argb_to_rgba(D3DCOLOR color)
{
	return (color << 8) | (color >> 24);
}

static unsigned char unit_to_byte(float value)
{
	if (!(value > 0.0f))
		return 0;
	if (value >= 1.0f)
		return 255;
	return (unsigned char)(value * 255.0f + 0.5f);
}

/* ---------- the engine port's hooks (halo_linux_source_fixups.h) */

/* the Wii draws the Xbox's screen: 640 by 480, nothing wider */
long halo_screen_width(void) { return GXB_SCREEN_WIDTH; }
long halo_screen_commit(void) { return GXB_SCREEN_WIDTH; }
long halo_shadow_map_scale(void) { return 1; }
void halo_screen_ui_offset(unsigned char centered) { (void)centered; }
void halo_vertex_shader_lighting(unsigned long handle) { (void)handle; }
void halo_screen_anti_alias(short x0, short y0, short x1, short y1) { (void)x0; (void)y0; (void)x1; (void)y1; }

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	(void)menus_active;
	(void)pointer;
	return 0;
}

/* ---------- vertical blank */

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure: frames presented */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

static void vertical_blank(void)
{
	D3DCALLBACK callback = vertical_blank_callback;

	if (pending_flips)
	{
		pending_flips--;
		flip_count++;
	}
	/* the game's (main.c main_vertical_blank_interrupt_handler) counts the
	blanks its frame throttle waits on, and polls the controllers */
	if (callback)
		callback(0);
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	vertical_blank_callback = callback;
	gxb_set_vertical_blank_handler(vertical_blank);
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	gxb_wait_vertical_blank();
}

/* ---------- device creation */

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	static DWORD direct3d;

	(void)sdk_version;
	return (Direct3D *)&direct3d;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

static void viewport_update_constants(void)
{
	/* Direct3D's reserved constants c[-38] and c[-37] map clip space to
	the screen; the depth buffer is 24 bits */
	const float zscale = 16777215.0f;

	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		memcpy(device.constants[NV2A_VSH_CONSTANT_BIAS - 38], device.viewport_scale, sizeof(device.viewport_scale));
		memcpy(device.constants[NV2A_VSH_CONSTANT_BIAS - 37], device.viewport_offset, sizeof(device.viewport_offset));
	}
	gxb_set_viewport((float)device.viewport.X, (float)device.viewport.Y, (float)device.viewport.Width,
		(float)device.viewport.Height, device.viewport.MinZ, device.viewport.MaxZ);
	/* the NV2A clips to the viewport, which is what keeps one split-screen
	view's drawing out of the other's */
	gxb_set_scissor((int)device.viewport.X, (int)device.viewport.Y, (int)device.viewport.Width,
		(int)device.viewport.Height);
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		gx_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, GXB_SCREEN_WIDTH, GXB_SCREEN_HEIGHT);
		gx_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, GXB_SCREEN_WIDTH, GXB_SCREEN_HEIGHT);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		for (index = 0; index < NV2A_VSH_ATTRIBUTE_COUNT; index++)
			device.attributes[index][3] = 1.0f;
		device.next_vertex_shader_id = 1;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
		}
		gxb_initialize();
		device.viewport.Width = GXB_SCREEN_WIDTH;
		device.viewport.Height = GXB_SCREEN_HEIGHT;
		device.viewport.MaxZ = 1.0f;
		viewport_update_constants();
		device.created = TRUE;
		platform_log("Direct3D: the GX device, %dx%d", GXB_SCREEN_WIDTH, GXB_SCREEN_HEIGHT);
	}
	*returned_device = device_pointer();
	return S_OK;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 1024;
	caps->MaxTextureHeight = 1024;
	caps->MaxVolumeExtent = 256;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 1024;
	caps->MaxAnisotropy = 1;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = NV2A_VSH_CONSTANT_COUNT;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	/* like Direct3D, the caller gets a reference it must release */
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

BOOL gx_surface_is_device_owned(const D3DSurface *surface)
{
	return surface && (surface->Data == device.back_buffer.Data || surface->Data == device.depth_buffer.Data);
}

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	/* like Direct3D, reset the viewport to the whole new target */
	if (device.render_target)
	{
		DWORD size = device.render_target->Size;
		DWORD format = device.render_target->Format;

		device.viewport.X = 0;
		device.viewport.Y = 0;
		if (size)
		{
			device.viewport.Width = (size & D3DSIZE_WIDTH_MASK) + 1;
			device.viewport.Height = ((size & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;
		}
		else
		{
			device.viewport.Width = 1UL << ((format & D3DFORMAT_USIZE_MASK) >> D3DFORMAT_USIZE_SHIFT);
			device.viewport.Height = 1UL << ((format & D3DFORMAT_VSIZE_MASK) >> D3DFORMAT_VSIZE_SHIFT);
		}
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

static BOOL drawing_to_screen(void)
{
	return device.render_target && device.render_target->Data == device.back_buffer.Data;
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* ---------- GPU synchronisation: the draws are done when the calls return */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	if (callback)
		callback(context);
}

/* ---------- visibility tests: everything is visible */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	(void)index;
	return S_OK;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	(void)index;
	/* more pixels than any test the game makes covers */
	if (result)
		*result = GXB_SCREEN_WIDTH * GXB_SCREEN_HEIGHT;
	if (time_stamp)
		*time_stamp = 0;
	return S_OK;
}

/* ---------- render and texture stage state */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	/* callers also store the value in D3D__RenderState themselves */
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(ZBias, D3DRS_ZBIAS)
COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	/* the definition's members are the pixel shader render states, kept
	for the combiners' translation to TEV */
	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
}

/* ---------- vertex shaders */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				/* skip: the count is in dwords, or in bytes with bit 27 */
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < NV2A_VSH_ATTRIBUTE_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	if (function)
	{
		unsigned long index;

		object->program = malloc(sizeof(*object->program));
		/* header: program type in the low word, instruction count in the high */
		if (!object->program ||
			!nv2a_vsh_decode((const uint32_t *)(function + 1), function[0] >> 16, object->program))
		{
			platform_log("Direct3D: vertex shader %lu has %lu instructions, more than the NV2A's",
				object->id, (unsigned long)(function[0] >> 16));
			free(object->program);
			object->program = NULL;
		}
	}
	parse_declaration(object, declaration);
	/* odd values are FVF codes; programmable shader handles are even */
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	/* the game deletes its shaders only as it quits; the objects are small */
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	device.vertex_shader = object;
	device.program_address = 0;
	device.program_slots[0] = object;
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object && object->program ? object->program->count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + NV2A_VSH_CONSTANT_BIAS;

	if (first < 0 || first >= NV2A_VSH_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > NV2A_VSH_CONSTANT_COUNT)
		constant_count = NV2A_VSH_CONSTANT_COUNT - first;
	memcpy(device.constants[first], constant_data, constant_count * sizeof(device.constants[0]));
}

/* the program that runs: the one loaded at the selected address, else the
current shader's own; its declaration is always the current shader's */
static const struct nv2a_vsh_program *current_program(void)
{
	struct vertex_shader_object *program = device.program_slots[device.program_address];

	if (!program)
		program = device.vertex_shader;
	return program ? program->program : NULL;
}

/* ---------- per-draw state */

static unsigned char gx_compare(DWORD function)
{
	/* D3DCMP_NEVER (0x200) to D3DCMP_ALWAYS (0x207) are in GX's order */
	return (unsigned char)(function & 7);
}

/* GX_BL_*: 0 zero, 1 one, 2 the other side's color, 3 its inverse, 4 source
alpha, 5 its inverse, 6 destination alpha, 7 its inverse. GX has no source
color as a source factor nor destination color as a destination factor;
those and the constant factors are taken as near as GX can. */
static unsigned char gx_blend_factor(DWORD factor, BOOL source)
{
	switch (factor)
	{
	case D3DBLEND_ZERO: return 0;
	case D3DBLEND_ONE: return 1;
	case D3DBLEND_SRCCOLOR: return source ? 1 : 2;
	case D3DBLEND_INVSRCCOLOR: return source ? 0 : 3;
	case D3DBLEND_SRCALPHA: case D3DBLEND_SRCALPHASAT: return 4;
	case D3DBLEND_INVSRCALPHA: return 5;
	case D3DBLEND_DESTALPHA: return 6;
	case D3DBLEND_INVDESTALPHA: return 7;
	case D3DBLEND_DESTCOLOR: return source ? 2 : 1;
	case D3DBLEND_INVDESTCOLOR: return source ? 3 : 0;
	case D3DBLEND_CONSTANTCOLOR: case D3DBLEND_CONSTANTALPHA: return 1;
	default: return 0;
	}
}

static void apply_raster_state(void)
{
	const DWORD *rs = D3D__RenderState;
	struct gxb_raster_state state;
	DWORD write = rs[D3DRS_COLORWRITEENABLE];

	memset(&state, 0, sizeof(state));
	state.z_test = rs[D3DRS_ZENABLE] != D3DZB_FALSE;
	state.z_write = state.z_test && rs[D3DRS_ZWRITEENABLE];
	state.z_function = gx_compare(rs[D3DRS_ZFUNC]);
	if (rs[D3DRS_ALPHABLENDENABLE])
	{
		if (rs[D3DRS_BLENDOP] == D3DBLENDOP_REVSUBTRACT || rs[D3DRS_BLENDOP] == D3DBLENDOP_REVSUBTRACTSIGNED)
		{
			state.blend_mode = 2;
		}
		else
		{
			state.blend_mode = 1;
			state.blend_source = gx_blend_factor(rs[D3DRS_SRCBLEND], TRUE);
			state.blend_destination = gx_blend_factor(rs[D3DRS_DESTBLEND], FALSE);
		}
	}
	if (rs[D3DRS_ALPHATESTENABLE])
	{
		state.alpha_test = 1;
		state.alpha_function = gx_compare(rs[D3DRS_ALPHAFUNC]);
		state.alpha_reference = (unsigned char)(rs[D3DRS_ALPHAREF] > 255 ? 255 : rs[D3DRS_ALPHAREF]);
	}
	else
	{
		state.alpha_function = 7;
	}
	/* the cull mode names the winding to discard */
	if (rs[D3DRS_CULLMODE] == D3DCULL_CW)
		state.cull = 1;
	else if (rs[D3DRS_CULLMODE] == D3DCULL_CCW)
		state.cull = 2;
	state.color_write = (write & (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE)) != 0;
	state.alpha_write = (write & D3DCOLORWRITEENABLE_ALPHA) != 0;
	gxb_set_raster_state(&state);
}

static unsigned char gx_wrap(DWORD mode)
{
	switch (mode)
	{
	case D3DTADDRESS_WRAP: return 1;   /* GX_REPEAT */
	case D3DTADDRESS_MIRROR: return 2; /* GX_MIRROR */
	default: return 0;                 /* GX_CLAMP */
	}
}

/* ---------- the pixel shader

The game's pixel shader is the combiner state in D3D__RenderState (the PS*
render states, which SetPixelShaderProgram fills), and it is translated
to TEV stages by nv2a_tev.c. A translation depends on the combiner state,
on which textures are sampled, on fog, and on which constant channels are 0
or 255 (nv2a_tev_constant_classes); each is made once and kept. */

#define SHADER_CACHE_SIZE 256
/* the combiner state, the sampled textures and fog, the constant classes */
#define SHADER_KEY_WORDS (sizeof(struct nv2a_combiners) / 4 + 1 + 5)

struct shader_entry
{
	BOOL used, translated;
	uint32_t key[SHADER_KEY_WORDS];
	struct nv2a_tev_program program;
};

static struct shader_entry shader_cache[SHADER_CACHE_SIZE];

static const struct nv2a_tev_program *shader_get(const struct nv2a_combiners *combiners,
	const struct nv2a_tev_options *options)
{
	uint32_t key[SHADER_KEY_WORDS], hash = 2166136261u;
	struct shader_entry *entry;
	unsigned long index, probe;
	const char *failure;

	memset(key, 0, sizeof(key));
	memcpy(key, combiners, sizeof(*combiners));
	key[sizeof(*combiners) / 4] = options->textures_sampled | ((uint32_t)options->fog << 8);
	nv2a_tev_constant_classes(options, key + sizeof(*combiners) / 4 + 1);
	for (index = 0; index < SHADER_KEY_WORDS; index++)
		hash = (hash ^ key[index]) * 16777619u;
	for (probe = 0; probe < 8; probe++)
	{
		entry = &shader_cache[(hash + probe) % SHADER_CACHE_SIZE];
		if (!entry->used || !memcmp(entry->key, key, sizeof(key)))
			break;
	}
	/* (a full neighbourhood: the first is made again) */
	if (probe == 8)
		entry = &shader_cache[hash % SHADER_CACHE_SIZE];
	if (!entry->used || memcmp(entry->key, key, sizeof(key)))
	{
		entry->used = TRUE;
		memcpy(entry->key, key, sizeof(key));
		entry->translated = nv2a_tev_compile(combiners, options, &entry->program, &failure);
		if (!entry->translated)
		{
			platform_log("Direct3D: pixel shader (combiners %08lx, textures %08lx, final %08lx %08lx) not drawn: %s",
				(unsigned long)combiners->combiner_count, (unsigned long)combiners->texture_modes,
				(unsigned long)combiners->final_inputs_abcd, (unsigned long)combiners->final_inputs_efg, failure);
		}
	}
	return entry->translated ? &entry->program : NULL;
}

/* a texture stage's texture, as GX samples it */
static BOOL stage_texture(int stage, struct gxb_texture *texture)
{
	BOOL power_of_two;

	memset(texture, 0, sizeof(*texture));
	if (!device.textures[stage] || !gx_texture_get(device.textures[stage], device.palettes[stage], texture))
	{
		if (device.textures[stage])
			stats.untextured_format++;
		return FALSE;
	}
	power_of_two = !(texture->width & (texture->width - 1)) && !(texture->height & (texture->height - 1));
	/* GX repeats only textures whose sides are powers of two */
	texture->wrap_s = power_of_two ? gx_wrap(D3D__TextureState[stage][D3DTSS_ADDRESSU]) : 0;
	texture->wrap_t = power_of_two ? gx_wrap(D3D__TextureState[stage][D3DTSS_ADDRESSV]) : 0;
	texture->linear = D3D__TextureState[stage][D3DTSS_MAGFILTER] != D3DTEXF_POINT;
	return TRUE;
}

/* the textures the shader samples, and the shader as TEV stages; FALSE
when the shader does not fit TEV (the draw is then skipped) */
static BOOL apply_pixel_shader(void)
{
	const DWORD *rs = D3D__RenderState;
	struct gxb_texture textures[GXB_MAXIMUM_TEXTURES];
	struct nv2a_combiners combiners;
	struct nv2a_tev_options options;
	const struct nv2a_tev_program *program;
	uint8_t konst[4][4], initial_color[4][4], initial_alpha[4][4];
	uint32_t c0[8], c1[8];
	int stage, index;

	memset(&options, 0, sizeof(options));
	for (stage = 0; stage < GXB_MAXIMUM_TEXTURES; stage++)
	{
		if (((rs[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f) && stage_texture(stage, &textures[stage]))
			options.textures_sampled |= (uint8_t)(1 << stage);
		else
			memset(&textures[stage], 0, sizeof(textures[stage]));
	}
	if (options.textures_sampled)
		stats.textured++;
	gxb_set_textures(textures, GXB_MAXIMUM_TEXTURES);

	memset(&combiners, 0, sizeof(combiners));
	for (index = 0; index < 8; index++)
	{
		combiners.rgb_inputs[index] = rs[D3DRS_PSRGBINPUTS0 + index];
		combiners.rgb_outputs[index] = rs[D3DRS_PSRGBOUTPUTS0 + index];
		combiners.alpha_inputs[index] = rs[D3DRS_PSALPHAINPUTS0 + index];
		combiners.alpha_outputs[index] = rs[D3DRS_PSALPHAOUTPUTS0 + index];
		c0[index] = options.c0[index] = rs[D3DRS_PSCONSTANT0_0 + index];
		c1[index] = options.c1[index] = rs[D3DRS_PSCONSTANT1_0 + index];
	}
	combiners.final_inputs_abcd = rs[D3DRS_PSFINALCOMBINERINPUTSABCD];
	combiners.final_inputs_efg = rs[D3DRS_PSFINALCOMBINERINPUTSEFG];
	combiners.combiner_count = rs[D3DRS_PSCOMBINERCOUNT];
	combiners.texture_modes = rs[D3DRS_PSTEXTUREMODES];
	options.final_c0 = rs[D3DRS_PSFINALCOMBINERCONSTANT0];
	options.final_c1 = rs[D3DRS_PSFINALCOMBINERCONSTANT1];
	options.fog = rs[D3DRS_FOGENABLE] != 0;
	program = shader_get(&combiners, &options);
	if (!program)
	{
		stats.skipped_shader++;
		return FALSE;
	}
	memset(initial_color, 0, sizeof(initial_color));
	memset(initial_alpha, 0, sizeof(initial_alpha));
	for (index = 0; index < 4; index++)
	{
		nv2a_tev_constant_value(&program->konst[index], c0, c1, options.final_c0, options.final_c1,
			rs[D3DRS_FOGCOLOR], konst[index]);
		if (program->initial_color[index].source)
		{
			nv2a_tev_constant_value(&program->initial_color[index], c0, c1, options.final_c0, options.final_c1,
				rs[D3DRS_FOGCOLOR], initial_color[index]);
		}
		if (program->initial_alpha[index].source)
		{
			nv2a_tev_constant_value(&program->initial_alpha[index], c0, c1, options.final_c0, options.final_c1,
				rs[D3DRS_FOGCOLOR], initial_alpha[index]);
		}
	}
	gxb_set_program(program, (const uint8_t (*)[4])konst, (const uint8_t (*)[4])initial_color,
		(const uint8_t (*)[4])initial_alpha);
	stats.shaded++;
	return TRUE;
}

/* the fog factor for a vertex's oFog, by the fog table mode (the NV2A
computes it for each pixel; here it is for each vertex) */
static float fog_factor(float fog)
{
	const DWORD *rs = D3D__RenderState;
	float density, start, end, factor;

	memcpy(&density, &rs[D3DRS_FOGDENSITY], sizeof(density));
	memcpy(&start, &rs[D3DRS_FOGSTART], sizeof(start));
	memcpy(&end, &rs[D3DRS_FOGEND], sizeof(end));
	switch (rs[D3DRS_FOGTABLEMODE])
	{
	case D3DFOG_EXP: factor = expf(-density * fog); break;
	case D3DFOG_EXP2: factor = expf(-(density * fog) * (density * fog)); break;
	case D3DFOG_LINEAR: factor = (end - fog) / (end - start > 1.0e-6f ? end - start : 1.0e-6f); break;
	default: factor = fog; break;
	}
	return factor < 0.0f ? 0.0f : factor > 1.0f ? 1.0f : factor;
}

/* ---------- vertices */

static void reserve_vertices(unsigned long count)
{
	if (count <= device.vertex_capacity)
		return;
	device.vertex_capacity = count + count / 2 + 64;
	free(device.vertices);
	free(device.clips);
	device.vertices = malloc(device.vertex_capacity * sizeof(*device.vertices));
	device.clips = malloc(device.vertex_capacity * sizeof(*device.clips));
}

static void reserve_indices(unsigned long count)
{
	if (count <= device.index_capacity)
		return;
	device.index_capacity = count + count / 2 + 64;
	free(device.indices);
	device.indices = malloc(device.index_capacity * sizeof(*device.indices));
}

/* one attribute of a vertex, as the NV2A reads it: in the Wii's byte
order, which is how the map converter leaves the maps' vertices and how the
game writes its own */
static void read_attribute(const struct vertex_element *element, const unsigned char *data, float *out)
{
	int index, count;

	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	switch (element->type)
	{
	case D3DVSDT_FLOAT1: case D3DVSDT_FLOAT2: case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT4: case D3DVSDT_FLOAT2H:
		count = element->bytes / 4;
		memcpy(out, data, count * sizeof(float));
		break;
	case D3DVSDT_D3DCOLOR:
	{
		uint32_t color;

		memcpy(&color, data, sizeof(color));
		color_to_vec4(color, out);
		break;
	}
	case D3DVSDT_SHORT1: case D3DVSDT_SHORT2: case D3DVSDT_SHORT3: case D3DVSDT_SHORT4:
	case D3DVSDT_NORMSHORT1: case D3DVSDT_NORMSHORT2: case D3DVSDT_NORMSHORT3: case D3DVSDT_NORMSHORT4:
	{
		BOOL normalized = (element->type & 0x0f) == 0x01;

		count = element->bytes / 2;
		for (index = 0; index < count; index++)
		{
			int16_t value;

			memcpy(&value, data + index * 2, sizeof(value));
			out[index] = normalized ? (value < -32767 ? -1.0f : value / 32767.0f) : (float)value;
		}
		break;
	}
	case D3DVSDT_NORMPACKED3:
	{
		uint32_t packed;

		memcpy(&packed, data, sizeof(packed));
		nv2a_unpack_normpacked3(packed, out);
		break;
	}
	case D3DVSDT_PBYTE1: case D3DVSDT_PBYTE2: case D3DVSDT_PBYTE3: case D3DVSDT_PBYTE4:
		for (index = 0; index < element->bytes; index++)
			out[index] = data[index] / 255.0f;
		break;
	default:
		break;
	}
}

/* the clip-space position, in Direct3D's terms, of what a program wrote:
the program's screen-space conversion undone, as the Linux port's GLSL
does it (nv2a_vsh.c), from the position it converted where that was kept */
static void clip_position(const struct nv2a_vsh_result *result, float clip[4])
{
	const float *c38 = device.constants[NV2A_VSH_CONSTANT_BIAS - 38];
	const float *c37 = device.constants[NV2A_VSH_CONSTANT_BIAS - 37];
	float scale[3];
	int axis;

	for (axis = 0; axis < 3; axis++)
		scale[axis] = device.viewport_scale[axis] != 0.0f ? device.viewport_scale[axis] : 1.0f;
	/* Direct3D 8 puts pixel centres on integer screen coordinates, GX (as
	OpenGL) on half-integers */
	if (result->clip_captured)
	{
		float w = result->clip[3];

		clip[0] = (result->clip[0] * c38[0] + (c37[0] + 0.5f - device.viewport_offset[0]) * w) / scale[0];
		clip[1] = (result->clip[1] * c38[1] + (c37[1] + 0.5f - device.viewport_offset[1]) * w) / scale[1];
		clip[2] = (result->clip[2] * c38[2] + (c37[2] - device.viewport_offset[2]) * w) / scale[2];
		clip[3] = w;
	}
	else
	{
		float w = result->position[3];

		clip[0] = (result->position[0] + 0.5f - device.viewport_offset[0]) / scale[0] * w;
		clip[1] = (result->position[1] + 0.5f - device.viewport_offset[1]) / scale[1] * w;
		clip[2] = (result->position[2] - device.viewport_offset[2]) / scale[2] * w;
		clip[3] = w;
	}
	/* a w of zero or not a number: behind the camera, which the clipper
	takes, as the Xbox's divide sent it to infinity */
	if (!(fabsf(clip[3]) > 0.0f))
	{
		clip[0] = clip[1] = clip[2] = 0.0f;
		clip[3] = -1.0f;
	}
}

/* the bytes of a color, red in the top byte, from a program's output */
static uint32_t output_color(const float *value)
{
	return ((uint32_t)unit_to_byte(value[0]) << 24) | ((uint32_t)unit_to_byte(value[1]) << 16) |
		((uint32_t)unit_to_byte(value[2]) << 8) | unit_to_byte(value[3]);
}

/* runs the program for one vertex's inputs, into the draw's vertex and
clip position */
static void transform_vertex(const struct nv2a_vsh_program *program, const float (*inputs)[4],
	struct gxb_vertex *vertex, float clip[4])
{
	struct nv2a_vsh_result result;
	int index;

	nv2a_vsh_run(program, (const float (*)[4])device.constants, inputs, &result);
	clip_position(&result, clip);
	vertex->color = output_color(result.diffuse);
	vertex->specular = output_color(result.specular);
	vertex->fog = D3D__RenderState[D3DRS_FOGENABLE] ? fog_factor(result.fog) : 1.0f;
	for (index = 0; index < GXB_MAXIMUM_TEXTURES; index++)
	{
		vertex->texcoords[index][0] = result.texcoords[index][0];
		vertex->texcoords[index][1] = result.texcoords[index][1];
	}
}

/* runs the program for vertices [first, first + count) of the streams */
static void transform_streams(unsigned long first, unsigned long count)
{
	struct vertex_shader_object *declaration = device.vertex_shader;
	const struct nv2a_vsh_program *program = current_program();
	float inputs[NV2A_VSH_ATTRIBUTE_COUNT][4];
	unsigned long vertex, element;

	reserve_vertices(count);
	for (vertex = 0; vertex < count; vertex++)
	{
		memcpy(inputs, device.attributes, sizeof(inputs));
		for (element = 0; element < declaration->element_count; element++)
		{
			const struct vertex_element *source = &declaration->elements[element];
			DWORD data = device.streams[source->stream].data;

			if (!data || source->reg >= NV2A_VSH_ATTRIBUTE_COUNT)
				continue;
			read_attribute(source, (const unsigned char *)data +
				(first + vertex) * device.streams[source->stream].stride + source->offset, inputs[source->reg]);
		}
		transform_vertex(program, (const float (*)[4])inputs, &device.vertices[vertex],
			device.clips[vertex]);
	}
}

/* the projection the draw's vertices are given to GX with, and their
positions for it (struct gxb_projection) */
static void fit_projection(unsigned long count, struct gxb_projection *projection)
{
	unsigned long index, lowest = 0, highest = 0;
	float lowest_w = 0.0f, highest_w = 0.0f;
	BOOL any = FALSE, fits = FALSE;
	float a = 0.0f, b = 0.0f;

	for (index = 0; index < count; index++)
	{
		float w = device.clips[index][3];

		if (!(w > 0.0f))
			continue;
		if (!any || w < lowest_w)
		{
			lowest_w = w;
			lowest = index;
		}
		if (!any || w > highest_w)
		{
			highest_w = w;
			highest = index;
		}
		any = TRUE;
	}
	if (any && highest_w - lowest_w > highest_w * 1.0e-5f)
	{
		/* z = a * w + b, through the nearest and the farthest vertex */
		a = (device.clips[highest][2] - device.clips[lowest][2]) / (highest_w - lowest_w);
		b = device.clips[lowest][2] - a * lowest_w;
		fits = TRUE;
		for (index = 0; index < count && fits; index++)
		{
			const float *clip = device.clips[index];
			float error = clip[2] - (a * clip[3] + b);

			if (fabsf(error) > 1.0e-3f * (fabsf(clip[2]) + fabsf(clip[3])) + 1.0e-6f)
				fits = FALSE;
		}
	}
	if (fits)
	{
		projection->perspective = 1;
		projection->p22 = 1.0f - a;
		projection->p23 = b;
		for (index = 0; index < count; index++)
		{
			device.vertices[index].position[0] = device.clips[index][0];
			device.vertices[index].position[1] = device.clips[index][1];
			device.vertices[index].position[2] = -device.clips[index][3];
		}
		stats.perspective++;
		return;
	}
	/* one w for all (2D, or an orthographic projection), or no projection
	fits: divided by w. A vertex behind the camera is put past the far
	plane, which clips its triangle as the NV2A would have */
	projection->perspective = 0;
	projection->p22 = projection->p23 = 0.0f;
	for (index = 0; index < count; index++)
	{
		const float *clip = device.clips[index];

		if (clip[3] > 0.0f)
		{
			float inverse = 1.0f / clip[3];

			device.vertices[index].position[0] = clip[0] * inverse;
			device.vertices[index].position[1] = clip[1] * inverse;
			device.vertices[index].position[2] = clip[2] * inverse;
		}
		else
		{
			device.vertices[index].position[0] = 0.0f;
			device.vertices[index].position[1] = 0.0f;
			device.vertices[index].position[2] = 2.0f;
		}
	}
	if (any && highest_w - lowest_w > highest_w * 1.0e-5f)
		stats.divided++;
	else
		stats.orthographic++;
}

static enum gxb_primitive gx_primitive(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return _gxb_points;
	case D3DPT_LINELIST: return _gxb_lines;
	case D3DPT_LINELOOP: case D3DPT_LINESTRIP: return _gxb_line_strip;
	case D3DPT_TRIANGLESTRIP: case D3DPT_QUADSTRIP: return _gxb_triangle_strip;
	case D3DPT_TRIANGLEFAN: case D3DPT_POLYGON: return _gxb_triangle_fan;
	case D3DPT_QUADLIST: return _gxb_quads;
	default: return _gxb_triangles;
	}
}

/* whether a draw can be made, with the state applied for it */
static BOOL draw_prepare(void)
{
	if (!device.created || !gxb_ready())
		return FALSE;
	if (!drawing_to_screen())
	{
		stats.skipped_target++;
		return FALSE;
	}
	if (!device.vertex_shader || !current_program())
	{
		stats.skipped_program++;
		return FALSE;
	}
	apply_raster_state();
	return apply_pixel_shader();
}

/* the draw's vertices [0, count) to GX, in order, or by the indices */
static void draw_transformed(D3DPRIMITIVETYPE type, unsigned long vertex_count, const uint16_t *indices,
	unsigned long count)
{
	struct gxb_projection projection;

	fit_projection(vertex_count, &projection);
	if (type == D3DPT_LINELOOP)
	{
		/* closed by its first vertex again */
		unsigned long index;

		reserve_indices(count + 1);
		for (index = 0; index < count; index++)
			device.indices[index] = indices ? indices[index] : (uint16_t)index;
		device.indices[count] = device.indices[0];
		indices = device.indices;
		count++;
	}
	gxb_draw(gx_primitive(type), &projection, device.vertices, indices, count);
	stats.draws++;
	stats.vertices += vertex_count;
}

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	D3D__IndexData = index_data ? (WORD *)index_data->Data : NULL;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	if (!vertex_count || !draw_prepare())
		return;
	transform_streams(start_vertex, vertex_count);
	draw_transformed(primitive_type, vertex_count, NULL, vertex_count);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	unsigned long minimum = 0xffff, maximum = 0, index;

	if (!vertex_count || !index_data || !draw_prepare())
		return;
	for (index = 0; index < vertex_count; index++)
	{
		if (index_data[index] < minimum)
			minimum = index_data[index];
		if (index_data[index] > maximum)
			maximum = index_data[index];
	}
	/* (the streams from the base vertex on: index i is vertex base + i) */
	transform_streams(device.base_vertex_index + minimum, maximum - minimum + 1);
	reserve_indices(vertex_count + 1);
	for (index = 0; index < vertex_count; index++)
		device.indices[index] = (uint16_t)(index_data[index] - minimum);
	draw_transformed(primitive_type, maximum - minimum + 1, device.indices, vertex_count);
}

/* ---------- immediate mode */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate_active = TRUE;
	device.immediate_type = primitive_type;
	device.immediate_count = 0;
}

static void immediate_emit(void)
{
	unsigned long floats = NV2A_VSH_ATTRIBUTE_COUNT * 4;

	if (device.immediate_count == device.immediate_capacity)
	{
		device.immediate_capacity = device.immediate_capacity ? device.immediate_capacity * 2 : 256;
		device.immediate_vertices = realloc(device.immediate_vertices,
			device.immediate_capacity * floats * sizeof(float));
	}
	memcpy(device.immediate_vertices + device.immediate_count * floats, device.attributes, floats * sizeof(float));
	device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	unsigned long floats = NV2A_VSH_ATTRIBUTE_COUNT * 4;
	unsigned long index, count = device.immediate_count;
	const struct nv2a_vsh_program *program;

	device.immediate_active = FALSE;
	if (!count || !draw_prepare())
		return;
	program = current_program();
	reserve_vertices(count);
	for (index = 0; index < count; index++)
	{
		transform_vertex(program, (const float (*)[4])(device.immediate_vertices + index * floats),
			&device.vertices[index], device.clips[index]);
	}
	draw_transformed(device.immediate_type, count, NULL, count);
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= NV2A_VSH_ATTRIBUTE_COUNT)
		return;
	device.attributes[reg][0] = a;
	device.attributes[reg][1] = b;
	device.attributes[reg][2] = c;
	device.attributes[reg][3] = d;
	/* like the hardware, writing register 0 completes a vertex */
	if (device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}

/* ---------- clearing */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	long left = (long)device.viewport.X, top = (long)device.viewport.Y;
	long right = left + (long)device.viewport.Width, bottom = top + (long)device.viewport.Height;
	BOOL clear_color = (flags & (D3DCLEAR_TARGET_R | D3DCLEAR_TARGET_G | D3DCLEAR_TARGET_B)) != 0;
	BOOL clear_alpha = (flags & D3DCLEAR_TARGET_A) != 0;
	BOOL clear_depth = (flags & D3DCLEAR_ZBUFFER) != 0 && device.depth_stencil;
	DWORD index;

	(void)stencil;
	if (!device.created || !gxb_ready())
		return;
	if (!drawing_to_screen())
	{
		stats.skipped_target++;
		return;
	}
	stats.clears++;
	/* the NV2A clips a clear to the viewport, which is what keeps a
	split-screen window's clear from wiping the other window */
	if (!count || !rectangles)
	{
		gxb_clear(left, top, right, bottom, clear_color, clear_alpha, clear_depth, argb_to_rgba(color), z);
		return;
	}
	for (index = 0; index < count; index++)
	{
		long x0 = rectangles[index].x1 > left ? rectangles[index].x1 : left;
		long y0 = rectangles[index].y1 > top ? rectangles[index].y1 : top;
		long x1 = rectangles[index].x2 < right ? rectangles[index].x2 : right;
		long y1 = rectangles[index].y2 < bottom ? rectangles[index].y2 : bottom;

		gxb_clear(x0, y0, x1, y1, clear_color, clear_alpha, clear_depth, argb_to_rgba(color), z);
	}
}

/* ---------- presentation */

static void log_frame(void)
{
	if (device.frame < 4 || device.frame % 600 == 0)
	{
		platform_log("Direct3D: frame %lu: %lu draws (%lu vertices: %lu perspective, %lu flat, %lu divided), "
			"%lu clears, %lu textured, %lu shaded; skipped %lu off-screen, %lu without a program, %lu whose pixel "
			"shader does not fit TEV, %lu textures",
			device.frame, stats.draws, stats.vertices, stats.perspective, stats.orthographic, stats.divided,
			stats.clears, stats.textured, stats.shaded, stats.skipped_target, stats.skipped_program,
			stats.skipped_shader, stats.untextured_format);
	}
	memset(&stats, 0, sizeof(stats));
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	if (!device.created)
		return;
	pending_flips++;
	gxb_present();
	log_frame();
	device.frame++;
	gx_texture_cache_begin_frame();
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}
