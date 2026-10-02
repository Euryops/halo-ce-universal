# Windows

`ninja windows` compiles the game with clang for 32-bit x86 Windows. The
result is a native executable, `build/windows/halo.exe`, with `SDL3.dll`
next to it. You must build on Windows.

The Windows build uses the platform layer of the Linux build
(`port/linux/src`). The graphics, the sound, the input, the settings and
the multiplayer functions are the same as on Linux. Refer to
[port/linux/README.md](../linux/README.md).

## Requirements

You do not need the Xbox SDK.

- Visual Studio or the Visual Studio Build Tools, with the C++ workload.
  The build uses the 32-bit (x86) MSVC libraries and a Windows 10 or 11
  SDK. It does not use the MSVC compiler.
- LLVM (clang and lld), Python 3 and ninja in the `PATH`. For example,
  enter `scoop install llvm python ninja`.
- A network connection for the first build. `configure.py` downloads the
  Visual C++ development package of SDL 3.4.16 to
  `build/windows/third_party`.

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja windows`.

`configure.py` generates the Windows build only on Windows.

The build uses all the instructions of the processor of the computer that
builds it (`-march=native`). Such a build does not always start on a
different computer. To make a build for other computers, enter
`python configure.py --portable`. Refer to the build options in the main
[README](../../README.md#build-options).

For a new optimization profile (`--pgo=train`), the build compiles the
profile runtime of LLVM for 32-bit x86 (`pgo/halo_profile_runtime.c`).
LLVM for Windows supplies this runtime only for x86-64.

## Start the game

Enter `build\windows\halo.exe`.

The game finds the game data as on Linux. Refer to "Start the game" in
[port/linux/README.md](../linux/README.md#start-the-game).

| Item | Location |
| --- | --- |
| Settings | `config.toml` next to `halo.exe` |
| Saved games | `%APPDATA%\halo`, or `paths.saves` in `config.toml` |
| Log | `debug.txt` in the data root (the folder that contains `maps\`) |

On Windows, the setting `debug.sample_seconds` (`HALO_SAMPLE`) writes the
location of the main thread to the log at this interval, in seconds. Thus
you can find a hang without a debugger. Subtract the image address in the
log from each address, add `0x400000`, and give the result to
`llvm-symbolizer --obj=build\windows\halo.exe`.

## Graphics hardware without OpenGL 4.5

The game needs OpenGL 4.5. Some graphics hardware has no driver with
OpenGL 4.5, for example the Intel HD Graphics of the years before 2016. On
such a computer, the game writes `cannot create an OpenGL context` to its
output and shows no window.

The option `--gles` makes a build that shows its graphics with OpenGL ES
3.0. The renderer is that of the Android build. On Windows, the build gets
OpenGL ES from ANGLE, which draws with Direct3D 11:

1. Enter `python configure.py --gles`.
2. Enter `ninja windows`. The build also makes `build/windows/libEGL.dll`
   (`angle/libEGL.c`), through which SDL3 gets to ANGLE.
3. Put the `libGLESv2.dll` of a 32-bit (x86) build of ANGLE next to
   `halo.exe`. The build does not download it. For example, the NuGet
   package `Avalonia.Angle.Windows.Natives` contains it as
   `runtimes/win-x86/native/av_libglesv2.dll`. Change the name of that
   file to `libGLESv2.dll`.

ANGLE gives OpenGL ES 3.0 on graphics hardware with Direct3D feature level
10.1 or later. If `libGLESv2.dll` is not next to `halo.exe`, the game asks
the graphics driver for OpenGL ES.

These are the differences from the OpenGL 4.5 build:

- The limits of the OpenGL ES renderer apply. Refer to "OpenGL ES" in
  [port/android/README.md](../android/README.md#opengl-es).
- The CPU puts the channels of a texture in their locations. With a
  swizzle, ANGLE showed some textures almost black. A compressed texture
  that has a Custom Edition channel order is decoded for this.
- Direct3D 11 has one blend constant. Thus ANGLE refuses a blend that uses
  the constant color and the constant alpha together. The meters of the
  weapons use such a blend. The build uses 1 or 0 as the constant alpha.
  A meter with a brightness between these shows at full brightness.
- A level starts some seconds later than in the OpenGL 4.5 build.

The environment variable `HALO_ANGLE_FEATURE_LEVEL` limits the Direct3D
feature level that ANGLE uses. Set it to `10.1` to test the build for old
graphics hardware on new graphics hardware. The first lines of the output
show the feature level as the shader models (`vs_4_1 ps_4_1`).

The Linux build with `--gles` asks the graphics driver for OpenGL ES 3.
This build is not tested.

## How the port operates

The game is 32-bit code, as on Linux, because its data contains 32-bit
pointers. The clang target `i686-pc-windows-msvc` gives the ABI of MSVC:
the structure layout, the 16-bit `wchar_t` and the calling conventions.
Thus the Windows build needs fewer changes than the Linux build.

The executable is large-address-aware, because the platform layer reserves
the Xbox memory at `0x80000000`.

### Headers

- The game and the platform layer use the Xbox SDK declarations of
  `port/include/xdk`, not the Windows SDK declarations with the same names.
  The compiler reads `port/include/xdk` before the Windows SDK.
- The C runtime is the static UCRT of Windows.
- `include/halo_windows_prefix.h` is the first header of each file. It gives
  Windows names to the Xbox SDK functions of the platform layer (for example
  `CreateFileA`, `ReadFile`, `Sleep`). Thus the code for Windows gets to
  Windows. The list of names is in `include/halo_windows_api_names.h`.
- `include/crt` changes some C runtime headers. The file functions of the
  game (`fopen`, `open`, `mkdir`) go through the translation of Xbox paths
  (`src/windows_crt.c`).
- `include/posix` declares the POSIX functions that the platform layer uses.

### Code for Windows

These files use only the Windows SDK:

| File | Contents |
| --- | --- |
| `src/win32_files.c`, `src/win32_net.c` | The file and socket functions of `port/linux/src/posix.h`. |
| `src/win32_posix.c` | The POSIX functions on Windows threads, critical sections, condition variables, `VirtualAlloc` and the performance counter. |
| `src/win32_memory_watch.c` | The write tracking of textures, with a vectored exception handler. It also writes reports of crashes, and the `debug.sample_seconds` reports. |

`port.json` gives the Linux files that these files replace, and the Windows
libraries of the link.

### Inline functions

MSVC makes a COMDAT copy of an external `__inline` function where it does
not inline a call. Some files call such functions through a prototype.
clang inlines more calls. Thus a function can have no copy. The build
prevents this:

- `port/linux/game/msvc_comdat.c` gives one weak definition of each header
  inline function.
- A wrapper takes the address of each `__inline` function in a `.c` file.
  Thus the compiler makes an external copy (`inline_export_wrapper` in
  `tools/windows_build.py`).

MSVC gives file scope to a structure tag in a prototype. clang does not.
Thus the build also includes the declarations of the Linux build
(`build/windows/halo_msvc_tags.h`).

## Limits

- Bink video is not available. The game skips the movies.
- The game uses only the ANSI file functions. A path to the data or the
  saved games with characters that are not in the code page of the system
  does not always operate.
