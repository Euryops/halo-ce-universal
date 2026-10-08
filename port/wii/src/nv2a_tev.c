/*
NV2A_TEV.C

The NV2A's register combiners as TEV stages (nv2a_tev.h). The semantics are
the Linux port's GLSL translation's (port/linux/src/nv2a_psh.c): each
general combiner stage computes A*B (or their dot product), C*D, and their
sum or a choice between them, from inputs mapped (clamped, inverted,
expanded...) out of the registers, maps the results by its output scale and
bias, clamps them to [-1, 1] and writes them back; the final combiner
computes A*B + (1-A)*C + D and G.

The translation goes through three steps.

1. Terms. Each value a stage writes is written out as a sum of terms, each
   a weight (plus or minus a power of two) times one of TEV's lerps -- a*(1-c)
   + b*c of three unsigned inputs, which covers a product, a product with an
   inverted input, and a single input -- plus a constant. Expanded inputs
   (2x - 1) are what makes the weights; two expanded inputs multiplied are
   taken as 2*lerp(1-y, y, x) - 1, which is one stage rather than three.
2. Operations. A sum of terms is computed in a chain of TEV operations, each
   adding (or subtracting) one term to the previous one's result as its d,
   and scaling: the largest weights go first, and each operation's scale is
   the ratio of its term's weight to the next one's, so every term ends up
   with its own weight. The constant is folded into the operations' biases
   where it fits, else it is a term of its own. An operation whose inputs
   want two textures, two colors or two constants (one of each per TEV stage)
   has one of them copied to a register first; so has an inverted input that
   cannot be the lerp's c.
3. Stages. The color pipe's operations and the alpha pipe's are paired into
   TEV stages, in order, as far as their inputs allow; then the values are
   given registers (four of each pipe), and the result is left in PREV.

Values are kept in one of three forms. A value that cannot be negative
(textures, colors, constants, and what is made from them) is stored as it
is, clamped to [0, 1] -- which, as the NV2A clamps to [-1, 1] and an input
mapped unsigned is clamped at 0 anyway, loses nothing when nothing reads it
signed. A value that can be negative and is read signed is stored as
(x + 1) / 2, clamped, which is the NV2A's own clamp too; an unsigned read of
one takes a clamped copy. And an rgb value whose three channels are the
same -- a dot product, or one made only of alpha inputs -- is computed once,
as a scalar, in the alpha pipe, from where the color pipe reads it as an
alpha input replicated (TEV's A0...) and the alpha pipe as itself. Dot
products are computed that way: channel by channel, each channel of a
texture or color picked into the alpha by a swap table, of which there are
four, one for each channel (with red, green and blue left as they are).
The blue of an rgb value the alpha pipe reads, and the channels of one a dot
product reads, are computed the same way, as the value's expression for
that channel, from the registers as the value's stage saw them.
*/

#include "nv2a_tev.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- GX's numbers */

enum
{
	CC_CPREV = 0, CC_APREV = 1, CC_TEXC = 8, CC_TEXA = 9, CC_RASC = 10, CC_RASA = 11,
	CC_ONE = 12, CC_HALF = 13, CC_KONST = 14, CC_ZERO = 15,
	CA_APREV = 0, CA_TEXA = 4, CA_RASA = 5, CA_KONST = 6, CA_ZERO = 7,
	TEV_ADD = 0, TEV_SUB = 1, TEV_COMP_R8_GT = 8, TEV_COMP_A8_GT = 14,
	TB_ZERO = 0, TB_ADDHALF = 1, TB_SUBHALF = 2,
	CS_SCALE_1 = 0, CS_SCALE_2 = 1, CS_SCALE_4 = 2, CS_DIVIDE_2 = 3,
	KSEL_K0 = 0x0c, KSEL_K0_R = 0x10, KSEL_K0_A = 0x1c,
	CH_RED = 0, CH_GREEN = 1, CH_BLUE = 2, CH_ALPHA = 3,
};

/* ---------- the NV2A's encoding (nv2a_psh.c) */

enum
{
	_register_zero = 0, _register_c0 = 1, _register_c1 = 2, _register_fog = 3,
	_register_v0 = 4, _register_v1 = 5, _register_t0 = 8, _register_r0 = 12, _register_r1 = 13,
	_register_v1r0_sum = 14, _register_ef_product = 15,
	REGISTER_COUNT = 16,
};

enum
{
	_mapping_unsigned_identity = 0x00, _mapping_unsigned_invert = 0x20,
	_mapping_expand_normal = 0x40, _mapping_expand_negate = 0x60,
	_mapping_half_bias_normal = 0x80, _mapping_half_bias_negate = 0xa0,
	_mapping_signed_identity = 0xc0, _mapping_signed_negate = 0xe0,
};

/* ---------- the compiler's own pieces */

enum { PIPE_COLOR, PIPE_ALPHA };
/* the lanes a value is read in: a channel (GX_CH_*), or all three colors */
enum { LANE_R = CH_RED, LANE_G = CH_GREEN, LANE_B = CH_BLUE, LANE_A = CH_ALPHA, LANE_RGB = 4 };
enum { PART_RGB, PART_ALPHA };

#define MAXIMUM_OPERATIONS 128
#define MAXIMUM_SLOTS 192
#define MAXIMUM_VALUES 160
#define MAXIMUM_CONSTANTS 24
#define MAXIMUM_TERMS 24
#define NONE (-1)

/* what one TEV input reads */
enum
{
	OPERAND_ZERO, OPERAND_ONE, OPERAND_HALF,
	/* texture stage (index), in a lane */
	OPERAND_TEXTURE,
	/* vertex color: 0 diffuse, 1 specular */
	OPERAND_COLOR,
	/* a constant (index into the constants), in a lane */
	OPERAND_CONSTANT,
	/* one of GX's fixed constants: 0 one, 1 seven eighths ... 7 one eighth */
	OPERAND_FRACTION,
	/* a register's value (index of the slot) */
	OPERAND_SLOT,
};

struct operand
{
	uint8_t kind;
	uint8_t lane;
	/* 1 - the value: only as a term's input, before it is made an operation's */
	uint8_t complement;
	short index;
};

/* a value in a register, before registers are given: the color pipe's or
the alpha pipe's, made by an operation or holding a constant from the start */
struct slot
{
	uint8_t pipe;
	short definition;
	short constant;
	short first_use_stage, last_use_stage;
	short register_index;
	uint8_t final;
};

struct operation
{
	uint8_t pipe;
	struct operand inputs[4];
	uint8_t op, bias, scale, clamp;
	short destination;
	short stage;
};

enum
{
	/* zero */
	VALUE_ZERO,
	/* a texture stage, a vertex color, a constant (index); its rgb or its
	alpha (part) */
	VALUE_TEXTURE, VALUE_COLOR, VALUE_CONSTANT,
	/* the fog register's alpha, the fog factor */
	VALUE_FOG_FACTOR,
	/* computed, in a slot */
	VALUE_SLOT,
};

/* what an NV2A register's rgb or alpha holds */
struct value
{
	uint8_t kind;
	uint8_t part;
	/* the slot holds (x + 1) / 2 */
	uint8_t biased;
	/* rgb, the three channels the same: held as a scalar in an alpha slot */
	uint8_t scalar;
	short index;
	short slot;
	float minimum, maximum;
	/* the slot clamped to [0, 1], for unsigned reads of a biased value, in
	each pipe */
	short clamped[2];
	/* a vector rgb value's red, green and blue computed apart, in the alpha
	pipe (values) */
	short lanes[3];
	/* how it was made, for computing a lane of it later: the stage, the
	output (0 AB, 1 CD, 2 sum), or the final combiner's (3 EF, 4 v1r0_sum) */
	short stage;
	uint8_t output;
};

/* a term of a sum: weight * lerp(a, b, c) */
struct term
{
	int8_t sign;
	int8_t exponent;
	struct operand a, b, c;
};

struct sum
{
	int term_count;
	struct term terms[MAXIMUM_TERMS];
	float constant;
	float minimum, maximum;
};

/* an input as a term's factor: scale * atom + offset, atom an unsigned
operand (or the factor a constant, scale 0) */
struct factor
{
	struct operand atom;
	float scale, offset;
};

struct compiler
{
	const struct nv2a_combiners *combiners;
	const struct nv2a_tev_options *options;
	struct nv2a_tev_program *program;

	int operation_count;
	struct operation operations[MAXIMUM_OPERATIONS];
	int slot_count;
	struct slot slots[MAXIMUM_SLOTS];
	int value_count;
	struct value values[MAXIMUM_VALUES];
	int constant_count;
	struct nv2a_tev_constant constants[MAXIMUM_CONSTANTS];
	short constant_register[MAXIMUM_CONSTANTS];

	int stage_count;
	/* the registers as each stage saw them, before it (stage_count + 1 for
	the final combiner): [register][part] */
	short registers[9][REGISTER_COUNT][2];
	/* the final combiner's EF and v1r0_sum, as values */
	short final_ef, final_v1r0_sum;
	short zero_value;

	uint8_t texture_present[4];
	uint8_t unique_c0, unique_c1, mux_msb;
	const char *failure;
};

static int fail(struct compiler *compiler, const char *why)
{
	if (!compiler->failure)
		compiler->failure = why;
	return NONE;
}

static struct operand operand(int kind, int index, int lane)
{
	struct operand result;

	memset(&result, 0, sizeof(result));
	result.kind = (uint8_t)kind;
	result.index = (short)index;
	result.lane = (uint8_t)lane;
	return result;
}

/* ---------- constants */

