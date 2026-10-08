#!/bin/bash
# syntax-check each game unit for the Wii's CPU (powerpc-eabi, big-endian, 32-bit)
cd /src
CC=/opt/devkitpro/devkitPPC/bin/powerpc-eabi-gcc
INC=$(cat /incs.txt)
FL="-mcpu=750 -meabi -mhard-float -DGEKKO -fms-extensions -fshort-wchar -fcommon -fno-strict-aliasing -fwrapv -std=gnu89 -D__STRICT_ANSI__ -w -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=return-type -include build/ppc/prefix.h -Iport/linux/include -include build/ppc/sem.h -Iport/include -iquote port/linux/game -Dxbox -DDEBUG -idirafter port/include/xdk"
check() { f="$1"; out=$(eval $CC $FL $INC -fsyntax-only "\"$f\"" 2>&1); if [ $? -eq 0 ]; then echo "OK $f"; else echo "FAIL $f"; echo "$out" | grep -m3 'error' | sed 's/^/   /'; fi; }
export -f check; export CC FL INC
tr '\n' '\0' < build/ppc/files.txt | xargs -0 -P8 -I{} bash -c 'check "$@"' _ {}
