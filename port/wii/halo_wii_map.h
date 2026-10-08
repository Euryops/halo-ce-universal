/*
HALO_WII_MAP.H

What a map converted for the Wii (port/wii/mapconv) holds that an Xbox map does
not. Nothing includes this yet: it is for the stages that read the maps (3: load,
4: the GX renderer, 6: audio).

- Every number in the header, the tags and the BSPs is big-endian, so the engine
  reads them as they are; every pointer in them is into the Wii's tag cache at
  HALO_WII_TAG_CACHE_BASE_ADDRESS (0x90100000) in place of the Xbox's 0x803A6000.
  Data the converter could not swap is listed in its report
  (left_in_xbox_order): animation frame data, BSP cluster and sound PAS data,
  meter stencils, recorded animation streams, and input device data.
- The header's reserved68 starts with HALO_WII_MAP_STAMP and the converter's
  version (big-endian), when the textures and sounds are the Wii's.
- A bitmap's format is HALO_WII_BITMAP_FORMAT_GX + a GX texture format, its
  swizzled flag is clear, and its pixels (pixels_offset, pixels_size, relative to
  the bitmap group's pixel data as before) are GX tiles: each mipmap level after
  the one before, each padded to whole tiles, a cube map's six faces in each
  level. texture_cache_bitmap_new recomputes pixels_size from the format, so it
  must know these sizes (mapconv/textures.py: gx_size).
- A sound permutation's and its sound's compression is
  HALO_WII_SOUND_COMPRESSION_DSP_ADPCM, and its samples are, for each channel, the
  standard 96-byte DSP ADPCM header (big-endian), then each channel's frames,
  each padded to 32 bytes (mapconv/sound.py).
*/

#ifndef __HALO_WII_MAP_H
#define __HALO_WII_MAP_H
#pragma once

#define HALO_WII_MAP_STAMP_OFFSET 0x68
#define HALO_WII_MAP_STAMP 'wii1'
#define HALO_WII_MAP_VERSION 1

/* + GX_TF_RGB5A3 (0x5), GX_TF_RGBA8 (0x6) or GX_TF_CMPR (0xE) */
#define HALO_WII_BITMAP_FORMAT_GX 0x40

#define HALO_WII_SOUND_COMPRESSION_DSP_ADPCM 0x40
#define HALO_WII_DSP_ADPCM_HEADER_SIZE 96

#endif /* __HALO_WII_MAP_H */
