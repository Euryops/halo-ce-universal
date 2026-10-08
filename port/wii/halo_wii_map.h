/*
HALO_WII_MAP.H

What a map converted for the Wii (port/wii/mapconv) holds that an Xbox map does
not. Nothing includes this yet: it is for the stages that read the maps (3: load,
4: the GX renderer, 6: audio).

- Every number in the header, the tags and the BSPs is big-endian, so the engine
  reads them as they are; every pointer in them is into the Wii's tag cache at
  HALO_WII_TAG_CACHE_BASE_ADDRESS (0x90100000) in place of the Xbox's 0x803A6000.
  That includes the data the engine reads as numbers: model animations' default
  and frame data (compressed or not), recorded animations' event streams,
  script nodes, UTF-16 strings, and BSP and model vertices and indices. Data no
  code reads is left as it was (BSP cluster and sound cluster data, meter tags,
  input device defaults); anything else is listed in the converter's report
  (left_in_xbox_order), which is empty for data laid out as the engine reads it.
- A recorded animation's version 4 event header byte (struct
  animation_event_header: time_delta:2, event_type:6) is rewritten for GCC's
  big-endian bitfields, which put time_delta in the top two bits, so the
  struct reads the same with no change to the engine.
- The header's reserved68 starts with HALO_WII_MAP_STAMP and the converter's
  version (big-endian), when the textures and sounds are the Wii's.
- A bitmap's format is HALO_WII_BITMAP_FORMAT_GX + a GX texture format, its
  swizzled flag is clear, and its pixels (pixels_offset, pixels_size, relative to
  the bitmap group's pixel data as before) are GX tiles: each mipmap level after
  the one before, each padded to whole tiles, a cube map's six faces in each
  level. A 3D texture (GX has none) is its slices, each a 2D image, one after
  another in each level. texture_cache_bitmap_new recomputes pixels_size from
  the format, so it must know these sizes (mapconv/textures.py: gx_size).
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
