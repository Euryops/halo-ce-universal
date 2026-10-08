"""Tag field definitions: what each tag group's fields are, and so which bytes swap.

The decomp has field definitions for only a handful of groups (bitmap_group.c,
collision_bsp_definitions.c, leaf_map.c, hs_scenario_definitions.c,
recorded_animation_definitions.c): the Xbox build that was decompiled did not need
the rest. The full set is Invader's description of the HEK's tag definitions
(https://github.com/SnowyMouse/invader, src/tag/hek/definition/*.json), the same
source Halo3DS's setup tool flattened into its schemas.json and walked over every
retail Xbox map. Those files are GPL-3.0, so they are fetched (fetch()) rather than
kept here; this module only reads them.

A definition file is a list of types: an enum (2 bytes), a bitfield (width bits),
or a struct (fields in order, with pads, inherits, count and bounds, and a size the
fields must add up to). layout() flattens a struct to the fields the walk needs.
"""

import hashlib
import io
import json
import os
import re
import shutil
import tarfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path

# Invader at the revision Halo3DS's schemas.json says it was made from.
INVADER_REVISION = 'a497b7457640dc2ee99fe8ad480d669e57347ef1'
INVADER_URL = f'https://codeload.github.com/SnowyMouse/invader/tar.gz/{INVADER_REVISION}'
# definitions_digest() of the files at that revision, so a fetch is the files
# this converter was checked against (the tarball's own bytes are not stable).
INVADER_DIGEST = '8213fd11e21024d935fff4ab933f72fa9788cde2bcba87428f118f449ab62265'

DEFAULT_DIRECTORY = Path(os.environ.get(
    'HALO_WII_DEFINITIONS',
    Path.home() / '.cache' / 'halo-wii-mapconv' / f'invader-{INVADER_REVISION[:8]}'))

# name: (size, swap width). Width 0 is bytes that never swap (strings, int8).
PRIMITIVES = {
    'Angle': (4, 4), 'ColorARGB': (16, 4), 'ColorARGBInt': (4, 4), 'ColorRGB': (12, 4),
    'Euler2D': (8, 4), 'Euler3D': (12, 4), 'Fraction': (4, 4), 'Index': (2, 2),
    'Matrix': (36, 4), 'Plane2D': (12, 4), 'Plane3D': (16, 4), 'Point2D': (8, 4),
    'Point2DInt': (4, 2), 'Point3D': (12, 4), 'Quaternion': (16, 4), 'Rectangle2D': (8, 2),
    'ScenarioScriptNodeValue': (4, 4), 'TagFourCC': (4, 4), 'TagID': (4, 4),
    'TagString': (32, 0), 'Vector2D': (8, 4), 'Vector3D': (12, 4), 'float': (4, 4),
    'int8': (1, 0), 'uint8': (1, 0), 'int16': (2, 2), 'uint16': (2, 2),
    'int32': (4, 4), 'uint32': (4, 4),
}
# The tag system's own structures, which the walk follows.
COMPOUNDS = {'Pointer': 4, 'TagReflexive': 12, 'TagDependency': 16, 'TagDataOffset': 20}


@dataclass(frozen=True)
class Field:
    """One flattened field: where it is in the struct the layout is of, what it is,
    and which struct and field name it came from (for the Xbox rules and the data
    handlers, which are keyed by those)."""
    offset: int
    kind: str           # 'swap', 'pointer', 'reflexive', 'dependency', 'data'
    width: int = 0      # for 'swap'
    struct: str = ''    # for 'reflexive': the element's struct
    owner: str = ''     # the struct the field is declared in
    name: str = ''      # the field's name there
    owner_offset: int = 0  # where the owner struct starts, from the layout's start


