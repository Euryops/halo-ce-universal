"""Walking a cache map's tags, and swapping a whole map's bytes.

The layout of an Xbox cache file (source/cache/cache_files.c):

- a 0x800-byte header (struct cache_file_header);
- the tag cache image, tag_data_size bytes at tag_data_offset, loaded at
  0x803A6000: a tag header (struct cache_file_tag_header), the tag table
  (struct cache_file_tag_instance, 32 bytes each), the names and the tags;
- each structure BSP, an image of its own that the scenario's BSP block gives the
  file offset, size and load address of;
- the bitmaps' pixels and the sounds' samples, at file offsets in their tags.

The definitions say what most fields are. What they do not say about the Xbox's
maps (the D3D vertex and index buffers, the words that are pointers on the Xbox
and offsets on the PC, the BSP header) is in the XBOX_* rules below, as Halo3DS's
relocate_cache.py found them by walking every retail map.
"""

import struct
from dataclasses import dataclass, field

from . import streams
from .swap import (Plan, WalkError, HS_DATA_ARRAY_CODES, HS_SYNTAX_NODE_CODES,
                   HS_DATA_ARRAY_SIZE, HS_SYNTAX_NODE_SIZE, byte_swap_codes)

HEADER_SIZE = 0x800
HEADER_SIGNATURE = 0x68656164   # 'head'
FOOTER_SIGNATURE = 0x666F6F74   # 'foot'
TAGS_SIGNATURE = 0x74616773     # 'tags'
SBSP = 0x73627370               # 'sbsp'
XBOX_CACHE_VERSION = 5
TAG_HEADER_SIZE = 36
TAG_INSTANCE_SIZE = 32
BSP_HEADER_SIZE = 24
D3D_RESOURCE_SIZE = 12          # Common, Data (the pointer), Lock
HS_DATUM_ARRAY_SIGNATURE = 0x64407440  # 'd@t@'

# The words of the header that are numbers; the rest are strings and zeros.
HEADER_WORDS = (0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x60, 0x64, 0x7FC)

VERTEX_STRUCTS = {  # VertexType -> the struct of one vertex
    0: 'ScenarioStructureBSPMaterialUncompressedRenderedVertex',
    1: 'ScenarioStructureBSPMaterialCompressedRenderedVertex',
    2: 'ScenarioStructureBSPMaterialUncompressedLightmapVertex',
    3: 'ScenarioStructureBSPMaterialCompressedLightmapVertex',
    4: 'ModelVertexUncompressed',
    5: 'ModelVertexCompressed',
}

# Data whose bytes are bytes: text, and things the engine reads a byte at a time.
DATA_BYTES = {
    ('Bitmap', 'compressed color plate data'), ('Bitmap', 'processed pixel data'),
    ('Font', 'pixels'), ('StringListString', 'string'), ('Scenario', 'script string data'),
    ('Scenario', 'editor scenario data'), ('ScenarioEditorComment', 'comment'),
    ('ScenarioSourceFile', 'source'), ('SoundPermutation', 'mouth data'),
    ('SoundPermutation', 'subtitle data'),
}
# Data the sound converter rewrites (sound.py); the walk leaves it.
DATA_CONVERTED = {('SoundPermutation', 'samples')}
# Data whose struct's rule plans it (streams.py), knowing the struct's other fields.
DATA_BY_RULE = {
    ('ModelAnimationsAnimation', 'default data'), ('ModelAnimationsAnimation', 'frame data'),
    ('ScenarioRecordedAnimation', 'recorded animation event stream'),
    ('ScenarioStructureBSPMaterial', 'uncompressed vertices'),
    ('ScenarioStructureBSPMaterial', 'compressed vertices'),
}
# Data no code in the decomp reads, left as bytes: BSP cluster and sound
# cluster data, meter tags (no meter definition in the engine) and input device
# defaults (input_device_defaults.h is empty).
DATA_UNREAD = {
    ('ScenarioStructureBSP', 'cluster data'), ('ScenarioStructureBSP', 'sound pas data'),
    ('Meter', 'encoded stencil'), ('InputDeviceDefaults', 'device id'), ('InputDeviceDefaults', 'profile'),
}


