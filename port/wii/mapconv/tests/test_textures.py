import random
import struct
import unittest

from mapconv import textures as t
from mapconv.tests import support


def gx_rgb5a3_decode(data, width, height):
    """Dolphin's reading of RGB5A3, to check the encoder against."""
    out = {}
    at = 0
    for ty in range(0, height, 4):
        for tx in range(0, width, 4):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 4):
                    v = struct.unpack_from('>H', data, at)[0]
                    at += 2
                    if v & 0x8000:
                        r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
                        px = (255, r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)
                    else:
                        a, r, g, b = (v >> 12) & 7, (v >> 8) & 15, (v >> 4) & 15, v & 15
                        px = (a << 5 | a << 2 | a >> 1, r * 17, g * 17, b * 17)
                    out[x, y] = px
    return out


def cmpr_decode(data, width, height):
    """CMPR to (a, r, g, b), read the way GX reads it."""
    return t.decode_dxt(t.cmpr_to_dxt1(data, width, height), width, height, t.DXT1)


class Textures(unittest.TestCase):
    def setUp(self):
        self.rng = random.Random(3)

    def test_swizzle_and_unswizzle_are_inverse(self):
        for w, h in ((4, 4), (8, 4), (4, 16), (32, 8), (1, 1), (2, 8)):
            data = self.rng.randbytes(w * h * 2)
            self.assertEqual(t.unswizzle(t.swizzle(data, w, h, 2), w, h, 2), data)

    def test_the_swizzle_is_the_xboxs_morton_order(self):
        # In a 4x4 texture, (1, 0) is the second pixel and (0, 1) the third.
        linear = bytes(range(16))
        swizzled = t.swizzle(linear, 4, 4, 1)
        self.assertEqual(list(swizzled[:4]), [0, 1, 4, 5])
        self.assertEqual(list(swizzled[4:8]), [2, 3, 6, 7])

    def test_dxt1_moves_into_cmpr_unchanged(self):
        for w, h in ((4, 4), (8, 8), (16, 8), (8, 32), (64, 64)):
            data = self.rng.randbytes(max(1, w // 4) * max(1, h // 4) * 8)
            cmpr = t.dxt1_to_cmpr(data, w, h)
            self.assertEqual(len(cmpr), t.gx_size(t.GX_CMPR, w, h))
            self.assertEqual(t.cmpr_to_dxt1(cmpr, w, h), data)
            self.assertEqual(cmpr_decode(cmpr, w, h), t.decode_dxt(data, w, h, t.DXT1))

    def test_cmpr_puts_the_first_pixel_in_the_top_bits(self):
        block = struct.pack('<HHI', 0xF800, 0x001F, 0b11_10_01_00)   # row 0: indices 0, 1, 2, 3
        cmpr = t.dxt1_to_cmpr(block, 4, 4)
        self.assertEqual(cmpr[:4], bytes([0xF8, 0x00, 0x00, 0x1F]))
        self.assertEqual(cmpr[4], 0b00_01_10_11)

    def test_rgb5a3_keeps_what_it_can_hold(self):
        w = h = 8
        pixels = [(self.rng.choice((255, self.rng.randrange(256))), self.rng.randrange(256),
                   self.rng.randrange(256), self.rng.randrange(256)) for _ in range(w * h)]
        decoded = gx_rgb5a3_decode(t.encode_rgb5a3(pixels, w, h), w, h)
        for i, (a, r, g, b) in enumerate(pixels):
            da, dr, dg, db = decoded[i % w, i // w]
            self.assertLessEqual(abs(da - a), 36)
            for x, y in ((dr, r), (dg, g), (db, b)):
                self.assertLessEqual(abs(x - y), 8 if a >> 5 == 7 else 17)

    def test_rgba8_tiles_hold_ar_then_gb(self):
        pixels = [(i, i + 1, i + 2, i + 3) for i in range(16)]
        data = t.encode_rgba8(pixels, 4, 4)
        self.assertEqual(data[:4], bytes([0, 1, 1, 2]))
        self.assertEqual(data[32:36], bytes([2, 3, 3, 4]))

    def test_pixel_meanings_follow_the_engine(self):
        # bitmap_format_to_a8r8g8b8: a8 is black, a8y8 is alpha in the high byte.
        self.assertEqual(t.decode_raw(b'\x80', 1, 1, t.A8), [(0x80, 0, 0, 0)])
        self.assertEqual(t.decode_raw(b'\x10\x80', 1, 1, t.A8Y8), [(0x80, 0x10, 0x10, 0x10)])
        self.assertEqual(t.decode_raw(struct.pack('<H', 0xF800), 1, 1, t.R5G6B5), [(255, 255, 0, 0)])
        self.assertEqual(t.decode_raw(struct.pack('<H', 0x801F), 1, 1, t.A1R5G5B5), [(255, 0, 0, 255)])
        self.assertEqual(t.decode_raw(bytes([1, 2, 3, 4]), 1, 1, t.A8R8G8B8), [(4, 3, 2, 1)])

    def test_the_bump_palette_is_the_engines(self):
        palette = t.load_vector_palette(support.SOURCE_ROOT)
        self.assertEqual(palette[0], 0xFF7A19CC)
        self.assertEqual(t.decode_raw(b'\x00', 1, 1, t.P8, palette), [(0xFF, 0x7A, 0x19, 0xCC)])

    def test_mipmaps_and_cube_faces_are_where_the_engine_puts_them(self):
        levels = t.mip_levels(16, 8, 1, t.TYPE_2D, 4, t.DXT1)
        self.assertEqual([(w, h) for w, h, *_ in levels], [(16, 8), (8, 4), (4, 4), (4, 4), (4, 4)])
        self.assertEqual(levels[0][-1], 64)
        cube = t.mip_levels(8, 8, 1, t.TYPE_CUBE, 1, t.A8R8G8B8)
        self.assertEqual([lvl[-1] for lvl in cube], [8 * 8 * 6 * 4, 4 * 4 * 6 * 4])

    def test_a_whole_bitmap_converts_level_by_level(self):
        palette = t.load_vector_palette(support.SOURCE_ROOT)
        for fmt in t.BITS_PER_PIXEL:
            for kind in (t.TYPE_2D, t.TYPE_CUBE):
                levels = t.mip_levels(16, 16, 1, kind, 2, fmt)
                pixels = self.rng.randbytes(sum(lvl[-1] for lvl in levels))
                flags = t.FLAG_COMPRESSED if fmt in t.COMPRESSED else t.FLAG_SWIZZLED
                gx, out = t.convert_bitmap(pixels, 16, 16, 1, kind, flags, 2, fmt, palette)
                self.assertEqual(gx, t.gx_format_for(fmt))
                expected = sum(t.gx_size(gx, w, h) * faces for w, h, d, faces, _ in levels)
                self.assertEqual(len(out), expected, (fmt, kind))

    def test_the_formats_go_where_the_plan_says(self):
        self.assertEqual(t.gx_format_for(t.DXT1), t.GX_CMPR)
        for fmt in (t.X8R8G8B8, t.A8R8G8B8, t.P8):
            self.assertEqual(t.gx_format_for(fmt), t.GX_RGBA8)
        for fmt in (t.A8, t.Y8, t.AY8, t.A8Y8, t.R5G6B5, t.A1R5G5B5, t.A4R4G4B4, t.DXT3, t.DXT5):
            self.assertEqual(t.gx_format_for(fmt), t.GX_RGB5A3)

    def test_3d_textures_are_refused(self):
        with self.assertRaises(ValueError):
            t.convert_bitmap(bytes(1024), 8, 8, 2, t.TYPE_3D, 0, 0, t.A8R8G8B8)


if __name__ == '__main__':
    unittest.main()
