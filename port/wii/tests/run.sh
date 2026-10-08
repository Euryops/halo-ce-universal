#!/bin/bash
# The Wii port's host tests: the vertex program interpreter against the
# game's own programs, then against the Linux port's GLSL for them run by
# Mesa (skipped where there is no EGL), and the pixel shaders' translation to
# TEV against an NV2A reference (the game's programs, then random ones).
# Needs the host's C compiler, and Mesa's libEGL.so.1 for the second.
set -euo pipefail
cd "$(dirname "$0")/../../.."
OUT=${TMPDIR:-/tmp}/halo-wii-tests
mkdir -p "$OUT"
cc -std=c99 -O1 -Wall -Wextra -Iport/wii/src -Isource/rasterizer/xbox \
	-o "$OUT/vsh_test" port/wii/tests/vsh_test.c port/wii/src/nv2a_vsh_run.c -lm
"$OUT/vsh_test"
# the Linux port's translator, built on the host with its include of xgpu.h
# pointed at xgpu_host.h
for f in nv2a_vsh xgpu_text; do
	sed 's/#include "xgpu.h"/#include "xgpu_host.h"/' port/linux/src/$f.c >"$OUT/${f}_host.c"
done
cc -std=gnu99 -O1 -Wall -Iport/wii/src -Iport/wii/tests -Isource/rasterizer/xbox \
	-o "$OUT/vsh_gl_test" port/wii/tests/vsh_gl_test.c port/wii/src/nv2a_vsh_run.c \
	"$OUT/nv2a_vsh_host.c" "$OUT/xgpu_text_host.c" -ldl -lm
"$OUT/vsh_gl_test"
cc -std=c99 -O2 -Wall -Iport/wii/src -Iport/wii/tests \
	-o "$OUT/tev_test" port/wii/tests/tev_test.c port/wii/src/nv2a_tev.c -lm
"$OUT/tev_test"