@dataclass
class Header:
    signature: int
    version: int
    file_length: int
    tag_data_offset: int
    tag_data_size: int
    name: str
    build: str
    footer: int

    @classmethod
    def parse(cls, raw, order='<'):
        if len(raw) < HEADER_SIZE:
            raise ValueError('shorter than a cache file header')
        word = lambda o: struct.unpack_from(order + 'I', raw, o)[0]
        text = lambda o: raw[o:o + 32].split(b'\0')[0].decode('latin-1')
        return cls(word(0), word(4), word(8), word(16), word(20), text(32), text(64), word(0x7FC))

    def check(self):
        if self.signature != HEADER_SIGNATURE or self.footer != FOOTER_SIGNATURE:
            raise ValueError('not a cache file (no head/foot signature in this byte order)')
        if self.version != XBOX_CACHE_VERSION:
            raise ValueError(f'cache version {self.version}: only the Xbox\'s version 5 is converted')


@dataclass
class Walk:
    """A walk of one region: its plan, the structure BSPs it found (scenario
    only), and what it could not walk."""
    plan: Plan
    bsps: list = field(default_factory=list)
    errors: list = field(default_factory=list)


def _descriptor(plan, offset, data_size):
    """A D3D vertex or index buffer header: plan it, and return where its data is."""
    plan.swap(offset, 4)
    target = plan.pointer(offset + 4, max(data_size, 1))
    plan.swap(offset + 8, 4)
    return target


def _struct_array(plan, defs, name, offset, count):
    """Elements made only of numbers (vertices): the layout's swaps, count times."""
    layout = defs.layout(name)
    if any(f.kind != 'swap' for f in layout):
        raise WalkError(f'{name} is not made only of numbers')
    size = defs.size(name)
    for i in range(count):
        for f in layout:
            plan.swap(offset + i * size + f.offset, f.width)


def _bitmap_data(walker, offset):
    plan = walker.plan
    # The Xbox's cache block index (where the PC has a pointer), then the
    # hardware format and base address the texture cache fills in at run time.
    for o in (36, 40, 44):
        plan.swap(offset + o, 4)


def _model_part(walker, offset):
    plan, defs = walker.plan, walker.defs
    triangle_type = plan.u16(offset + 68)
    triangles = plan.u32(offset + 72)
    vertex_type = plan.u16(offset + 84)
    vertices = plan.u32(offset + 88)
    if triangles and triangle_type not in (0, 1):
        raise WalkError(f'model part: triangle buffer type {triangle_type}')
    if vertices and vertex_type not in (4, 5):
        raise WalkError(f'model part: vertex type {vertex_type}')
    indices = (triangles * 3 if triangle_type == 0 else triangles + 2) if triangles else 0
    target = plan.pointer(offset + 76, indices * 2)
    if target is not None:
        plan.swap_array(target, 2, indices)
    descriptor = plan.pointer(offset + 80, D3D_RESOURCE_SIZE)
    if descriptor is not None:
        _descriptor(plan, descriptor, indices * 2)
    # A CPU copy of the vertex pointer, stale in the file; moved if it is in the cache.
    plan.swap(offset + 96, 4, pointer=True)
    descriptor = plan.pointer(offset + 100, D3D_RESOURCE_SIZE)
    if descriptor is not None:
        name = VERTEX_STRUCTS[vertex_type] if vertices else None
        target = _descriptor(plan, descriptor, vertices * defs.size(name) if name else 0)
        if target is not None and vertices:
            _struct_array(plan, defs, name, target, vertices)


# A BSP material's vertex data (object_lights.c, structure_lens_flares.c): its
# rendered vertices, then its lightmap vertices.
MATERIAL_VERTEX_DATA = (
    (216, 'ScenarioStructureBSPMaterialUncompressedRenderedVertex',
     'ScenarioStructureBSPMaterialUncompressedLightmapVertex'),
    (236, 'ScenarioStructureBSPMaterialCompressedRenderedVertex',
     'ScenarioStructureBSPMaterialCompressedLightmapVertex'),
)


