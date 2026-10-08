#!/bin/bash
# Build the Wii port: every game unit, the Wii platform layer (port/wii/src) and
# the parts of the Linux port that need no host OS, linked with libogc into a
# .dol, and the Homebrew Channel folder around it. Run from anywhere in a
# checkout; needs docker and the devkitpro/devkitppc image.
#   ./port/wii/build.sh          -> build/wii/sd/apps/halo/{boot.dol,meta.xml,icon.png},
#                                   and the same as build/wii/halo-wii-sd.zip
#   ./port/wii/build.sh clean    -> start again from nothing
# Units are only recompiled when their source is newer than their object, so
# after a header change, build clean.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [ "${1:-}" = clean ]; then rm -rf build/wii; fi
if [ "${1:-}" != --inside ]; then
	mkdir -p build/wii
	exec docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/src -w /src devkitpro/devkitppc \
		bash port/wii/build.sh --inside
fi

D=/opt/devkitpro
CC=$D/devkitPPC/bin/powerpc-eabi-gcc
OBJDUMP=$D/devkitPPC/bin/powerpc-eabi-objdump
B=build/wii
mkdir -p $B/game $B/platform $B/musl $B/zlib $B/sd/apps/halo

# the game's units and include folders (port/linux/port.json), and the
# generated MSVC inline semantics, as port/wii/check.sh
python3 - <<'PY'
import json, pathlib
c = json.load(open('port/linux/port.json'))
g = c['game']; ex = set(g['exclude'])
fs = sorted(p.as_posix() for p in pathlib.Path(g['root']).rglob('*.c') if p.as_posix() not in ex)
fs += sorted(p.as_posix() for p in pathlib.Path(c['game_sources']).glob('*.c'))
open('build/wii/files.txt', 'w').write('\n'.join(fs) + '\n')
open('build/wii/incs.txt', 'w').write(' '.join('-I"%s"' % d for d in g['include_dirs']))
PY
[ $B/sem.h -nt port/wii/halo_wii_prefix.h ] || \
	python3 tools/linux_msvc_semantics.py --output $B/sem.h --all-inlines --tags source --inlines source --inlines port/include/xdk
[ -f $B/platform_sem.h ] || python3 tools/linux_msvc_semantics.py --output $B/platform_sem.h --inlines port/include/xdk

# The Wii's ABI: Broadway (750CL), hardware floats, big-endian, 32-bit. As
# on Linux: 16-bit wchar_t, MSVC extensions, C89 tentative definitions,
# no optimisation that assumes away MSVC-tolerated UB, no fused multiply-adds.
ABI="-mrvl -mcpu=750 -meabi -mhard-float -DGEKKO -O2 -fms-extensions -fshort-wchar -fcommon \
 -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -ffp-contract=off"
GAME="$ABI -std=gnu89 -D__STRICT_ANSI__ -w -include port/wii/halo_wii_prefix.h -Iport/linux/include \
 -include $B/sem.h -Iport/include -iquote port/linux/game -Dxbox -DDEBUG -idirafter port/include/xdk $(cat $B/incs.txt)"
PLATFORM="$ABI -std=gnu11 -D_GNU_SOURCE -DHALO_LINUX_PLATFORM_LAYER -w -include port/wii/halo_wii_prefix.h \
 -include $B/platform_sem.h -Iport/linux/src -Iport/linux/include -Iport/include -Isource -Isource/cseries \
 -idirafter port/include/xdk"
GXTEST="$PLATFORM -Iport/wii/src -Isource/rasterizer/xbox"
export CC GAME PLATFORM GXTEST B

# compile <source> <object> <flags variable>, if the object is older
compile() {
	[ "$2" -nt "$1" ] && return 0
	eval "$CC ${!3} -c \"\$1\" -o \"\$2\"" 2>"$2.err" || { echo "FAIL $1"; head -5 "$2.err"; return 1; }
	rm -f "$2.err"
}
export -f compile
game_object() { compile "$1" "$B/game/$(echo "$1" | tr '/ ' '__' | sed 's/\.c$/.o/')" GAME; }
export -f game_object
echo "==> game units ($(wc -l <$B/files.txt))"
tr '\n' '\0' <$B/files.txt | xargs -0 -P"$(nproc)" -I{} bash -c 'game_object "$1"' _ {}

echo "==> platform layer"
# port/wii/src, and the Linux layer's units that touch no host OS: the
# pooled COMMON globals, the wide-character runtime, Bink's null player and
# the debug monitor's empty module list
for f in port/wii/src/wii_crt.c port/wii/src/wii_xbox.c port/wii/src/d3d8_gx.c port/wii/src/d3d8_gx_resources.c \
		port/linux/src/halo_linker_common.c \
		port/linux/src/msvc_wide.c port/linux/src/bink_null.c port/linux/src/xbdm.c; do
	compile "$f" "$B/platform/$(basename "$f" .c).o" PLATFORM