static int constant_index(struct compiler *compiler, int source, int index, const uint8_t *value)
{
	int constant;

	for (constant = 0; constant < compiler->constant_count; constant++)
	{
		const struct nv2a_tev_constant *existing = &compiler->constants[constant];

		if (existing->source == source && existing->index == index &&
			(source != _nv2a_constant_literal || !memcmp(existing->value, value, 4)))
		{
			return constant;
		}
	}
	if (compiler->constant_count == MAXIMUM_CONSTANTS)
		return fail(compiler, "too many constants");
	compiler->constants[constant].source = (uint8_t)source;
	compiler->constants[constant].index = (uint8_t)index;
	if (value)
		memcpy(compiler->constants[constant].value, value, 4);
	compiler->constant_register[constant] = NONE;
	return compiler->constant_count++;
}

/* a constant's channel (or its three colors) as the program sees it: 0, 1,
or NONE when it is to be read */
static int constant_fold(const struct compiler *compiler, int constant, int lane)
{
	const struct nv2a_tev_constant *c = &compiler->constants[constant];
	const struct nv2a_tev_options *options = compiler->options;
	uint32_t color;
	int channel, first = lane == LANE_RGB ? 0 : lane, last = lane == LANE_RGB ? 2 : lane, folded = NONE;

	switch (c->source)
	{
	case _nv2a_constant_c0: color = options->c0[c->index & 7]; break;
	case _nv2a_constant_c1: color = options->c1[c->index & 7]; break;
	case _nv2a_constant_final_c0: color = options->final_c0; break;
	case _nv2a_constant_final_c1: color = options->final_c1; break;
	default: return NONE;
	}
	for (channel = first; channel <= last; channel++)
	{
		/* D3DCOLOR: alpha in the top byte, then red, green, blue */
		unsigned byte = (color >> (channel == LANE_A ? 24 : 16 - 8 * channel)) & 0xff;
		int number = byte == 0 ? 0 : byte == 255 ? 1 : NONE;

		if (number == NONE || (channel != first && number != folded))
			return NONE;
		folded = number;
	}
	return folded;
}

/* a number as an operand: 0, 1, one of GX's eighths, or a literal byte */
static struct operand number_operand(struct compiler *compiler, float number)
{
	int eighths = (int)floorf(number * 8.0f + 0.5f);
	uint8_t literal[4];
	int byte;

	if (number <= 0.0f)
		return operand(OPERAND_ZERO, 0, LANE_RGB);
	if (number >= 1.0f)
		return operand(OPERAND_ONE, 0, LANE_RGB);
	if (fabsf(eighths / 8.0f - number) < 1.0e-6f && eighths >= 1)
		return operand(OPERAND_FRACTION, 8 - eighths, LANE_RGB);
	byte = (int)floorf(number * 255.0f + 0.5f);
	literal[0] = literal[1] = literal[2] = literal[3] = (uint8_t)byte;
	return operand(OPERAND_CONSTANT, constant_index(compiler, _nv2a_constant_literal, 0, literal), LANE_A);
}

/* ---------- slots and operations */

static int new_slot(struct compiler *compiler, int pipe)
{
	struct slot *slot;

	if (compiler->slot_count == MAXIMUM_SLOTS)
		return fail(compiler, "too many values");
	slot = &compiler->slots[compiler->slot_count];
	memset(slot, 0, sizeof(*slot));
	slot->pipe = (uint8_t)pipe;
	slot->definition = NONE;
	slot->constant = NONE;
	slot->register_index = NONE;
	return compiler->slot_count++;
}

/* the texture, color or constant selection an operand needs of its stage,
as a small code: kind in the top bits, so that two operands conflict when
their codes differ */
static int texture_need(const struct operand *input)
{
	return input->kind == OPERAND_TEXTURE ? input->index : NONE;
}

static int color_need(const struct operand *input)
{
	return input->kind == OPERAND_COLOR ? input->index : NONE;
}

/* the swap table's alpha a texture or color read wants: NONE when any will
do (the color pipe's rgb reads, which every table leaves as they are) */
static int lane_need(int pipe, const struct operand *input)
{
	if (input->kind != OPERAND_TEXTURE && input->kind != OPERAND_COLOR)
		return NONE;
	if (pipe == PIPE_COLOR)
		return input->lane == LANE_A ? LANE_A : NONE;
	return input->lane == LANE_RGB ? LANE_A : input->lane;
}

/* the constant selection (GX_TEV_KCSEL_* or KASEL_* without the register),
as a code; NONE for none */
static int konst_need(int pipe, const struct operand *input)
{
	switch (input->kind)
	{
	case OPERAND_CONSTANT:
		return 0x100 | (input->index << 3) | (pipe == PIPE_COLOR && input->lane == LANE_RGB ? 4 : input->lane);
	case OPERAND_FRACTION:
		return input->index;
	case OPERAND_ONE:
		return pipe == PIPE_ALPHA ? 0 : NONE;
	case OPERAND_HALF:
		return pipe == PIPE_ALPHA ? 4 : NONE;
	default:
		return NONE;
	}
}

static int operation_append(struct compiler *compiler, int pipe, const struct operand inputs[4],
	int op, int bias, int scale, int clamp)
{
	struct operation *operation;
	int slot;

	if (compiler->operation_count == MAXIMUM_OPERATIONS)
		return fail(compiler, "too many operations");
	slot = new_slot(compiler, pipe);
	if (slot == NONE)
		return NONE;
	operation = &compiler->operations[compiler->operation_count];
	memset(operation, 0, sizeof(*operation));
	operation->pipe = (uint8_t)pipe;
	memcpy(operation->inputs, inputs, sizeof(operation->inputs));
	operation->op = (uint8_t)op;
	operation->bias = (uint8_t)bias;
	operation->scale = (uint8_t)scale;
	operation->clamp = (uint8_t)clamp;
	operation->destination = (short)slot;
	operation->stage = NONE;
	compiler->slots[slot].definition = (short)compiler->operation_count++;
	return slot;
}

static int operation(struct compiler *compiler, int pipe, struct operand a, struct operand b, struct operand c,
	struct operand d, int op, int bias, int scale, int clamp);

/* the operand, alone, in a register of the pipe */
static struct operand copied(struct compiler *compiler, int pipe, struct operand input)
{
	struct operand zero = operand(OPERAND_ZERO, 0, LANE_RGB);
	int slot;

	if (input.complement)
	{
		/* 1 - x: lerp(1, 0, x) */
		input.complement = 0;
		slot = operation(compiler, pipe, operand(OPERAND_ONE, 0, LANE_RGB), zero, input, zero,
			TEV_ADD, TB_ZERO, CS_SCALE_1, 0);
	}
	else
	{
		slot = operation(compiler, pipe, zero, zero, zero, input, TEV_ADD, TB_ZERO, CS_SCALE_1, 0);
	}
	return slot == NONE ? zero : operand(OPERAND_SLOT, slot, LANE_RGB);
}

/* an operation, its inputs made fit for one stage first: no inverted a or
b, one texture (in one lane), one color, one constant selection */
static int operation(struct compiler *compiler, int pipe, struct operand a, struct operand b, struct operand c,
	struct operand d, int op, int bias, int scale, int clamp)
{
	struct operand inputs[4];
	int index, other;

	inputs[0] = a;
	inputs[1] = b;
	inputs[2] = c;
	inputs[3] = d;
	for (index = 0; index < 4; index++)
	{
		if (inputs[index].complement)
			inputs[index] = copied(compiler, pipe, inputs[index]);
	}
	for (index = 1; index < 4; index++)
	{
		for (other = 0; other < index; other++)
		{
			const struct operand *x = &inputs[index], *y = &inputs[other];
			int conflict =
				(texture_need(x) != NONE && texture_need(y) != NONE &&
					(texture_need(x) != texture_need(y) ||
					(lane_need(pipe, x) != NONE && lane_need(pipe, y) != NONE && lane_need(pipe, x) != lane_need(pipe, y)))) ||
				(color_need(x) != NONE && color_need(y) != NONE &&
					(color_need(x) != color_need(y) ||
					(lane_need(pipe, x) != NONE && lane_need(pipe, y) != NONE && lane_need(pipe, x) != lane_need(pipe, y)))) ||
				(konst_need(pipe, x) != NONE && konst_need(pipe, y) != NONE && konst_need(pipe, x) != konst_need(pipe, y));

			if (conflict)
			{
				inputs[index] = copied(compiler, pipe, inputs[index]);
				break;
			}
		}
	}
	if (compiler->failure)
		return NONE;
	return operation_append(compiler, pipe, inputs, op, bias, scale, clamp);
}

/* ---------- values */

static int new_value(struct compiler *compiler, int kind, int part, int index)
{
	struct value *value;

	if (compiler->value_count == MAXIMUM_VALUES)
		return fail(compiler, "too many values");
	value = &compiler->values[compiler->value_count];
	memset(value, 0, sizeof(*value));
	value->kind = (uint8_t)kind;
	value->part = (uint8_t)part;
	value->index = (short)index;
	value->slot = NONE;
	value->clamped[0] = value->clamped[1] = NONE;
	value->lanes[0] = value->lanes[1] = value->lanes[2] = NONE;
	value->stage = NONE;
	value->minimum = 0.0f;
	value->maximum = kind == VALUE_ZERO ? 0.0f : 1.0f;
	return compiler->value_count++;
}

static int compute_lane(struct compiler *compiler, int value_index, int lane);

