# Wii port (work in progress)

The plan, stage by stage, is in euryo's `docs/plans/2026-10-09-halo-on-the-wii.md`.
Everything here needs docker and the devkitPPC image (`docker pull devkitpro/devkitppc`).

## Build the .dol (stage 1)

From the repo root:

    ./port/wii/build.sh           # about a minute from clean on four cores
    ./port/wii/build.sh clean     # after a header change: units rebuild only when their .c is newer

The result is the Homebrew Channel folder, ready to copy to the root of an SD card:

    build/wii/sd/apps/halo/boot.dol
    build/wii/sd/apps/halo/meta.xml
    build/wii/sd/apps/halo/icon.png

What it does today: it boots, keeps the game's fixed places out of the heap, brings up a
text console and runs the game's own `main` (`source/shell/shell_xbox.c`). The game's
log, `d:\debug.txt`, is the console, so its start-up shows on screen: the memory it
places, the map it cannot read (there is no file system yet) and the asserts where it
stops, at the cache thread and the Direct3D device. No game data is needed.

## What is in the link

| part | where |
| --- | --- |
| all 498 game units (`source/`, `port/linux/game/`) | as `port/linux/port.json` lists them |
| entry point, video and console | `src/wii_main.c` (libogc; the link wraps `main` and `exit`) |
| C runtime names, printf with `%I64`, `fopen` by Xbox path | `src/wii_crt.c` |
| XAPI memory, time, errors, events, launch info | `src/wii_xbox.c` |
| pooled COMMON globals, wide-char runtime, null Bink, empty xbdm | from `port/linux/src/`, unchanged |
| the game's maths and zlib | `port/third_party/musl-math`, `port/third_party/zlib` |
| everything else (Direct3D, DirectSound, XInput, XNet, files, ...) | `build/wii/wii_stubs.c` |

`wii_stubs.c` is generated on each build by `gen_stubs.py` from what the first link
pass finds missing: each stub prints `wii: stub: <name>` the first time it runs and
returns 0. A stage replaces stubs by writing the real function in `src/`, and the
stub stops being generated.

## Memory

`halo_wii_capacity.h` takes the place of `port/linux/include/halo_port_capacity.h`
(the Wii prefix includes it first). The Wii uses the Xbox's own pool sizes, not
the native builds' 128-player ones, and its own fixed places:

| what | where |
| --- | --- |
| game state, 6 MB | `0x81200000`, the top of MEM1 |
| tag cache, 22 MB | `0x90100000`, the foot of MEM2 |

The Xbox's maps are linked to a tag cache at `0x803A6000`, where the Wii's code is,
so the map converter (stage 2) will have to rebase them to `0x90100000`
(`source/cache/physical_memory_map.c` takes the Wii's address under `GEKKO`).

## Boot it in Dolphin with no display

On the box, with euryo's runner (`docs/dolphin.md` there):

    ~/euryo/scripts/dolphin/run.sh build/wii/sd/apps/halo/boot.dol 25 <outdir>

The frame is `<outdir>/Dump/Frames/framedump_1.png`.

## Syntax check

    ./port/wii/check.sh           # 498 OK, 0 FAIL

It writes the inputs under `build/ppc/` (git-ignored) and runs `ppc-syntax-check.sh`
in the devkitPPC image; failures, with their first errors, are in `build/ppc/result.txt`.
