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

`build/wii/halo-wii-sd.zip` is the same folder zipped: unzip it at the root of the card.

What it does today: it boots, keeps the game's fixed places out of the heap, mounts the
SD card, brings up a text console and runs the game's own `main`
(`source/shell/shell_xbox.c`). The game's log, `d:\debug.txt`, is the console, and the
console is copied to `sd:/halo/debug.txt`, so a run on a real Wii can be read afterwards
on a PC. With a `halo/maps` folder on the card (empty will do), the game's start-up shows:
the memory it places, its cache thread starting, its `z:\` cache files made on the card,
and then `failed to create D3D object`, where it stops: the renderer is stage 4. Without
the folder it stops earlier, at `no valid map directory exists`. Each halt prints its
stack; resolve the addresses with
`powerpc-eabi-addr2line -f -e build/wii/halo.elf <address>...` in the devkitPPC image.

## What is in the link

| part | where |
| --- | --- |
| all 498 game units (`source/`, `port/linux/game/`) | as `port/linux/port.json` lists them |
| entry point, video and console | `src/wii_main.c` (libogc; the link wraps `main` and `exit`) |
| SD card, the console's copy on it, threads and waitable objects | `src/wii_os.c` (libogc), behind `src/wii_os.h` |
| C runtime names, printf with `%I64`, `fopen` by Xbox path | `src/wii_crt.c` |
| XAPI memory, time, errors, events, mutexes, threads, files, `ReadFileEx`/`WriteFileEx` | `src/wii_xbox.c` |
| pooled COMMON globals, wide-char runtime, null Bink, empty xbdm | from `port/linux/src/`, unchanged |
| the game's maths and zlib | `port/third_party/musl-math`, `port/third_party/zlib` |
| everything else (Direct3D, DirectSound, XInput, XNet, files, ...) | `build/wii/wii_stubs.c` |

`wii_stubs.c` is generated on each build by `gen_stubs.py` from what the first link
pass finds missing: each stub prints `wii: stub: <name>` the first time it runs and
returns 0. A stage replaces stubs by writing the real function in `src/`, and the
stub stops being generated.

The XDK's headers and libogc's cannot be read by one unit (`BOOL`, `u32` and others
clash), and the port's own `sys/stat.h` is MSVC's, so anything that needs libogc or
newlib's `stat` goes in `wii_os.c` and is called through `wii_os.h` in plain C types.

## Files

The Xbox's drives are folders of `sd:/halo`: `d:\` is `sd:/halo` itself (the maps go in
`sd:/halo/maps`), and every other drive is `sd:/halo/<letter>`, made on first use.
FAT is case-insensitive as FATX is. `ReadFileEx` and `WriteFileEx` finish at once, and
their completion routine runs at the asking thread's next alertable wait, as on Win32.
The engine's threads run at main's priority (64). Directory listings (`FindFirstFile`),
file times, free space and save games (`XCreateSaveGame` and the rest, in the Xbox's
`UDATA\<id>\` layout, as the Linux port) work on the card.

**The card needs room.** On its first start the game makes six cache files in
`sd:/halo/z` and sizes them at once, as it did on the Xbox's hard disk: 770 MB, with
`savegame.bin` (6 MB) beside them. A full card is an error the game halts on
(`error_code==ERROR_SUCCESS` in `cache_files_windows.c`), not a quiet short file. With
the converted maps (about 1.8 GB), 4 GB is the smallest card that will do.

## Memory

`halo_wii_capacity.h` takes the place of `port/linux/include/halo_port_capacity.h`
(the Wii prefix includes it first). The Wii uses the Xbox's own pool sizes, not
the native builds' 128-player ones, and its own fixed places:

| what | where |
| --- | --- |
| game state, 6 MB | `0x81200000`, the top of MEM1 |
| tag cache, 22 MB | `0x90100000`, the foot of MEM2 |
| texture cache, 22 MB | `0x91700000` |
| sound cache, 4 MB | `0x92D00000` |

The heap is what is left: about 3.5 MB of MEM1 above the program (whose pooled
globals are 10 MB of it), then about 2.9 MB of MEM2 below IOS. libogc's `sbrk` moves
from MEM1 to MEM2 for good at the first request that does not fit, so nothing large
may come from `malloc`; the caches have places for that reason.

The Xbox's maps are linked to a tag cache at `0x803A6000`, where the Wii's code is,
so the map converter (stage 2) will have to rebase them to `0x90100000`
(`source/cache/physical_memory_map.c` takes the Wii's address under `GEKKO`).

## Boot it in Dolphin with no display

On the box, with euryo's runner (`docs/dolphin.md` there):

    mkdir -p <card> && cp -r build/wii/sd/apps <card>/
    ~/euryo/scripts/dolphin/run.sh <card>/apps/halo/boot.dol 25 <outdir> <card>

The frame is `<outdir>/Dump/Frames/framedump_1.png`. With the fourth argument, Dolphin's
SD card is made from `<card>` and written back to it when Dolphin stops, so the run's
`<card>/halo/debug.txt` is there to read.

## Syntax check

    ./port/wii/check.sh           # 498 OK, 0 FAIL

It writes the inputs under `build/ppc/` (git-ignored) and runs `ppc-syntax-check.sh`
in the devkitPPC image; failures, with their first errors, are in `build/ppc/result.txt`.
