"""Bitmaps: the Xbox's texture formats to the Wii's GX formats.

- DXT1 goes to CMPR, which is the same compression in a different order (8x8
  tiles of four 4x4 blocks, big-endian colours, the first pixel in the top bits),
  so it is moved, not recompressed.
- The 32-bit formats and P8 (whose palette is the engine's global_vector_palette,
  bump vectors that want the precision) go to RGBA8.
- Everything else (the 8- and 16-bit formats, DXT3 and DXT5) goes to RGB5A3.

What the pixels mean is bitmap_format_to_a8r8g8b8 in source/bitmaps/bitmaps.c, and
where each mipmap is, bitmap_2d_address and bitmap_cube_map_address there: the
levels one after another, each the size the engine computes (4 pixels at the
least when compressed); a cube map's level holds its six faces. The Xbox swizzles
the uncompressed formats (Morton order, x bit first), which is undone first.
"""

import re
import struct
from pathlib import Path

# BitmapDataFormat (bitmap_group.c)
A8, Y8, AY8, A8Y8, R5G6B5, A1R5G5B5, A4R4G4B4, X8R8G8B8, A8R8G8B8 = 0, 1, 2, 3, 6, 8, 9, 10, 11
DXT1, DXT3, DXT5, P8 = 14, 15, 16, 17
BITS_PER_PIXEL = {A8: 8, Y8: 8, AY8: 8, A8Y8: 16, R5G6B5: 16, A1R5G5B5: 16, A4R4G4B4: 16,
                  X8R8G8B8: 32, A8R8G8B8: 32, DXT1: 4, DXT3: 8, DXT5: 8, P8: 8}
COMPRESSED = {DXT1, DXT3, DXT5}

# BitmapDataType
TYPE_2D, TYPE_3D, TYPE_CUBE = 0, 1, 2
# BitmapDataFlags
FLAG_COMPRESSED, FLAG_PALETTIZED, FLAG_SWIZZLED = 1 << 1, 1 << 2, 1 << 3

# The formats a converted bitmap is in (port/wii/halo_wii_map.h): the GX format
# number, offset so it is not taken for one of the Xbox's.
GX_FORMAT_BASE = 0x40
GX_RGB5A3, GX_RGBA8, GX_CMPR = 0x5, 0x6, 0xE
TILE = {GX_RGB5A3: (4, 4, 32), GX_RGBA8: (4, 4, 64), GX_CMPR: (8, 8, 32)}  # w, h, bytes


def gx_format_for(xbox_format):
    if xbox_format == DXT1:
        return GX_CMPR
    if xbox_format in (X8R8G8B8, A8R8G8B8, P8):
        return GX_RGBA8
    if xbox_format in BITS_PER_PIXEL:
        return GX_RGB5A3
    raise ValueError(f'bitmap format {xbox_format} is not one the Xbox has')


def load_vector_palette(source_root):
    """global_vector_palette (bitmaps.c): 256 0xAARRGGBB colours."""
    text = (Path(source_root) / 'source' / 'bitmaps' / 'bitmaps.c').read_text(errors='replace')
    body = text[text.index('global_vector_palette[NUMBER_OF_ENTRIES_IN_PALETTE] ='):]
    body = body[body.index('{') + 1:body.index('}')]
    palette = [int(v, 16) for v in re.findall(r'0x[0-9A-Fa-f]{8}', body)]
    if len(palette) != 256:
        raise ValueError(f'global_vector_palette has {len(palette)} entries, not 256')
    return palette


# ---------- layout