/* a value read in a pipe and lane, as an operand (not yet clamped or
unbiased); the value's range and form come back with it */
static int read_value(struct compiler *compiler, int value_index, int pipe, int lane, struct operand *result,
	const struct value **form)
{
	const struct value *value = &compiler->values[value_index];

	if (value->part == PART_ALPHA)
		lane = LANE_A;
	*form = value;
	switch (value->kind)
	{
	case VALUE_ZERO:
		*result = operand(OPERAND_ZERO, 0, lane);
		return 1;
	case VALUE_TEXTURE:
		*result = operand(OPERAND_TEXTURE, value->index, lane);
		return 1;
	case VALUE_COLOR:
		*result = operand(OPERAND_COLOR, value->index, lane);
		return 1;
	case VALUE_CONSTANT:
	{
		/* channels that are 0 or 255 are numbers */
		int folded = constant_fold(compiler, value->index, lane);

		*result = folded == 0 ? operand(OPERAND_ZERO, 0, lane) : folded == 1 ? operand(OPERAND_ONE, 0, lane) :
			operand(OPERAND_CONSTANT, value->index, lane);
		return 1;
	}
	case VALUE_FOG_FACTOR:
		compiler->program->fog_texture = 1;
		*result = operand(OPERAND_TEXTURE, NV2A_TEV_FOG_TEXTURE, LANE_A);
		return 1;
	case VALUE_SLOT:
		if (compiler->slots[value->slot].pipe == PIPE_ALPHA || pipe == PIPE_COLOR)
		{
			*result = operand(OPERAND_SLOT, value->slot, lane);
			return 1;
		}
		/* a vector in the color pipe, read for one channel by the alpha pipe */
		if (lane == LANE_RGB || lane == LANE_A)
		{
			fail(compiler, "internal: a vector read whole by the alpha pipe");
			return 0;
		}
		{
			int lane_value = compute_lane(compiler, value_index, lane);

			if (lane_value == NONE)
				return 0;
			return read_value(compiler, lane_value, pipe, lane, result, form);
		}
	}
	return 0;
}

/* the value as an unsigned operand: max(x, 0) */
static int unsigned_operand(struct compiler *compiler, int value_index, int pipe, int lane, struct operand *result)
{
	const struct value *form;

	if (!read_value(compiler, value_index, pipe, lane, result, &form))
		return 0;
	if (form->biased || form->minimum < 0.0f)
	{
		struct value *value = (struct value *)form;
		int slot = value->clamped[pipe];

		if (slot == NONE)
		{
			struct operand zero = operand(OPERAND_ZERO, 0, LANE_RGB);

			if (form->biased)
			{
				/* (u - 1/2) * 2, clamped */
				slot = operation(compiler, pipe, zero, zero, zero, *result, TEV_ADD, TB_SUBHALF, CS_SCALE_2, 1);
			}
			else
			{
				slot = operation(compiler, pipe, zero, zero, zero, *result, TEV_ADD, TB_ZERO, CS_SCALE_1, 1);
			}
			if (slot == NONE)
				return 0;
			value = &compiler->values[value - compiler->values];
			value->clamped[pipe] = (short)slot;
		}
		*result = operand(OPERAND_SLOT, slot, LANE_RGB);
	}
	return 1;
}

/* ---------- inputs */

/* the value a combiner input byte reads: its register and part */
static int input_value(struct compiler *compiler, int stage, unsigned input, int alpha_portion, int final)
{
	int reg = input & 0x0f;
	int alpha_channel = (input & 0x10) != 0;
	int part = alpha_channel ? PART_ALPHA : PART_RGB;

	if (reg == _register_c0 || reg == _register_c1)
	{
		int source, index;

		if (final)
		{
			source = reg == _register_c0 ? _nv2a_constant_final_c0 : _nv2a_constant_final_c1;
			index = 0;
		}
		else
		{
			source = reg == _register_c0 ? _nv2a_constant_c0 : _nv2a_constant_c1;
			index = (reg == _register_c0 ? compiler->unique_c0 : compiler->unique_c1) ? stage : 0;
		}
		return new_value(compiler, VALUE_CONSTANT, part, constant_index(compiler, source, index, NULL));
	}
	if (reg == _register_fog)
	{
		if (part == PART_RGB)
			return new_value(compiler, VALUE_CONSTANT, PART_RGB, constant_index(compiler, _nv2a_constant_fog, 0, NULL));
		if (!compiler->options->fog)
		{
			uint8_t one[4] = { 255, 255, 255, 255 };

			return new_value(compiler, VALUE_CONSTANT, PART_ALPHA, constant_index(compiler, _nv2a_constant_literal, 0, one));
		}
		return new_value(compiler, VALUE_FOG_FACTOR, PART_ALPHA, 0);
	}
	if (final && (reg == _register_v1r0_sum || reg == _register_ef_product))
	{
		int value = reg == _register_v1r0_sum ? compiler->final_v1r0_sum : compiler->final_ef;

		return part == PART_ALPHA ? compiler->zero_value : value;
	}
	(void)alpha_portion;
	return compiler->registers[stage][reg][part];
}

/* an input byte (register, channel, mapping) as a factor, read in a pipe
and lane */
static int input_factor(struct compiler *compiler, int stage, unsigned input, int alpha_portion, int pipe, int lane,
	struct factor *factor)
{
	int mapping = input & 0xe0;
	int alpha_channel = (input & 0x10) != 0;
	int value_index;
	const struct value *form;
	struct operand atom;

	/* the alpha portion reads blue where the rgb portion reads rgb */
	if (alpha_portion && !alpha_channel)
		lane = LANE_B;
	value_index = input_value(compiler, stage, input, alpha_portion, 0);
	if (value_index == NONE)
		return 0;
	memset(factor, 0, sizeof(*factor));
	if (compiler->values[value_index].kind == VALUE_ZERO)
	{
		static const float zero_mapped[8] = { 0.0f, 1.0f, -1.0f, 1.0f, -0.5f, 0.5f, 0.0f, 0.0f };

		factor->atom = operand(OPERAND_ZERO, 0, LANE_RGB);
		factor->offset = zero_mapped[mapping >> 5];
		return 1;
	}
	if (mapping >= _mapping_signed_identity)
	{
		float sign = mapping == _mapping_signed_identity ? 1.0f : -1.0f;

		if (!read_value(compiler, value_index, pipe, lane, &atom, &form))
			return 0;
		factor->atom = atom;
		if (form->biased)
		{
			factor->scale = 2.0f * sign;
			factor->offset = -sign;
		}
		else if (form->minimum >= 0.0f)
		{
			factor->scale = sign;
		}
		else
		{
			fail(compiler, "internal: a signed read of a value kept unsigned");
			return 0;
		}
		return 1;
	}
	if (!unsigned_operand(compiler, value_index, pipe, lane, &atom))
		return 0;
	factor->atom = atom;
	switch (mapping)
	{
	case _mapping_unsigned_identity: factor->scale = 1.0f; break;
	case _mapping_unsigned_invert: factor->atom.complement = 1; factor->scale = 1.0f; break;
	case _mapping_expand_normal: factor->scale = 2.0f; factor->offset = -1.0f; break;
	case _mapping_expand_negate: factor->scale = -2.0f; factor->offset = 1.0f; break;
	case _mapping_half_bias_normal: factor->scale = 1.0f; factor->offset = -0.5f; break;
	default: factor->scale = -1.0f; factor->offset = 0.5f; break;
	}
	return 1;
}

/* ---------- sums */

static int add_term(struct compiler *compiler, struct sum *sum, float weight, struct operand a, struct operand b,
	struct operand c)
{
	struct term *term;
	int exponent;
	float magnitude = fabsf(weight);

	if (magnitude == 0.0f)
		return 1;
	if (sum->term_count == MAXIMUM_TERMS)
		return fail(compiler, "too many terms") != NONE;
	exponent = (int)floorf(log2f(magnitude) + 0.5f);
	if (fabsf(ldexpf(1.0f, exponent) - magnitude) > 1.0e-6f * magnitude)
		return fail(compiler, "internal: a weight that is not a power of two") != NONE;
	/* an inverted c is the lerp the other way round */
	if (c.complement)
	{
		struct operand swap = a;

		a = b;
		b = swap;
		c.complement = 0;
	}
	term = &sum->terms[sum->term_count++];
	term->sign = (int8_t)(weight < 0.0f ? -1 : 1);
	term->exponent = (int8_t)exponent;
	term->a = a;
	term->b = b;
	term->c = c;
	return 1;
}

static struct operand zero_operand(void)
{
	return operand(OPERAND_ZERO, 0, LANE_RGB);
}

static struct operand one_operand(void)
{
	return operand(OPERAND_ONE, 0, LANE_RGB);
}

/* weight * x, x an atom (perhaps inverted) */
static int add_single(struct compiler *compiler, struct sum *sum, float weight, struct operand x)
{
	if (x.kind == OPERAND_ZERO)
	{
		if (x.complement)
			sum->constant += weight;
		return 1;
	}
	if (x.complement)
	{
		x.complement = 0;
		return add_term(compiler, sum, weight, one_operand(), zero_operand(), x);
	}
	return add_term(compiler, sum, weight, zero_operand(), x, one_operand());
}

/* weight * x * y, x and y atoms (perhaps inverted) */
static int add_product(struct compiler *compiler, struct sum *sum, float weight, struct operand x, struct operand y)
{
	if ((x.kind == OPERAND_ZERO && !x.complement) || (y.kind == OPERAND_ZERO && !y.complement))
		return 1;
	if (x.kind == OPERAND_ZERO)
		return add_single(compiler, sum, weight, y);
	if (y.kind == OPERAND_ZERO)
		return add_single(compiler, sum, weight, x);
	if (x.kind == OPERAND_ONE && !x.complement)
		return add_single(compiler, sum, weight, y);
	if (y.kind == OPERAND_ONE && !y.complement)
		return add_single(compiler, sum, weight, x);
	/* the inverted one as c: (1-y)*x = lerp(x, 0, y) */
	if (x.complement && !y.complement)
	{
		struct operand swap = x;

		x = y;
		y = swap;
	}
	if (y.complement)
	{
		y.complement = 0;
		return add_term(compiler, sum, weight, x, zero_operand(), y);
	}
	return add_term(compiler, sum, weight, zero_operand(), x, y);
}

