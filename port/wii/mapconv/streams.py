"""Data the definitions call bytes, but the engine reads as numbers: a model
animation's default and frame data, and a recorded animation's event stream.
Their layouts are the engine's readers in the decomp:

- source/models/model_animations.c: per node, the default data holds what the
  node does not animate, and each frame what it does, in node order: rotation
  (an 8-byte quaternion, four shorts), translation (three floats), scale (a
  float). A compressed animation (flag bit 0) keeps keyframes at
  compressed_data_offset in the frame data: struct compressed_animation_header
  (eleven offsets, then a long per rotated node), longs per translated and scaled
  node (the first keyframe << 12 | the keyframe count), keyframe frame indices
  (words), 6-byte quaternions (three words), points and scales (floats).
- source/cutscene/recorded_animation_initialize.c: the stream starts with a unit
  control, whose fields the version tables there give. Versions 1 to 3 then
  hold v1 events (recorded_animation_playback_v1.c: a short type, a word time
  delta, then the event's numbers); version 4 holds an animation state (six
  shorts) and events of recorded_animation_playback.c: a header byte that is a
  bitfield (time delta code, event type), the time delta in one or two more
  bytes, then a byte, a short, two floats, or a difference (two chars or two
  shorts).

Each planner works on a copy of what it would plan, and plans nothing if the
data turns out not to be what the reader expects; the caller then leaves it as
bytes and says so.
"""

ROTATION, TRANSLATION, SCALE = 'rotation', 'translation', 'scale'
COMPRESSED_HEADER_SIZE = 0x2C
KEYFRAME_COUNT_BITS = 12


class NotThisLayout(Exception):
    pass


class Swaps:
    """Swaps to plan, collected first and bounds-checked against the data."""
    def __init__(self, plan, start, size):
        self.plan, self.start, self.size = plan, start, size
        self.items = []

    def add(self, offset, width, count=1):
        if offset < 0 or offset + width * count > self.size:
            raise NotThisLayout(f'{offset:#x}+{width * count:#x} past the data\'s {self.size:#x} bytes')
        self.items += [(self.start + offset + i * width, width) for i in range(count)]

    def bits(self, offset):
        if not 0 <= offset < self.size:
            raise NotThisLayout('a header byte past the data')
        self.items.append((self.start + offset, 'bitfield'))

    def u16(self, offset):
        if offset < 0 or offset + 2 > self.size:
            raise NotThisLayout('a word past the data')
        return self.plan.u16(self.start + offset)

    def u32(self, offset):
        if offset < 0 or offset + 4 > self.size:
            raise NotThisLayout('a long past the data')
        return self.plan.u32(self.start + offset)

    def byte(self, offset):
        if not 0 <= offset < self.size:
            raise NotThisLayout('a byte past the data')
        return self.plan.data[self.start + offset]

    def commit(self):
        for offset, width in self.items:
            if width == 'bitfield':
                self.plan.bitfield_2_6(offset)
            else:
                self.plan.swap(offset, width)


# ---------- model animations

def _animated(flags, node_count):
    return [bool(flags[n >> 5] >> (n & 31) & 1) for n in range(node_count)]


