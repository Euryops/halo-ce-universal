"""A whole map, Xbox to Wii: textures, sounds, then the byte swap.

The order matters: the bitmaps' and sounds' fields are read and rewritten while
the map is still little-endian, where the walk can read them, and the swap comes
last and turns those fields over with the rest.

A converted texture or sound goes where the old one was when it fits, and at
the end of the file when it does not (an 8-bit texture is 16-bit as RGB5A3);
the space it leaves is not reclaimed, so every other file offset in the map, known
to the converter or not, still points at what it did.
"""

import struct
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field

from . import sound, textures
from .swap import TO_WII
from .tags import Header, TAG_INSTANCE_SIZE, swap_map, walk_tag_cache

# The stamp in the header's unused bytes (cache_file_header.reserved68) that says
# the textures and sounds are the Wii's: 'wii1', then the converter's version.
STAMP_OFFSET = 0x68
STAMP = b'wii1'
VERSION = 1
BITMAP, SOUND = 0x6269746D, 0x736E6421   # 'bitm', 'snd!'


def field_offset(defs, struct_name, name):
    return defs.field_offset(struct_name, name)


@dataclass
class Report:
    bitmaps: int = 0
    bitmaps_moved: int = 0
    sounds: int = 0
    sounds_moved: int = 0
    formats: dict = field(default_factory=dict)
    skipped: list = field(default_factory=list)