/* weight * f * g */
static int add_factors(struct compiler *compiler, struct sum *sum, float weight, const struct factor *f,
	const struct factor *g)
{
	/* two expanded (or half-biased) inputs: (s/2)(2x-1) * (t/2)(2y-1), and
	(2x-1)(2y-1) = 2*lerp(1-y, y, x) - 1 */
	if (f->scale != 0.0f && g->scale != 0.0f && !f->atom.complement && !g->atom.complement &&
		f->offset == -f->scale / 2.0f && g->offset == -g->scale / 2.0f)
	{
		struct operand inverted = g->atom;
		float w = weight * f->scale * g->scale / 4.0f;

		inverted.complement = 1;
		sum->constant -= w;
		return add_term(compiler, sum, 2.0f * w, inverted, g->atom, f->atom);
	}
	return add_product(compiler, sum, weight * f->scale * g->scale, f->atom, g->atom) &&
		add_single(compiler, sum, weight * f->scale * g->offset, f->atom) &&
		add_single(compiler, sum, weight * f->offset * g->scale, g->atom) &&
		(sum->constant += weight * f->offset * g->offset, 1);
}

/* the range of a sum (each term is in [0, 1] times its weight) */
static void sum_range(struct sum *sum)
{
	int index;

	sum->minimum = sum->maximum = sum->constant;
	for (index = 0; index < sum->term_count; index++)
	{
		float weight = sum->terms[index].sign * ldexpf(1.0f, sum->terms[index].exponent);

		if (weight < 0.0f)
			sum->minimum += weight;
		else
			sum->maximum += weight;
	}
}

static void sum_scale(struct sum *sum, float scale)
{
	int index, exponent = (int)floorf(log2f(scale) + 0.5f);

	for (index = 0; index < sum->term_count; index++)
		sum->terms[index].exponent = (int8_t)(sum->terms[index].exponent + exponent);
	sum->constant *= scale;
}

static int scale_code(int exponent)
{
	switch (exponent)
	{
	case 1: return CS_SCALE_2;
	case 2: return CS_SCALE_4;
	case -1: return CS_DIVIDE_2;
	default: return CS_SCALE_1;
	}
}

/* one step of a sum's chain: a term (or none), the scale after it */
struct step
{
	int term;
	int scale_exponent;
	int bias;
};

/* the sum computed in a chain of operations in the pipe; its last one
clamps if asked. The slot of the result, or NONE */
static int sum_compute(struct compiler *compiler, int pipe, struct sum *sum, int clamp)
{
	struct step steps[MAXIMUM_TERMS * 3];
	int order[MAXIMUM_TERMS];
	int step_count, index, other, attempt, fraction;
	struct operand d = zero_operand();
	int slot = NONE;

	/* a single input weighted a half, a quarter or an eighth takes its weight
	from one of GX's fractions, where the lerp's 1 is, rather than from a
	halving of the whole chain at its end, which would leave the steps before
	holding values larger than TEV's registers do */
	for (index = 0; index < sum->term_count; index++)
	{
		struct term *term = &sum->terms[index];

		if (term->exponent >= 0 || term->exponent < -3)
			continue;
		/* GX's 1/2, 1/4 and 1/8 */
		fraction = term->exponent == -1 ? 4 : term->exponent == -2 ? 6 : 7;
		if (term->a.kind == OPERAND_ZERO && term->c.kind == OPERAND_ONE)
			term->c = operand(OPERAND_FRACTION, fraction, LANE_RGB);
		else if (term->a.kind == OPERAND_ONE && term->b.kind == OPERAND_ZERO)
			term->a = operand(OPERAND_FRACTION, fraction, LANE_RGB);
		else
			continue;
		term->exponent = 0;
	}
	for (attempt = 0; attempt < 2; attempt++)
	{
		float remainder;

		/* the largest weights first */
		for (index = 0; index < sum->term_count; index++)
			order[index] = index;
		for (index = 1; index < sum->term_count; index++)
		{
			for (other = index; other > 0 && sum->terms[order[other]].exponent > sum->terms[order[other - 1]].exponent; other--)
			{
				int swap = order[other];

				order[other] = order[other - 1];
				order[other - 1] = swap;
			}
		}
		/* each step scales by the ratio of its term's weight to the next's
		(at most 4, so more steps where they are far apart), the last by its
		own weight (at least a half, so more steps where it is less) */
		step_count = 0;
		for (index = 0; index < sum->term_count; index++)
		{
			int exponent = sum->terms[order[index]].exponent;
			int next = index + 1 < sum->term_count ? sum->terms[order[index + 1]].exponent : 0;
			int ratio = exponent - next;
			int first = 1;

			do
			{
				int scale = ratio > 2 ? 2 : ratio < -1 ? -1 : ratio;

				steps[step_count].term = first ? order[index] : NONE;
				steps[step_count].bias = 0;
				steps[step_count++].scale_exponent = scale;
				ratio -= scale;
				first = 0;
			} while (ratio != 0);
		}
		/* the constant in the biases: a step's bias counts half the product of
		its scale and the scales after it */
		remainder = sum->constant;
		for (index = 0; index < step_count; index++)
		{
			int exponent = 0;
			float weight;

			for (other = index; other < step_count; other++)
				exponent += steps[other].scale_exponent;
			weight = ldexpf(0.5f, exponent);
			if (remainder >= weight - 1.0e-6f)
			{
				steps[index].bias = 1;
				remainder -= weight;
			}
			else if (remainder <= -weight + 1.0e-6f)
			{
				steps[index].bias = -1;
				remainder += weight;
			}
		}
		if (fabsf(remainder) < 1.0f / 1024.0f)
			break;
		/* else the whole constant as a term of its own */
		if (attempt == 1)
			return fail(compiler, "internal: a constant that does not fit");
		{
			float magnitude = fabsf(sum->constant);
			int exponent = (int)ceilf(log2f(magnitude));

			if (!add_single(compiler, sum, (sum->constant < 0.0f ? -1.0f : 1.0f) * ldexpf(1.0f, exponent),
				number_operand(compiler, ldexpf(magnitude, -exponent))))
			{
				return NONE;
			}
			sum->constant = 0.0f;
		}
	}
	if (step_count == 0)
	{
		/* a constant alone (or zero) */
		steps[0].term = NONE;
		steps[0].scale_exponent = 0;
		steps[0].bias = 0;
		step_count = 1;
	}
	/* the first term can be the first step's d, when it is a single input
	added, with the same weight as the next term */
	index = 0;
	if (step_count >= 2 && steps[0].term != NONE && steps[0].scale_exponent == 0 && !steps[0].bias &&
		steps[1].term != NONE)
	{
		const struct term *first = &sum->terms[steps[0].term];

		if (first->sign > 0 && first->a.kind == OPERAND_ZERO && first->c.kind == OPERAND_ONE && !first->b.complement)
		{
			d = first->b;
			index = 1;
		}
	}
	for (; index < step_count; index++)
	{
		const struct step *step = &steps[index];
		struct operand a = zero_operand(), b = zero_operand(), c = zero_operand();
		int op = TEV_ADD;

		if (step->term != NONE)
		{
			const struct term *term = &sum->terms[step->term];

			a = term->a;
			b = term->b;
			c = term->c;
			op = term->sign < 0 ? TEV_SUB : TEV_ADD;
		}
		slot = operation(compiler, pipe, a, b, c, d, op,
			step->bias > 0 ? TB_ADDHALF : step->bias < 0 ? TB_SUBHALF : TB_ZERO,
			scale_code(step->scale_exponent), clamp && index + 1 == step_count);
		if (slot == NONE)
			return NONE;
		d = operand(OPERAND_SLOT, slot, LANE_RGB);
	}
	return slot;
}

/* ---------- the general combiner stages */

struct portion
{
	uint32_t inputs, outputs;
};

static void stage_portion(const struct compiler *compiler, int stage, int alpha, struct portion *portion)
{
	portion->inputs = alpha ? compiler->combiners->alpha_inputs[stage] : compiler->combiners->rgb_inputs[stage];
	portion->outputs = alpha ? compiler->combiners->alpha_outputs[stage] : compiler->combiners->rgb_outputs[stage];
}

/* the output mapping: x -> (x + bias) * scale */
static void output_mapping(uint32_t flags, float *bias, float *scale)
{
	*bias = 0.0f;
	*scale = 1.0f;
	switch (flags & 0x38)
	{
	case 0x08: *bias = -0.5f; break;
	case 0x10: *scale = 2.0f; break;
	case 0x18: *bias = -0.5f; *scale = 2.0f; break;
	case 0x20: *scale = 4.0f; break;
	case 0x30: *scale = 0.5f; break;
	}
}

/* an input of the rgb portion that reads the same in all three channels */
static int input_is_scalar(const struct compiler *compiler, int stage, unsigned input)
{
	int reg = input & 0x0f;
	int value;

	if (input & 0x10)
		return 1;
	if (reg == _register_zero || reg >= _register_v1r0_sum)
		return 1;
	if (reg == _register_c0 || reg == _register_c1 || reg == _register_fog)
		return 0;
	value = compiler->registers[stage][reg][PART_RGB];
	return compiler->values[value].kind == VALUE_ZERO || compiler->values[value].scalar;
}