def plan_animation(plan, animation):
    """animation: a dict of the animation's fields (read in the plan's order) and
    where its data is: frame count, frame size, node count, flags, the three node
    bit vectors, compressed data offset, and (offset, size) of default and frame
    data in the region (None where there is none). Returns what it left as bytes,
    as {name: size}."""
    left = {}
    nodes = animation['node count']
    if nodes > 64:      # two longs of flags: the engine's maximum
        return {name: data[1] for name, data in (('default data', animation['default data']),
                                                 ('frame data', animation['frame data'])) if data}
    rotated = _animated(animation['rotation flags'], nodes)
    translated = _animated(animation['translation flags'], nodes)
    scaled = _animated(animation['scale flags'], nodes)

    default = animation['default data']
    if default:
        swaps = Swaps(plan, *default)
        at = 0
        try:
            for n in range(nodes):
                if not rotated[n]:
                    swaps.add(at, 2, 4)
                    at += 8
                if not translated[n]:
                    swaps.add(at, 4, 3)
                    at += 12
                if not scaled[n]:
                    swaps.add(at, 4)
                    at += 4
            if at != default[1]:
                raise NotThisLayout(f'{at} bytes of defaults, the data has {default[1]}')
            swaps.commit()
        except NotThisLayout:
            left['default data'] = default[1]

    frames = animation['frame data']
    if not frames:
        return left
    frame_size = sum(8 * r + 12 * t + 4 * s for r, t, s in zip(rotated, translated, scaled))
    compressed = animation['flags'] & 1
    offset = animation['compressed data offset']
    uncompressed_end = offset if compressed else frames[1]
    swaps = Swaps(plan, *frames)
    try:
        if frame_size != animation['frame size']:
            raise NotThisLayout(f'frames of {frame_size} bytes, the animation says {animation["frame size"]}')
        count = min(animation['frame count'], uncompressed_end // frame_size) if frame_size else 0
        if not compressed and count * frame_size != frames[1]:
            raise NotThisLayout('the frame data is not frame count frames')
        at = 0
        for _ in range(count):
            for n in range(nodes):
                if rotated[n]:
                    swaps.add(at, 2, 4)
                    at += 8
                if translated[n]:
                    swaps.add(at, 4, 3)
                    at += 12
                if scaled[n]:
                    swaps.add(at, 4)
                    at += 4
        if compressed:
            _compressed(swaps, offset, nodes, rotated, translated, scaled)
        swaps.commit()
    except NotThisLayout:
        left['frame data'] = frames[1]
    return left


def _compressed(swaps, base, nodes, rotated, translated, scaled):
    offsets = [swaps.u32(base + 4 * i) for i in range(11)]
    swaps.add(base, 4, 11)
    (rotation_indices, default_rotations, rotation_keys, translation_headers, translation_indices,
     default_translations, translation_keys, scale_headers, scale_indices, default_scales, scale_keys) = offsets

    def keyframes(headers_at, count):
        swaps.add(base + headers_at, 4, count)
        end = 0
        for i in range(count):
            header = swaps.u32(base + headers_at + 4 * i)
            end = max(end, (header >> KEYFRAME_COUNT_BITS) + (header & ((1 << KEYFRAME_COUNT_BITS) - 1)))
        return end

    rotations = keyframes(COMPRESSED_HEADER_SIZE, sum(rotated))
    translations = keyframes(translation_headers, sum(translated))
    scales = keyframes(scale_headers, sum(scaled))
    swaps.add(base + rotation_indices, 2, rotations)
    swaps.add(base + default_rotations, 2, 3 * nodes)
    swaps.add(base + rotation_keys, 2, 3 * rotations)
    swaps.add(base + translation_indices, 2, translations)
    swaps.add(base + default_translations, 4, 3 * nodes)
    swaps.add(base + translation_keys, 4, 3 * translations)
    swaps.add(base + scale_indices, 2, scales)
    swaps.add(base + default_scales, 4, sum(scaled))
    swaps.add(base + scale_keys, 4, scales)


# ---------- recorded animations

# Per version table: (size, swap width) of each field (recorded_animation_initialize.c)
UNIT_CONTROL_VERSIONS = [
    [(1, 0), (1, 0), (2, 2), (2, 2), (2, 2), (8, 4), (12, 4), (12, 4), (12, 4)],
    [(4, 4)],
    [(2, 2)],
    [(2, 2)],
]
ANIMATION_STATE_SIZE = 12   # three (yaw, pitch) shorts

# v1 events: type -> (size with the header, [(offset, width)] after it)
V1_EVENTS = {0: (4, []), 1: (4, []), 2: (6, []), 3: (6, []), 4: (6, [(4, 2)]), 5: (6, [(4, 2)]),
             6: (12, [(4, 4), (8, 4)]), 7: (4, []), 8: (4, [])}
V1_EVENTS.update({t: (16, [(4, 4), (8, 4), (12, 4)]) for t in range(9, 16)})
V1_EVENTS.update({t: (12, [(4, 4), (8, 4)]) for t in range(16, 23)})
V1_END = 1

# v4 events: type -> [(size, width)] of its data
V4_EVENTS = {0: [], 1: [], 2: [(1, 0)], 3: [(1, 0)], 4: [(2, 2)], 5: [(2, 2)], 6: [(4, 4), (4, 4)]}
V4_EVENTS.update({t: [(1, 0), (1, 0)] for t in range(7, 15)})
V4_EVENTS.update({t: [(2, 2), (2, 2)] for t in range(15, 23)})
V4_END = 1


def unit_control_layout(version):
    """(size, [(offset, width)]) of a unit control of this data version."""
    if max(version, 1) > len(UNIT_CONTROL_VERSIONS):
        raise NotThisLayout(f'unit control version {version}')
    at, swaps = 0, []
    for table in UNIT_CONTROL_VERSIONS[:max(version, 1)]:
        for size, width in table:
            swaps += [(at + i, width) for i in range(0, size, width)] if width else []
            at += size
    return at, swaps


def plan_event_stream(plan, version, unit_control_version, start, size):
    """Plan a recorded animation's stream; returns the bytes left unplanned (0
    when the stream is what the readers expect)."""
    swaps = Swaps(plan, start, size)
    try:
        if not 1 <= version <= 4:
            raise NotThisLayout(f'recorded animation version {version}')
        control_size, control = unit_control_layout(unit_control_version)
        for offset, width in control:
            swaps.add(offset, width)
        at = control_size
        if version < 4:
            while at < size:
                kind = swaps.u16(at)
                if kind not in V1_EVENTS:
                    raise NotThisLayout(f'v1 event type {kind}')
                length, fields = V1_EVENTS[kind]
                swaps.add(at, 2, 2)
                for offset, width in fields:
                    swaps.add(at + offset, width)
                at += length
                if kind == V1_END:
                    break
        else:
            swaps.add(at, 2, ANIMATION_STATE_SIZE // 2)
            at += ANIMATION_STATE_SIZE
            while at < size:
                delta, kind = plan.bitfields_2_6(start + at)
                if kind not in V4_EVENTS:
                    raise NotThisLayout(f'event type {kind}')
                swaps.bits(at)
                at += 1
                if delta == 2:
                    swaps.byte(at)
                    at += 1
                elif delta == 3:
                    swaps.add(at, 2)
                    at += 2
                if kind == V4_END:
                    break
                for length, width in V4_EVENTS[kind]:
                    if width:
                        swaps.add(at, width)
                    elif at + length > size:
                        raise NotThisLayout('an event past the stream')
                    at += length
        if at > size:
            raise NotThisLayout('an event past the stream')
        swaps.commit()
        return 0
    except NotThisLayout:
        return size
