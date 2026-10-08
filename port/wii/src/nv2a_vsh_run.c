/*
NV2A_VSH_RUN.C

The Xbox's vertex programs, interpreted on the CPU (nv2a_vsh_run.h). The
semantics are those of the Linux port's GLSL translation (nv2a_vsh.c), which
draws every one of the game's 67 programs correctly: the MAC and ILU units
read the same three operands, results are written only after both have
read, and when both units run the ILU's result goes to r1.
*/

#include "nv2a_vsh_run.h"

#include <math.h>
#include <string.h>

enum
{
	_mac_nop, _mac_mov, _mac_mul, _mac_add, _mac_mad, _mac_dp3, _mac_dph, _mac_dp4,
	_mac_dst, _mac_min, _mac_max, _mac_slt, _mac_sge, _mac_arl,
};

enum
{
	_ilu_nop, _ilu_mov, _ilu_rcp, _ilu_rcc, _ilu_rsq, _ilu_exp, _ilu_log, _ilu_lit,
};

enum
{
	_mux_temporary = 1, _mux_input, _mux_constant,
};

/* output register addresses (o[]) */
enum
{
	_output_position = 0,
	_output_diffuse = 3,
	_output_specular = 4,
	_output_fog = 5,
	_output_point_size = 6,
	_output_back_diffuse = 7,
	_output_back_specular = 8,
	_output_texcoord0 = 9,
};