/* the sum for one of a portion's outputs (0 AB, 1 CD, 2 the sum without a
multiplexer), in a pipe and lane: without the output mapping */
static int portion_sum(struct compiler *compiler, int stage, int alpha, int output, int pipe, int lane, struct sum *sum)
{
	struct portion portion;
	uint32_t flags;
	int pair;

	stage_portion(compiler, stage, alpha, &portion);
	flags = portion.outputs >> 12;
	for (pair = 0; pair < 2; pair++)
	{
		unsigned first = (portion.inputs >> (pair ? 8 : 24)) & 0xff;
		unsigned second = (portion.inputs >> (pair ? 0 : 16)) & 0xff;
		int dot = !alpha && (flags & (pair ? 0x01 : 0x02));
		struct factor f, g;

		if (output != 2 && output != pair)
			continue;
		if (dot)
		{
			int channel;

			for (channel = LANE_R; channel <= LANE_B; channel++)
			{
				if (!input_factor(compiler, stage, first, alpha, PIPE_ALPHA, channel, &f) ||
					!input_factor(compiler, stage, second, alpha, PIPE_ALPHA, channel, &g) ||
					!add_factors(compiler, sum, 1.0f, &f, &g))
				{
					return 0;
				}
			}
		}
		else if (!input_factor(compiler, stage, first, alpha, pipe, lane, &f) ||
			!input_factor(compiler, stage, second, alpha, pipe, lane, &g) ||
			!add_factors(compiler, sum, 1.0f, &f, &g))
		{
			return 0;
		}
	}
	return !compiler->failure;
}

/* how the value written to a register part at a stage is read, until it is
written again: at all, signed */
static void readers(const struct compiler *compiler, int stage, int reg, int part, int *read, int *read_signed)
{
	const struct nv2a_combiners *combiners = compiler->combiners;
	int later, alpha, index;

	*read = *read_signed = 0;
	for (later = stage + 1; later < compiler->stage_count; later++)
	{
		int written = 0;

		for (alpha = 0; alpha < 2; alpha++)
		{
			struct portion portion;
			uint32_t flags;

			stage_portion(compiler, later, alpha, &portion);
			flags = portion.outputs >> 12;
			for (index = 0; index < 4; index++)
			{
				unsigned input = (portion.inputs >> (24 - 8 * index)) & 0xff;
				int input_part = (input & 0x10) ? PART_ALPHA : PART_RGB;

				if ((int)(input & 0x0f) != reg || input_part != part)
					continue;
				*read = 1;
				if ((input & 0xe0) >= _mapping_signed_identity)
					*read_signed = 1;
			}
			if (alpha && part == PART_ALPHA)
			{
				if ((int)(portion.outputs & 0x0f) == reg || (int)((portion.outputs >> 4) & 0x0f) == reg ||
					(int)((portion.outputs >> 8) & 0x0f) == reg)
				{
					written = 1;
				}
			}
			if (!alpha && part == PART_RGB)
			{
				if ((int)(portion.outputs & 0x0f) == reg || (int)((portion.outputs >> 4) & 0x0f) == reg ||
					(int)((portion.outputs >> 8) & 0x0f) == reg)
				{
					written = 1;
				}
			}
			if (!alpha && part == PART_ALPHA)
			{
				if (((flags & 0x80) && (int)((portion.outputs >> 4) & 0x0f) == reg) ||
					((flags & 0x40) && (int)(portion.outputs & 0x0f) == reg))
				{
					written = 1;
				}
			}
			/* the multiplexer reads r0's alpha */
			if ((flags & 0x04) && reg == _register_r0 && part == PART_ALPHA)
				*read = 1;
		}
		if (written)
			return;
	}
	/* the final combiner */
	{
		uint32_t abcd = combiners->final_inputs_abcd, efg = combiners->final_inputs_efg;

		if (!abcd && !efg)
		{
			if (reg == _register_r0)
				*read = 1;
			return;
		}
		for (index = 0; index < 7; index++)
		{
			unsigned input = index < 4 ? (abcd >> (24 - 8 * index)) & 0xff : (efg >> (24 - 8 * (index - 4))) & 0xff;
			int input_part = (input & 0x10) ? PART_ALPHA : PART_RGB;
			int input_reg = input & 0x0f;

			if (input_reg == reg && input_part == part)
				*read = 1;
			/* v1r0_sum reads v1's and r0's rgb */
			if (input_reg == _register_v1r0_sum && part == PART_RGB &&
				(reg == _register_v1 || reg == _register_r0))
			{
				*read = 1;
			}
		}
	}
}

/* a portion output's sum mapped and in its storage form; the value's form
fields filled in */
static int store_sum(struct compiler *compiler, struct sum *sum, uint32_t flags, int biased, int pipe,
	struct value *value)
{
	float bias, scale;
	int slot;

	output_mapping(flags, &bias, &scale);
	sum->constant += bias;
	sum_scale(sum, scale);
	sum_range(sum);
	value->minimum = sum->minimum < -1.0f ? -1.0f : sum->minimum;
	value->maximum = sum->maximum > 1.0f ? 1.0f : sum->maximum;
	/* (2: biased whatever the range, for the multiplexer's two sides) */
	if (value->minimum >= 0.0f && biased != 2)
		biased = 0;
	if (biased)
	{
		sum_scale(sum, 0.5f);
		sum->constant += 0.5f;
	}
	else if (value->minimum < 0.0f)
	{
		/* read only unsigned: clamped at 0 loses nothing */
		value->minimum = 0.0f;
	}
	value->biased = (uint8_t)(biased != 0);
	slot = sum_compute(compiler, pipe, sum, 1);
	value->slot = (short)slot;
	value->kind = VALUE_SLOT;
	return slot != NONE;
}

/* a multiplexed sum: r0's alpha (its top bit) picks CD over AB. The output
mapping and the clamp after it are the same for both, so each side is mapped
and stored first, in one form, and then picked */
static int multiplexed(struct compiler *compiler, int stage, int alpha, int pipe, int lane, uint32_t flags,
	int biased, struct value *value)
{
	struct sum products[2];
	struct value parts[2];
	struct operand condition, half, zero = zero_operand();
	int index, slot, compare = pipe == PIPE_COLOR ? TEV_COMP_R8_GT : TEV_COMP_A8_GT;
	int negative = 0;

	if (!compiler->mux_msb)
		compiler->program->approximations |= _nv2a_tev_approximation_mux_lsb;
	for (index = 0; index < 2; index++)
	{
		float bias, scale;

		memset(&products[index], 0, sizeof(products[index]));
		if (!portion_sum(compiler, stage, alpha, index, pipe, lane, &products[index]))
			return 0;
		output_mapping(flags, &bias, &scale);
		sum_range(&products[index]);
		if ((products[index].minimum + bias) * scale < 0.0f)
			negative = 1;
	}
	for (index = 0; index < 2; index++)
	{
		memset(&parts[index], 0, sizeof(parts[index]));
		if (!store_sum(compiler, &products[index], flags, biased && negative ? 2 : 0, pipe, &parts[index]))
			return 0;
	}
	if (!unsigned_operand(compiler, compiler->registers[stage][_register_r0][PART_ALPHA], pipe, LANE_A, &condition))
		return 0;
	/* (r0.a > 127 ? CD : 0) + (128 > r0.a ? AB : 0) */
	half = number_operand(compiler, 127.0f / 255.0f);
	slot = operation(compiler, pipe, condition, half, operand(OPERAND_SLOT, parts[1].slot, LANE_RGB), zero,
		compare, TB_ZERO, CS_SCALE_1, 0);
	if (slot == NONE)
		return 0;
	half = pipe == PIPE_COLOR ? operand(OPERAND_HALF, 0, LANE_RGB) : number_operand(compiler, 128.0f / 255.0f);
	slot = operation(compiler, pipe, half, condition, operand(OPERAND_SLOT, parts[0].slot, LANE_RGB),
		operand(OPERAND_SLOT, slot, LANE_RGB), compare, TB_ZERO, CS_SCALE_1, 0);
	if (slot == NONE)
		return 0;
	value->kind = VALUE_SLOT;
	value->slot = (short)slot;
	value->biased = parts[0].biased;
	value->minimum = parts[0].minimum < parts[1].minimum ? parts[0].minimum : parts[1].minimum;
	value->maximum = parts[0].maximum > parts[1].maximum ? parts[0].maximum : parts[1].maximum;
	return 1;
}

/* computes one output of a portion (0 AB, 1 CD, 2 sum) as a new value, in a
pipe and lane; biased: to be kept as (x + 1) / 2 if it can be negative */
static int portion_output(struct compiler *compiler, int stage, int alpha, int output, int pipe, int lane,
	int biased, int scalar)
{
	struct portion portion;
	uint32_t flags;
	int value_index;
	struct value result;

	stage_portion(compiler, stage, alpha, &portion);
	flags = portion.outputs >> 12;
	memset(&result, 0, sizeof(result));
	if (output == 2 && (flags & 0x04))
	{
		if (!multiplexed(compiler, stage, alpha, pipe, lane, flags, biased, &result))
			return NONE;
	}
	else
	{
		struct sum sum;

		memset(&sum, 0, sizeof(sum));
		if (!portion_sum(compiler, stage, alpha, output, pipe, lane, &sum) ||
			!store_sum(compiler, &sum, flags, biased, pipe, &result))
		{
			return NONE;
		}
	}
	value_index = new_value(compiler, VALUE_SLOT, alpha ? PART_ALPHA : PART_RGB, 0);
	if (value_index == NONE)
		return NONE;
	{
		struct value *value = &compiler->values[value_index];

		value->slot = result.slot;
		value->biased = result.biased;
		value->minimum = result.minimum;
		value->maximum = result.maximum;
		value->scalar = (uint8_t)scalar;
		value->stage = (short)stage;
		value->output = (uint8_t)output;
	}
	return value_index;
}

