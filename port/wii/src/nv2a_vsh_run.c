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

void nv2a_unpack_normpacked3(uint32_t packed, float out[4])
{
	int32_t x = (int32_t)(packed << 21) >> 21;
	int32_t y = (int32_t)(packed << 10) >> 21;
	int32_t z = (int32_t)packed >> 22;

	out[0] = (float)x / 1023.0f;
	out[1] = (float)y / 1023.0f;
	out[2] = (float)z / 511.0f;
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
		out[0] = ((color >> 16) & 0xff) / 255.0f;
		out[1] = ((color >> 8) & 0xff) / 255.0f;
		out[2] = (color & 0xff) / 255.0f;
		out[3] = (color >> 24) / 255.0f;
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
			out[index] = normalized ? (value < -32767 ? -1.0f : value / 32767.0f) : (float)value;
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
			out[index] = data[index] / 255.0f;
		break;
	default:
		break;
	}
}

void nv2a_vsh_clip_position(const struct nv2a_vsh_result *result, const float c38[4], const float c37[4],
	const float viewport_scale[3], const float viewport_offset[3], float clip[4])
{
	float scale[3];
	int axis;

	for (axis = 0; axis < 3; axis++)
		scale[axis] = viewport_scale[axis] != 0.0f ? viewport_scale[axis] : 1.0f;
	/* Direct3D 8 puts pixel centres on integer screen coordinates, GX (as
	OpenGL) on half-integers */
	if (result->clip_captured)
	{
		float w = result->clip[3];

		clip[0] = (result->clip[0] * c38[0] + (c37[0] + 0.5f - viewport_offset[0]) * w) / scale[0];
		clip[1] = (result->clip[1] * c38[1] + (c37[1] + 0.5f - viewport_offset[1]) * w) / scale[1];
		clip[2] = (result->clip[2] * c38[2] + (c37[2] - viewport_offset[2]) * w) / scale[2];
		clip[3] = w;
	}
	else
	{
		float w = result->position[3];

		clip[0] = (result->position[0] + 0.5f - viewport_offset[0]) / scale[0] * w;
		clip[1] = (result->position[1] + 0.5f - viewport_offset[1]) / scale[1] * w;
		clip[2] = (result->position[2] - viewport_offset[2]) / scale[2] * w;
		clip[3] = w;
	}
	if (!(fabsf(clip[3]) > 0.0f))
	{
		clip[0] = clip[1] = clip[2] = 0.0f;
		clip[3] = -1.0f;
	}
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
			splat(ilu, 1.0f / sqrtf(fabsf(C[0])));
			break;
		case _ilu_exp:
			ilu[0] = exp2f(floorf(C[0]));
			ilu[1] = C[0] - floorf(C[0]);
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
				float e = floorf(log2f(x));

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
			a0 = (int)floorf(mac[0] + 0.001f);
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
