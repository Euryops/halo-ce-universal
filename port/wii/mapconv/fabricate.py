"""Made-up Xbox cache maps, for checking the swap without a disc.

fabricate_map() builds a map in the Xbox's layout from a set of definitions: a
header, a tag cache with a tag header, a tag table, names, one tag of each group
asked for (with random numbers, random block counts, data, references, and the
Xbox's vertex and index buffers where models and BSPs have them), and a
structure BSP image for each of the scenario's BSPs. Every byte the walk should
not touch (pads, strings) is random too, so a round trip that changed one shows.
"""

import random
import struct

from .swap import XBOX_TAG_CACHE, TAG_CACHE_SIZE, WII_TAG_CACHE
from .tags import (HEADER_SIZE, HEADER_SIGNATURE, FOOTER_SIGNATURE, TAGS_SIGNATURE, SBSP,
                   XBOX_CACHE_VERSION, TAG_HEADER_SIZE, TAG_INSTANCE_SIZE, BSP_HEADER_SIZE,
                   D3D_RESOURCE_SIZE, HS_DATUM_ARRAY_SIGNATURE, VERTEX_STRUCTS,
                   DATA_CONVERTED)
from .swap import HS_DATA_ARRAY_SIZE, HS_SYNTAX_NODE_SIZE


def _harmless(rng):
    """A random word that is not an address in either tag cache."""
    while True:
        value = rng.getrandbits(32)
        if not (XBOX_TAG_CACHE <= value < XBOX_TAG_CACHE + TAG_CACHE_SIZE
                or WII_TAG_CACHE <= value < WII_TAG_CACHE + TAG_CACHE_SIZE):
            return value


class Address(int):
    """An address the fabricator wrote: the swap must move it to the Wii's cache."""


class Region:
    def __init__(self, defs, rng, base, max_depth=3, max_count=3, media=False):
        self.defs, self.rng, self.base = defs, rng, base
        self.media = [] if media else None     # (offset of a file offset, bytes) for the file
        self.buf = bytearray()
        self.max_depth, self.max_count = max_depth, max_count
        self.names = []
        self.bsp_entries = []      # offsets of ScenarioBSP elements
        # What was written, for a test to check the swapped map against:
        self.pointers = {}         # offset -> the address there
        self.numbers = {}          # offset -> width, for the definitions' numbers
    def alloc(self, size, align=4):
        self.buf += bytes((-len(self.buf)) % align)
        at = len(self.buf)
        self.buf += self.rng.randbytes(size)
        if len(self.buf) > TAG_CACHE_SIZE:
            raise ValueError('made-up tags outgrew the tag cache')
        return at

    def address(self, offset):
        return Address(self.base + offset)

    def put(self, fmt, offset, *values):
        struct.pack_into('<' + fmt, self.buf, offset, *values)
        at = offset
        for code, value in zip(fmt.replace('4H', 'HHHH'), values):
            self.numbers.pop(at, None)
            if isinstance(value, Address):
                self.pointers[at] = int(value)
            else:
                self.pointers.pop(at, None)
            at += struct.calcsize('<' + code)

    def name(self, text):
        at = self.alloc(len(text) + 1, 1)
        self.buf[at:at + len(text) + 1] = text.encode() + b'\0'
        self.names.append(at)
        return at

    def fill(self, struct_name, offset, depth=0):
        defs, rng = self.defs, self.rng
        for f in defs.layout(struct_name):
            at = offset + f.offset
            if f.kind == 'swap':
                self.numbers[at] = f.width
            elif f.kind == 'pointer':
                self.put('I', at, 0 if rng.random() < .5 else self.address(rng.choice(self.names or [0])))
            elif f.kind == 'reflexive':
                count = rng.randint(0, self.max_count) if depth < self.max_depth else 0
                if f.struct == 'ScenarioBSP':
                    count = max(count, 1)
                size = defs.size(f.struct)
                target = self.alloc(count * size) if count else 0
                self.put('III', at, count, self.address(target) if count else 0, 0)
                for i in range(count):
                    self.fill(f.struct, target + i * size, depth + 1)
            elif f.kind == 'dependency':
                name = self.name(f'tags\\made-up\\{rng.getrandbits(24):06x}')
                self.put('IIII', at, rng.getrandbits(32), self.address(name), 0, rng.getrandbits(32))
            elif f.kind == 'data':
                self._data(f, at)
        for owner, base in sorted({(f.owner, offset + f.owner_offset) for f in defs.layout(struct_name)}):
            if owner in FABRICATE_RULES:
                FABRICATE_RULES[owner](self, base)
            if self.media is not None and owner in MEDIA_RULES:
                MEDIA_RULES[owner](self, base)

    def _data(self, f, at):
        rng, key = self.rng, (f.owner, f.name)
        if key in DATA_CONVERTED:   # in the file, not the tag cache
            self.put('IIIII', at, rng.randint(1, 4096), 0, rng.getrandbits(31), 0, 0)
            return
        if rng.random() < .2:
            self.put('IIIII', at, 0, 0, 0, 0, 0)
            return
        if key == ('Scenario', 'script syntax data'):
            nodes = rng.randint(0, 8)
            size = HS_DATA_ARRAY_SIZE + nodes * HS_SYNTAX_NODE_SIZE
            target = self.alloc(size)
            self.put('hh', target + 32, nodes, HS_SYNTAX_NODE_SIZE)
            self.put('I', target + 40, HS_DATUM_ARRAY_SIGNATURE)
            self.put('I', target + 52, self.address(target + HS_DATA_ARRAY_SIZE))
        else:
            size = rng.randint(1, 64) * 4
            target = self.alloc(size)
        self.put('IIIII', at, size, 0, 0, self.address(target), 0)

    def descriptor(self, data_offset):
        at = self.alloc(D3D_RESOURCE_SIZE)
        self.put('III', at, _harmless(self.rng), self.address(data_offset), _harmless(self.rng))
        return at

    def vertices(self, vertex_type, count):
        name = VERTEX_STRUCTS[vertex_type]
        return self.alloc(count * self.defs.size(name))


