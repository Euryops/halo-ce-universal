"""The disc image, the build check, and the command line on a made-up disc."""

import hashlib
import io
import tempfile
import unittest
import zlib
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from mapconv import builds
from mapconv.__main__ import main
from mapconv.fabricate import fabricate_map
from mapconv.tests import support
from mapconv.xiso import Xiso, XisoError, write_xiso


def compress(expanded):
    return expanded[:builds.HEADER_SIZE] + zlib.compress(expanded[builds.HEADER_SIZE:])


class Disc(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp(prefix='mapconv-disc-'))

    def test_files_come_out_of_the_image(self):
        files = {'default.xbe': b'XBEH' * 100, 'maps/ui.map': b'u' * 5000, 'maps/bloodgulch.map': b'b' * 3,
                 'maps/sub/other.map': b'o', 'movies/intro.bik': b'm' * 2049}
        write_xiso(self.folder / 'halo.iso', files)
        image = Xiso(self.folder / 'halo.iso')
        self.assertEqual(set(image.maps()), {'ui', 'bloodgulch'})
        for name, data in files.items():
            self.assertEqual(image.read(name), data)

    def test_a_short_image_is_refused(self):
        write_xiso(self.folder / 'halo.iso', {'maps/ui.map': b'u' * 9000})
        data = (self.folder / 'halo.iso').read_bytes()
        (self.folder / 'short.iso').write_bytes(data[:-4096])
        with self.assertRaises(XisoError):
            Xiso(self.folder / 'short.iso')

    def test_something_else_is_not_an_image(self):
        (self.folder / 'no.iso').write_bytes(bytes(0x20000))
        with self.assertRaises(XisoError):
            Xiso(self.folder / 'no.iso')

    def test_a_map_is_known_by_its_hash_compressed_or_not(self):
        expanded = fabricate_map(support.fixture(), seed=4)
        disc = compress(expanded)
        table = {'bloodgulch': (len(disc), hashlib.sha256(disc).hexdigest(),
                                len(expanded), hashlib.sha256(expanded).hexdigest())}
        with mock.patch.dict(builds.MAPS, table, clear=True):
            self.assertEqual(builds.identify('bloodgulch', len(disc), hashlib.sha256(disc).hexdigest()), 'disc')
            self.assertEqual(builds.identify('bloodgulch', len(expanded), hashlib.sha256(expanded).hexdigest()),
                             'expanded')
            with self.assertRaises(builds.BuildError):
                builds.identify('bloodgulch', len(disc), hashlib.sha256(disc + b'x').hexdigest())
            with self.assertRaises(builds.BuildError):
                builds.identify('carousel2', 1, '')
        self.assertEqual(builds.expand(disc), expanded)
        self.assertEqual(builds.expand(expanded), expanded)

    def test_the_known_build_has_every_retail_map(self):
        self.assertEqual(len(builds.MAPS), 24)
        self.assertIn('bloodgulch', builds.MAPS)
        for disc_size, disc_hash, size, sha in builds.MAPS.values():
            self.assertTrue(disc_size < size and len(disc_hash) == len(sha) == 64)

    def test_the_command_line_round_trips_and_converts_a_made_up_disc(self):
        defs_dir = support.fixture_definitions.write(self.folder / 'defs')
        expanded = fabricate_map(support.fixture(), seed=5, media=True, name='bloodgulch')
        disc = compress(expanded)
        write_xiso(self.folder / 'halo.iso', {'maps/bloodgulch.map': disc, 'maps/ui.map': b'not a map'})
        table = {'bloodgulch': (len(disc), hashlib.sha256(disc).hexdigest(),
                                len(expanded), hashlib.sha256(expanded).hexdigest())}
        with mock.patch.dict(builds.MAPS, table, clear=True):
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(main(['--definitions', str(defs_dir), 'check', str(self.folder / 'halo.iso')]), 1)
            self.assertIn('bloodgulch: build 01.10.12.2276, disc', out.getvalue())
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(main(['--definitions', str(defs_dir), 'roundtrip', str(self.folder / 'halo.iso'),
                                       '--maps', 'bloodgulch']), 0)
            self.assertIn('same bytes', out.getvalue())
            with redirect_stdout(io.StringIO()):
                self.assertEqual(main(['--definitions', str(defs_dir), 'convert', str(self.folder / 'halo.iso'),
                                       str(self.folder / 'sd'), '--maps', 'bloodgulch', '--workers', '1']), 0)
        converted = (self.folder / 'sd' / 'halo' / 'maps' / 'bloodgulch.map').read_bytes()
        self.assertEqual(converted[:4], b'head')
        self.assertEqual(converted[0x68:0x6C], b'wii1')
        self.assertTrue((self.folder / 'sd' / 'mapconv-report.json').exists())


if __name__ == '__main__':
    unittest.main()
