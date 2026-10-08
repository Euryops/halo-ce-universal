#!/bin/bash
# The Wii port's host tests: the vertex program interpreter against the
# game's own programs. Needs only the host's C compiler.
set -euo pipefail
cd "$(dirname "$0")/../../.."
OUT=${TMPDIR:-/tmp}/halo-wii-tests
mkdir -p "$OUT"
cc -std=c99 -O1 -Wall -Wextra -Iport/wii/src -Isource/rasterizer/xbox \
	-o "$OUT/vsh_test" port/wii/tests/vsh_test.c port/wii/src/nv2a_vsh_run.c -lm
"$OUT/vsh_test"
