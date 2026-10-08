"""python3 -m mapconv: Halo CE's Xbox maps to the Wii's (run it from port/wii).

    fetch-definitions           get Invader's tag definitions (once; GPL-3.0, kept outside the repo)
    check SOURCE                which maps SOURCE has, and whether each is build 01.10.12.2276
    roundtrip SOURCE            swap each map to the Wii and back: the same bytes, or which differ
    convert SOURCE OUT          the SD card's maps: OUT/halo/maps/<name>.map, and a report
    selftest                    the round trip on made-up maps, with the fetched definitions

SOURCE is an Xbox disc image (.iso/.xiso) or a folder of .map files.
"""

import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path

from . import builds, definitions
from .convert import Converter
from .fabricate import fabricate_map
from .swap import TO_WII, TO_XBOX
from .tags import swap_map
from .textures import load_vector_palette
from .xiso import Xiso

SOURCE_ROOT = Path(__file__).resolve().parents[3]


class Source:
    """The maps in a disc image or a folder: name -> a function giving the bytes."""
    def __init__(self, path):
        path = Path(path)
        if path.is_dir():
            self.maps = {p.stem.lower(): (lambda p=p: p.read_bytes()) for p in sorted(path.glob('*.map'))}
        else:
            image = Xiso(path)
            self.maps = {name: (lambda p=p: image.read(p)) for name, p in sorted(image.maps().items())}
        if not self.maps:
            raise SystemExit(f'{path}: no .map files')

    def select(self, names):
        if not names:
            return self.maps
        missing = set(names) - set(self.maps)
        if missing:
            raise SystemExit(f'not in the source: {", ".join(sorted(missing))}')
        return {n: self.maps[n] for n in names}


def load(name, read, any_build):
    raw = read()
    try:
        kind = builds.identify(name, len(raw), hashlib.sha256(raw).hexdigest())
    except builds.BuildError as error:
        if not any_build:
            raise
        print(f'  {name}: {error} (going on: --any-build)', file=sys.stderr)
        kind = None
    expanded = builds.expand(raw)
    if kind == 'disc':
        builds.check_expanded(name, expanded)
    return expanded


def command_check(args):
    source = Source(args.source)
    bad = 0
    for name, read in source.select(args.maps).items():
        raw = read()
        try:
            kind = builds.identify(name, len(raw), hashlib.sha256(raw).hexdigest())
            print(f'{name}: build {builds.BUILD}, {kind}')
        except builds.BuildError as error:
            bad += 1
            print(f'{name}: {error}')
    missing = sorted(set(builds.MAPS) - set(source.maps))
    if missing:
        print(f'not in the source: {", ".join(missing)}')
    return 1 if bad else 0


def command_roundtrip(args):
    defs = definitions.Definitions.load(args.definitions)
    source = Source(args.source)
    failed = 0
    for name, read in source.select(args.maps).items():
        started = time.time()
        expanded = load(name, read, args.any_build)
        wii = swap_map(expanded, defs, TO_WII)
        back = swap_map(wii.data, defs, TO_XBOX)
        same = back.data == expanded and not wii.errors and not back.errors
        failed += not same
        summary = wii.summary()
        print(f'{name}: {"same bytes" if same else "DIFFERENT"} after a round trip; '
              f'{summary["words_swapped"]} words swapped, {summary["pointers_moved"]} pointers moved, '
              f'{summary["bsps"]} BSPs, {time.time() - started:.1f}s')
        for error in (wii.errors + back.errors)[:10]:
            print(f'  {error}')
        if not same and back.data != expanded:
            first = next(i for i, (a, b) in enumerate(zip(back.data, expanded)) if a != b)
            print(f'  first different byte at file offset {first:#x}')
        if summary['left_in_xbox_order']:
            print(f'  data left in the Xbox\'s byte order: {summary["left_in_xbox_order"]}')
    return 1 if failed else 0


def command_convert(args):
    defs = definitions.Definitions.load(args.definitions)
    palette = load_vector_palette(SOURCE_ROOT)
    source = Source(args.source)
    out = Path(args.out) / 'halo' / 'maps'
    out.mkdir(parents=True, exist_ok=True)
    reports = {}
    for name, read in source.select(args.maps).items():
        started = time.time()
        expanded = load(name, read, args.any_build)
        converted, report, summary = Converter(defs, palette, args.workers).convert(expanded)
        partial = out / f'.{name}.map.partial'
        partial.write_bytes(converted)
        partial.replace(out / f'{name}.map')
        reports[name] = {**summary, 'bitmaps': report.bitmaps, 'bitmaps_moved': report.bitmaps_moved,
                         'gx_formats': report.formats, 'sounds': report.sounds,
                         'sounds_moved': report.sounds_moved, 'skipped': report.skipped,
                         'bytes': len(converted), 'seconds': round(time.time() - started, 1)}
        print(f'{name}: {report.bitmaps} bitmaps, {report.sounds} sounds, {len(report.skipped)} skipped, '
              f'{len(converted) / 1e6:.1f} MB, {time.time() - started:.0f}s')
    (Path(args.out) / 'mapconv-report.json').write_text(json.dumps(reports, indent=2) + '\n')
    return 0


def command_selftest(args):
    defs = definitions.Definitions.load(args.definitions)
    for seed in range(args.seeds):
        xbox = fabricate_map(defs, seed=seed, media=True)
        wii = swap_map(xbox, defs, TO_WII)
        back = swap_map(wii.data, defs, TO_XBOX)
        ok = back.data == xbox and not wii.errors
        print(f'seed {seed}: {len(xbox)} bytes, {wii.summary()["words_swapped"]} words swapped: '
              f'{"same bytes" if ok else "DIFFERENT"}')
        if not ok:
            return 1
    return 0


def command_fetch(args):
    directory = definitions.fetch(args.definitions)
    print(f'Invader {definitions.INVADER_REVISION[:8]}\'s tag definitions are in {directory}')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog='python3 -m mapconv', description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--definitions', default=definitions.DEFAULT_DIRECTORY,
                        help='where Invader\'s definitions are (default %(default)s)')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('fetch-definitions').set_defaults(run=command_fetch)
    for name, run in (('check', command_check), ('roundtrip', command_roundtrip), ('convert', command_convert)):
        command = commands.add_parser(name)
        command.add_argument('source')
        if name == 'convert':
            command.add_argument('out')
            command.add_argument('--workers', type=int, default=os.cpu_count(),
                                 help='processes for the sound encoding')
        command.add_argument('--maps', type=lambda s: s.split(','), default=None,
                             help='only these maps (comma-separated names)')
        command.add_argument('--any-build', action='store_true',
                             help='go on with maps that are not build 01.10.12.2276 (unchecked)')
        command.set_defaults(run=run)
    selftest = commands.add_parser('selftest')
    selftest.add_argument('--seeds', type=int, default=10)
    selftest.set_defaults(run=command_selftest)
    args = parser.parse_args(argv)
    try:
        return args.run(args)
    except (builds.BuildError, FileNotFoundError, ValueError) as error:
        print(f'mapconv: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