def _bsp_material(walker, offset):
    plan, defs = walker.plan, walker.defs
    rendered, lightmap = plan.u32(offset + 180), plan.u32(offset + 200)
    for at, rendered_struct, lightmap_struct in MATERIAL_VERTEX_DATA:
        data = _data_at(walker, offset + at)
        if not data:
            continue
        r, l = defs.size(rendered_struct), defs.size(lightmap_struct)
        if data[1] == rendered * r + lightmap * l:
            _struct_array(plan, defs, rendered_struct, data[0], rendered)
            _struct_array(plan, defs, lightmap_struct, data[0] + rendered * r, lightmap)
        elif data[1] == rendered * r:
            _struct_array(plan, defs, rendered_struct, data[0], rendered)
        else:
            plan.left_as_is[f'ScenarioStructureBSPMaterial.{"un" if at == 216 else ""}compressed vertices'] += data[1]
    for start in (176, 196):    # rendered vertices, lightmap vertices
        vertex_type = plan.u16(offset + start)
        vertices = plan.u32(offset + start + 4)
        # The pad that holds the CPU address of the vertices on the Xbox.
        plan.swap(offset + start + 12, 4, pointer=True)
        if not vertices:
            # Not followed with no vertices, as Halo3DS does not: it may be stale.
            plan.swap(offset + start + 16, 4, pointer=True)
            continue
        descriptor = plan.pointer(offset + start + 16, D3D_RESOURCE_SIZE)
        if descriptor is None:
            continue
        if vertices and vertex_type not in (0, 1, 2, 3):
            raise WalkError(f'BSP material: vertex type {vertex_type}')
        name = VERTEX_STRUCTS[vertex_type] if vertices else None
        target = _descriptor(plan, descriptor, vertices * defs.size(name) if name else 0)
        if target is not None and vertices:
            _struct_array(plan, defs, name, target, vertices)


def _named(walker, struct_name, name):
    try:
        return walker.defs.field_offset(struct_name, name)
    except KeyError as error:
        raise WalkError(str(error)) from None


def _data_at(walker, at):
    """(offset in the region, size) of the tag data whose header is at, or None."""
    plan = walker.plan
    size, address = plan.u32(at), plan.u32(at + 12)
    if not size or not address:
        return None
    return plan.resolve(address, size), size


def _animation(walker, offset):
    plan = walker.plan
    field = lambda name: offset + _named(walker, 'ModelAnimationsAnimation', name)
    vector = lambda name: [plan.u32(field(name)), plan.u32(field(name) + 4)]
    left = streams.plan_animation(plan, {
        'frame count': plan.u16(field('frame count')), 'frame size': plan.u16(field('frame size')),
        'node count': plan.u16(field('node count')), 'flags': plan.u16(field('flags')),
        'translation flags': vector('node transform flag data'),
        'rotation flags': vector('node rotation flag data'),
        'scale flags': vector('node scale flag data'),
        'compressed data offset': plan.u32(field('offset to compressed data')),
        'default data': _data_at(walker, field('default data')),
        'frame data': _data_at(walker, field('frame data')),
    })
    for name, size in left.items():
        plan.left_as_is[f'ModelAnimationsAnimation.{name}'] += size


def _recorded_animation(walker, offset):
    plan = walker.plan
    field = lambda name: offset + _named(walker, 'ScenarioRecordedAnimation', name)
    stream = _data_at(walker, field('recorded animation event stream'))
    if stream:
        left = streams.plan_event_stream(plan, plan.data[field('version')],
                                         plan.data[field('unit control data version')], *stream)
        if left:
            plan.left_as_is['ScenarioRecordedAnimation.recorded animation event stream'] += left


def _scenario_bsp(walker, offset):
    plan = walker.plan
    walker.bsps.append((plan.u32(offset), plan.u32(offset + 4), plan.u32(offset + 8)))
    plan.swap(offset + 8, 4, pointer=True)     # its load address, in the tag cache


# struct -> (the offsets in it the rule plans, the rule)
XBOX_RULES = {
    'BitmapData': ({36}, _bitmap_data),
    'ModelGeometryPart': ({76, 80, 96, 100}, _model_part),
    'ScenarioStructureBSPMaterial': ({176 + 12, 176 + 16, 196 + 12, 196 + 16}, _bsp_material),
    'ScenarioBSP': ({8}, _scenario_bsp),
    'ModelAnimationsAnimation': (set(), _animation),
    'ScenarioRecordedAnimation': (set(), _recorded_animation),
}