static uint32_t field(const uint32_t *instruction, int word, int low_bit, int bit_count)
{
	return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

static void decode_operand(const uint32_t *instruction, int which, struct nv2a_vsh_operand *operand)
{
	switch (which)
	{
	case 0: /* A */
		operand->negate = field(instruction, 1, 8, 1);
		operand->swizzle[0] = field(instruction, 1, 6, 2);
		operand->swizzle[1] = field(instruction, 1, 4, 2);
		operand->swizzle[2] = field(instruction, 1, 2, 2);
		operand->swizzle[3] = field(instruction, 1, 0, 2);
		operand->index = field(instruction, 2, 28, 4);
		operand->mux = field(instruction, 2, 26, 2);
		break;
	case 1: /* B */
		operand->negate = field(instruction, 2, 25, 1);
		operand->swizzle[0] = field(instruction, 2, 23, 2);
		operand->swizzle[1] = field(instruction, 2, 21, 2);
		operand->swizzle[2] = field(instruction, 2, 19, 2);
		operand->swizzle[3] = field(instruction, 2, 17, 2);
		operand->index = field(instruction, 2, 13, 4);
		operand->mux = field(instruction, 2, 11, 2);
		break;
	default: /* C */
		operand->negate = field(instruction, 2, 10, 1);
		operand->swizzle[0] = field(instruction, 2, 8, 2);
		operand->swizzle[1] = field(instruction, 2, 6, 2);
		operand->swizzle[2] = field(instruction, 2, 4, 2);
		operand->swizzle[3] = field(instruction, 2, 2, 2);
		operand->index = (field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2);
		operand->mux = field(instruction, 3, 28, 2);
		break;
	}
}

int nv2a_vsh_decode(const uint32_t *words, unsigned long count, struct nv2a_vsh_program *program)
{
	unsigned long index;

	memset(program, 0, sizeof(*program));
	if (count > NV2A_VSH_MAXIMUM_INSTRUCTIONS)
		return 0;
	for (index = 0; index < count; index++)
	{
		const uint32_t *word = words + index * 4;
		struct nv2a_vsh_instruction *instruction = &program->instructions[index];
		int which;

		instruction->mac = field(word, 1, 21, 4);
		instruction->ilu = field(word, 1, 25, 3);
		instruction->constant = field(word, 1, 13, 8);
		instruction->input = field(word, 1, 9, 4);
		instruction->mac_mask = field(word, 3, 24, 4);
		instruction->temporary = field(word, 3, 20, 4);
		instruction->ilu_mask = field(word, 3, 16, 4);
		instruction->output_mask = field(word, 3, 12, 4);
		instruction->output_is_register = field(word, 3, 11, 1);
		instruction->output_address = field(word, 3, 3, 8);
		instruction->output_from_ilu = field(word, 3, 2, 1);
		instruction->relative = field(word, 3, 1, 1);
		for (which = 0; which < 3; which++)
			decode_operand(word, which, &instruction->operands[which]);
		instruction->captures_clip = instruction->ilu == _ilu_rcc &&
			instruction->operands[2].mux == _mux_temporary && instruction->operands[2].index == 12;
		program->count = index + 1;
		if (field(word, 3, 0, 1))
			break;
	}
	return 1;
}

/* the fetch's scales, multiplied by rather than divided by: the Wii's
CPU takes 17 cycles over a division */
#define INVERSE_255 (1.0f / 255.0f)
#define INVERSE_511 (1.0f / 511.0f)
#define INVERSE_1023 (1.0f / 1023.0f)
#define INVERSE_32767 (1.0f / 32767.0f)

void nv2a_unpack_normpacked3(uint32_t packed, float out[4])
{
	int32_t x = (int32_t)(packed << 21) >> 21;
	int32_t y = (int32_t)(packed << 10) >> 21;
	int32_t z = (int32_t)packed >> 22;

	out[0] = (float)x * INVERSE_1023;
	out[1] = (float)y * INVERSE_1023;
	out[2] = (float)z * INVERSE_511;
	out[3] = 1.0f;
}

unsigned long nv2a_vsh_type_bytes(unsigned type)
{
	switch (type)
	{
	case NV2A_VSDT_FLOAT1: return 4;
	case NV2A_VSDT_FLOAT2: return 8;
	case NV2A_VSDT_FLOAT3: return 12;
	case NV2A_VSDT_FLOAT4: return 16;
	case NV2A_VSDT_FLOAT2H: return 12;
	case NV2A_VSDT_D3DCOLOR: return 4;
	case NV2A_VSDT_SHORT1: case NV2A_VSDT_NORMSHORT1: return 2;
	case NV2A_VSDT_SHORT2: case NV2A_VSDT_NORMSHORT2: return 4;
	case NV2A_VSDT_SHORT3: case NV2A_VSDT_NORMSHORT3: return 6;
	case NV2A_VSDT_SHORT4: case NV2A_VSDT_NORMSHORT4: return 8;
	case NV2A_VSDT_NORMPACKED3: return 4;
	case NV2A_VSDT_PBYTE1: return 1;
	case NV2A_VSDT_PBYTE2: return 2;
	case NV2A_VSDT_PBYTE3: return 3;
	case NV2A_VSDT_PBYTE4: return 4;
	default: return 0;
	}
}

void nv2a_vsh_fetch(unsigned type, const unsigned char *data, float out[4])
{
	unsigned long bytes = nv2a_vsh_type_bytes(type);
	unsigned long index;

	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	switch (type)
	{
	case NV2A_VSDT_FLOAT1: case NV2A_VSDT_FLOAT2: case NV2A_VSDT_FLOAT3: case NV2A_VSDT_FLOAT4:
	case NV2A_VSDT_FLOAT2H:
		memcpy(out, data, bytes);
		break;
	case NV2A_VSDT_D3DCOLOR:
	{
		uint32_t color;

		/* A8R8G8B8 in a word: the registers' x is red */
		memcpy(&color, data, sizeof(color));
		out[0] = ((color >> 16) & 0xff) * INVERSE_255;
		out[1] = ((color >> 8) & 0xff) * INVERSE_255;
		out[2] = (color & 0xff) * INVERSE_255;
		out[3] = (color >> 24) * INVERSE_255;
		break;
	}
	case NV2A_VSDT_SHORT1: case NV2A_VSDT_SHORT2: case NV2A_VSDT_SHORT3: case NV2A_VSDT_SHORT4:
	case NV2A_VSDT_NORMSHORT1: case NV2A_VSDT_NORMSHORT2: case NV2A_VSDT_NORMSHORT3: case NV2A_VSDT_NORMSHORT4:
	{
		int normalized = (type & 0x0f) == 0x01;

		for (index = 0; index < bytes / 2; index++)
		{
			int16_t value;

			memcpy(&value, data + index * 2, sizeof(value));
			out[index] = normalized ? (value < -32767 ? -1.0f : value * INVERSE_32767) : (float)value;
		}
		break;
	}
	case NV2A_VSDT_NORMPACKED3:
	{
		uint32_t packed;

		memcpy(&packed, data, sizeof(packed));
		nv2a_unpack_normpacked3(packed, out);
		break;
	}
	case NV2A_VSDT_PBYTE1: case NV2A_VSDT_PBYTE2: case NV2A_VSDT_PBYTE3: case NV2A_VSDT_PBYTE4:
		for (index = 0; index < bytes; index++)
			out[index] = data[index] * INVERSE_255;
		break;
	default:
		break;
	}
}

/* the clip position from a program's position (screen space) and, where
it kept it, the clip position it converted */
static void clip_inverse(const float viewport_scale[3], float inverse[3])
{
	int axis;

	for (axis = 0; axis < 3; axis++)
		inverse[axis] = 1.0f / (viewport_scale[axis] != 0.0f ? viewport_scale[axis] : 1.0f);
}

static inline void clip_position(const float position[4], const float *kept, const float c38[4], const float c37[4],
	const float inverse[3], const float viewport_offset[3], float clip[4])
{
	/* Direct3D 8 puts pixel centres on integer screen coordinates, GX (as
	OpenGL) on half-integers */
	if (kept)
	{
		float w = kept[3];

		clip[0] = (kept[0] * c38[0] + (c37[0] + 0.5f - viewport_offset[0]) * w) * inverse[0];
		clip[1] = (kept[1] * c38[1] + (c37[1] + 0.5f - viewport_offset[1]) * w) * inverse[1];
		clip[2] = (kept[2] * c38[2] + (c37[2] - viewport_offset[2]) * w) * inverse[2];
		clip[3] = w;
	}
	else
	{
		float w = position[3];

		clip[0] = (position[0] + 0.5f - viewport_offset[0]) * inverse[0] * w;
		clip[1] = (position[1] + 0.5f - viewport_offset[1]) * inverse[1] * w;
		clip[2] = (position[2] - viewport_offset[2]) * inverse[2] * w;
		clip[3] = w;
	}
	if (!(fabsf(clip[3]) > 0.0f))
	{
		clip[0] = clip[1] = clip[2] = 0.0f;
		clip[3] = -1.0f;
	}
}

void nv2a_vsh_clip_position(const struct nv2a_vsh_result *result, const float c38[4], const float c37[4],
	const float viewport_scale[3], const float viewport_offset[3], float clip[4])
{
	float inverse[3];

	clip_inverse(viewport_scale, inverse);
	clip_position(result->position, result->clip_captured ? result->clip : NULL, c38, c37, inverse,
		viewport_offset, clip);
}

static void write_masked(float *destination, const float *value, unsigned mask)
{
	if (mask & 8) destination[0] = value[0];
	if (mask & 4) destination[1] = value[1];
	if (mask & 2) destination[2] = value[2];
	if (mask & 1) destination[3] = value[3];
}

static void splat(float *out, float value)
{
	out[0] = out[1] = out[2] = out[3] = value;
}

/* floorf, without the call: (int) truncates towards zero; numbers this
large are whole already, and a NaN stays a NaN */
static inline float nv2a_floor(float x)
{
	float whole;

	if (!(fabsf(x) < 8388608.0f))
		return x;
	whole = (float)(int)x;
	return whole > x ? whole - 1.0f : whole;
}

/* the reciprocal square root of |x|. The Wii's CPU has no square root
instruction (sqrtf is a library routine there); its reciprocal square root
estimate, made exact by three Newton steps in double, is much quicker */
static inline float nv2a_rsq(float x)
{
#if defined(GEKKO)
	double a = fabs((double)x), y;

	if (!(a > 0.0) || a > 3.4e38)
		return 1.0f / sqrtf(fabsf(x));
	__asm__("frsqrte %0,%1" : "=f"(y) : "f"(a));
	y = y * (1.5 - 0.5 * a * y * y);
	y = y * (1.5 - 0.5 * a * y * y);
	y = y * (1.5 - 0.5 * a * y * y);
	return (float)y;
#else
	return 1.0f / sqrtf(fabsf(x));
#endif
}

static float rcc(float x)
{
	float r = 1.0f / x;

	if (r > 0.0f)
		r = r < 5.42101e-20f ? 5.42101e-20f : r > 1.884467e+19f ? 1.884467e+19f : r;
	else
		r = r < -1.884467e+19f ? -1.884467e+19f : r > -5.42101e-20f ? -5.42101e-20f : r;
	return r;
}

void nv2a_vsh_run(const struct nv2a_vsh_program *program, const float (*constants)[4],
	const float (*inputs)[4], struct nv2a_vsh_result *result)
{
	/* r0-r11, and [12] the position output, which r12 reads back */
	float r[13][4];
	float outputs[16][4];
	int a0 = 0;
	unsigned long index;

	memset(r, 0, sizeof(r));
	memset(outputs, 0, sizeof(outputs));
	r[12][3] = 1.0f;
	for (index = 3; index < 16; index++)
		outputs[index][3] = 1.0f;
	splat(outputs[_output_fog], 1.0f);
	splat(outputs[_output_point_size], 1.0f);
	result->clip_captured = 0;

	for (index = 0; index < program->count; index++)
	{
		const struct nv2a_vsh_instruction *instruction = &program->instructions[index];
		float operand[3][4], mac[4], ilu[4];
		int which, component;

		for (which = 0; which < 3; which++)
		{
			const struct nv2a_vsh_operand *source = &instruction->operands[which];
			const float *value;
			static const float zero[4];

			switch (source->mux)
			{
			case _mux_temporary:
				value = r[source->index <= 12 ? source->index : 0];
				break;
			case _mux_input:
				value = inputs[instruction->input];
				break;
			case _mux_constant:
				if (instruction->relative)
				{
					int address = a0 + instruction->constant;

					address = address < 0 ? 0 : address >= NV2A_VSH_CONSTANT_COUNT ?
						NV2A_VSH_CONSTANT_COUNT - 1 : address;
					value = constants[address];
				}
				else
				{
					value = constants[instruction->constant];
				}
				break;
			default:
				value = zero;
				break;
			}
			for (component = 0; component < 4; component++)
			{
				float x = value[source->swizzle[component]];

				operand[which][component] = source->negate ? -x : x;
			}
		}

#define A operand[0]
#define B operand[1]
#define C operand[2]
		switch (instruction->mac)
		{
		case _mac_mov: case _mac_arl:
			memcpy(mac, A, sizeof(mac));
			break;
		case _mac_mul:
			for (component = 0; component < 4; component++) mac[component] = A[component] * B[component];
			break;
		case _mac_add:
			for (component = 0; component < 4; component++) mac[component] = A[component] + C[component];
			break;
		case _mac_mad:
			for (component = 0; component < 4; component++) mac[component] = A[component] * B[component] + C[component];
			break;
		case _mac_dp3:
			splat(mac, A[0] * B[0] + A[1] * B[1] + A[2] * B[2]);
			break;
		case _mac_dph:
			splat(mac, A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + B[3]);
			break;
		case _mac_dp4:
			splat(mac, A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3]);
			break;
		case _mac_dst:
			mac[0] = 1.0f; mac[1] = A[1] * B[1]; mac[2] = A[2]; mac[3] = B[3];
			break;
		case _mac_min:
			for (component = 0; component < 4; component++) mac[component] = A[component] < B[component] ? A[component] : B[component];
			break;
		case _mac_max:
			for (component = 0; component < 4; component++) mac[component] = A[component] > B[component] ? A[component] : B[component];
			break;
		case _mac_slt:
			for (component = 0; component < 4; component++) mac[component] = A[component] < B[component] ? 1.0f : 0.0f;
			break;
		case _mac_sge:
			for (component = 0; component < 4; component++) mac[component] = A[component] >= B[component] ? 1.0f : 0.0f;
			break;
		default:
			splat(mac, 0.0f);
			break;
		}
		switch (instruction->ilu)
		{
		case _ilu_mov:
			memcpy(ilu, C, sizeof(ilu));
			break;
		case _ilu_rcp:
			splat(ilu, 1.0f / C[0]);
			break;
		case _ilu_rcc:
			splat(ilu, rcc(C[0]));
			break;
		case _ilu_rsq:
			splat(ilu, nv2a_rsq(C[0]));
			break;
		case _ilu_exp:
			ilu[0] = exp2f(nv2a_floor(C[0]));
			ilu[1] = C[0] - nv2a_floor(C[0]);
			ilu[2] = exp2f(C[0]);
			ilu[3] = 1.0f;
			break;
		case _ilu_log:
		{
			float x = fabsf(C[0]);

			if (x == 0.0f)
			{
				ilu[0] = -1.0e30f; ilu[1] = 1.0f; ilu[2] = -1.0e30f; ilu[3] = 1.0f;
			}
			else
			{
				float e = nv2a_floor(log2f(x));

				ilu[0] = e; ilu[1] = x / exp2f(e); ilu[2] = log2f(x); ilu[3] = 1.0f;
			}
			break;
		}
		case _ilu_lit:
		{
			float exponent = C[3] < -127.9961f ? -127.9961f : C[3] > 127.9961f ? 127.9961f : C[3];

			ilu[0] = 1.0f;
			ilu[1] = C[0] > 0.0f ? C[0] : 0.0f;
			ilu[2] = C[0] > 0.0f ? powf(C[1] > 0.0f ? C[1] : 0.0f, exponent) : 0.0f;
			ilu[3] = 1.0f;
			break;
		}
		default:
			splat(ilu, 0.0f);
			break;
		}
#undef A
#undef B
#undef C

		if (instruction->captures_clip)
		{
			memcpy(result->clip, r[12], sizeof(result->clip));
			result->clip_captured = 1;
		}

		/* results are written only after both units have read their inputs */
		if (instruction->mac == _mac_arl)
			a0 = (int)nv2a_floor(mac[0] + 0.001f);
		else if (instruction->mac != _mac_nop && instruction->mac_mask)
			write_masked(r[instruction->temporary <= 12 ? instruction->temporary : 0], mac, instruction->mac_mask);
		if (instruction->ilu != _ilu_nop && instruction->ilu_mask)
		{
			unsigned temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;

			write_masked(r[temporary <= 12 ? temporary : 0], ilu, instruction->ilu_mask);
		}
		if (instruction->output_mask && instruction->output_is_register &&
			(instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0)
		{
			unsigned address = instruction->output_address;

			if (address == _output_position)
				write_masked(r[12], instruction->output_from_ilu ? ilu : mac, instruction->output_mask);
			else if (address < 16)
				write_masked(outputs[address], instruction->output_from_ilu ? ilu : mac, instruction->output_mask);
		}
	}

	memcpy(result->position, r[12], sizeof(result->position));
	memcpy(result->diffuse, outputs[_output_diffuse], sizeof(result->diffuse));
	memcpy(result->specular, outputs[_output_specular], sizeof(result->specular));
	memcpy(result->back_diffuse, outputs[_output_back_diffuse], sizeof(result->back_diffuse));
	memcpy(result->back_specular, outputs[_output_back_specular], sizeof(result->back_specular));
	result->fog = outputs[_output_fog][0];
	result->point_size = outputs[_output_point_size][0];
	memcpy(result->texcoords, outputs[_output_texcoord0], sizeof(result->texcoords));
}

/* ---------- the batch executor */

#define ROW_OFFSET(row, component) ((((row) * 4) + (component)) * NV2A_VSH_BATCH)
#define CONSTANT_OFFSET(slot) (NV2A_VSH_FILE_COUNT * 4 * NV2A_VSH_BATCH + (slot) * NV2A_VSH_BATCH)

/* the components (bit 3 x ... bit 0 w) an operand's swizzle reads for the
components of the result in used */
static unsigned swizzle_reads(const uint8_t swizzle[4], unsigned used)
{
	unsigned reads = 0, component;

	for (component = 0; component < 4; component++)
	{
		if (used & (8 >> component))
			reads |= 8 >> swizzle[component];
	}
	return reads;
}

static int operand_row(const struct nv2a_vsh_instruction *instruction, int which)
{
	const struct nv2a_vsh_operand *operand = &instruction->operands[which];

	if (operand->mux == _mux_temporary)
		return NV2A_VSH_FILE_TEMPORARIES + (operand->index <= 12 ? operand->index : 0);
	if (operand->mux == _mux_input)
		return NV2A_VSH_FILE_INPUTS + instruction->input;
	return -1;
}

/* the components of each operand (A, B, C), as the units take it (after
its swizzle), that they use for the components of their results in mac_used
and ilu_used */
static void operand_needs(int mac, int ilu, unsigned mac_used, unsigned ilu_used, unsigned needs[3])
{
	needs[0] = needs[1] = needs[2] = 0;
	switch (mac)
	{
	case _mac_mov:
		needs[0] |= mac_used;
		break;
	case _mac_arl:
		/* and, where an output takes it, the operand as it is */
		needs[0] |= 8 | mac_used;
		break;
	case _mac_mul: case _mac_min: case _mac_max: case _mac_slt: case _mac_sge:
		needs[0] |= mac_used;
		needs[1] |= mac_used;
		break;
	case _mac_add:
		needs[0] |= mac_used;
		needs[2] |= mac_used;
		break;
	case _mac_mad:
		needs[0] |= mac_used;
		needs[1] |= mac_used;
		needs[2] |= mac_used;
		break;
	case _mac_dp3:
		if (mac_used)
		{
			needs[0] |= 0xe;
			needs[1] |= 0xe;
		}
		break;
	case _mac_dph:
		if (mac_used)
		{
			needs[0] |= 0xe;
			needs[1] |= 0xf;
		}
		break;
	case _mac_dp4:
		if (mac_used)
		{
			needs[0] |= 0xf;
			needs[1] |= 0xf;
		}
		break;
	case _mac_dst:
		/* (1, a.y * b.y, a.z, b.w) */
		needs[0] |= mac_used & 6;
		needs[1] |= mac_used & 5;
		break;
	default:
		break;
	}
	switch (ilu)
	{
	case _ilu_mov:
		needs[2] |= ilu_used;
		break;
	case _ilu_rcp: case _ilu_rcc: case _ilu_rsq:
		if (ilu_used)
			needs[2] |= 8;
		break;
	case _ilu_exp: case _ilu_log:
		if (ilu_used)
			needs[2] |= 8;
		break;
	case _ilu_lit:
		if (ilu_used)
			needs[2] |= 0xd;
		break;
	default:
		break;
	}
}

/* the slot of a constant's component with a sign, made if it is new */
static int constant_slot(struct nv2a_vsh_compiled *compiled, unsigned constant, unsigned component, unsigned negate)
{
	unsigned long slot;

	for (slot = 0; slot < compiled->slot_count; slot++)
	{
		if (compiled->slot_constants[slot] == constant && compiled->slot_components[slot] == component &&
			compiled->slot_negates[slot] == negate)
			return (int)slot;
	}
	if (compiled->slot_count == NV2A_VSH_CONSTANT_SLOTS)
		return -1;
	compiled->slot_constants[slot] = (uint8_t)constant;
	compiled->slot_components[slot] = (uint8_t)component;
	compiled->slot_negates[slot] = (uint8_t)negate;
	compiled->slot_count++;
	return (int)slot;
}

void nv2a_vsh_compile(const struct nv2a_vsh_program *program, const uint8_t outputs[16],
	struct nv2a_vsh_compiled *compiled)
{
	uint8_t live[NV2A_VSH_FILE_COUNT];
	static struct nv2a_vsh_compiled_instruction compiled_instructions[NV2A_VSH_MAXIMUM_INSTRUCTIONS];
	uint8_t kept[NV2A_VSH_MAXIMUM_INSTRUCTIONS];
	long index;
	unsigned long count = 0, row;

	memset(compiled, 0, sizeof(*compiled));
	memset(live, 0, sizeof(live));
	memset(kept, 0, sizeof(kept));
	live[NV2A_VSH_FILE_POSITION] = 0xf;
	for (row = 1; row < 16; row++)
		live[NV2A_VSH_FILE_OUTPUTS + row] = outputs[row] & 0xf;

	/* backwards: what each instruction must compute, given what is read
	after it; then what is read before it */
	for (index = (long)program->count - 1; index >= 0; index--)
	{
		const struct nv2a_vsh_instruction *instruction = &program->instructions[index];
		struct nv2a_vsh_compiled_instruction *out = &compiled_instructions[index];
		unsigned mac_used = 0, ilu_used = 0, needs[3];
		int mac_row = -1, ilu_row = -1, output_row = -1;
		int which;

		memset(out, 0, sizeof(*out));
		out->mac = instruction->mac;
		out->ilu = instruction->ilu;
		if (instruction->mac != _mac_nop && instruction->mac != _mac_arl && instruction->mac_mask)
			mac_row = NV2A_VSH_FILE_TEMPORARIES + (instruction->temporary <= 12 ? instruction->temporary : 0);
		if (instruction->ilu != _ilu_nop && instruction->ilu_mask)
		{
			unsigned temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;

			ilu_row = NV2A_VSH_FILE_TEMPORARIES + (temporary <= 12 ? temporary : 0);
		}
		if (instruction->output_mask && instruction->output_is_register &&
			(instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0 &&
			instruction->output_address < 16)
		{
			output_row = instruction->output_address == 0 ? NV2A_VSH_FILE_POSITION :
				NV2A_VSH_FILE_OUTPUTS + instruction->output_address;
		}

		if (mac_row >= 0)
		{
			out->mac_mask = instruction->mac_mask & live[mac_row];
			out->mac_row = (uint8_t)mac_row;
			mac_used |= out->mac_mask;
		}
		if (ilu_row >= 0)
		{
			out->ilu_mask = instruction->ilu_mask & live[ilu_row];
			out->ilu_row = (uint8_t)ilu_row;
			ilu_used |= out->ilu_mask;
		}
		if (output_row >= 0)
		{
			unsigned mask = instruction->output_mask & live[output_row];

			if (instruction->output_from_ilu)
			{
				out->ilu_output_mask = (uint8_t)mask;
				out->ilu_output_row = (uint8_t)output_row;
				ilu_used |= mask;
			}
			else
			{
				out->mac_output_mask = (uint8_t)mask;
				out->mac_output_row = (uint8_t)output_row;
				mac_used |= mask;
			}
		}
		out->mac_used = (uint8_t)mac_used;
		out->ilu_used = (uint8_t)ilu_used;
		out->captures_clip = instruction->captures_clip;
		if (!mac_used && !ilu_used && instruction->mac != _mac_arl && !instruction->captures_clip)
			continue;
		kept[index] = 1;

		/* the writes, all of the mask whether used or not: what was there
		before is not read after */
		if (mac_row >= 0)
			live[mac_row] &= ~instruction->mac_mask;
		if (ilu_row >= 0)
			live[ilu_row] &= ~instruction->ilu_mask;
		if (output_row >= 0)
			live[output_row] &= ~instruction->output_mask;
		/* the reads */
		operand_needs(instruction->mac, instruction->ilu, mac_used, ilu_used, needs);
		for (which = 0; which < 3; which++)
		{
			const struct nv2a_vsh_operand *operand = &instruction->operands[which];
			int source_row = operand_row(instruction, which), component;

			out->needs[which] = (uint8_t)needs[which];
			if (source_row >= 0)
				live[source_row] |= (uint8_t)swizzle_reads(operand->swizzle, needs[which]);
			for (component = 0; component < 4; component++)
			{
				struct nv2a_vsh_source *source = &out->sources[which][component];
				unsigned swizzled = operand->swizzle[component];

				if (!(needs[which] & (8 >> component)))
					continue;
				source->constant = instruction->constant;
				source->component = (uint8_t)swizzled;
				source->negate = operand->negate;
				if (source_row >= 0)
				{
					source->kind = operand->negate ? 2 : 0;
					source->offset = (uint16_t)ROW_OFFSET(source_row, swizzled);
				}
				else if (operand->mux == _mux_constant && instruction->relative)
				{
					source->kind = 1;
				}
				else
				{
					/* a constant where it is; a mux of 0 reads nothing (zero),
					which a slot of a zero gives */
					int slot = operand->mux == _mux_constant ?
						constant_slot(compiled, instruction->constant, swizzled, operand->negate) :
						constant_slot(compiled, 0xff, 0, 0);

					if (slot < 0)
					{
						/* more than there are slots (never, with the game's
						programs): spread for the instruction */
						source->kind = 3;
						source->constant = operand->mux == _mux_constant ? instruction->constant : 0xff;
					}
					else
					{
						source->kind = 0;
						source->offset = (uint16_t)CONSTANT_OFFSET(slot);
					}
				}
			}
		}
		if (instruction->captures_clip)
			live[NV2A_VSH_FILE_POSITION] = 0xf;

		/* the MAC can write where it goes when it goes to one place and
		this instruction reads nothing of that place */
		if (mac_used && !(out->mac_mask && out->mac_output_mask) && instruction->mac != _mac_arl)
		{
			unsigned destination = out->mac_mask ? out->mac_row : out->mac_output_row;
			int reads_destination = 0;

			for (which = 0; which < 3; which++)
			{
				if (operand_row(instruction, which) == (int)destination && needs[which])
					reads_destination = 1;
			}
			if (instruction->captures_clip && destination == NV2A_VSH_FILE_POSITION)
				reads_destination = 1;
			out->mac_direct = !reads_destination;
		}
	}

	memcpy(compiled->live_in, live, sizeof(live));
	for (index = 0; index < (long)program->count; index++)
	{
		if (!kept[index])
			continue;
		compiled->instructions[count++] = compiled_instructions[index];
		if (compiled_instructions[index].captures_clip)
			compiled->clip_captured = 1;
	}
	compiled->count = count;
}

/* each loop runs over the batch's vertices; the rows it reads and writes
never overlap (the compiler sends a result that would overwrite its own
operand through the results' rows) */
#define LANES(...) \
	do { \
		unsigned long lane; \
		for (lane = 0; lane < count; lane++) { __VA_ARGS__; } \
	} while (0)

static void copy_row(float *restrict destination, const float *restrict source, unsigned long count)
{
	LANES(destination[lane] = source[lane]);
}

/* the components of mask of a result to a row's */
static void store(float (*destination)[NV2A_VSH_BATCH], unsigned mask, float (*values)[NV2A_VSH_BATCH],
	unsigned long count)
{
	int component;

	for (component = 0; component < 4; component++)
	{
		if ((mask & (8 >> component)) && destination[component] != values[component])
			copy_row(destination[component], values[component], count);
	}
}

/* a scalar to the components of mask */
static void store_scalar(float (*destination)[NV2A_VSH_BATCH], unsigned mask, const float *scalar,
	unsigned long count)
{
	int component;

	for (component = 0; component < 4; component++)
	{
		if ((mask & (8 >> component)) && destination[component] != scalar)
			copy_row(destination[component], scalar, count);
	}
}

/* the first component of a mask (bit 3 x), or -1 */
static int first_component(unsigned mask)
{
	return mask & 8 ? 0 : mask & 4 ? 1 : mask & 2 ? 2 : mask & 1 ? 3 : -1;
}

void nv2a_vsh_run_batch(const struct nv2a_vsh_compiled *compiled, const float (*constants)[4],
	struct nv2a_vsh_lanes *lanes, unsigned long count)
{
	float *base = &lanes->rows[0][0][0];
	unsigned long index, row, slot;
	int component;

	/* the constants read where they are, across the batch */
	for (slot = 0; slot < compiled->slot_count; slot++)
	{
		float *out = lanes->constants[slot];
		float value = compiled->slot_constants[slot] == 0xff ? 0.0f :
			constants[compiled->slot_constants[slot]][compiled->slot_components[slot]];

		if (compiled->slot_negates[slot])
			value = -value;
		LANES(out[lane] = value);
	}
	/* what is read before it is written starts as the hardware has it */
	for (row = 0; row < NV2A_VSH_FILE_COUNT; row++)
	{
		unsigned live = compiled->live_in[row];

		if (!live || (row >= NV2A_VSH_FILE_INPUTS && row < NV2A_VSH_FILE_OUTPUTS))
			continue;
		for (component = 0; component < 4; component++)
		{
			float value = 0.0f, *out = lanes->rows[row][component];

			if (!(live & (8 >> component)))
				continue;
			if (row == NV2A_VSH_FILE_POSITION)
				value = component == 3 ? 1.0f : 0.0f;
			else if (row == NV2A_VSH_FILE_OUTPUTS + NV2A_VSH_OUTPUT_FOG ||
				row == NV2A_VSH_FILE_OUTPUTS + NV2A_VSH_OUTPUT_POINT_SIZE)
				value = 1.0f;
			else if (row >= NV2A_VSH_FILE_OUTPUTS + 3)
				value = component == 3 ? 1.0f : 0.0f;
			LANES(out[lane] = value);
		}
	}

	for (index = 0; index < compiled->count; index++)
	{
		const struct nv2a_vsh_compiled_instruction *instruction = &compiled->instructions[index];
		const float *operands[3][4];
		float (*mac)[NV2A_VSH_BATCH] = lanes->results[0];
		float (*ilu)[NV2A_VSH_BATCH] = lanes->results[1];
		float *scalar = lanes->results[2][0];
		unsigned mac_used = instruction->mac_used, ilu_used = instruction->ilu_used;
		int which;

		/* each operand component's row: where it is, or gathered at a0, or
		negated */
		for (which = 0; which < 3; which++)
		{
			unsigned needs = instruction->needs[which];

			for (component = 0; component < 4; component++)
			{
				const struct nv2a_vsh_source *source = &instruction->sources[which][component];

				if (!(needs & (8 >> component)))
					continue;
				if (source->kind == 0)
				{
					operands[which][component] = base + source->offset;
				}
				else
				{
					float *out = lanes->scratch[which][component];

					if (source->kind == 2)
					{
						const float *in = base + source->offset;

						LANES(out[lane] = -in[lane]);
					}
					else if (source->kind == 3)
					{
						float value = source->constant == 0xff ? 0.0f : constants[source->constant][source->component];

						if (source->negate)
							value = -value;
						LANES(out[lane] = value);
					}
					else
					{
						const int *a0 = lanes->a0;
						unsigned element = source->component;

						LANES(
							int address = a0[lane] + source->constant;
							float value;
							address = address < 0 ? 0 : address >= NV2A_VSH_CONSTANT_COUNT ? NV2A_VSH_CONSTANT_COUNT - 1 : address;
							value = constants[address][element];
							out[lane] = source->negate ? -value : value);
					}
					operands[which][component] = out;
				}
			}
		}
#define A(c) operands[0][c]
#define B(c) operands[1][c]
#define C(c) operands[2][c]

		if (instruction->captures_clip)
		{
			for (component = 0; component < 4; component++)
				copy_row(lanes->clip[component], lanes->rows[NV2A_VSH_FILE_POSITION][component], count);
		}

		/* the MAC: where its result goes straight to its row, that row is
		where it is made */
		if (instruction->mac_direct)
			mac = lanes->rows[instruction->mac_mask ? instruction->mac_row : instruction->mac_output_row];
		switch (instruction->mac)
		{
		case _mac_mov:
			for (component = 0; component < 4; component++)
				if (mac_used & (8 >> component))
				{
					float *restrict out = mac[component];
					const float *a = A(component);

					LANES(out[lane] = a[lane]);
				}
			break;
		case _mac_arl:
		{
			const float *a = A(0);
			int *a0 = lanes->a0;

			LANES(a0[lane] = (int)nv2a_floor(a[lane] + 0.001f));
			/* an output that takes it gets the operand */
			for (component = 0; component < 4; component++)
				if (mac_used & (8 >> component))
					copy_row(mac[component], A(component), count);
			break;
		}
		case _mac_mul:
			for (component = 0; component < 4; component++)
				if (mac_used & (8 >> component))
				{
					float *restrict out = mac[component];
					const float *a = A(component), *b = B(component);

					LANES(out[lane] = a[lane] * b[lane]);
				}
			break;
		case _mac_add:
			for (component = 0; component < 4; component++)
				if (mac_used & (8 >> component))
				{
					float *restrict out = mac[component];
					const float *a = A(component), *c = C(component);

					LANES(out[lane] = a[lane] + c[lane]);
				}
			break;
		case _mac_mad:
			for (component = 0; component < 4; component++)
				if (mac_used & (8 >> component))
				{
					float *restrict out = mac[component];
					const float *a = A(component), *b = B(component), *c = C(component);

					LANES(out[lane] = a[lane] * b[lane] + c[lane]);
				}
			break;
		case _mac_dp3: case _mac_dph: case _mac_dp4:
			if (mac_used)
			{
				/* made in the first component used, copied to the others */
				float *restrict out = mac[first_component(mac_used)];
				const float *a0 = A(0), *a1 = A(1), *a2 = A(2), *b0 = B(0), *b1 = B(1), *b2 = B(2);

				if (instruction->mac == _mac_dp3)
					LANES(out[lane] = a0[lane] * b0[lane] + a1[lane] * b1[lane] + a2[lane] * b2[lane]);
				else if (instruction->mac == _mac_dph)
				{
					const float *b3 = B(3);

					LANES(out[lane] = a0[lane] * b0[lane] + a1[lane] * b1[lane] + a2[lane] * b2[lane] + b3[lane]);
				}
				else
				{
					const float *a3 = A(3), *b3 = B(3);

					LANES(out[lane] = a0[lane] * b0[lane] + a1[lane] * b1[lane] + a2[lane] * b2[lane] +
						a3[lane] * b3[lane]);
				}
				store_scalar(mac, mac_used, out, count);
			}
			break;
		case _mac_dst:
			if (mac_used & 8) { float *out = mac[0]; LANES(out[lane] = 1.0f); }
			if (mac_used & 4) { float *restrict out = mac[1]; const float *a = A(1), *b = B(1); LANES(out[lane] = a[lane] * b[lane]); }
			if (mac_used & 2) copy_row(mac[2], A(2), count);
			if (mac_used & 1) copy_row(mac[3], B(3), count);
			break;
		case _mac_min: case _mac_max: case _mac_slt: case _mac_sge:
			for (component = 0; component < 4; component++)
				if (mac_used & (8 >> component))
				{
					float *restrict out = mac[component];
					const float *a = A(component), *b = B(component);

					switch (instruction->mac)
					{
					case _mac_min: LANES(out[lane] = a[lane] < b[lane] ? a[lane] : b[lane]); break;
					case _mac_max: LANES(out[lane] = a[lane] > b[lane] ? a[lane] : b[lane]); break;
					case _mac_slt: LANES(out[lane] = a[lane] < b[lane] ? 1.0f : 0.0f); break;
					default: LANES(out[lane] = a[lane] >= b[lane] ? 1.0f : 0.0f); break;
					}
				}
			break;
		default:
			for (component = 0; component < 4; component++)
				if (mac_used & (8 >> component)) { float *out = mac[component]; LANES(out[lane] = 0.0f); }
			break;
		}

		/* the ILU */
		if (ilu_used)
		{
			const float *c = instruction->needs[2] & 8 ? C(0) : NULL;

			switch (instruction->ilu)
			{
			case _ilu_mov:
				for (component = 0; component < 4; component++)
					if (ilu_used & (8 >> component))
						copy_row(ilu[component], C(component), count);
				break;
			case _ilu_rcp:
				LANES(scalar[lane] = 1.0f / c[lane]);
				store_scalar(ilu, ilu_used, scalar, count);
				break;
			case _ilu_rcc:
				LANES(scalar[lane] = rcc(c[lane]));
				store_scalar(ilu, ilu_used, scalar, count);
				break;
			case _ilu_rsq:
				LANES(scalar[lane] = nv2a_rsq(c[lane]));
				store_scalar(ilu, ilu_used, scalar, count);
				break;
			case _ilu_exp:
				/* (2^floor(x), x - floor(x), 2^x, 1), each only where used */
				if (ilu_used & 8) LANES(ilu[0][lane] = exp2f(nv2a_floor(c[lane])));
				if (ilu_used & 4) LANES(ilu[1][lane] = c[lane] - nv2a_floor(c[lane]));
				if (ilu_used & 2) LANES(ilu[2][lane] = exp2f(c[lane]));
				if (ilu_used & 1) LANES(ilu[3][lane] = 1.0f);
				break;
			case _ilu_log:
				LANES(
					float x = fabsf(c[lane]);
					if (x == 0.0f)
					{
						ilu[0][lane] = -1.0e30f; ilu[1][lane] = 1.0f; ilu[2][lane] = -1.0e30f; ilu[3][lane] = 1.0f;
					}
					else
					{
						float e = nv2a_floor(log2f(x));

						ilu[0][lane] = e; ilu[1][lane] = x / exp2f(e); ilu[2][lane] = log2f(x); ilu[3][lane] = 1.0f;
					});
				break;
			case _ilu_lit:
			{
				const float *y = C(1), *w = C(3);

				LANES(
					float x = c[lane], power = w[lane];
					float exponent = power < -127.9961f ? -127.9961f : power > 127.9961f ? 127.9961f : power;
					ilu[0][lane] = 1.0f;
					ilu[1][lane] = x > 0.0f ? x : 0.0f;
					ilu[2][lane] = x > 0.0f ? powf(y[lane] > 0.0f ? y[lane] : 0.0f, exponent) : 0.0f;
					ilu[3][lane] = 1.0f);
				break;
			}
			default:
				for (component = 0; component < 4; component++)
					if (ilu_used & (8 >> component)) { float *out = ilu[component]; LANES(out[lane] = 0.0f); }
				break;
			}
		}
#undef A
#undef B
#undef C

		/* the writes, after both units have read */
		if (mac_used && !instruction->mac_direct)
		{
			if (instruction->mac_mask)
				store(lanes->rows[instruction->mac_row], instruction->mac_mask, mac, count);
			if (instruction->mac_output_mask)
				store(lanes->rows[instruction->mac_output_row], instruction->mac_output_mask, mac, count);
		}
		if (ilu_used)
		{
			if (instruction->ilu_mask)
				store(lanes->rows[instruction->ilu_row], instruction->ilu_mask, ilu, count);
			if (instruction->ilu_output_mask)
				store(lanes->rows[instruction->ilu_output_row], instruction->ilu_output_mask, ilu, count);
		}
	}
}

void nv2a_vsh_lanes_clip_positions(const struct nv2a_vsh_compiled *compiled, const struct nv2a_vsh_lanes *lanes,
	unsigned long count, const float c38[4], const float c37[4], const float viewport_scale[3],
	const float viewport_offset[3], float (*clips)[4])
{
	unsigned long lane;
	int component;
	float inverse[3];

	clip_inverse(viewport_scale, inverse);
	for (lane = 0; lane < count; lane++)
	{
		float position[4], kept[4];

		for (component = 0; component < 4; component++)
		{
			position[component] = lanes->rows[NV2A_VSH_FILE_POSITION][component][lane];
			kept[component] = lanes->clip[component][lane];
		}
		clip_position(position, compiled->clip_captured ? kept : NULL, c38, c37, inverse, viewport_offset,
			clips[lane]);
	}
}

void nv2a_vsh_fetch_lanes(unsigned type, const unsigned char *data, unsigned long stride, unsigned long count,
	float (*row)[NV2A_VSH_BATCH])
{
	unsigned long bytes = nv2a_vsh_type_bytes(type), lane, index;

	switch (type)
	{
	case NV2A_VSDT_FLOAT1: case NV2A_VSDT_FLOAT2: case NV2A_VSDT_FLOAT3: case NV2A_VSDT_FLOAT4:
	case NV2A_VSDT_FLOAT2H:
		for (lane = 0; lane < count; lane++, data += stride)
		{
			float value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

			memcpy(value, data, bytes);
			row[0][lane] = value[0]; row[1][lane] = value[1]; row[2][lane] = value[2]; row[3][lane] = value[3];
		}
		break;
	case NV2A_VSDT_NORMPACKED3:
		for (lane = 0; lane < count; lane++, data += stride)
		{
			uint32_t packed;

			memcpy(&packed, data, sizeof(packed));
			row[0][lane] = (float)((int32_t)(packed << 21) >> 21) * INVERSE_1023;
			row[1][lane] = (float)((int32_t)(packed << 10) >> 21) * INVERSE_1023;
			row[2][lane] = (float)((int32_t)packed >> 22) * INVERSE_511;
			row[3][lane] = 1.0f;
		}
		break;
	case NV2A_VSDT_PBYTE1: case NV2A_VSDT_PBYTE2: case NV2A_VSDT_PBYTE3: case NV2A_VSDT_PBYTE4:
		for (lane = 0; lane < count; lane++, data += stride)
		{
			for (index = 0; index < 4; index++)
				row[index][lane] = index < bytes ? data[index] * INVERSE_255 : index == 3 ? 1.0f : 0.0f;
		}
		break;
	case NV2A_VSDT_NORMSHORT1: case NV2A_VSDT_NORMSHORT2: case NV2A_VSDT_NORMSHORT3: case NV2A_VSDT_NORMSHORT4:
		for (lane = 0; lane < count; lane++, data += stride)
		{
			for (index = 0; index < 4; index++)
			{
				int16_t value;

				if (index >= bytes / 2)
				{
					row[index][lane] = index == 3 ? 1.0f : 0.0f;
					continue;
				}
				memcpy(&value, data + index * 2, sizeof(value));
				row[index][lane] = value < -32767 ? -1.0f : value * INVERSE_32767;
			}
		}
		break;
	default:
		/* the shorts and the color: one vertex at a time */
		for (lane = 0; lane < count; lane++, data += stride)
		{
			float value[4];

			nv2a_vsh_fetch(type, data, value);
			row[0][lane] = value[0]; row[1][lane] = value[1]; row[2][lane] = value[2]; row[3][lane] = value[3];
		}
		break;
	}
}

#undef LANES

void nv2a_vsh_lanes_result(const struct nv2a_vsh_compiled *compiled, const struct nv2a_vsh_lanes *lanes,
	unsigned long lane, struct nv2a_vsh_result *result)
{
	int component, texcoord;

	for (component = 0; component < 4; component++)
	{
		result->position[component] = lanes->rows[NV2A_VSH_FILE_POSITION][component][lane];
		result->clip[component] = lanes->clip[component][lane];
		result->diffuse[component] = lanes->rows[NV2A_VSH_FILE_OUTPUTS + NV2A_VSH_OUTPUT_DIFFUSE][component][lane];
		result->specular[component] = lanes->rows[NV2A_VSH_FILE_OUTPUTS + NV2A_VSH_OUTPUT_SPECULAR][component][lane];
		result->back_diffuse[component] =
			lanes->rows[NV2A_VSH_FILE_OUTPUTS + NV2A_VSH_OUTPUT_BACK_DIFFUSE][component][lane];
		result->back_specular[component] =
			lanes->rows[NV2A_VSH_FILE_OUTPUTS + NV2A_VSH_OUTPUT_BACK_SPECULAR][component][lane];
		for (texcoord = 0; texcoord < 4; texcoord++)
			result->texcoords[texcoord][component] =
				lanes->rows[NV2A_VSH_FILE_OUTPUTS + NV2A_VSH_OUTPUT_TEXCOORD0 + texcoord][component][lane];
	}
	result->clip_captured = compiled->clip_captured;
	result->fog = lanes->rows[NV2A_VSH_FILE_OUTPUTS + NV2A_VSH_OUTPUT_FOG][0][lane];
	result->point_size = lanes->rows[NV2A_VSH_FILE_OUTPUTS + NV2A_VSH_OUTPUT_POINT_SIZE][0][lane];
}
