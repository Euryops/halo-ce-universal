"""Swap twice, get the same bytes: the check that stands in for a disc.

Each made-up map (fabricate.py) goes to the Wii and back, and must come back
byte for byte. On the way, every number the fabricator wrote must read the same
big-endian on the Wii side, and every address it wrote must be the same place
in the Wii's tag cache, so a walk that skipped a block, or swapped one twice,
fails here even though the round trip alone would not show it.
"""

import struct
import unittest

from mapconv.fabricate import fabricate_map
from mapconv.swap import TO_WII, TO_XBOX, XBOX_TAG_CACHE, WII_TAG_CACHE
from mapconv.tags import Header, swap_map
from mapconv.tests import support


class RoundTrip(unittest.TestCase):
    def check(self, defs, seed, **options):
        truth = []
        xbox = fabricate_map(defs, seed=seed, truth=truth, **options)
        wii = swap_map(xbox, defs, TO_WII)
        self.assertEqual(wii.errors, [])
        self.assertNotEqual(wii.data, xbox)
        self.assertEqual(len(wii.data), len(xbox))
        for file_offset, region in truth:
            for at, address in region.pointers.items():
                moved = struct.unpack_from('>I', wii.data, file_offset + at)[0]
                self.assertEqual(moved, address - XBOX_TAG_CACHE + WII_TAG_CACHE, f'pointer at {at:x}')
            for at, width in region.numbers.items():
                code = 'H' if width == 2 else 'I'
                self.assertEqual(struct.unpack_from('>' + code, wii.data, file_offset + at),
                                 struct.unpack_from('<' + code, xbox, file_offset + at), f'number at {at:x}')
            for at, (first, second) in region.bitfields.items():
                self.assertEqual(wii.data[file_offset + at], first << 6 | second, f'bitfield at {at:x}')
        header = Header.parse(wii.data, '>')
        header.check()
        self.assertEqual((header.name, header.build), ('madeup', '01.10.12.2276'))
        back = swap_map(wii.data, defs, TO_XBOX)
        self.assertEqual(back.errors, [])
        self.assertEqual(back.data, xbox)
        return wii

    def test_made_up_definitions(self):
        defs = support.fixture()
        for seed in range(25):
            with self.subTest(seed=seed):
                self.check(defs, seed, media=seed % 2 == 1)

    def test_the_walk_reaches_everything_the_fabricator_wrote(self):
        defs = support.fixture()
        counts = set()
        for seed in range(6):
            wii = self.check(defs, seed, max_depth=4, max_count=4)
            counts |= set(wii.tags.plan.counts) | {n for b in wii.bsps for n in b.plan.counts}
        for name in ('Scenario', 'ScenarioThing', 'ScenarioBSP', 'ScenarioStructureBSPMaterial',
                     'ModelAnimationsAnimation', 'ScenarioRecordedAnimation',
                     'ModelGeometryPart', 'BitmapData', 'SoundPermutation', 'Unit', 'ObjectAttachment'):
            self.assertIn(name, counts)

    def test_every_group_of_invaders_definitions(self):
        defs = support.invader()
        self.assertEqual(len(defs.roots), 82)
        for seed in range(4):
            with self.subTest(seed=seed):
                wii = self.check(defs, seed, media=seed % 2 == 1)
                walked = set(wii.tags.plan.counts)
                self.assertTrue({defs.roots[g] for g in defs.roots} - {'ScenarioStructureBSP'} <= walked)

    def test_nothing_the_engine_reads_is_left_unswapped(self):
        for defs in (support.fixture(), support.invader()):
            for seed in range(10):
                wii = self.check(defs, seed)
                left = wii.summary()['left_in_xbox_order']
                left.pop('Scenario.unknown data', None)     # the fixture's own, to be reported
                self.assertEqual(left, {}, seed)

    def test_a_map_already_swapped_is_refused(self):
        defs = support.fixture()
        wii = swap_map(fabricate_map(defs, seed=1), defs, TO_WII).data
        with self.assertRaises(ValueError):
            swap_map(wii, defs, TO_WII)


if __name__ == '__main__':
    unittest.main()