def _script_syntax(walker, offset, size):
    """The scenario's script nodes: a data array header and its nodes, swapped by
    the codes in hs_scenario_definitions.c (hs_syntax_data_byte_swap)."""
    plan = walker.plan
    if size < HS_DATA_ARRAY_SIZE or (size - HS_DATA_ARRAY_SIZE) % HS_SYNTAX_NODE_SIZE:
        raise WalkError(f'script syntax data of {size} bytes is not a data array')
    if plan.u32(offset + 40) != HS_DATUM_ARRAY_SIGNATURE:
        raise WalkError('script syntax data has no d@t@ signature')
    # The array's data pointer, which hs_scenario_postprocess points after the header.
    plan.swap(offset + 52, 4, pointer=True)
    swaps, header_size, _ = byte_swap_codes(HS_DATA_ARRAY_CODES)
    assert header_size == HS_DATA_ARRAY_SIZE
    for o, width in swaps:
        if o != 52:
            plan.swap(offset + o, width)
    swaps, node_size, _ = byte_swap_codes(HS_SYNTAX_NODE_CODES)
    for i in range((size - HS_DATA_ARRAY_SIZE) // node_size):
        for o, width in swaps:
            plan.swap(offset + HS_DATA_ARRAY_SIZE + i * node_size + o, width)


def _words(width):
    def handler(walker, offset, size):
        if size % width:
            raise WalkError(f'{size} bytes is not a whole number of {width}-byte words')
        walker.plan.swap_array(offset, width, size // width)
    return handler


DATA_HANDLERS = {
    ('Scenario', 'script syntax data'): _script_syntax,
    ('UnicodeStringListString', 'string'): _words(2),     # UTF-16
    ('HUDMessageText', 'text data'): _words(2),            # UTF-16
    ('ModelAnimationsAnimation', 'frame info'): _words(4),  # dx, dy, dz, dyaw: floats
}


class Walker:
    def __init__(self, plan, defs):
        self.plan, self.defs = plan, defs
        self.bsps = []

    def walk(self, name, offset, depth=0):
        plan = self.plan
        if (name, offset) in plan.visited:
            return
        if depth > 64:
            raise WalkError(f'{name}: blocks nested past 64 deep')
        plan.resolve(plan.base + offset, self.defs.size(name))
        plan.visited.add((name, offset))
        plan.counts[name] += 1
        rules = {}      # (rule, where its struct starts), once each
        for f in self.defs.layout(name):
            at = offset + f.offset
            rule = XBOX_RULES.get(f.owner)
            if rule:
                rules[(rule[1], offset + f.owner_offset)] = True
                if f.offset - f.owner_offset in rule[0]:
                    continue
            try:
                self._field(f, at, depth)
            except WalkError as error:
                raise WalkError(f'{f.owner}.{f.name} @ {at:x}: {error}') from None
        for rule, at in rules:
            rule(self, at)

    def _field(self, f, at, depth):
        plan = self.plan
        if f.kind == 'swap':
            plan.swap(at, f.width)
        elif f.kind == 'pointer':
            plan.swap(at, 4, pointer=True)
        elif f.kind == 'reflexive':
            count = plan.u32(at)
            size = self.defs.size(f.struct)
            plan.swap(at, 4)
            plan.swap(at + 8, 4)
            if count > 0x100000:
                raise WalkError(f'block count {count}')
            target = plan.pointer(at + 4, count * size) if count else None
            if not count:
                return
            if target is None:
                raise WalkError(f'{count} elements at a null pointer')
            for i in range(count):
                self.walk(f.struct, target + i * size, depth + 1)
        elif f.kind == 'dependency':
            plan.swap(at, 4)                    # group tag
            plan.swap(at + 4, 4, pointer=True)  # name (in the tag cache, maybe another region)
            plan.swap(at + 8, 4)                # name length
            plan.swap(at + 12, 4)               # tag index
        elif f.kind == 'data':
            size = plan.u32(at)
            for o in (0, 4, 8, 16):             # size, flags, file offset, definition
                plan.swap(at + o, 4)
            key = (f.owner, f.name)
            if key in DATA_CONVERTED or plan.u32(at + 12) == 0:
                plan.swap(at + 12, 4, pointer=True)
                return
            target = plan.pointer(at + 12, size)
            if key in DATA_HANDLERS:
                DATA_HANDLERS[key](self, target, size)
            elif key not in DATA_BYTES and key not in DATA_BY_RULE and key not in DATA_UNREAD and size:
                plan.left_as_is[f'{f.owner}.{f.name}'] += size


def walk_tag_cache(data, base, defs, direction):
    """Plan the tag cache image: its header, the tag table and every tag."""
    plan = Plan(data, base, direction)
    walker = Walker(plan, defs)
    result = Walk(plan)
    words = struct.unpack_from(direction.order + '9I', data)
    if words[8] != TAGS_SIGNATURE:
        raise WalkError('the tag header has no tags signature')
    tag_count = words[3]
    if not 0 < tag_count <= 0xFFFF:
        raise WalkError(f'{tag_count} tags')
    for o in range(0, TAG_HEADER_SIZE, 4):
        if o not in (0, 20, 28):
            plan.swap(o, 4)
    table = plan.pointer(0, tag_count * TAG_INSTANCE_SIZE, required=True)
    for count_at, pointer_at in ((16, 20), (24, 28)):   # vertex and index buffers
        count = plan.u32(count_at)
        descriptors = plan.pointer(pointer_at, count * D3D_RESOURCE_SIZE)
        if descriptors is not None:
            for i in range(count):
                _descriptor(plan, descriptors + i * D3D_RESOURCE_SIZE, 1)
    for i in range(tag_count):
        entry = table + i * TAG_INSTANCE_SIZE
        for o in (0, 4, 8, 12, 24, 28):
            plan.swap(entry + o, 4)
        name = plan.pointer(entry + 16, 1, required=True)
        group = plan.u32(entry)
        tag_name = data[name:data.find(b'\0', name, name + 256)].decode('latin-1')
        row = {'index': i, 'group': group.to_bytes(4, 'big').decode('latin-1'), 'name': tag_name}
        if group == SBSP:
            plan.swap(entry + 20, 4, pointer=True)  # its body is the BSP's own region
            continue
        try:
            root = defs.roots[group]
        except KeyError:
            plan.swap(entry + 20, 4, pointer=True)
            result.errors.append({**row, 'error': 'no definition for this group'})
            continue
        try:
            body = plan.pointer(entry + 20, defs.size(root), required=True)
            walker.walk(root, body)
        except WalkError as error:
            result.errors.append({**row, 'error': str(error)})
    result.bsps = walker.bsps
    return result


def walk_bsp(data, address, defs, direction):
    """Plan one structure BSP's image: its header (struct
    cache_file_structure_bsp_header) and its tag."""
    plan = Plan(data, address, direction)
    result = Walk(plan)
    walker = Walker(plan, defs)
    try:
        if plan.u32(20) != SBSP:
            raise WalkError('the BSP header has no sbsp signature')
        plan.swap(20, 4)
        body = plan.pointer(0, defs.size('ScenarioStructureBSP'), required=True)
        for count_at, pointer_at in ((4, 8), (12, 16)):
            count = plan.u32(count_at)
            plan.swap(count_at, 4)
            descriptors = plan.pointer(pointer_at, count * D3D_RESOURCE_SIZE)
            if descriptors is not None:
                for i in range(count):
                    _descriptor(plan, descriptors + i * D3D_RESOURCE_SIZE, 1)
        walker.walk('ScenarioStructureBSP', body)
    except WalkError as error:
        result.errors.append({'error': str(error)})
    return result


@dataclass
class SwapResult:
    data: bytes
    header: Header
    tags: Walk
    bsps: list
    errors: list

    def summary(self):
        left = sum((w.plan.left_as_is for w in [self.tags] + self.bsps), start=type(self.tags.plan.left_as_is)())
        return {
            'map': self.header.name, 'build': self.header.build,
            'tag_bytes': self.header.tag_data_size,
            'words_swapped': sum(w.plan.word_count for w in [self.tags] + self.bsps),
            'pointers_moved': sum(w.plan.pointer_count for w in [self.tags] + self.bsps),
            'bsps': len(self.bsps),
            'left_in_xbox_order': dict(left),
            'errors': self.errors,
        }


def swap_map(raw, defs, direction):
    """Every number in the map's header, tags and BSPs in the other byte order,
    and every pointer moved to the other tag cache. Bitmaps' pixels and sounds'
    samples are not touched here (textures.py and sound.py convert them)."""
    header = Header.parse(raw, direction.order)
    header.check()
    if header.tag_data_offset < HEADER_SIZE or header.tag_data_offset + header.tag_data_size > len(raw):
        raise ValueError('the tag data is outside the file')
    out = bytearray(raw)
    target_order = '>' if direction.order == '<' else '<'
    for o in HEADER_WORDS:
        value = struct.unpack_from(direction.order + 'I', raw, o)[0]
        struct.pack_into(target_order + 'I', out, o, value)

    start, end = header.tag_data_offset, header.tag_data_offset + header.tag_data_size
    tags = walk_tag_cache(raw[start:end], direction.old_base, defs, direction)
    out[start:end] = tags.plan.apply()
    errors = list(tags.errors)
    bsps = []
    for i, (file_offset, size, address) in enumerate(tags.bsps):
        if file_offset < HEADER_SIZE or file_offset + size > len(raw):
            errors.append({'bsp': i, 'error': 'outside the file'})
            continue
        if file_offset < end and start < file_offset + size:
            errors.append({'bsp': i, 'error': 'overlaps the tag data'})
            continue
        walk = walk_bsp(raw[file_offset:file_offset + size], address, defs, direction)
        bsps.append(walk)
        errors += [{'bsp': i, **e} for e in walk.errors]
        out[file_offset:file_offset + size] = walk.plan.apply()
    return SwapResult(bytes(out), header, tags, bsps, errors)