static int final_value(struct compiler *compiler, int which, int pipe, int lane);

/* a channel of a vector rgb value, computed in the alpha pipe from its
expression */
static int compute_lane(struct compiler *compiler, int value_index, int lane)
{
	struct value *value = &compiler->values[value_index];
	int result;

	if (value->lanes[lane] != NONE)
		return value->lanes[lane];
	if (value->stage == NONE)
		return fail(compiler, "internal: a lane of a value with no expression");
	if (value->output >= 3)
	{
		result = final_value(compiler, value->output, PIPE_ALPHA, lane);
	}
	else
	{
		result = portion_output(compiler, value->stage, 0, value->output, PIPE_ALPHA, lane, value->biased, 0);
		if (result != NONE)
		{
			compiler->values[result].biased = compiler->values[value_index].biased;
			compiler->values[result].part = PART_ALPHA;
		}
	}
	value = &compiler->values[value_index];
	value->lanes[lane] = (short)result;
	return result;
}

/* the writes of one general combiner stage */
static int combiner_stage(struct compiler *compiler, int stage)
{
	short (*before)[2] = compiler->registers[stage];
	short (*after)[2] = compiler->registers[stage + 1];
	struct write
	{
		int reg, part, alpha, output, blue;
	} writes[12];
	int write_count = 0, index, alpha;

	memcpy(after, before, sizeof(compiler->registers[0]));
	/* in the NV2A's order: the rgb portion's AB, its blue to alpha, CD, its
	blue, the sum; then the alpha portion's */
	for (alpha = 0; alpha < 2; alpha++)
	{
		struct portion portion;
		uint32_t flags;
		int output;

		stage_portion(compiler, stage, alpha, &portion);
		flags = portion.outputs >> 12;
		for (output = 0; output < 3; output++)
		{
			int reg = output == 0 ? (portion.outputs >> 4) & 0x0f : output == 1 ? portion.outputs & 0x0f :
				(portion.outputs >> 8) & 0x0f;

			if (reg != _register_v0 && reg != _register_v1 && (reg < _register_t0 || reg > _register_r1))
				continue;
			writes[write_count].reg = reg;
			writes[write_count].part = alpha ? PART_ALPHA : PART_RGB;
			writes[write_count].alpha = alpha;
			writes[write_count].output = output;
			writes[write_count++].blue = 0;
			if (!alpha && output < 2 && (flags & (output == 0 ? 0x80 : 0x40)))
			{
				writes[write_count].reg = reg;
				writes[write_count].part = PART_ALPHA;
				writes[write_count].alpha = 0;
				writes[write_count].output = output;
				writes[write_count++].blue = 1;
			}
		}
	}
	for (index = 0; index < write_count; index++)
	{
		const struct write *write = &writes[index];
		int later, read, read_signed, value_index;

		/* a later write to the same part replaces it */
		for (later = index + 1; later < write_count; later++)
		{
			if (writes[later].reg == write->reg && writes[later].part == write->part)
				break;
		}
		if (later < write_count)
			continue;
		readers(compiler, stage, write->reg, write->part, &read, &read_signed);
		if (!read)
		{
			/* not computed; nothing reads it (zero, should anything) */
			after[write->reg][write->part] = compiler->zero_value;
			continue;
		}
		if (write->alpha)
		{
			value_index = portion_output(compiler, stage, 1, write->output, PIPE_ALPHA, LANE_A, read_signed, 0);
		}
		else
		{
			struct portion portion;
			uint32_t flags;
			int pair, scalar = 1, dot = 0;

			stage_portion(compiler, stage, 0, &portion);
			flags = portion.outputs >> 12;
			/* scalar when each product is a dot product or of two scalars */
			for (pair = 0; pair < 2; pair++)
			{
				int pair_dot = (flags & (pair ? 0x01 : 0x02)) != 0;

				if (write->output != 2 && write->output != pair)
					continue;
				dot |= pair_dot;
				if (!pair_dot &&
					(!input_is_scalar(compiler, stage, (portion.inputs >> (pair ? 8 : 24)) & 0xff) ||
					!input_is_scalar(compiler, stage, (portion.inputs >> (pair ? 0 : 16)) & 0xff)))
				{
					scalar = 0;
				}
			}
			if (write->blue || scalar)
			{
				/* in the alpha pipe: the blue written to alpha, and the scalars */
				value_index = portion_output(compiler, stage, 0, write->output, PIPE_ALPHA, LANE_B, read_signed, 1);
			}
			else if (dot)
			{
				return fail(compiler, "a dot product summed with a vector") != NONE;
			}
			else
			{
				value_index = portion_output(compiler, stage, 0, write->output, PIPE_COLOR, LANE_RGB, read_signed, 0);
			}
			if (value_index != NONE)
				compiler->values[value_index].part = write->part;
		}
		if (value_index == NONE)
			return 0;
		after[write->reg][write->part] = (short)value_index;
	}
	return !compiler->failure;
}

/* ---------- the final combiner */

/* a final combiner input as an atom: clamped to [0, 1], perhaps inverted */
static int final_atom(struct compiler *compiler, unsigned input, int pipe, int lane, struct operand *atom)
{
	int value_index;
	int alpha_channel = (input & 0x10) != 0;

	if (pipe == PIPE_ALPHA && !alpha_channel)
		lane = LANE_B;
	if (pipe == PIPE_COLOR && alpha_channel)
		lane = LANE_A;
	if ((input & 0x0f) == _register_v1r0_sum || (input & 0x0f) == _register_ef_product)
	{
		if (alpha_channel)
		{
			value_index = compiler->zero_value;
		}
		else
		{
			value_index = final_value(compiler, (input & 0x0f) == _register_ef_product ? 3 : 4, pipe, lane);
			if (value_index == NONE)
				return 0;
		}
	}
	else
	{
		value_index = input_value(compiler, compiler->stage_count, input, pipe == PIPE_ALPHA, 1);
		if (value_index == NONE)
			return 0;
	}
	if (compiler->values[value_index].kind == VALUE_ZERO)
		*atom = zero_operand();
	else if (!unsigned_operand(compiler, value_index, pipe, lane, atom))
		return 0;
	if (input & 0x20)
		atom->complement = 1;
	return 1;
}

/* EF (3) or v1r0_sum (4) as a value: a vector in the color pipe, or a
channel in the alpha pipe */
static int final_value(struct compiler *compiler, int which, int pipe, int lane)
{
	uint32_t efg = compiler->combiners->final_inputs_efg;
	short *cache = which == 3 ? &compiler->final_ef : &compiler->final_v1r0_sum;
	struct sum sum;
	struct value *value;
	int value_index, slot;

	if (pipe == PIPE_COLOR && *cache != NONE)
		return *cache;
	memset(&sum, 0, sizeof(sum));
	if (which == 3)
	{
		struct operand e, f;

		if (!final_atom(compiler, (efg >> 24) & 0xff, pipe, lane, &e) ||
			!final_atom(compiler, (efg >> 16) & 0xff, pipe, lane, &f) ||
			!add_product(compiler, &sum, 1.0f, e, f))
		{
			return NONE;
		}
	}
	else
	{
		unsigned settings = efg & 0xff;
		struct operand v1, r0;

		if (!final_atom(compiler, _register_v1 | ((settings & 0x40) ? 0x20 : 0), pipe, lane, &v1) ||
			!final_atom(compiler, _register_r0 | ((settings & 0x20) ? 0x20 : 0), pipe, lane, &r0) ||
			!add_single(compiler, &sum, 1.0f, v1) || !add_single(compiler, &sum, 1.0f, r0))
		{
			return NONE;
		}
	}
	slot = sum_compute(compiler, pipe, &sum, 1);
	if (slot == NONE)
		return NONE;
	value_index = new_value(compiler, VALUE_SLOT, pipe == PIPE_ALPHA ? PART_ALPHA : PART_RGB, 0);
	if (value_index == NONE)
		return NONE;
	value = &compiler->values[value_index];
	value->slot = (short)slot;
	value->stage = (short)compiler->stage_count;
	value->output = (uint8_t)which;
	if (pipe == PIPE_COLOR)
		*cache = (short)value_index;
	return value_index;
}

/* the result into PREV: one pipe's final sum */
static int final_result(struct compiler *compiler, int pipe, struct sum *sum)
{
	int slot = sum_compute(compiler, pipe, sum, 1);

	if (slot == NONE)
		return 0;
	compiler->slots[slot].final = 1;
	return 1;
}

