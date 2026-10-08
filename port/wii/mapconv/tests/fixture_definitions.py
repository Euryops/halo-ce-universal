"""A small set of made-up tag definitions in Invader's format, so the tests run
without fetching Invader's. They have every kind of field the walk knows (numbers
of each width, enums and bitfields, strings, arrays, bounds, inline structs,
inheritance, blocks, references and data) and the structures the Xbox rules are
for, at the offsets the rules use."""

import json
from pathlib import Path


def f(name, kind, **more):
    return {'name': name, 'type': kind, **more}


def pad(size):
    return {'type': 'pad', 'size': size}


def struct(name, size, fields, **more):
    return {'name': name, 'type': 'struct', 'size': size, 'fields': fields, **more}


TYPES = [
    {'name': 'ThingKind', 'type': 'enum', 'options': ['a', 'b', 'c']},
    {'name': 'ThingFlags', 'type': 'bitfield', 'fields': ['x', 'y'], 'width': 32},
    {'name': 'SmallFlags', 'type': 'bitfield', 'fields': ['x'], 'width': 8},
    {'name': 'WordFlags', 'type': 'bitfield', 'fields': ['x'], 'width': 16},
    {'name': 'VertexType', 'type': 'enum', 'options': ['0', '1', '2', '3', '4', '5']},
    {'name': 'BitmapDataFormat', 'type': 'enum', 'options': ['a8']},
    {'name': 'SoundFormat', 'type': 'enum', 'options': ['pcm', 'xbox adpcm']},

    struct('ThingInline', 12, [f('where', 'Point2D'), f('which', 'Index'), f('kind', 'ThingKind')]),
    struct('ScenarioThing', 100, [
        f('name', 'TagString'), f('small', 'int8'), f('small flags', 'SmallFlags'), f('word', 'int16'),
        f('flags', 'ThingFlags'), f('inline', 'ThingInline'), f('pair', 'int16', count=3), pad(2),
        f('range', 'float', bounds=True), f('colour', 'ColorARGBInt'), f('box', 'Rectangle2D'),
        f('id', 'TagID'), f('children', 'TagReflexive', struct='ScenarioThing'), pad(4),
    ]),
    struct('ScenarioBSP', 32, [f('bsp start', 'uint32'), f('bsp size', 'uint32'), f('bsp address', 'uint32'),
                               pad(4), f('structure bsp', 'TagDependency')]),
    struct('ScenarioRecordedAnimation', 64, [
        f('name', 'TagString'), f('version', 'int8'), f('raw animation data', 'int8'),
        f('unit control data version', 'int8'), pad(1), f('length of animation', 'int16'), pad(6),
        f('recorded animation event stream', 'TagDataOffset')]),
    struct('Scenario', 132, [
        f('sky', 'TagDependency'), f('structure bsps', 'TagReflexive', struct='ScenarioBSP'),
        f('recorded animations', 'TagReflexive', struct='ScenarioRecordedAnimation'),
        f('script syntax data', 'TagDataOffset'), f('script string data', 'TagDataOffset'),
        f('things', 'TagReflexive', struct='ScenarioThing'), f('unknown data', 'TagDataOffset'),
        f('plane', 'Plane3D'), pad(4),
    ], **{'class': 'scenario'}),

    struct('ScenarioStructureBSPMaterialUncompressedRenderedVertex', 56, [
        f('position', 'Point3D'), f('normal', 'Vector3D'), f('binormal', 'Vector3D'),
        f('tangent', 'Vector3D'), f('texture coords', 'Point2D')]),
    struct('ScenarioStructureBSPMaterialCompressedRenderedVertex', 32, [
        f('position', 'Point3D'), f('normal', 'uint32'), f('binormal', 'uint32'), f('tangent', 'uint32'),
        f('texture coords', 'Point2D')]),
    struct('ScenarioStructureBSPMaterialUncompressedLightmapVertex', 20, [
        f('normal', 'Vector3D'), f('texture coords', 'Point2D')]),
    struct('ScenarioStructureBSPMaterialCompressedLightmapVertex', 8, [
        f('normal', 'uint32'), f('u', 'int16'), f('v', 'int16')]),
    struct('ModelVertexUncompressed', 68, [
        f('position', 'Point3D'), f('normal', 'Vector3D'), f('binormal', 'Vector3D'), f('tangent', 'Vector3D'),
        f('texture coords', 'Point2D'), f('node0 index', 'Index'), f('node1 index', 'Index'),
        f('node0 weight', 'float'), f('node1 weight', 'float')]),
    struct('ModelVertexCompressed', 32, [
        f('position', 'Point3D'), f('normal', 'uint32'), f('binormal', 'uint32'), f('tangent', 'uint32'),
        f('u', 'int16'), f('v', 'int16'), f('node0 index', 'int8'), f('node1 index', 'int8'),
        f('node0 weight', 'uint16')]),

    struct('ScenarioStructureBSPMaterial', 256, [
        f('shader', 'TagDependency'), f('surfaces', 'int32'), f('centroid', 'Point3D'), pad(144),
        f('rendered vertices type', 'VertexType'), pad(2), f('rendered vertices count', 'uint32'),
        f('rendered vertices offset', 'uint32'), pad(4), f('rendered vertices index pointer', 'Pointer'),
        f('lightmap vertices type', 'VertexType'), pad(2), f('lightmap vertices count', 'uint32'),
        f('lightmap vertices offset', 'uint32'), pad(4), f('lightmap vertices index pointer', 'Pointer'),
        f('uncompressed vertices', 'TagDataOffset'), f('compressed vertices', 'TagDataOffset')]),
    struct('ScenarioStructureBSP', 64, [
        f('lightmaps bitmap', 'TagDependency'), f('materials', 'TagReflexive', struct='ScenarioStructureBSPMaterial'),
        f('cluster data', 'TagDataOffset'), f('things', 'TagReflexive', struct='ScenarioThing'), pad(4)],
        **{'class': 'scenario_structure_bsp'}),

    struct('ModelGeometryPart', 104, [
        f('flags', 'ThingFlags'), f('shader index', 'Index'), f('prev', 'uint8'), f('next', 'uint8'),
        f('centroid', 'Point3D'), pad(48),
        f('triangle buffer type', 'VertexType'), pad(2), f('triangle count', 'uint32'),
        f('triangle offset', 'uint32'), f('triangle offset 2', 'uint32'), f('vertex type', 'VertexType'), pad(2),
        f('vertex count', 'uint32'), pad(4), f('vertex pointer', 'Pointer'), f('vertex offset', 'uint32')]),
    struct('Model', 16, [f('flags', 'ThingFlags'), f('parts', 'TagReflexive', struct='ModelGeometryPart')],
           **{'class': 'model'}),

    struct('BitmapData', 48, [
        f('bitmap class', 'TagFourCC'), f('width', 'uint16'), f('height', 'uint16'), f('depth', 'uint16'),
        f('type', 'ThingKind'), f('format', 'BitmapDataFormat'), f('flags', 'WordFlags'),
        f('registration point', 'Point2DInt'), f('mipmap count', 'uint16'), pad(2),
        f('pixel data offset', 'uint32'), f('pixel data size', 'uint32'), f('bitmap tag id', 'TagID'),
        f('pointer', 'Pointer'), pad(8)]),
    struct('Bitmap', 56, [
        f('type', 'ThingKind'), pad(2), f('compressed color plate data', 'TagDataOffset'),
        f('processed pixel data', 'TagDataOffset'), f('bitmap data', 'TagReflexive', struct='BitmapData')],
        **{'class': 'bitmap'}),

    struct('SoundPermutation', 120, [
        f('name', 'TagString'), f('skip fraction', 'Fraction'), f('gain', 'Fraction'), f('format', 'SoundFormat'),
        f('next permutation index', 'Index'), f('samples pointer', 'uint32'), f('tag id 0', 'TagID'),
        f('buffer size', 'uint32'), f('tag id 1', 'TagID'), f('samples', 'TagDataOffset'),
        f('mouth data', 'TagDataOffset'), f('subtitle data', 'TagDataOffset')]),
    struct('SoundPitchRange', 48, [f('name', 'TagString'), f('natural pitch', 'float'),
                                   f('permutations', 'TagReflexive', struct='SoundPermutation')]),
    struct('Sound', 140, [
        f('flags', 'ThingFlags'), f('sound class', 'ThingKind'), f('sample rate', 'ThingKind'), pad(100),
        f('channel count', 'ThingKind'), f('format', 'SoundFormat'), f('promotion sound', 'TagDependency'),
        f('pitch ranges', 'TagReflexive', struct='SoundPitchRange')], **{'class': 'sound'}),

    {'name': 'AnimationFlags', 'type': 'bitfield', 'fields': ['compressed data'], 'width': 16},
    struct('ModelAnimationsAnimation', 132, [
        f('name', 'TagString'), f('frame count', 'uint16'), f('frame size', 'uint16'),
        f('node count', 'uint16'), f('flags', 'AnimationFlags'), f('weight', 'Fraction'),
        f('frame info', 'TagDataOffset'), f('node transform flag data', 'uint32', count=2),
        f('node rotation flag data', 'uint32', count=2), f('node scale flag data', 'uint32', count=2),
        f('offset to compressed data', 'uint32'), f('default data', 'TagDataOffset'),
        f('frame data', 'TagDataOffset')]),
    struct('ModelAnimations', 12, [f('animations', 'TagReflexive', struct='ModelAnimationsAnimation')],
           **{'class': 'model_animations'}),
    struct('UnicodeStringListString', 20, [f('string', 'TagDataOffset')]),
    struct('UnicodeStringList', 12, [f('strings', 'TagReflexive', struct='UnicodeStringListString')],
           **{'class': 'unicode_string_list'}),

    struct('ObjectAttachment', 20, [f('type', 'TagDependency'), f('marker', 'Index'), pad(2)]),
    struct('Object', 32, [f('model', 'TagDependency'), f('attachments', 'TagReflexive', struct='ObjectAttachment'),
                          f('mass', 'float')], **{'class': 'object'}),
    struct('Unit', 64, [f('seats', 'int16', count=2), f('speed', 'Angle', bounds=True), pad(20)],
           inherits='Object', **{'class': 'unit'}),
]

FOURCC = """
        TAG_FOURCC_SCENARIO = 0x73636E72,
        TAG_FOURCC_SCENARIO_STRUCTURE_BSP = 0x73627370,
        TAG_FOURCC_MODEL = 0x6D6F6465,
        TAG_FOURCC_BITMAP = 0x6269746D,
        TAG_FOURCC_SOUND = 0x736E6421,
        TAG_FOURCC_UNICODE_STRING_LIST = 0x75737472,
        TAG_FOURCC_OBJECT = 0x6F626A65,
        TAG_FOURCC_UNIT = 0x756E6974,
        TAG_FOURCC_MODEL_ANIMATIONS = 0x616E7472,
"""


def write(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'fixture.json').write_text(json.dumps(TYPES))
    (directory / 'fourcc.hpp').write_text(FOURCC)
    return directory
