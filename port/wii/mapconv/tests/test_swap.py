import struct
import unittest

from mapconv.swap import (Plan, TO_WII, TO_XBOX, WalkError, XBOX_TAG_CACHE, WII_TAG_CACHE,
                          HS_DATA_ARRAY_CODES, HS_SYNTAX_NODE_CODES, BS_BEGIN_ARRAY, BS_END_ARRAY,
                          BS_2BYTE, BS_4BYTE, byte_swap_codes)


class ByteSwapCodes(unittest.TestCase):
    def test_the_script_tables_are_the_sizes_the_engine_says(self):
        # sizeof(struct data_array) and sizeof(struct hs_syntax_node)
        self.assertEqual(byte_swap_codes(HS_DATA_ARRAY_CODES)[1], 56)
        self.assertEqual(byte_swap_codes(HS_SYNTAX_NODE_CODES)[1], 20)
        swaps = byte_swap_codes(HS_DATA_ARRAY_CODES)[0]
        self.assertEqual(swaps, [(32, 2), (34, 2), (40, 4), (44, 2), (46, 2), (48, 2), (50, 2), (52, 4)])

    def test_nested_arrays_repeat(self):
        codes = [BS_BEGIN_ARRAY, 1, BS_2BYTE, BS_BEGIN_ARRAY, 3, BS_4BYTE, 2, BS_END_ARRAY, BS_2BYTE, BS_END_ARRAY]
        swaps, size, used = byte_swap_codes(codes)
        self.assertEqual(swaps, [(0, 2), (2, 4), (8, 4), (14, 4), (20, 2)])
        self.assertEqual((size, used), (22, len(codes)))


class Plans(unittest.TestCase):
    def test_a_word_planned_two_ways_is_an_error(self):
        plan = Plan(bytes(16), XBOX_TAG_CACHE, TO_WII)
        plan.swap(4, 4)
        plan.swap(4, 4)
        with self.assertRaises(WalkError):
            plan.swap(4, 2)
        with self.assertRaises(WalkError):
            plan.swap(6, 2)
        with self.assertRaises(WalkError):
            plan.swap(4, 4, pointer=True)

    def test_pointers_move_and_numbers_turn_over(self):
        data = struct.pack('<IIHH', XBOX_TAG_CACHE + 0x40, 0x12345678, 0xABCD, 0)
        plan = Plan(data, XBOX_TAG_CACHE, TO_WII)
        plan.swap(0, 4, pointer=True)
        plan.swap(4, 4)
        plan.swap(8, 2)
        out = plan.apply()
        self.assertEqual(struct.unpack('>IIHH', out), (WII_TAG_CACHE + 0x40, 0x12345678, 0xABCD, 0))
        back = Plan(out, WII_TAG_CACHE, TO_XBOX)
        back.swap(0, 4, pointer=True)
        back.swap(4, 4)
        back.swap(8, 2)
        self.assertEqual(back.apply(), data)

    def test_a_pointer_outside_the_cache_is_only_turned_over(self):
        plan = Plan(struct.pack('<I', 0x00400000), XBOX_TAG_CACHE, TO_WII)
        plan.swap(0, 4, pointer=True)
        self.assertEqual(struct.unpack('>I', plan.apply())[0], 0x00400000)

    def test_a_pointer_already_in_the_wiis_cache_is_refused(self):
        plan = Plan(struct.pack('<I', WII_TAG_CACHE + 4), XBOX_TAG_CACHE, TO_WII)
        plan.swap(0, 4, pointer=True)
        with self.assertRaises(ValueError):
            plan.apply()

    def test_a_pointer_out_of_the_region_is_an_error(self):
        plan = Plan(struct.pack('<I', XBOX_TAG_CACHE + 0x100), XBOX_TAG_CACHE, TO_WII)
        with self.assertRaises(WalkError):
            plan.pointer(0, 4)


if __name__ == '__main__':
    unittest.main()
