"""Made-up Xbox cache maps, for checking the swap without a disc.

fabricate_map() builds a map in the Xbox's layout from a set of definitions: a
header, a tag cache with a tag header, a tag table, names, one tag of each group
asked for (with random numbers, random block counts, data, references, and the
Xbox's vertex and index buffers where models and BSPs have them), and a
structure BSP image for each of the scenario's BSPs. Every byte the walk should
not touch (pads, strings) is random too, so a round trip that changed one shows.
"""

import random
import re
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
        self.bitfields = {}        # offset -> (2-bit field, 6-bit field) of a bitfield byte
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
        codes = ''.join(code * int(count or 1) for count, code in re.findall(r'(\d*)(\w)', fmt))
        for code, value in zip(codes, values):
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
    from .tags import MATERIAL_VERTEX_DATA
    rng = region.rng
    counts = []
    for start, types in ((176, (0, 1)), (196, (2, 3))):
        vertex_type, vertices = rng.choice(types), rng.randint(0, 5)
        region.put('HH', at + start, vertex_type, 0)
        region.put('I', at + start + 4, vertices)
        region.put('I', at + start + 12, _harmless(rng))
        data = region.vertices(vertex_type, vertices) if vertices else 0
        region.put('I', at + start + 16, region.address(region.descriptor(data)) if vertices else 0)
        counts.append(vertices)
    for offset, rendered, lightmap in MATERIAL_VERTEX_DATA:
        r, l = region.defs.size(rendered), region.defs.size(lightmap)
        size = counts[0] * r + (counts[1] * l if rng.random() < .7 else 0)
        data = region.alloc(size) if size else 0
        for i in range(counts[0]):
            for f in region.defs.layout(rendered):
                region.numbers[data + i * r + f.offset] = f.width
        if size > counts[0] * r:
            for i in range(counts[1]):
                for f in region.defs.layout(lightmap):
                    region.numbers[data + counts[0] * r + i * l + f.offset] = f.width
        _data_header(region, at + offset, data, size)


def _scenario_bsp(region, at):
    region.bsp_entries.append(at)


# The formats a made-up bitmap is in, and whether the Xbox swizzles them.
MEDIA_FORMATS = [0, 1, 2, 3, 6, 8, 9, 10, 11, 14, 15, 16, 17]


def _media_bitmap(region, at):
    from .textures import mip_levels, COMPRESSED, FLAG_COMPRESSED, FLAG_SWIZZLED, TYPE_2D, TYPE_3D, TYPE_CUBE
    rng = region.rng
    xbox_format = rng.choice(MEDIA_FORMATS)
    kind = rng.choice((TYPE_2D, TYPE_2D, TYPE_CUBE, TYPE_3D))
    width = 1 << rng.randint(2, 5)
    height = width if kind == TYPE_CUBE else 1 << rng.randint(2, 5)
    depth = 1 << rng.randint(1, 3) if kind == TYPE_3D else 1
    mipmaps = rng.randint(0, 2)
    flags = FLAG_COMPRESSED if xbox_format in COMPRESSED else FLAG_SWIZZLED
    pixels = rng.randbytes(sum(level[-1] for level in mip_levels(width, height, depth, kind, mipmaps, xbox_format)))
    region.put('I4H', at, 0x6269746D, width, height, depth, kind)
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


def _numbers(region, at, width, count=1):
    for i in range(count):
        region.numbers[at + i * width] = width


def _data_header(region, at, target, size):
    region.put('IIIII', at, size, 0, 0, region.address(target) if size else 0, 0)


