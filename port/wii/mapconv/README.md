# Map converter (stage 2)

Turns Halo CE's Xbox maps into the Wii's: every tag byte-swapped to big-endian by
walking the tag field definitions, every pointer moved to the Wii's tag cache, the
textures in GX formats and the sounds in DSP ADPCM. Python 3 and nothing else; run it
from `port/wii`.

    python3 -m mapconv fetch-definitions            # once: Invader's tag definitions (below)
    python3 -m mapconv check ~/inbox/halo.iso       # is it build 01.10.12.2276, map by map
    python3 -m mapconv roundtrip ~/inbox/halo.iso   # swap each map there and back: same bytes?
    python3 -m mapconv convert ~/inbox/halo.iso out # out/halo/maps/*.map, for the SD card's /halo/maps
    python3 -m mapconv selftest                     # the round trip on made-up maps

`SOURCE` is the disc image (`.iso`/`.xiso`, the game partition found where it is) or a
folder of `.map` files. A map that is not the retail 01.10.12.2276 one, by its SHA-256
on the disc or expanded, is refused (`--any-build` goes on regardless, unchecked).

## Tests

    python3 -m unittest discover -s mapconv/tests -t .

There is no disc yet, so the check that matters is the round trip on made-up maps
(`tests/test_roundtrip.py`): `fabricate.py` builds Xbox maps from the definitions,
with random numbers, blocks, references, data, model and BSP vertex buffers, BSP
images, animations (compressed and not) and recorded animation streams (v1 and v4), and each goes to the Wii and back, byte for byte. On the way, every number the
fabricator wrote must read the same big-endian and every address it wrote must point
at the same place in the Wii's cache. It runs on a small made-up definition set
(`tests/fixture_definitions.py`) always, and on all 82 of Invader's groups once
they are fetched. On a disc, `roundtrip` is the same check on the real maps.

## Where the knowledge comes from

| what | from |
| --- | --- |
| the field definitions of all 82 groups | Invader, `src/tag/hek/definition/*.json` at `a497b745` (GPL-3.0, so fetched to `~/.cache/halo-wii-mapconv`, not kept here; `HALO_WII_DEFINITIONS` moves it) |
| the cache file, tag header and tag table | `source/cache/cache_files.c` |
| the script nodes' swap | `hs_data_array_codes` and `hs_syntax_node_codes`, `source/hs/hs_scenario_definitions.c`, run by a port of `_byte_swap_data` |
| the Xbox's vertex/index buffers, BSP images, and the words that are pointers only on the Xbox | Halo3DS's `relocate_cache.py`, which walked every retail map (`tags.py`, `XBOX_RULES`) |
| the build's map hashes | Halo3DS's `catalog.json` (CC0) |
| what each texture format's pixels mean, where mipmaps and faces are, the P8 palette | `source/bitmaps/bitmaps.c` |
| Xbox ADPCM | `port/linux/src/dsound_sdl.c` (64 samples a block, the header's first) |
| model animation data, compressed or not | the readers in `source/models/model_animations.c` (`streams.py`) |
| recorded animation streams (v1 and v4) | `source/cutscene/recorded_animation_initialize.c`, `recorded_animation_playback_v1.c`, `recorded_animation_playback.c` |
| BSP material vertex data | `source/objects/object_lights.c`, `source/structures/structure_lens_flares.c` |

The definitions were checked against Halo3DS's own flattening of them
(`schemas.json`): all 4,765 of its fields are where this converter puts them, with
the same width.

## What a converted map is

`../halo_wii_map.h` says it for the engine. The textures: DXT1 to CMPR (the
blocks moved, not recompressed), the 32-bit formats and P8 bump maps to RGBA8,
everything else to RGB5A3. The sounds: DSP ADPCM, one 96-byte header per channel.
A texture or sound that no longer fits in its old place goes at the end of the file.

## Open

- **Not run on a real map.** Everything above is checked on made-up data only.
- Every data blob the engine reads is swapped by the layout its reader in the decomp
  expects; one that does not match that layout is left as it is and listed by
  `roundtrip` and in the report (`left_in_xbox_order`), so a real map shows at once
  whether any reader was misread. Data nothing reads (BSP cluster and sound cluster
  data, meter tags, input device defaults) stays as bytes.
- 3D textures become their slices, each a 2D GX image; the Xbox's 3D swizzle is
  taken to be XGSwizzleBox's (x, y, z bits in turn), not checked on real data.
- The cube map face order and the linear (non-swizzled) bitmaps' row pitch follow
  `bitmaps.c` and are not checked against the Xbox's own GPU layout.
- The sound converter is pure Python: about 0.1 s per second of audio on one core,
  so it uses every core (`--workers`).
