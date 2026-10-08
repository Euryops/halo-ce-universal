"""Byte-swapping a map's tags by walking their field definitions.

A region is a run of tag data loaded at one address: the tag cache (the tag
header, the tag table, the names and every tag but the structure BSPs) or one
structure BSP. The walk reads the region in the byte order it is in, and plans
what to do to each word: swap a 2- or 4-byte number, or swap a pointer and move
it from the Xbox's tag cache to the Wii's. Nothing is changed until the walk is
done, so a count or a pointer is always read as it was, and a word that two
walks reach (a block two tags share, a descriptor the tag header and a model
both list) is planned once; two plans that disagree about a word are an error,
not a guess.

The same walk runs either way: from the Xbox (little-endian, 0x803A6000) to the
Wii (big-endian, 0x90100000) and back. Back from forward is the same bytes, which
is what tests/test_roundtrip.py checks.
"""

import struct
from collections import Counter
from dataclasses import dataclass, field

XBOX_TAG_CACHE = 0x803A6000
WII_TAG_CACHE = 0x90100000      # port/wii/halo_wii_capacity.h
TAG_CACHE_SIZE = 0x1600000      # source/cache/physical_memory_map.h

LITTLE, BIG = '<', '>'


@dataclass(frozen=True)
class Direction:
    order: str          # the byte order the region is in now
    old_base: int       # the tag cache the pointers are to now
    new_base: int

    def rebase(self, value):
        if self.old_base <= value < self.old_base + TAG_CACHE_SIZE:
            return value - self.old_base + self.new_base
        if self.new_base <= value < self.new_base + TAG_CACHE_SIZE:
            # It would come back as a pointer into the old cache: not reversible.
            raise ValueError(f'pointer {value:08x} is already in the tag cache it would move to')
        return value


TO_WII = Direction(LITTLE, XBOX_TAG_CACHE, WII_TAG_CACHE)
TO_XBOX = Direction(BIG, WII_TAG_CACHE, XBOX_TAG_CACHE)


class WalkError(ValueError):
    pass


# What a plan holds for each byte of the region: nothing, the first byte of a
# planned word (its width, and whether it is a pointer), or a later byte of one.
_NONE, _LATER, _WORD2, _WORD4, _POINTER = 0, 1, 2, 4, 5


@dataclass
class Plan:
    """What to do to a region: one mark per byte (as above), so that a 22 MB
    tag cache costs 22 MB to plan."""
    data: bytes
    base: int           # the region's address, in the direction's old tag cache
    direction: Direction
    marks: bytearray = None
    visited: set = field(default_factory=set)
    counts: Counter = field(default_factory=Counter)
    left_as_is: Counter = field(default_factory=Counter)   # data whose bytes were not walked

    def __post_init__(self):
        self.marks = bytearray(len(self.data))

    # reading, in the region's present byte order

    def u16(self, offset):
        self._inside(offset, 2)
        return struct.unpack_from(self.direction.order + 'H', self.data, offset)[0]

    def u32(self, offset):
        self._inside(offset, 4)
        return struct.unpack_from(self.direction.order + 'I', self.data, offset)[0]

    def _inside(self, offset, size):
        if offset < 0 or offset + size > len(self.data):
            raise WalkError(f'{offset:x}+{size:x} is outside the region ({len(self.data):x} bytes)')

    def resolve(self, address, size):
        """The offset in this region of size bytes at address, or an error."""
        offset = address - self.base
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise WalkError(f'{address:08x}+{size:x} is outside the region at {self.base:08x}')
        return offset

    # planning

    def swap(self, offset, width, pointer=False):
        if width not in (2, 4):
            raise WalkError(f'cannot swap {width} bytes')
        self._inside(offset, width)
        mark = _POINTER if pointer else width
        planned = self.marks[offset]
        if planned == mark:
            return
        if planned or any(self.marks[offset + 1:offset + width]):
            raise WalkError(f'{offset:x} is planned as {_describe(planned)} and as {_describe(mark)}')
        self.marks[offset] = mark
        self.marks[offset + 1:offset + width] = b'\1' * (width - 1)

    def swap_array(self, offset, width, count):
        for i in range(count):
            self.swap(offset + i * width, width)

    def pointer(self, offset, size=1, required=False):
        """Plan the pointer at offset and return its target's offset in this
        region (None for a null pointer; an error for one outside it)."""
        self.swap(offset, 4, pointer=True)
        address = self.u32(offset)
        if not address:
            if required:
                raise WalkError(f'null pointer at {offset:x}')
            return None
        return self.resolve(address, size)

    def words(self):
        """(offset, mark) of each planned word."""
        marks = self.marks
        at = -1
        for mark in (_WORD2, _WORD4, _POINTER):
            needle = bytes([mark])
            at = marks.find(needle)
            while at >= 0:
                yield at, mark
                at = marks.find(needle, at + 1)

    @property
    def word_count(self):
        return sum(self.marks.count(m) for m in (_WORD2, _WORD4, _POINTER))

    @property
    def pointer_count(self):
        return self.marks.count(_POINTER)

    def apply(self):
        out = bytearray(self.data)
        source = self.direction.order
        target = BIG if source == LITTLE else LITTLE
        rebase = self.direction.rebase
        for offset, mark in self.words():
            if mark == _WORD2:
                out[offset], out[offset + 1] = out[offset + 1], out[offset]
            elif mark == _WORD4:
                out[offset:offset + 4] = self.data[offset:offset + 4][::-1]
            else:
                value = struct.unpack_from(source + 'I', self.data, offset)[0]
                struct.pack_into(target + 'I', out, offset, rebase(value))
        return bytes(out)


