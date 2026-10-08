"""A whole made-up map through the converter: what the Wii's map says about its
textures and sounds, read back after swapping it to the Xbox's order again."""

import struct
import unittest

from mapconv import sound, textures
from mapconv.convert import Converter, STAMP, STAMP_OFFSET, field_offset
from mapconv.fabricate import fabricate_map
from mapconv.swap import TO_XBOX, TO_WII
from mapconv.tags import Header, swap_map, walk_tag_cache
from mapconv.tests import support


class Convert(unittest.TestCase):
    def converted(self, defs, seed):
        xbox = fabricate_map(defs, seed=seed, media=True)
        palette = textures.load_vector_palette(support.SOURCE_ROOT)
        wii, report, summary = Converter(defs, palette, workers=1).convert(xbox)
        self.assertEqual(summary['errors'], [])
        self.assertEqual(wii[STAMP_OFFSET:STAMP_OFFSET + 4], STAMP)
        header = Header.parse(wii, '>')
        self.assertEqual(header.file_length, len(wii))
        unstamped = wii[:STAMP_OFFSET] + bytes(8) + wii[STAMP_OFFSET + 8:]
        back = swap_map(unstamped, defs, TO_XBOX)
        self.assertEqual(back.errors, [])
        return xbox, back.data, report

    def elements(self, defs, data, name):
        header = Header.parse(data)
        walk = walk_tag_cache(data[header.tag_data_offset:][:header.tag_data_size], TO_WII.old_base, defs, TO_WII)
        return [header.tag_data_offset + at for n, at in walk.plan.visited if n == name]

    def test_textures_are_in_gx_formats_where_the_tags_say(self):
        defs = support.fixture()
        seen = 0
        for seed in range(8):
            xbox, after, report = self.converted(defs, seed)
            self.assertEqual(report.skipped, [])
            before = {at: struct.unpack_from('<6H', xbox, at + 4) for at in self.elements(defs, xbox, 'BitmapData')}
            for at in self.elements(defs, after, 'BitmapData'):
                width, height, depth, kind, fmt, flags = struct.unpack_from('<6H', after, at + 4)
                mipmaps = struct.unpack_from('<H', after, at + 20)[0]
                offset, size = struct.unpack_from('<II', after, at + 24)
                xbox_format = before[at][4]
                gx = textures.gx_format_for(xbox_format)
                self.assertEqual(fmt, textures.GX_FORMAT_BASE + gx)
                self.assertFalse(flags & textures.FLAG_SWIZZLED)
                levels = textures.mip_levels(width, height, depth, kind, mipmaps, xbox_format)
                self.assertEqual(size, sum(textures.gx_size(gx, w, h) * faces * d for w, h, d, faces, _ in levels))
                self.assertEqual(offset % 32, 0)
                self.assertLessEqual(offset + size, len(after))
                seen += 1
        self.assertGreater(seen, 5)

    def test_sounds_are_dsp_adpcm_where_the_tags_say(self):
        defs = support.fixture()
        samples_at = field_offset(defs, 'SoundPermutation', 'samples')
        format_at = field_offset(defs, 'SoundPermutation', 'format')
        seen = 0
        for seed in range(8):
            xbox, after, report = self.converted(defs, seed)
            for at in self.elements(defs, after, 'SoundPermutation'):
                self.assertEqual(struct.unpack_from('<H', after, at + format_at)[0], sound.COMPRESSION_DSP_ADPCM)
                size, _, offset = struct.unpack_from('<III', after, at + samples_at)
                count, nibbles = struct.unpack_from('>II', after, offset)
                frames = -(-count // 14) * 8
                channels = size // (sound.DSP_HEADER_SIZE + frames + (-frames) % 32)
                self.assertIn(channels, (1, 2))
                self.assertEqual(size, channels * (sound.DSP_HEADER_SIZE + frames + (-frames) % 32))
                seen += 1
        self.assertGreater(seen, 3)

    def test_with_invaders_definitions(self):
        defs = support.invader()
        xbox, after, report = self.converted(defs, 1)
        self.assertGreater(report.bitmaps + report.sounds, 0)


if __name__ == '__main__':
    unittest.main()
