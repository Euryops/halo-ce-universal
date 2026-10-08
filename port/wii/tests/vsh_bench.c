/*
VSH_BENCH.C

How long the game's vertex programs take on the host, the interpreter
(nv2a_vsh_run) against the batch executor the device runs
(nv2a_vsh_run_batch, compiled for the outputs the device reads), in
nanoseconds a vertex. Host numbers are not the Wii's, but the ratio between
the two, and between programs, is a guide; skintest.dol measures the Wii's
(in Dolphin, its emulated clock).

    vsh_bench [program...]      default: the skinned model programs 10 9 17 27 15 and the grass's 33
*/

#include "nv2a_vsh_run.h"
#include "vsh_programs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static float constants[NV2A_VSH_CONSTANT_COUNT][4];

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

/* d3d8_gx.c's device_outputs */
static const uint8_t device_outputs[16] = { 0, 0, 0, 0xf, 0xf, 0x8, 0, 0, 0, 0xc, 0xc, 0xc, 0xc, 0, 0, 0 };

static void bench(int index)
{
	const uint32_t *words = vsh_program_words(index);
	static struct nv2a_vsh_program program;
	static struct nv2a_vsh_compiled compiled;
	static struct nv2a_vsh_lanes lanes;
	float inputs[NV2A_VSH_ATTRIBUTE_COUNT][4];
	struct nv2a_vsh_result result;
	const int vertices = 200000;
	double start, interpreter, batch;
	volatile float sink = 0.0f;
	int vertex, reg, component;
	unsigned long lane;

	nv2a_vsh_decode(words + 1, words[0] >> 16, &program);
	nv2a_vsh_compile(&program, device_outputs, &compiled);
	for (reg = 0; reg < NV2A_VSH_ATTRIBUTE_COUNT; reg++)
		for (component = 0; component < 4; component++)
			inputs[reg][component] = 0.1f * (reg + component);
	/* node indices in reach of the node matrices */
	inputs[5][0] = inputs[5][1] = 3.0f / 255.0f;
	for (reg = 0; reg < NV2A_VSH_ATTRIBUTE_COUNT; reg++)
		for (component = 0; component < 4; component++)
			for (lane = 0; lane < NV2A_VSH_BATCH; lane++)
				lanes.rows[NV2A_VSH_FILE_INPUTS + reg][component][lane] = inputs[reg][component];

	start = now();
	for (vertex = 0; vertex < vertices; vertex++)
	{
		nv2a_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, &result);
		sink += result.position[0];
	}
	interpreter = (now() - start) / vertices * 1e9;
	start = now();
	for (vertex = 0; vertex < vertices; vertex += NV2A_VSH_BATCH)
	{
		nv2a_vsh_run_batch(&compiled, (const float (*)[4])constants, &lanes, NV2A_VSH_BATCH);
		sink += lanes.rows[NV2A_VSH_FILE_POSITION][0][0];
	}
	batch = (now() - start) / vertices * 1e9;
	printf("program %2d: %3lu instructions, %3lu kept: interpreter %6.1f ns, batch %6.1f ns a vertex (%.1fx)\n",
		index, program.count, compiled.count, interpreter, batch, interpreter / batch);
	(void)sink;
}

int main(int argc, char **argv)
{
	static const int defaults[] = { 10, 9, 17, 27, 15, 33 };
	int index, row;

	for (row = 0; row < NV2A_VSH_CONSTANT_COUNT; row++)
		for (index = 0; index < 4; index++)
			constants[row][index] = 0.01f * ((row * 7 + index * 3) % 50) + 0.1f;
	constants[NV2A_VSH_CONSTANT_BIAS - 89][3] = 255.9375f;
	if (argc > 1)
		for (index = 1; index < argc; index++)
			bench(atoi(argv[index]));
	else
		for (index = 0; index < (int)(sizeof(defaults) / sizeof(defaults[0])); index++)
			bench(defaults[index]);
	return 0;
}