def _describe(mark):
    return {_NONE: 'nothing', _LATER: 'part of another word', _WORD2: 'a 2-byte number',
            _WORD4: 'a 4-byte number', _POINTER: 'a pointer'}[mark]


# ---------- the Halo byte-swap codes (source/memory/byte_swapping.h)

BS_1BYTE, BS_2BYTE, BS_4BYTE, BS_8BYTE = 1, -2, -4, -8
BS_BEGIN_ARRAY, BS_END_ARRAY = -100, -101

# source/hs/hs_scenario_definitions.c: hs_data_array_codes, hs_syntax_node_codes
HS_DATA_ARRAY_CODES = [BS_BEGIN_ARRAY, 1, 32, BS_2BYTE, BS_2BYTE, 1, 3, BS_4BYTE,
                       BS_2BYTE, BS_2BYTE, BS_2BYTE, BS_2BYTE, BS_4BYTE, BS_END_ARRAY]
HS_SYNTAX_NODE_CODES = [BS_BEGIN_ARRAY, 1, BS_2BYTE, BS_2BYTE, BS_2BYTE, BS_2BYTE,
                        BS_4BYTE, BS_4BYTE, BS_4BYTE, BS_END_ARRAY]
HS_DATA_ARRAY_SIZE = 56
HS_SYNTAX_NODE_SIZE = 20


def byte_swap_codes(codes, start=0):
    """The swaps a byte-swap code table makes, as (offset, width), and its size:
    _byte_swap_data in source/memory/byte_swapping.c, without the data."""
    if codes[start] != BS_BEGIN_ARRAY or codes[start + 1] < 0:
        raise ValueError('a byte-swap code table starts with _begin_bs_array and a count')
    swaps, offset, index = [], 0, start
    for _ in range(codes[start + 1]):
        index = start + 2
        while codes[index] != BS_END_ARRAY:
            code = codes[index]
            if code in (BS_2BYTE, BS_4BYTE):
                swaps.append((offset, -code))
                offset -= code
                index += 1
            elif code == BS_8BYTE:
                raise ValueError('8-byte swaps are not in any tag')
            elif code == BS_BEGIN_ARRAY:
                inner, size, used = byte_swap_codes(codes, index)
                swaps += [(offset + o, w) for o, w in inner]
                offset += size
                index += used
            elif code > 0:
                offset += code
                index += 1
            else:
                raise ValueError(f'invalid byte-swap code {code}')
    return swaps, offset, index + 1 - start


def plan_codes(plan, codes, offset, count):
    swaps, size, _ = byte_swap_codes(codes)
    for i in range(count):
        for o, width in swaps:
            plan.swap(offset + i * size + o, width)