def mip_levels(width, height, depth, kind, mipmap_count, xbox_format):
    """(width, height, depth, faces, byte size) of each level, as the engine lays them out."""
    minimum = 4 if xbox_format in COMPRESSED else 1
    bpp = BITS_PER_PIXEL[xbox_format]
    faces = 6 if kind == TYPE_CUBE else 1
    levels = []
    w, h, d = width, height if kind != TYPE_CUBE else width, depth if kind == TYPE_3D else 1
    for _ in range(mipmap_count + 1):
        levels.append((w, h, d, faces, w * h * d * faces * bpp // 8))
        w, h, d = max(minimum, w >> 1), max(minimum, h >> 1), max(1, d >> 1)
    return levels


def unswizzle(data, width, height, bytes_per_pixel):
    """The Xbox's swizzle undone: pixel (x, y) is at the Morton index of its bits."""
    xs, ys = _morton_bits(width, height)
    out = bytearray(width * height * bytes_per_pixel)
    b = bytes_per_pixel
    for y in range(height):
        row = y * width
        for x in range(width):
            src = (xs[x] | ys[y]) * b
            dst = (row + x) * b
            out[dst:dst + b] = data[src:src + b]
    return bytes(out)


def swizzle(data, width, height, bytes_per_pixel):
    xs, ys = _morton_bits(width, height)
    out = bytearray(width * height * bytes_per_pixel)
    b = bytes_per_pixel
    for y in range(height):
        for x in range(width):
            src = (y * width + x) * b
            dst = (xs[x] | ys[y]) * b
            out[dst:dst + b] = data[src:src + b]
    return bytes(out)


def _morton_bits_3d(width, height, depth):
    """Each coordinate's share of the Morton index: x, y then z bits, while
    each dimension still has bits (XGSwizzleBox's order)."""
    shares = [[0] * width, [0] * height, [0] * depth]
    dest, bit = 1, 1
    while bit < width or bit < height or bit < depth:
        for axis, size in enumerate((width, height, depth)):
            if bit < size:
                for v in range(size):
                    if v & bit:
                        shares[axis][v] |= dest
                dest <<= 1
        bit <<= 1
    return shares


def unswizzle_3d(data, width, height, depth, bytes_per_pixel):
    xs, ys, zs = _morton_bits_3d(width, height, depth)
    out = bytearray(width * height * depth * bytes_per_pixel)
    b = bytes_per_pixel
    for z in range(depth):
        for y in range(height):
            row = (z * height + y) * width
            for x in range(width):
                src = (xs[x] | ys[y] | zs[z]) * b
                out[(row + x) * b:(row + x + 1) * b] = data[src:src + b]
    return bytes(out)


def swizzle_3d(data, width, height, depth, bytes_per_pixel):
    xs, ys, zs = _morton_bits_3d(width, height, depth)
    out = bytearray(width * height * depth * bytes_per_pixel)
    b = bytes_per_pixel
    for z in range(depth):
        for y in range(height):
            for x in range(width):
                src = ((z * height + y) * width + x) * b
                dst = (xs[x] | ys[y] | zs[z]) * b
                out[dst:dst + b] = data[src:src + b]
    return bytes(out)


def _morton_bits(width, height):
    xs, ys = [0] * width, [0] * height
    dest, bit = 1, 1
    while bit < width or bit < height:
        if bit < width:
            for x in range(width):
                if x & bit:
                    xs[x] |= dest
            dest <<= 1
        if bit < height:
            for y in range(height):
                if y & bit:
                    ys[y] |= dest
            dest <<= 1
        bit <<= 1
    return xs, ys


# ---------- decoding to (a, r, g, b)

def _expand(v, bits):
    return (v << (8 - bits)) | (v >> (2 * bits - 8)) if bits < 8 else v


def decode_raw(data, width, height, xbox_format, palette=None):
    """Linear pixels of an uncompressed format to a list of (a, r, g, b)."""
    n = width * height
    f = xbox_format
    if f == A8:
        return [(v, 0, 0, 0) for v in data[:n]]
    if f == Y8:
        return [(255, v, v, v) for v in data[:n]]
    if f == AY8:
        return [(v, v, v, v) for v in data[:n]]
    if f == P8:
        if palette is None:
            raise ValueError('a P8 bitmap needs the engine\'s vector palette')
        return [((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255)
                for c in (palette[v] for v in data[:n])]
    if f in (X8R8G8B8, A8R8G8B8):
        # The engine takes the word as it is, alpha included, for both.
        return [(a, r, g, b) for b, g, r, a in struct.iter_unpack('4B', data[:n * 4])]
    words = struct.unpack_from(f'<{n}H', data)
    if f == A8Y8:
        return [(w >> 8, w & 255, w & 255, w & 255) for w in words]
    if f == R5G6B5:
        return [(255, _expand(w >> 11, 5), _expand((w >> 5) & 63, 6), _expand(w & 31, 5)) for w in words]
    if f == A1R5G5B5:
        return [((w >> 15) * 255, _expand((w >> 10) & 31, 5), _expand((w >> 5) & 31, 5), _expand(w & 31, 5))
                for w in words]
    if f == A4R4G4B4:
        return [(((w >> 12) & 15) * 17, ((w >> 8) & 15) * 17, ((w >> 4) & 15) * 17, (w & 15) * 17)
                for w in words]
    raise ValueError(f'format {f} is not an uncompressed one')


def _colors(c0, c1, four):
    def rgb(c):
        return _expand(c >> 11, 5), _expand((c >> 5) & 63, 6), _expand(c & 31, 5)
    a, b = rgb(c0), rgb(c1)
    if four:
        return [(255,) + a, (255,) + b,
                (255,) + tuple((2 * x + y) // 3 for x, y in zip(a, b)),
                (255,) + tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
    return [(255,) + a, (255,) + b, (255,) + tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0, 0)]


def decode_dxt(data, width, height, xbox_format):
    """DXT1/3/5 to a list of (a, r, g, b), width x height."""
    block = 8 if xbox_format == DXT1 else 16
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    out = [(0, 0, 0, 0)] * (width * height)
    for by in range(bh):
        for bx in range(bw):
            at = (by * bw + bx) * block
            colour = at if xbox_format == DXT1 else at + 8
            c0, c1, bits = struct.unpack_from('<HHI', data, colour)
            palette = _colors(c0, c1, xbox_format != DXT1 or c0 > c1)
            if xbox_format == DXT3:
                alpha_bits = int.from_bytes(data[at:at + 8], 'little')
                alphas = [((alpha_bits >> (4 * i)) & 15) * 17 for i in range(16)]
            elif xbox_format == DXT5:
                a0, a1 = data[at], data[at + 1]
                ramp = [a0, a1] + ([((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1 else
                                   [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
                alpha_bits = int.from_bytes(data[at + 2:at + 8], 'little')
                alphas = [ramp[(alpha_bits >> (3 * i)) & 7] for i in range(16)]
            else:
                alphas = None
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < width and y < height:
                    a, r, g, b = palette[(bits >> (2 * i)) & 3]
                    out[y * width + x] = (alphas[i] if alphas else a, r, g, b)
    return out


# ---------- encoding to GX

def _tiles(width, height, tile_w, tile_h):
    for ty in range(0, max(height, 1), tile_h):
        for tx in range(0, max(width, 1), tile_w):
            yield tx, ty


def encode_rgb5a3(pixels, width, height):
    out = bytearray()
    for tx, ty in _tiles(width, height, 4, 4):
        for y in range(ty, ty + 4):
            for x in range(tx, tx + 4):
                a, r, g, b = pixels[y * width + x] if x < width and y < height else (0, 0, 0, 0)
                if a >> 5 == 7:
                    value = 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3
                else:
                    value = (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4
                out += value.to_bytes(2, 'big')
    return bytes(out)


def encode_rgba8(pixels, width, height):
    out = bytearray()
    for tx, ty in _tiles(width, height, 4, 4):
        tile = [pixels[y * width + x] if x < width and y < height else (0, 0, 0, 0)
                for y in range(ty, ty + 4) for x in range(tx, tx + 4)]
        out += bytes(c for a, r, g, b in tile for c in (a, r))
        out += bytes(c for a, r, g, b in tile for c in (g, b))
    return bytes(out)


def dxt1_to_cmpr(data, width, height):
    """DXT1 blocks moved into CMPR's tiles: no colour is recomputed."""
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    out = bytearray()
    for tx, ty in _tiles(bw, bh, 2, 2):
        for by, bx in ((ty, tx), (ty, tx + 1), (ty + 1, tx), (ty + 1, tx + 1)):
            if bx >= bw or by >= bh:
                out += bytes(8)
                continue
            c0, c1, bits = struct.unpack_from('<HHI', data, (by * bw + bx) * 8)
            rows = bytes(sum(((bits >> (2 * (4 * y + x))) & 3) << (6 - 2 * x) for x in range(4))
                         for y in range(4))
            out += struct.pack('>HH', c0, c1) + rows
    return bytes(out)


def cmpr_to_dxt1(data, width, height):
    """The inverse of dxt1_to_cmpr (for the tests)."""
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    blocks = {}
    at = 0
    for tx, ty in _tiles(bw, bh, 2, 2):
        for by, bx in ((ty, tx), (ty, tx + 1), (ty + 1, tx), (ty + 1, tx + 1)):
            c0, c1 = struct.unpack_from('>HH', data, at)
            rows = data[at + 4:at + 8]
            bits = sum(((rows[y] >> (6 - 2 * x)) & 3) << (2 * (4 * y + x)) for y in range(4) for x in range(4))
            blocks[(bx, by)] = struct.pack('<HHI', c0, c1, bits)
            at += 8
    return b''.join(blocks[(bx, by)] for by in range(bh) for bx in range(bw))


def gx_size(gx, width, height):
    tw, th, tb = TILE[gx]
    if gx == GX_CMPR:
        width, height = max(1, (width + 3) // 4) * 4, max(1, (height + 3) // 4) * 4
    return ((width + tw - 1) // tw) * ((height + th - 1) // th) * tb


def convert_image(data, width, height, xbox_format, swizzled, palette=None):
    """One 2D image (a level, or a face of one) to its GX format: (gx, bytes)."""
    gx = gx_format_for(xbox_format)
    if xbox_format == DXT1:
        return gx, dxt1_to_cmpr(data, width, height)
    if xbox_format in COMPRESSED:
        pixels = decode_dxt(data, width, height, xbox_format)
    else:
        if swizzled:
            data = unswizzle(data, width, height, BITS_PER_PIXEL[xbox_format] // 8)
        pixels = decode_raw(data, width, height, xbox_format, palette)
    if gx == GX_RGBA8:
        return gx, encode_rgba8(pixels, width, height)
    return gx, encode_rgb5a3(pixels, width, height)


def convert_bitmap(pixels, width, height, depth, kind, flags, mipmap_count, xbox_format, palette=None):
    """A bitmap's pixels (every level, every face) in its GX format, levels one
    after another, faces within a level one after another: (gx, bytes).

    GX has no 3D textures, so a 3D one's slices are 2D images, one after another
    in each level, for the renderer to choose between."""
    gx = gx_format_for(xbox_format)
    swizzled = bool(flags & FLAG_SWIZZLED) and xbox_format not in COMPRESSED
    if kind == TYPE_3D:
        out = bytearray()
        at = 0
        bpp = BITS_PER_PIXEL[xbox_format]
        for w, h, d, _, size in mip_levels(width, height, depth, kind, mipmap_count, xbox_format):
            level = pixels[at:at + size]
            if len(level) < size:
                raise ValueError(f'the pixels end {size - len(level)} bytes short of level {w}x{h}x{d}')
            if swizzled:
                level = unswizzle_3d(level, w, h, d, bpp // 8)
            slice_size = size // d
            for z in range(d):
                _, converted = convert_image(level[z * slice_size:(z + 1) * slice_size], w, h, xbox_format,
                                             False, palette)
                out += converted
            at += size
        return gx, bytes(out)
    out = bytearray()
    at = 0
    bpp = BITS_PER_PIXEL[xbox_format]
    for w, h, d, faces, size in mip_levels(width, height, depth, kind, mipmap_count, xbox_format):
        face_size = size // faces
        for face in range(faces):
            image = pixels[at + face * face_size:at + (face + 1) * face_size]
            if len(image) < face_size:
                raise ValueError(f'the pixels end {face_size - len(image)} bytes short of level {w}x{h}')
            _, converted = convert_image(image, w, h, xbox_format, bool(flags & FLAG_SWIZZLED), palette)
            out += converted
        at += size
    return gx, bytes(out)