def _bitmap_data(region, at):
    for o in (36, 40, 44):
        region.put('I', at + o, _harmless(region.rng))


def _model_part(region, at):
    rng = region.rng
    triangle_type, triangles = rng.randint(0, 1), rng.randint(0, 5)
    vertex_type, vertices = rng.randint(4, 5), rng.randint(0, 5)
    indices = (triangles * 3 if triangle_type == 0 else triangles + 2) if triangles else 0
    index_data = region.alloc(indices * 2) if indices else 0
    region.put('HH', at + 68, triangle_type, 0)
    region.put('I', at + 72, triangles)
    region.put('I', at + 76, region.address(index_data) if indices else 0)
    region.put('I', at + 80, region.address(region.descriptor(index_data)) if indices else 0)
    region.put('HH', at + 84, vertex_type, 0)
    region.put('I', at + 88, vertices)
    region.put('I', at + 96, _harmless(rng))
    data = region.vertices(vertex_type, vertices) if vertices else 0
    region.put('I', at + 100, region.address(region.descriptor(data)) if vertices else 0)


def _bsp_material(region, at):
    rng = region.rng
    for start, types in ((176, (0, 1)), (196, (2, 3))):
        vertex_type, vertices = rng.choice(types), rng.randint(0, 5)
        region.put('HH', at + start, vertex_type, 0)
        region.put('I', at + start + 4, vertices)
        region.put('I', at + start + 12, _harmless(rng))
        data = region.vertices(vertex_type, vertices) if vertices else 0
        region.put('I', at + start + 16, region.address(region.descriptor(data)) if vertices else 0)


def _scenario_bsp(region, at):
    region.bsp_entries.append(at)


# The formats a made-up bitmap is in, and whether the Xbox swizzles them.
MEDIA_FORMATS = [0, 1, 2, 3, 6, 8, 9, 10, 11, 14, 15, 16, 17]


def _media_bitmap(region, at):
    from .textures import mip_levels, COMPRESSED, FLAG_COMPRESSED, FLAG_SWIZZLED, TYPE_2D, TYPE_CUBE
    rng = region.rng
    xbox_format = rng.choice(MEDIA_FORMATS)
    kind = rng.choice((TYPE_2D, TYPE_2D, TYPE_CUBE))
    width = 1 << rng.randint(2, 5)
    height = width if kind == TYPE_CUBE else 1 << rng.randint(2, 5)
    mipmaps = rng.randint(0, 2)
    flags = FLAG_COMPRESSED if xbox_format in COMPRESSED else FLAG_SWIZZLED
    pixels = rng.randbytes(sum(level[-1] for level in mip_levels(width, height, 1, kind, mipmaps, xbox_format)))
    region.put('I4H', at, 0x6269746D, width, height, 1, kind)
    region.put('HHHHH', at + 12, xbox_format, flags, 0, 0, mipmaps)
    region.put('II', at + 24, 0, len(pixels))
    region.media.append((at + 24, pixels))


def _media_permutation(region, at):
    from .convert import field_offset
    rng = region.rng
    compression = rng.randint(0, 1)
    data = rng.randbytes(72 * rng.randint(1, 6) if compression else 4 * rng.randint(8, 200))
    samples = at + field_offset(region.defs, 'SoundPermutation', 'samples')
    region.put('H', at + field_offset(region.defs, 'SoundPermutation', 'format'), compression)
    region.put('IIIII', samples, len(data), 0, 0, 0, 0)
    region.media.append((samples + 8, data))


def _media_sound(region, at):
    from .convert import field_offset
    region.put('H', at + field_offset(region.defs, 'Sound', 'sample rate'), region.rng.randint(0, 1))
    region.put('H', at + field_offset(region.defs, 'Sound', 'channel count'), region.rng.randint(0, 1))


FABRICATE_RULES = {
    'BitmapData': _bitmap_data,
    'ModelGeometryPart': _model_part,
    'ScenarioStructureBSPMaterial': _bsp_material,
    'ScenarioBSP': _scenario_bsp,
}


