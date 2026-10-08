/*
VSH_DIS.C

Prints the game's vertex programs (rasterizer_xbox_vertex_shaders_data.inc)
as the interpreter (port/wii/src/nv2a_vsh_run.c) decodes them, one
instruction a line, with constants named as Direct3D numbers them (c[-96]
is the hardware's 0). For reading what a program does with its constants.

    cc -Iport/wii/src -Isource/rasterizer/xbox -o vsh_dis port/wii/tests/vsh_dis.c port/wii/src/nv2a_vsh_run.c -lm
    ./vsh_dis 10       one program; no argument prints all 67
*/

#include "nv2a_vsh_run.h"
#include "vsh_programs.h"

#include <stdio.h>
#include <stdlib.h>

static const char *mac_names[] = { "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4", "dst", "min", "max", "slt", "sge", "arl" };
static const char *ilu_names[] = { "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit" };
static const char *output_names[] = { "oPos", "o1", "o2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", "o13", "o14", "o15" };

static void print_mask(unsigned mask)
{
	putchar('.');
	if (mask & 8) putchar('x');
	if (mask & 4) putchar('y');
	if (mask & 2) putchar('z');
	if (mask & 1) putchar('w');
}

static void print_operand(const struct nv2a_vsh_instruction *instruction, int which)
{
	const struct nv2a_vsh_operand *operand = &instruction->operands[which];
	int component;

	printf(", %s", operand->negate ? "-" : "");
	switch (operand->mux)
	{
	case 1: printf(operand->index == 12 ? "oPos" : "r%d", operand->index); break;
	case 2: printf("v%d", instruction->input); break;
	case 3:
		if (instruction->relative)
			printf("c[a0%+d]", instruction->constant - NV2A_VSH_CONSTANT_BIAS);
		else
			printf("c[%d]", instruction->constant - NV2A_VSH_CONSTANT_BIAS);
		break;
	default: printf("?"); break;
	}
	if (operand->swizzle[0] != 0 || operand->swizzle[1] != 1 || operand->swizzle[2] != 2 || operand->swizzle[3] != 3)
	{
		putchar('.');
		for (component = 0; component < 4; component++)
			putchar("xyzw"[operand->swizzle[component]]);
	}
}

static void print_destinations(const struct nv2a_vsh_instruction *instruction, int ilu)
{
	int any = 0;

	if (ilu ? instruction->ilu_mask : instruction->mac_mask)
	{
		unsigned temporary = ilu ? (instruction->mac ? 1 : instruction->temporary) : instruction->temporary;

		printf("r%u", temporary);
		print_mask(ilu ? instruction->ilu_mask : instruction->mac_mask);
		any = 1;
	}
	if (instruction->output_mask && instruction->output_is_register && !!instruction->output_from_ilu == !!ilu)
	{
		printf("%s%s", any ? "+" : "", output_names[instruction->output_address & 15]);
		print_mask(instruction->output_mask);
		any = 1;
	}
	if (!any)
		printf("_");
}

static void print_program(int index)
{
	const uint32_t *words = vsh_program_words(index);
	struct nv2a_vsh_program program;
	unsigned long count = words[0] >> 16, at;

	nv2a_vsh_decode(words + 1, count, &program);
	printf("program %d: %lu instructions\n", index, program.count);
	for (at = 0; at < program.count; at++)
	{
		const struct nv2a_vsh_instruction *instruction = &program.instructions[at];

		printf("%3lu ", at);
		if (instruction->mac)
		{
			printf(" %s ", mac_names[instruction->mac]);
			if (instruction->mac == 13)
				printf("a0.x");
			else
				print_destinations(instruction, 0);
			print_operand(instruction, 0);
			if (instruction->mac != 1 && instruction->mac != 13 && instruction->mac != 3)
				print_operand(instruction, 1);
			if (instruction->mac == 4 || instruction->mac == 3)
				print_operand(instruction, 2);
		}
		if (instruction->ilu)
		{
			printf("%s %s ", instruction->mac ? " +" : "", ilu_names[instruction->ilu]);
			print_destinations(instruction, 1);
			print_operand(instruction, 2);
		}
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	int index;

	if (argc > 1)
	{
		print_program(atoi(argv[1]));
		return 0;
	}
	for (index = 0; index < VSH_PROGRAM_COUNT; index++)
		print_program(index);
	return 0;
}