done
# wii_main.c and wii_os.c see libogc and not the XDK
LIBOGC="$ABI -std=gnu11 -Wall -I$D/libogc/include"
TEVTEST="$LIBOGC -Iport/wii/src -Iport/wii/tests -Iport/wii/tevtest"
export TEVTEST
compile port/wii/src/wii_main.c $B/platform/wii_main.o LIBOGC
compile port/wii/src/wii_os.c $B/platform/wii_os.o LIBOGC
# the GX device's libogc half, and the vertex programs' interpreter and the
# pixel shaders' translation to TEV (plain C)
compile port/wii/src/gx_backend.c $B/platform/gx_backend.o LIBOGC
compile port/wii/src/nv2a_vsh_run.c $B/platform/nv2a_vsh_run.o LIBOGC
compile port/wii/src/nv2a_tev.c $B/platform/nv2a_tev.o LIBOGC
# the game's sin, pow and the rest, the same on every port (port/include/halo_math.h)
MUSL="$ABI -std=gnu11 -w -Iport/third_party/musl-math/include -include port/third_party/musl-math/include/libm.h"
for f in port/third_party/musl-math/src/*.c; do compile "$f" "$B/musl/$(basename "$f" .c).o" MUSL; done
# the port's zlib, which inflates the maps (its names prefixed z_)
ZLIB="$ABI -std=gnu11 -DZ_PREFIX -Dz_errmsg=z_port_errmsg -w"
for n in adler32 crc32 inffast inflate inftrees uncompr zutil; do
	compile port/third_party/zlib/$n.c $B/zlib/$n.o ZLIB
done

echo "==> link"
# libogc's start-up calls main: wrap it (wii_main.c), and exit with it
LINK=(-mrvl -mcpu=750 -meabi -mhard-float -Wl,--wrap=main -Wl,--wrap=exit
	$B/game/*.o $B/platform/*.o $B/musl/*.o $B/zlib/*.o)
LIBS=(-L$D/libogc/lib/wii -lfat -logc -lm)
# first without stubs, to learn what is missing; then every missing name gets one
$CC "${LINK[@]}" "${LIBS[@]}" -o $B/halo.elf >$B/link1.log 2>&1 || true
$OBJDUMP -r $B/game/*.o $B/platform/*.o | awk 'NF==3 && $2 ~ /^R_PPC/' | sort -u >$B/relocs.txt
python3 port/wii/gen_stubs.py $B/link1.log $B/relocs.txt $B/wii_stubs.c
STUBS="$ABI -std=gnu89 -w -fno-builtin"
$CC $STUBS -c $B/wii_stubs.c -o $B/wii_stubs.o
$CC "${LINK[@]}" $B/wii_stubs.o "${LIBS[@]}" -Wl,-Map,$B/halo.map -o $B/halo.elf
$D/tools/bin/elf2dol $B/halo.elf $B/sd/apps/halo/boot.dol

echo "==> GX test scene"
# gxtest.dol: the GX device drawing a scene with the game's own vertex
# programs, for when there are no maps (gxtest/gxtest_scene.c)
mkdir -p $B/gxtest/sd/apps/halo-gxtest
compile port/wii/gxtest/gxtest_scene.c $B/gxtest/gxtest_scene.o GXTEST
compile port/wii/gxtest/gxtest_main.c $B/gxtest/gxtest_main.o LIBOGC
$CC -mrvl -mcpu=750 -meabi -mhard-float $B/gxtest/gxtest_main.o $B/gxtest/gxtest_scene.o \
	$B/platform/d3d8_gx.o $B/platform/d3d8_gx_resources.o $B/platform/gx_backend.o $B/platform/nv2a_vsh_run.o \
	$B/platform/nv2a_tev.o \
	-L$D/libogc/lib/wii -logc -lm -o $B/gxtest/gxtest.elf
$D/tools/bin/elf2dol $B/gxtest/gxtest.elf $B/gxtest/sd/apps/halo-gxtest/boot.dol

echo "==> shader sheet"
# tevtest.dol: the game's pixel shaders as TEV stages, a tile each, for
# Dolphin's frame dumps to be checked against (tevtest/tev_sheet.h)
mkdir -p $B/tevtest/sd/apps/halo-tevtest
compile port/wii/tevtest/tevtest_main.c $B/tevtest/tevtest_main.o TEVTEST
$CC -mrvl -mcpu=750 -meabi -mhard-float $B/tevtest/tevtest_main.o $B/platform/gx_backend.o $B/platform/nv2a_tev.o \
	-L$D/libogc/lib/wii -logc -lm -o $B/tevtest/tevtest.elf
$D/tools/bin/elf2dol $B/tevtest/tevtest.elf $B/tevtest/sd/apps/halo-tevtest/boot.dol

echo "==> Homebrew Channel folder"
cp port/wii/hbc/meta.xml port/wii/hbc/icon.png $B/sd/apps/halo/
# the same, zipped, to unzip at the root of a card
(cd $B/sd && rm -f ../halo-wii-sd.zip && python3 -c "import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED)
for root, dirs, files in os.walk('apps'):
    for name in sorted(files): z.write(os.path.join(root, name))" ../halo-wii-sd.zip)
ls -l $B/sd/apps/halo $B/halo-wii-sd.zip