static int final_combiner(struct compiler *compiler)
{
	uint32_t abcd = compiler->combiners->final_inputs_abcd, efg = compiler->combiners->final_inputs_efg;
	struct sum color, alpha;

	memset(&color, 0, sizeof(color));
	memset(&alpha, 0, sizeof(alpha));
	if (!abcd && !efg)
	{
		/* r0 as it is */
		struct operand rgb, a;

		if (!final_atom(compiler, _register_r0, PIPE_COLOR, LANE_RGB, &rgb) ||
			!final_atom(compiler, _register_r0 | 0x10, PIPE_ALPHA, LANE_A, &a) ||
			!add_single(compiler, &color, 1.0f, rgb) || !add_single(compiler, &alpha, 1.0f, a))
		{
			return 0;
		}
	}
	else
	{
		struct operand a, b, c, d, g;

		/* A*B + (1-A)*C + D, D first, as the chain's d where it can be */
		if (!final_atom(compiler, (abcd >> 24) & 0xff, PIPE_COLOR, LANE_RGB, &a) ||
			!final_atom(compiler, (abcd >> 16) & 0xff, PIPE_COLOR, LANE_RGB, &b) ||
			!final_atom(compiler, (abcd >> 8) & 0xff, PIPE_COLOR, LANE_RGB, &c) ||
			!final_atom(compiler, abcd & 0xff, PIPE_COLOR, LANE_RGB, &d) ||
			!final_atom(compiler, (efg >> 8) & 0xff, PIPE_ALPHA, LANE_A, &g) ||
			!add_single(compiler, &color, 1.0f, d))
		{
			return 0;
		}
		if (a.kind == OPERAND_ZERO)
		{
			/* A is 0 or 1: C or B */
			if (!add_single(compiler, &color, 1.0f, a.complement ? b : c))
				return 0;
		}
		else if (b.kind == OPERAND_ZERO && !b.complement)
		{
			struct operand inverted = a;

			inverted.complement = (uint8_t)!a.complement;
			if (!add_product(compiler, &color, 1.0f, inverted, c))
				return 0;
		}
		else if (c.kind == OPERAND_ZERO && !c.complement)
		{
			if (!add_product(compiler, &color, 1.0f, a, b))
				return 0;
		}
		else if (!add_term(compiler, &color, 1.0f, c, b, a))
		{
			return 0;
		}
		if (!add_single(compiler, &alpha, 1.0f, g))
			return 0;
	}
	return final_result(compiler, PIPE_ALPHA, &alpha) && final_result(compiler, PIPE_COLOR, &color);
}

/* ---------- stages */

struct stage_needs
{
	int texture, texture_lane, color, color_lane;
};

static int operation_needs(const struct operation *operation, struct stage_needs *needs)
{
	int index;

	for (index = 0; index < 4; index++)
	{
		const struct operand *input = &operation->inputs[index];
		int lane = lane_need(operation->pipe, input);

		if (texture_need(input) != NONE)
		{
			if (needs->texture != NONE && needs->texture != texture_need(input))
				return 0;
			needs->texture = texture_need(input);
			if (lane != NONE)
			{
				if (needs->texture_lane != NONE && needs->texture_lane != lane)
					return 0;
				needs->texture_lane = lane;
			}
		}
		if (color_need(input) != NONE)
		{
			if (needs->color != NONE && needs->color != color_need(input))
				return 0;
			needs->color = color_need(input);
			if (lane != NONE)
			{
				if (needs->color_lane != NONE && needs->color_lane != lane)
					return 0;
				needs->color_lane = lane;
			}
		}
	}
	return 1;
}

static int ready(const struct compiler *compiler, const struct operation *operation, int stage)
{
	int index;

	for (index = 0; index < 4; index++)
	{
		const struct operand *input = &operation->inputs[index];

		if (input->kind == OPERAND_SLOT)
		{
			int definition = compiler->slots[input->index].definition;

			if (definition != NONE && (compiler->operations[definition].stage == NONE ||
				compiler->operations[definition].stage >= stage))
			{
				return 0;
			}
		}
	}
	return 1;
}

/* pairs the pipes' operations into stages, in each pipe's order */
#define MAXIMUM_SCHEDULE 64

static int schedule(struct compiler *compiler, short stage_operations[MAXIMUM_SCHEDULE][2])
{
	int next[2] = { 0, 0 };
	int stage = 0, remaining = compiler->operation_count;

	while (remaining > 0)
	{
		int chosen[2] = { NONE, NONE };
		int pipe;

		if (stage == MAXIMUM_SCHEDULE)
			return fail(compiler, "more than 64 TEV stages") != NONE;
		for (pipe = 0; pipe < 2; pipe++)
		{
			while (next[pipe] < compiler->operation_count && compiler->operations[next[pipe]].pipe != pipe)
				next[pipe]++;
			if (next[pipe] < compiler->operation_count && ready(compiler, &compiler->operations[next[pipe]], stage))
				chosen[pipe] = next[pipe];
		}
		if (chosen[0] != NONE && chosen[1] != NONE)
		{
			struct stage_needs needs = { NONE, NONE, NONE, NONE };

			if (!operation_needs(&compiler->operations[chosen[0]], &needs) ||
				!operation_needs(&compiler->operations[chosen[1]], &needs))
			{
				/* the earlier one now, the other later */
				chosen[chosen[0] < chosen[1] ? 1 : 0] = NONE;
			}
		}
		if (chosen[0] == NONE && chosen[1] == NONE)
			return fail(compiler, "internal: nothing ready") != NONE;
		for (pipe = 0; pipe < 2; pipe++)
		{
			stage_operations[stage][pipe] = (short)chosen[pipe];
			if (chosen[pipe] != NONE)
			{
				compiler->operations[chosen[pipe]].stage = (short)stage;
				next[pipe]++;
				remaining--;
			}
		}
		stage++;
	}
	compiler->program->stage_count = stage;
	if (stage > NV2A_TEV_MAXIMUM_STAGES)
	{
		static char why[48];

		snprintf(why, sizeof(why), "%d TEV stages, more than 16", stage);
		return fail(compiler, why) != NONE;
	}
	return 1;
}

/* gives the slots registers, PREV kept for the results */
static int allocate(struct compiler *compiler)
{
	int slot, pipe, stage, index;
	int stage_count = compiler->program->stage_count;

	for (slot = 0; slot < compiler->slot_count; slot++)
	{
		struct slot *s = &compiler->slots[slot];

		s->first_use_stage = s->definition == NONE ? -1 : compiler->operations[s->definition].stage;
		s->last_use_stage = s->first_use_stage;
	}
	for (index = 0; index < compiler->operation_count; index++)
	{
		const struct operation *operation = &compiler->operations[index];
		int input;

		for (input = 0; input < 4; input++)
		{
			if (operation->inputs[input].kind == OPERAND_SLOT)
			{
				struct slot *s = &compiler->slots[operation->inputs[input].index];

				if (s->last_use_stage < operation->stage)
					s->last_use_stage = operation->stage;
			}
		}
	}
	for (pipe = 0; pipe < 2; pipe++)
	{
		short holder[4] = { NONE, NONE, NONE, NONE };

		for (stage = -1; stage < stage_count; stage++)
		{
			int r;

			/* registers whose values were last read at this stage are free for
			its writes (a stage reads, then writes) */
			for (r = 0; r < 4; r++)
			{
				if (holder[r] != NONE && compiler->slots[holder[r]].last_use_stage <= stage &&
					compiler->slots[holder[r]].first_use_stage < stage)
				{
					holder[r] = NONE;
				}
			}
			for (slot = 0; slot < compiler->slot_count; slot++)
			{
				struct slot *s = &compiler->slots[slot];

				if (s->pipe != pipe || s->first_use_stage != stage)
					continue;
				if (s->final)
				{
					if (holder[0] != NONE)
						return fail(compiler, "PREV still in use at the end") != NONE;
					r = 0;
				}
				else
				{
					/* REG0-2 first, PREV last */
					for (r = 1; r <= 4; r++)
					{
						if (holder[r & 3] == NONE)
							break;
					}
					if (r > 4)
						return fail(compiler, "more than four registers") != NONE;
					r &= 3;
				}
				holder[r] = (short)slot;
				s->register_index = (short)r;
			}
		}
	}
	return 1;
}

/* ---------- the program */

static uint8_t konst_select(struct compiler *compiler, int pipe, const struct operand *input)
{
	int k;

	if (input->kind == OPERAND_FRACTION)
		return (uint8_t)input->index;
	if (input->kind == OPERAND_ONE)
		return 0;
	if (input->kind == OPERAND_HALF)
		return 4;
	k = compiler->constant_register[input->index];
	if (pipe == PIPE_COLOR && input->lane == LANE_RGB)
		return (uint8_t)(KSEL_K0 + k);
	return (uint8_t)(KSEL_K0_R + 4 * input->lane + k);
}

static uint8_t color_input(struct compiler *compiler, const struct operand *input, struct nv2a_tev_stage *stage)
{
	switch (input->kind)
	{
	case OPERAND_ZERO: return CC_ZERO;
	case OPERAND_ONE: return CC_ONE;
	case OPERAND_HALF: return CC_HALF;
	case OPERAND_TEXTURE: return input->lane == LANE_A ? CC_TEXA : CC_TEXC;
	case OPERAND_COLOR: return input->lane == LANE_A ? CC_RASA : CC_RASC;
	case OPERAND_CONSTANT:
	case OPERAND_FRACTION:
		stage->konst_color = konst_select(compiler, PIPE_COLOR, input);
		return CC_KONST;
	default:
	{
		const struct slot *slot = &compiler->slots[input->index];

		return (uint8_t)(2 * slot->register_index + (slot->pipe == PIPE_ALPHA ? 1 : 0));
	}
	}
}

static uint8_t alpha_input(struct compiler *compiler, const struct operand *input, struct nv2a_tev_stage *stage)
{
	switch (input->kind)
	{
	case OPERAND_ZERO: return CA_ZERO;
	case OPERAND_TEXTURE: return CA_TEXA;
	case OPERAND_COLOR: return CA_RASA;
	case OPERAND_ONE:
	case OPERAND_HALF:
	case OPERAND_CONSTANT:
	case OPERAND_FRACTION:
		stage->konst_alpha = konst_select(compiler, PIPE_ALPHA, input);
		return CA_KONST;
	default:
		return (uint8_t)compiler->slots[input->index].register_index;
	}
}

