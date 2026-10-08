# Wii port (work in progress)

`halo_wii_prefix.h` is the Wii counterpart of `port/linux/include/halo_linux_prefix.h`
for powerpc-eabi gcc (devkitPPC): no x86 guard, gcc inline rules, 16-bit `wchar_t`,
`__int64` and the x86 calling conventions defined away.

`ppc-syntax-check.sh` syntax-checks every game unit with it. At `a3fa6eec` it passes
470 of 498 units. Run it in `devkitpro/devkitppc` with the repo at `/src`, the header
copied to `build/ppc/prefix.h`, `build/ppc/sem.h` from `tools/linux_msvc_semantics.py`,
`build/ppc/files.txt` (the units) and `/incs.txt` (the `-I` list from `port/linux/port.json`).
