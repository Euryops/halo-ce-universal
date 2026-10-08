# Wii port (work in progress)

`halo_wii_prefix.h` is the Wii counterpart of `port/linux/include/halo_linux_prefix.h`
for powerpc-eabi gcc (devkitPPC): no x86 guard, gcc inline rules, 16-bit `wchar_t`,
`__int64` and the x86 calling conventions defined away.

## Run the check

On the box (or any machine with docker and `docker pull devkitpro/devkitppc`), from the repo root:

    ./port/wii/check.sh

It writes the inputs under `build/ppc/` (git-ignored), runs `ppc-syntax-check.sh` in the
devkitPPC image and prints `470 OK, 28 FAIL` at `a3fa6eec`. The failing units, with their
first errors, are in `build/ppc/result.txt`.