def fabricate_map(defs, groups=None, seed=0, name='madeup', build='01.10.12.2276',
                  max_depth=3, max_count=3, media=False, truth=None):
    """A made-up Xbox map with one tag of each group (all of defs' groups by
    default; a scenario is always there, as maps have one). If truth is a list,
    it is given (file offset, Region) for the tag cache and each BSP, whose
    pointers and numbers say what the fabricator wrote where."""
    rng = random.Random(seed)
    sbsp_root = next((g for g, n in defs.roots.items() if n == 'ScenarioStructureBSP'), SBSP)
    if groups is None:
        groups = sorted(defs.roots)
    groups = [g for g in groups if g != sbsp_root]
    tags = Region(defs, rng, XBOX_TAG_CACHE, max_depth, max_count, media)
    tags.alloc(TAG_HEADER_SIZE)
    instances = tags.alloc((len(groups) + 1) * TAG_INSTANCE_SIZE)
    for i, group in enumerate(groups):
        root = defs.roots[group]
        body = tags.alloc(defs.size(root))
        name_at = tags.name(f'made-up\\{root.lower()}')
        tags.fill(root, body)
        tags.put('IIIIIIII', instances + i * TAG_INSTANCE_SIZE, group, _harmless(rng), _harmless(rng),
                 (rng.getrandbits(16) << 16) | i, tags.address(name_at), tags.address(body),
                 _harmless(rng), _harmless(rng))
    # The BSP's own table entry, whose body is not in the tag cache.
    sbsp_index = len(groups)
    tags.put('IIIIIIII', instances + sbsp_index * TAG_INSTANCE_SIZE, SBSP, _harmless(rng), _harmless(rng),
             sbsp_index, tags.address(tags.name('made-up\\bsp')), 0, 0, 0)
    for count_at in (16, 24):   # vertex and index buffer lists
        count = rng.randint(0, 3)
        table = tags.alloc(count * D3D_RESOURCE_SIZE)
        for i in range(count):
            tags.put('III', table + i * D3D_RESOURCE_SIZE, _harmless(rng),
                     tags.address(tags.alloc(16)), _harmless(rng))
        tags.put('II', count_at, count, tags.address(table) if count else 0)
    tags.put('IiIIIIIII', 0, tags.address(instances), rng.getrandbits(31), rng.getrandbits(32),
             sbsp_index + 1, *struct.unpack_from('<IIII', tags.buf, 16), TAGS_SIGNATURE)

    # Each BSP: its own image, loaded after the tags in the tag cache, in the file after the tags.
    tag_offset = HEADER_SIZE
    file_end = tag_offset + len(tags.buf)
    images = []
    address = XBOX_TAG_CACHE + ((len(tags.buf) + 0xFFF) & ~0xFFF)
    for entry in tags.bsp_entries:
        bsp = Region(defs, rng, address, max_depth, max_count)
        bsp.names = []
        bsp.alloc(BSP_HEADER_SIZE)
        body = bsp.alloc(defs.size('ScenarioStructureBSP'))
        bsp.fill('ScenarioStructureBSP', body)
        lists = []
        for _ in range(2):
            count = rng.randint(0, 2)
            table = bsp.alloc(count * D3D_RESOURCE_SIZE)
            for i in range(count):
                bsp.put('III', table + i * D3D_RESOURCE_SIZE, _harmless(rng),
                        bsp.address(bsp.alloc(8)), _harmless(rng))
            lists.append((count, bsp.address(table) if count else 0))
        bsp.put('IIIIII', 0, bsp.address(body), *lists[0], *lists[1], SBSP)
        if address + len(bsp.buf) > XBOX_TAG_CACHE + TAG_CACHE_SIZE:
            raise ValueError('made-up BSP outgrew the tag cache')
        file_end += (-file_end) % 0x800
        tags.put('III', entry, file_end, len(bsp.buf), Address(address))
        images.append((file_end, bytes(bsp.buf)))
        if truth is not None:
            truth.append((file_end, bsp))
        file_end += len(bsp.buf)

    for at, data in tags.media or ():
        file_end += (-file_end) % 32
        tags.put('I', at, file_end)
        images.append((file_end, data))
        file_end += len(data)
    if truth is not None:
        truth.insert(0, (tag_offset, tags))
    raw = bytearray(file_end)
    raw[tag_offset:tag_offset + len(tags.buf)] = tags.buf
    for at, image in images:
        raw[at:at + len(image)] = image
    struct.pack_into('<IIII', raw, 0, HEADER_SIGNATURE, XBOX_CACHE_VERSION, len(raw), rng.getrandbits(32))
    struct.pack_into('<II', raw, 16, tag_offset, len(tags.buf))
    raw[32:64] = name.encode().ljust(32, b'\0')
    raw[64:96] = build.encode().ljust(32, b'\0')
    struct.pack_into('<II', raw, 0x60, 0, rng.getrandbits(32))
    struct.pack_into('<I', raw, 0x7FC, FOOTER_SIGNATURE)
    return bytes(raw)


MEDIA_RULES = {
    'BitmapData': _media_bitmap,
    'SoundPermutation': _media_permutation,
    'Sound': _media_sound,
}