class Converter:
    def __init__(self, defs, palette, workers=None):
        self.defs, self.palette, self.workers = defs, palette, workers

    def convert(self, expanded):
        header = Header.parse(expanded)
        header.check()
        out = bytearray(expanded)
        report = Report()
        start = header.tag_data_offset
        walk = walk_tag_cache(expanded[start:start + header.tag_data_size],
                              TO_WII.old_base, self.defs, TO_WII)
        if walk.errors:
            raise ValueError(f'{header.name}: {len(walk.errors)} tags could not be walked, '
                             f'the first: {walk.errors[0]}')
        tags = Tags(out, start, walk.plan.base)
        table = tags.u32(0) - tags.base
        jobs = []
        for i in range(tags.u32(12)):
            entry = table + i * TAG_INSTANCE_SIZE
            group, body = tags.u32(entry), tags.u32(entry + 20)
            if group == BITMAP:
                self._bitmaps(tags, body - tags.base, report)
            elif group == SOUND:
                jobs += self._sounds(tags, body - tags.base)
        self._convert_sounds(tags, jobs, report)
        struct.pack_into('<I', out, 8, len(out))        # the file's length
        swapped = swap_map(bytes(out), self.defs, TO_WII)
        if swapped.errors:
            raise ValueError(f'{header.name}: {swapped.errors[0]}')
        result = bytearray(swapped.data)
        result[STAMP_OFFSET:STAMP_OFFSET + 8] = STAMP + struct.pack('>I', VERSION)
        return bytes(result), report, swapped.summary()

    # ---------- bitmaps

    def _bitmaps(self, tags, body, report):
        defs = self.defs
        group_offset = tags.u32(body + field_offset(defs, 'Bitmap', 'processed pixel data') + 8)
        reflexive = body + field_offset(defs, 'Bitmap', 'bitmap data')
        count, address = tags.u32(reflexive), tags.u32(reflexive + 4)
        size = defs.size('BitmapData')
        for i in range(count):
            at = address - tags.base + i * size
            width, height, depth, kind, xbox_format, flags = struct.unpack_from('<6H', tags.data, tags.start + at + 4)
            mipmaps = struct.unpack_from('<H', tags.data, tags.start + at + 20)[0]
            offset, length = struct.unpack_from('<II', tags.data, tags.start + at + 24)
            pixels_at = group_offset + offset
            pixels = bytes(tags.data[pixels_at:pixels_at + length])
            report.bitmaps += 1
            try:
                gx, converted = textures.convert_bitmap(pixels, width, height, depth, kind, flags,
                                                        mipmaps, xbox_format, self.palette)
            except ValueError as error:
                report.skipped.append(f'bitmap {i} of the bitmap at {address:08x}: {error}')
                continue
            new_at = tags.place(pixels_at, length, converted, 32)
            report.bitmaps_moved += new_at != pixels_at
            report.formats[gx] = report.formats.get(gx, 0) + 1
            struct.pack_into('<HH', tags.data, tags.start + at + 12,
                             textures.GX_FORMAT_BASE + gx, flags & ~textures.FLAG_SWIZZLED)
            struct.pack_into('<II', tags.data, tags.start + at + 24, new_at - group_offset, len(converted))

    # ---------- sounds

    def _sounds(self, tags, body):
        defs = self.defs
        channels = 2 if tags.u16(body + field_offset(defs, 'Sound', 'channel count')) == 1 else 1
        rate = 44100 if tags.u16(body + field_offset(defs, 'Sound', 'sample rate')) == 1 else 22050
        ranges = body + field_offset(defs, 'Sound', 'pitch ranges')
        jobs = []
        # The sound's own format, which the game checks before a permutation's.
        if tags.u32(ranges):
            tags.put16(body + field_offset(defs, 'Sound', 'format'), sound.COMPRESSION_DSP_ADPCM)
        range_size = defs.size('SoundPitchRange')
        permutations_at = field_offset(defs, 'SoundPitchRange', 'permutations')
        compression_at = field_offset(defs, 'SoundPermutation', 'format')
        samples_at = field_offset(defs, 'SoundPermutation', 'samples')
        permutation_size = defs.size('SoundPermutation')
        for r in range(tags.u32(ranges)):
            pitch_range = tags.u32(ranges + 4) - tags.base + r * range_size
            for p in range(tags.u32(pitch_range + permutations_at)):
                permutation = tags.u32(pitch_range + permutations_at + 4) - tags.base + p * permutation_size
                compression = tags.u16(permutation + compression_at)
                length, file_offset = tags.u32(permutation + samples_at), tags.u32(permutation + samples_at + 8)
                data = bytes(tags.data[file_offset:file_offset + length])
                jobs.append((permutation, compression_at, samples_at, file_offset, length,
                             (data, compression, channels, rate)))
        return jobs

    def _convert_sounds(self, tags, jobs, report):
        arguments = [job[-1] for job in jobs]
        if self.workers == 1 or len(jobs) < 2:
            results = [sound.convert_samples(*a) for a in arguments]
        else:
            with ProcessPoolExecutor(self.workers) as pool:
                results = list(pool.map(_convert_one, arguments, chunksize=4))
        for (permutation, compression_at, samples_at, file_offset, length, _), converted in zip(jobs, results):
            report.sounds += 1
            new_at = tags.place(file_offset, length, converted, 32)
            report.sounds_moved += new_at != file_offset
            tags.put16(permutation + compression_at, sound.COMPRESSION_DSP_ADPCM)
            tags.put32(permutation + samples_at, len(converted))
            tags.put32(permutation + samples_at + 8, new_at)


def _convert_one(arguments):
    return sound.convert_samples(*arguments)


class Tags:
    """The map, read through the tag cache's addresses (little-endian)."""
    def __init__(self, data, start, base):
        self.data, self.start, self.base = data, start, base

    def u16(self, offset):
        return struct.unpack_from('<H', self.data, self.start + offset)[0]

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, self.start + offset)[0]

    def put16(self, offset, value):
        struct.pack_into('<H', self.data, self.start + offset, value)

    def put32(self, offset, value):
        struct.pack_into('<I', self.data, self.start + offset, value)

    def place(self, old_at, old_length, new, align):
        """Where new goes in the file: old's place if it fits, else the end."""
        if len(new) <= old_length and old_at % align == 0:
            self.data[old_at:old_at + len(new)] = new
            return old_at
        self.data.extend(bytes((-len(self.data)) % align))
        at = len(self.data)
        self.data.extend(new)
        return at