class Definitions:
    def __init__(self, types, roots):
        self.types = types      # name -> Invader type dict
        self.roots = roots      # group tag (int) -> root struct name
        self._layouts = {}

    @classmethod
    def load(cls, directory=DEFAULT_DIRECTORY):
        directory = Path(directory)
        if not (directory / 'fourcc.hpp').exists():
            raise FileNotFoundError(
                f'no tag definitions in {directory}: run `python3 -m mapconv fetch-definitions`')
        types = {}
        for file in sorted(directory.glob('*.json')):
            for definition in json.loads(file.read_text()):
                if definition['name'] in types:
                    raise ValueError(f'{file.name}: {definition["name"]} defined twice')
                types[definition['name']] = definition
        fourcc = {name.lower(): int(value, 16) for name, value in re.findall(
            r'TAG_FOURCC_(\w+)\s*=\s*(0x[0-9a-fA-F]+)', (directory / 'fourcc.hpp').read_text())}
        roots = {fourcc[d['class']]: d['name'] for d in types.values()
                 if 'class' in d and d['class'] in fourcc}
        # Halo3DS: both collection groups have the same layout of tag references.
        if 'ui_widget_collection' in fourcc and 'TagCollection' in types:
            roots[fourcc['ui_widget_collection']] = 'TagCollection'
        return cls(types, roots)

    def size(self, name):
        if name in PRIMITIVES:
            return PRIMITIVES[name][0]
        if name in COMPOUNDS:
            return COMPOUNDS[name]
        definition = self.types[name]
        if definition['type'] == 'enum':
            return 2
        if definition['type'] == 'bitfield':
            return definition['width'] // 8
        return definition['size']

    def field_offset(self, struct_name, field_name):
        """Where a struct's own (or inherited) field starts, whatever it is."""
        definition = self.types[struct_name]
        offset = 0
        if 'inherits' in definition:
            try:
                return self.field_offset(definition['inherits'], field_name)
            except KeyError:
                offset = self.size(definition['inherits'])
        for field in definition['fields']:
            if field.get('name') == field_name:
                return offset
            if field['type'] == 'pad':
                offset += field['size']
            else:
                offset += (self.size(field['type']) * field.get('count', 1)
                           * (2 if field.get('bounds') else 1))
        raise KeyError(f'{struct_name} has no field {field_name!r}')

    def layout(self, name):
        """The struct's fields with every inline struct and inherited struct
        flattened in, each at its offset from the struct's start. Pads and
        bytes that never swap are left out."""
        if name not in self._layouts:
            fields = []
            self._flatten(name, 0, fields)
            self._layouts[name] = fields
        return self._layouts[name]

    def _flatten(self, name, base, out):
        definition = self.types[name]
        offset = 0
        if 'inherits' in definition:
            self._flatten(definition['inherits'], base, out)
            offset = self.size(definition['inherits'])
        for field in definition['fields']:
            kind = field['type']
            if kind == 'pad':
                offset += field['size']
                continue
            size = self.size(kind)
            count = field.get('count', 1) * (2 if field.get('bounds') else 1)
            for i in range(count):
                at = base + offset + i * size
                common = dict(owner=name, name=field.get('name', ''), owner_offset=base)
                if kind in PRIMITIVES:
                    width = PRIMITIVES[kind][1]
                    if width:
                        for j in range(size // width):
                            out.append(Field(at + j * width, 'swap', width, **common))
                elif kind == 'Pointer':
                    out.append(Field(at, 'pointer', 4, **common))
                elif kind == 'TagReflexive':
                    out.append(Field(at, 'reflexive', struct=field['struct'], **common))
                elif kind == 'TagDependency':
                    out.append(Field(at, 'dependency', **common))
                elif kind == 'TagDataOffset':
                    out.append(Field(at, 'data', **common))
                elif self.types[kind]['type'] in ('enum', 'bitfield'):
                    if size > 1:
                        out.append(Field(at, 'swap', size, **common))
                else:
                    self._flatten(kind, at, out)
            offset += size * count
        if offset != definition['size']:
            raise ValueError(f'{name}: fields add up to {offset}, the definition says {definition["size"]}')


def definitions_digest(directory):
    """sha256 over the definition files' names and bytes, in name order."""
    digest = hashlib.sha256()
    for file in sorted(Path(directory).iterdir()):
        if file.suffix in ('.json', '.hpp'):
            digest.update(file.name.encode() + b'\0' + file.read_bytes())
    return digest.hexdigest()


def fetch(directory=DEFAULT_DIRECTORY, url=INVADER_URL):
    """Download Invader at INVADER_REVISION and keep its tag definitions, its
    group tags and its licence in directory."""
    directory = Path(directory)
    with urllib.request.urlopen(url, timeout=120) as response:
        archive = tarfile.open(fileobj=io.BytesIO(response.read()), mode='r:gz')
    staging = directory.with_name(directory.name + '.partial')
    staging.mkdir(parents=True, exist_ok=True)
    for member in archive.getmembers():
        path = member.name.split('/', 1)[-1]
        if member.isfile() and (path.startswith('src/tag/hek/definition/') and path.endswith('.json')
                                or path == 'include/invader/hek/fourcc.hpp'
                                or path == 'LICENSE.md' or path == 'COPYING'):
            (staging / os.path.basename(path)).write_bytes(archive.extractfile(member).read())
    actual = definitions_digest(staging)
    if actual != INVADER_DIGEST:
        raise ValueError(f'the definitions fetched from {url} are not the ones this converter '
                         f'was checked against (digest {actual})')
    if directory.exists():
        shutil.rmtree(directory)
    staging.replace(directory)
    return directory