def _animation(region, at):
    from .convert import field_offset
    rng, defs = region.rng, region.defs
    field = lambda name: at + field_offset(defs, 'ModelAnimationsAnimation', name)
    nodes = rng.randint(1, 40)
    rotated, translated, scaled = ([rng.random() < .5 for _ in range(nodes)] for _ in range(3))

    def vector(bits):
        value = sum(1 << n for n, b in enumerate(bits) if b)
        return value & 0xFFFFFFFF, value >> 32
    compressed = rng.random() < .5
    frames = rng.randint(1, 5)
    frame_size = sum(8 * r + 12 * t + 4 * c for r, t, c in zip(rotated, translated, scaled))
    region.put('HH', field('frame count'), frames, frame_size)
    region.put('H', field('node count'), nodes)
    region.put('H', field('flags'), int(compressed) | rng.getrandbits(2) << 1)
    region.put('II', field('node transform flag data'), *vector(translated))
    region.put('II', field('node rotation flag data'), *vector(rotated))
    region.put('II', field('node scale flag data'), *vector(scaled))

    default_size = sum(8 * (not r) + 12 * (not t) + 4 * (not c) for r, t, c in zip(rotated, translated, scaled))
    default = region.alloc(default_size) if default_size else 0
    cursor = default
    for r, t, c in zip(rotated, translated, scaled):
        for animated, width, count in ((r, 2, 4), (t, 4, 3), (c, 4, 1)):
            if not animated:
                _numbers(region, cursor, width, count)
                cursor += width * count
    _data_header(region, field('default data'), default, default_size)

    uncompressed = frames * frame_size
    parts = []      # (offset in the compressed block, width, count)
    if compressed:
        def keys(count):
            headers, first = [], 0
            for _ in range(count):
                n = rng.randint(0, 3)
                headers.append(first << 12 | n)
                first += n
            return headers, first
        rotation_headers, rotations = keys(sum(rotated))
        translation_headers, translations = keys(sum(translated))
        scale_headers, scales = keys(sum(scaled))
        layout, at_ = {}, 44 + 4 * len(rotation_headers)
        for name, width, count in (('translation headers', 4, len(translation_headers)),
                                   ('scale headers', 4, len(scale_headers)),
                                   ('rotation indices', 2, rotations), ('default rotations', 2, 3 * nodes),
                                   ('rotation keys', 2, 3 * rotations), ('translation indices', 2, translations),
                                   ('default translations', 4, 3 * nodes), ('translation keys', 4, 3 * translations),
                                   ('scale indices', 2, scales), ('default scales', 4, sum(scaled)),
                                   ('scale keys', 4, scales)):
            at_ += (-at_) % width
            layout[name] = at_
            parts.append((at_, width, count))
            at_ += width * count
        block = at_
    data = region.alloc(uncompressed + (block if compressed else 0))
    cursor = data
    for _ in range(frames):
        for r, t, c in zip(rotated, translated, scaled):
            for animated, width, count in ((r, 2, 4), (t, 4, 3), (c, 4, 1)):
                if animated:
                    _numbers(region, cursor, width, count)
                    cursor += width * count
    if compressed:
        base = data + uncompressed
        offsets = [layout[n] for n in ('rotation indices', 'default rotations', 'rotation keys',
                                       'translation headers', 'translation indices', 'default translations',
                                       'translation keys', 'scale headers', 'scale indices', 'default scales',
                                       'scale keys')]
        region.put('11I', base, *offsets)
        region.put(f'{len(rotation_headers)}I', base + 44, *rotation_headers)
        region.put(f'{len(translation_headers)}I', base + layout['translation headers'], *translation_headers)
        region.put(f'{len(scale_headers)}I', base + layout['scale headers'], *scale_headers)
        _numbers(region, base, 4, 11 + len(rotation_headers))
        for offset, width, count in parts:
            _numbers(region, base + offset, width, count)
    region.put('I', field('offset to compressed data'), uncompressed if compressed else 0)
    _data_header(region, field('frame data'), data, uncompressed + (block if compressed else 0))


def _recorded_animation(region, at):
    from .convert import field_offset
    from . import streams
    rng, defs = region.rng, region.defs
    field = lambda name: at + field_offset(defs, 'ScenarioRecordedAnimation', name)
    version, control_version = rng.randint(1, 4), rng.randint(0, 4)
    region.buf[field('version')] = version
    region.buf[field('unit control data version')] = control_version
    control_size, control = streams.unit_control_layout(control_version)
    events = []
    if version < 4:
        for _ in range(rng.randint(0, 8)):
            events.append(rng.choice([k for k in streams.V1_EVENTS if k != streams.V1_END]))
        size = control_size + sum(streams.V1_EVENTS[k][0] for k in events) + 4
    else:
        for _ in range(rng.randint(0, 8)):
            events.append((rng.randint(0, 3), rng.choice([k for k in streams.V4_EVENTS if k != streams.V4_END])))
        size = control_size + streams.ANIMATION_STATE_SIZE + 1 + sum(
            1 + (delta - 1 if delta > 1 else 0) + sum(n for n, _ in streams.V4_EVENTS[k]) for delta, k in events)
    stream = region.alloc(size)
    for offset, width in control:
        _numbers(region, stream + offset, width)
    cursor = stream + control_size
    if version < 4:
        for kind in events + [streams.V1_END]:
            length, fields = streams.V1_EVENTS[kind]
            region.put('H', cursor, kind)
            _numbers(region, cursor, 2, 2)
            for offset, width in fields:
                _numbers(region, cursor + offset, width)
            cursor += length
    else:
        _numbers(region, cursor, 2, streams.ANIMATION_STATE_SIZE // 2)
        cursor += streams.ANIMATION_STATE_SIZE
        for delta, kind in events + [(0, streams.V4_END)]:
            region.buf[cursor] = kind << 2 | delta
            region.bitfields[cursor] = (delta, kind)
            cursor += 1
            if delta == 2:
                cursor += 1
            elif delta == 3:
                _numbers(region, cursor, 2)
                cursor += 2
            for length, width in streams.V4_EVENTS[kind] if kind != streams.V4_END else ():
                if width:
                    _numbers(region, cursor, width)
                cursor += length
    assert cursor == stream + size
    _data_header(region, field('recorded animation event stream'), stream, size)


FABRICATE_RULES = {
    'BitmapData': _bitmap_data,
    'ModelGeometryPart': _model_part,
    'ScenarioStructureBSPMaterial': _bsp_material,
    'ScenarioBSP': _scenario_bsp,
    'ModelAnimationsAnimation': _animation,
    'ScenarioRecordedAnimation': _recorded_animation,
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