/* gives each constant a constant register, or else a register's initial
value */
static int place_constants(struct compiler *compiler)
{
	int index, input, next = 0;

	for (index = 0; index < compiler->operation_count; index++)
	{
		for (input = 0; input < 4; input++)
		{
			const struct operand *operand_ = &compiler->operations[index].inputs[input];

			if (operand_->kind != OPERAND_CONSTANT || compiler->constant_register[operand_->index] != NONE)
				continue;
			if (next == 4)
				return fail(compiler, "more than four constants") != NONE;
			compiler->constant_register[operand_->index] = (short)next;
			compiler->program->konst[next++] = compiler->constants[operand_->index];
		}
	}
	return 1;
}

static void emit(struct compiler *compiler, short stage_operations[MAXIMUM_SCHEDULE][2])
{
	struct nv2a_tev_program *program = compiler->program;
	int stage_index, pipe;

	for (stage_index = 0; stage_index < program->stage_count; stage_index++)
	{
		struct nv2a_tev_stage *stage = &program->stages[stage_index];
		struct stage_needs needs = { NONE, NONE, NONE, NONE };

		memset(stage, 0, sizeof(*stage));
		stage->texture = 0xff;
		stage->color = 0xff;
		for (pipe = 0; pipe < 2; pipe++)
		{
			struct nv2a_tev_combine *combine = pipe == PIPE_COLOR ? &stage->rgb : &stage->alpha;
			int index = stage_operations[stage_index][pipe];
			const struct operation *operation;
			int input;
			uint8_t *fields[4];

			fields[0] = &combine->a;
			fields[1] = &combine->b;
			fields[2] = &combine->c;
			fields[3] = &combine->d;
			if (index == NONE)
			{
				/* nothing: PREV kept as it is */
				combine->a = combine->b = combine->c = pipe == PIPE_COLOR ? CC_ZERO : CA_ZERO;
				combine->d = pipe == PIPE_COLOR ? CC_CPREV : CA_APREV;
				combine->dest = 0;
				continue;
			}
			operation = &compiler->operations[index];
			operation_needs(operation, &needs);
			for (input = 0; input < 4; input++)
			{
				*fields[input] = pipe == PIPE_COLOR ? color_input(compiler, &operation->inputs[input], stage) :
					alpha_input(compiler, &operation->inputs[input], stage);
			}
			combine->op = operation->op;
			combine->bias = operation->bias;
			combine->scale = operation->scale;
			combine->clamp = operation->clamp;
			combine->dest = (uint8_t)compiler->slots[operation->destination].register_index;
		}
		if (needs.texture != NONE)
		{
			stage->texture = (uint8_t)needs.texture;
			stage->texture_swap = (uint8_t)(needs.texture_lane == NONE || needs.texture_lane == LANE_A ? 0 :
				needs.texture_lane + 1);
		}
		if (needs.color != NONE)
		{
			stage->color = (uint8_t)needs.color;
			stage->color_swap = (uint8_t)(needs.color_lane == NONE || needs.color_lane == LANE_A ? 0 :
				needs.color_lane + 1);
		}
	}
}

int nv2a_tev_compile(const struct nv2a_combiners *combiners, const struct nv2a_tev_options *options,
	struct nv2a_tev_program *program, const char **failure)
{
	static struct compiler compiler_storage;
	struct compiler *compiler = &compiler_storage;
	short stage_operations[MAXIMUM_SCHEDULE][2];
	int stage, reg, table;

	memset(compiler, 0, sizeof(*compiler));
	memset(program, 0, sizeof(*program));
	compiler->combiners = combiners;
	compiler->options = options;
	compiler->program = program;
	compiler->final_ef = compiler->final_v1r0_sum = NONE;
	compiler->stage_count = (int)(combiners->combiner_count & 0xff);
	if (compiler->stage_count > 8)
		compiler->stage_count = 8;
	compiler->unique_c0 = (combiners->combiner_count & 0x1000) != 0;
	compiler->unique_c1 = (combiners->combiner_count & 0x10000) != 0;
	compiler->mux_msb = (combiners->combiner_count & 0x100) != 0;
	/* table n leaves red, green and blue and puts channel n - 1 in alpha;
	table 0 is as it is */
	for (table = 0; table < 4; table++)
	{
		program->swap_tables[table][0] = CH_RED;
		program->swap_tables[table][1] = CH_GREEN;
		program->swap_tables[table][2] = CH_BLUE;
		program->swap_tables[table][3] = (uint8_t)(table == 0 ? CH_ALPHA : table - 1);
	}

	/* the registers at the start: textures, colors, r0's alpha is t0's */
	compiler->zero_value = (short)new_value(compiler, VALUE_ZERO, PART_RGB, 0);
	for (reg = 0; reg < REGISTER_COUNT; reg++)
		compiler->registers[0][reg][PART_RGB] = compiler->registers[0][reg][PART_ALPHA] = compiler->zero_value;
	for (stage = 0; stage < 4; stage++)
	{
		unsigned mode = (combiners->texture_modes >> (5 * stage)) & 0x1f;
		int sampled = mode != 0 && mode != 0x04 && mode != 0x05 && mode != 0x0a && mode != 0x11 &&
			(options->textures_sampled & (1 << stage));

		if (mode == 0x04 || (mode > 0x03 && sampled))
			program->approximations |= _nv2a_tev_approximation_texture_mode;
		if (mode == 0x03)
			program->approximations |= _nv2a_tev_approximation_texture_mode;
		if (!sampled)
			continue;
		compiler->texture_present[stage] = 1;
		compiler->registers[0][_register_t0 + stage][PART_RGB] = (short)new_value(compiler, VALUE_TEXTURE, PART_RGB, stage);
		compiler->registers[0][_register_t0 + stage][PART_ALPHA] = (short)new_value(compiler, VALUE_TEXTURE, PART_ALPHA, stage);
	}
	compiler->registers[0][_register_r0][PART_ALPHA] = compiler->registers[0][_register_t0][PART_ALPHA];
	for (reg = 0; reg < 2; reg++)
	{
		compiler->registers[0][_register_v0 + reg][PART_RGB] = (short)new_value(compiler, VALUE_COLOR, PART_RGB, reg);
		compiler->registers[0][_register_v0 + reg][PART_ALPHA] = (short)new_value(compiler, VALUE_COLOR, PART_ALPHA, reg);
	}

	for (stage = 0; stage < compiler->stage_count; stage++)
	{
		if (!combiner_stage(compiler, stage))
			break;
	}
	if (!compiler->failure)
		final_combiner(compiler);
#ifdef NV2A_TEV_TRACE
	{
		/* the operations, before they are paired (the host test, -v) */
		int i, j;
		static const char *kinds[] = { "0", "1", "1/2", "t", "v", "k", "f", "s" };
		static const char *lanes = "rgba*";

		for (i = 0; i < compiler->operation_count; i++)
		{
			const struct operation *o = &compiler->operations[i];

			printf("  op %2d %s s%-3d =", i, o->pipe ? "A" : "C", o->destination);
			for (j = 0; j < 4; j++)
				printf(" %s%d%c%s", kinds[o->inputs[j].kind], o->inputs[j].index, lanes[o->inputs[j].lane],
					o->inputs[j].complement ? "~" : "");
			printf("  op%d b%d s%d c%d\n", o->op, o->bias, o->scale, o->clamp);
		}
	}
#endif
	if (!compiler->failure)
		schedule(compiler, stage_operations);
	if (!compiler->failure)
		allocate(compiler);
	if (!compiler->failure)
		place_constants(compiler);
	if (compiler->failure)
	{
		if (failure)
			*failure = compiler->failure;
		return 0;
	}
	emit(compiler, stage_operations);
	if (failure)
		*failure = NULL;
	return 1;
}

void nv2a_tev_constant_value(const struct nv2a_tev_constant *constant, const uint32_t c0[8], const uint32_t c1[8],
	uint32_t final_c0, uint32_t final_c1, uint32_t fog_color, uint8_t rgba[4])
{
	uint32_t color;

	switch (constant->source)
	{
	case _nv2a_constant_c0: color = c0[constant->index & 7]; break;
	case _nv2a_constant_c1: color = c1[constant->index & 7]; break;
	case _nv2a_constant_final_c0: color = final_c0; break;
	case _nv2a_constant_final_c1: color = final_c1; break;
	case _nv2a_constant_fog: color = fog_color | 0xff000000UL; break;
	case _nv2a_constant_literal:
		memcpy(rgba, constant->value, 4);
		return;
	default: color = 0; break;
	}
	rgba[0] = (uint8_t)(color >> 16);
	rgba[1] = (uint8_t)(color >> 8);
	rgba[2] = (uint8_t)color;
	rgba[3] = (uint8_t)(color >> 24);
}

void nv2a_tev_constant_classes(const struct nv2a_tev_options *options, uint32_t classes[5])
{
	int index, channel;

	memset(classes, 0, 5 * sizeof(uint32_t));
	for (index = 0; index < 18; index++)
	{
		uint32_t color = index < 8 ? options->c0[index] : index < 16 ? options->c1[index - 8] :
			index == 16 ? options->final_c0 : options->final_c1;

		for (channel = 0; channel < 4; channel++)
		{
			unsigned byte = (color >> (8 * channel)) & 0xff;
			unsigned bits = byte == 0 ? 1 : byte == 255 ? 2 : 0;
			int bit = (index * 4 + channel) * 2;

			classes[bit / 32] |= bits << (bit % 32);
		}
	}
}
