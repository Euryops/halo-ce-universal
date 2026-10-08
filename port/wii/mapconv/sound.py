"""Sounds: the Xbox's ADPCM (or 16-bit PCM) to Nintendo's DSP ADPCM.

A permutation's samples are compression 0 (16-bit little-endian PCM) or 1 (Xbox
ADPCM: per block a 4-byte header for each channel, then 4-byte groups of eight
nibbles alternating between channels; 64 samples per channel, the header's sample
the first, the 64th nibble padding, as port/linux/src/dsound_sdl.c decodes it).

DSP ADPCM is what the Wii's DSP decodes in hardware: 8-byte frames of 14 samples,
a header byte (which of 8 coefficient pairs, and a shift) and 14 nibbles, each
sample predicted from the two before. Each channel gets the standard 96-byte DSP
header (big-endian: sample count, nibble count, rate, loop, the 16 coefficients,
the initial predictor/scale and history), and the converted samples are

    [header, channel 0] [header, channel 1] ... [frames, channel 0] [frames, channel 1] ...

with each channel's frames padded to a 32-byte boundary, as the DSP's DMA wants.
"""

import struct

COMPRESSION_NONE, COMPRESSION_XBOX_ADPCM = 0, 1
COMPRESSION_DSP_ADPCM = 0x40    # port/wii/halo_wii_map.h

XBOX_BLOCK_BYTES, XBOX_BLOCK_SAMPLES = 36, 64
DSP_FRAME_BYTES, DSP_FRAME_SAMPLES = 8, 14
DSP_HEADER_SIZE = 96

IMA_STEPS = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def _clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def decode_xbox_adpcm(data, channels):
    """Xbox ADPCM to a list of sample lists, one per channel."""
    block_bytes = XBOX_BLOCK_BYTES * channels
    out = [[] for _ in range(channels)]
    for block in range(len(data) // block_bytes):
        base = block * block_bytes
        for c in range(channels):
            predictor, index = struct.unpack_from('<hB', data, base + 4 * c)
            index = min(index, 88)
            samples = [predictor]
            for group in range(8):
                at = base + 4 * channels + (group * channels + c) * 4
                for byte in data[at:at + 4]:
                    for nibble in (byte & 15, byte >> 4):
                        if len(samples) == XBOX_BLOCK_SAMPLES:
                            break       # the 64th nibble only pads the block
                        step = IMA_STEPS[index]
                        diff = step >> 3
                        if nibble & 1: diff += step >> 2
                        if nibble & 2: diff += step >> 1
                        if nibble & 4: diff += step
                        predictor = _clamp16(predictor - diff if nibble & 8 else predictor + diff)
                        index = min(max(index + IMA_INDEX[nibble & 7], 0), 88)
                        samples.append(predictor)
            out[c] += samples
    return out


def encode_xbox_adpcm(channels_samples):
    """Samples to Xbox ADPCM (for making test data; not a careful encoder)."""
    channels = len(channels_samples)
    length = len(channels_samples[0])
    out = bytearray()
    states = [[0, 0] for _ in range(channels)]
    for start in range(0, length, XBOX_BLOCK_SAMPLES):
        headers, nibbles = bytearray(), [[] for _ in range(channels)]
        for c in range(channels):
            block = channels_samples[c][start:start + XBOX_BLOCK_SAMPLES]
            block += [block[-1] if block else 0] * (XBOX_BLOCK_SAMPLES - len(block))
            predictor, index = block[0], states[c][1]
            headers += struct.pack('<hBB', predictor, index, 0)
            for sample in block[1:]:
                step = IMA_STEPS[index]
                delta = sample - predictor
                nibble = 8 if delta < 0 else 0
                delta = abs(delta)
                diff = step >> 3
                if delta >= step: nibble |= 4; delta -= step; diff += step
                if delta >= step >> 1: nibble |= 2; delta -= step >> 1; diff += step >> 1
                if delta >= step >> 2: nibble |= 1; diff += step >> 2
                predictor = _clamp16(predictor - diff if nibble & 8 else predictor + diff)
                index = min(max(index + IMA_INDEX[nibble & 7], 0), 88)
                nibbles[c].append(nibble)
            nibbles[c].append(0)
            states[c] = [predictor, index]
        out += headers
        for group in range(8):
            for c in range(channels):
                n = nibbles[c][group * 8:group * 8 + 8]
                out += bytes(n[i] | n[i + 1] << 4 for i in range(0, 8, 2))
    return bytes(out)


def decode_pcm16(data, channels):
    n = len(data) // (2 * channels)
    values = struct.unpack_from(f'<{n * channels}h', data)
    return [list(values[c::channels]) for c in range(channels)]


# ---------- DSP ADPCM

def _fit_coefficients(samples):
    """Eight predictor pairs for these samples: the least-squares pair of each
    stretch of frames, sorted by the pair, then merged into eight by k-means on
    the pairs; each in 4.11 fixed point, as the DSP takes them."""
    pairs = []
    stretch = DSP_FRAME_SAMPLES * 8
    for start in range(2, len(samples), stretch):
        r11 = r22 = r12 = r01 = r02 = 0.0
        for n in range(start, min(start + stretch, len(samples))):
            s0, s1, s2 = samples[n], samples[n - 1], samples[n - 2]
            r11 += s1 * s1; r22 += s2 * s2; r12 += s1 * s2; r01 += s0 * s1; r02 += s0 * s2
        det = r11 * r22 - r12 * r12
        if det > 1e-6 * (r11 * r22 + 1):
            a1 = (r01 * r22 - r02 * r12) / det
            a2 = (r02 * r11 - r01 * r12) / det
            if abs(a1) < 4 and abs(a2) < 4:
                pairs.append((a1, a2))
    defaults = [(0.0, 0.0), (1.0, 0.0), (2.0, -1.0), (1.5, -0.6), (1.8, -0.85), (0.5, 0.0),
                (1.9, -0.95), (1.2, -0.3)]
    centres = list(defaults)
    if pairs:
        pairs.sort()
        k = min(8, len(pairs))
        centres = [pairs[(i * len(pairs)) // k] for i in range(k)]
        for _ in range(8):
            groups = [[] for _ in centres]
            for p in pairs:
                best = min(range(len(centres)), key=lambda i: (p[0] - centres[i][0]) ** 2 + (p[1] - centres[i][1]) ** 2)
                groups[best].append(p)
            centres = [(sum(a for a, _ in g) / len(g), sum(b for _, b in g) / len(g)) if g else c
                       for g, c in zip(groups, centres)]
        centres += defaults[:8 - len(centres)]
    coefficients = []
    for a1, a2 in centres:
        coefficients += [max(-32768, min(32767, round(a1 * 2048))), max(-32768, min(32767, round(a2 * 2048)))]
    return coefficients


def _encode_frame(frame, coefficients, h1, h2):
    """The best (header byte, nibbles, error, h1, h2) for 14 samples."""
    best = None
    for pair in range(8):
        c1, c2 = coefficients[2 * pair], coefficients[2 * pair + 1]
        # The smallest shift whose nibbles reach the largest residual from a
        # perfect history, then one more in case the history drifts.
        p1, p2, peak = h1, h2, 0
        for s in frame:
            residual = s - ((c1 * p1 + c2 * p2 + 1024) >> 11)
            peak = max(peak, abs(residual))
            p2, p1 = p1, s
        scale = 0
        while scale < 12 and (peak >> scale) > 7:
            scale += 1
        for shift in (scale, min(scale + 1, 12)):
            p1, p2, error, nibbles = h1, h2, 0, []
            for s in frame:
                prediction = c1 * p1 + c2 * p2
                residual = (s << 11) - prediction
                n = (residual + (1 << (10 + shift))) >> (11 + shift) if residual > 0 else \
                    -((-residual + (1 << (10 + shift))) >> (11 + shift))
                n = -8 if n < -8 else 7 if n > 7 else n
                decoded = _clamp16((((n << shift) << 11) + 1024 + prediction) >> 11)
                error += (s - decoded) ** 2
                nibbles.append(n)
                p2, p1 = p1, decoded
                if best is not None and error >= best[2]:
                    break
            else:
                if best is None or error < best[2]:
                    best = ((pair << 4) | shift, nibbles, error, p1, p2)
    return best


def encode_dsp(samples):
    """One channel of 16-bit samples to (coefficients, frames)."""
    coefficients = _fit_coefficients(samples)
    frames = bytearray()
    h1 = h2 = 0
    for start in range(0, len(samples), DSP_FRAME_SAMPLES):
        frame = samples[start:start + DSP_FRAME_SAMPLES]
        frame = frame + [0] * (DSP_FRAME_SAMPLES - len(frame))
        header, nibbles, _, h1, h2 = _encode_frame(frame, coefficients, h1, h2)
        frames.append(header)
        frames += bytes(((nibbles[i] & 15) << 4) | (nibbles[i + 1] & 15) for i in range(0, 14, 2))
    return coefficients, bytes(frames)


def decode_dsp(frames, coefficients, count):
    out, h1, h2 = [], 0, 0
    for at in range(0, len(frames), DSP_FRAME_BYTES):
        header = frames[at]
        c1, c2 = coefficients[2 * (header >> 4)], coefficients[2 * (header >> 4) + 1]
        shift = header & 15
        for byte in frames[at + 1:at + 8]:
            for n in (byte >> 4, byte & 15):
                n = n - 16 if n >= 8 else n
                s = _clamp16((((n << shift) << 11) + 1024 + c1 * h1 + c2 * h2) >> 11)
                out.append(s)
                h2, h1 = h1, s
    return out[:count]


def dsp_header(count, rate, coefficients, first_header):
    """The standard DSP ADPCM header: 96 bytes, big-endian."""
    nibbles = (count // 14) * 16 + (count % 14 + 2 if count % 14 else 0)
    return struct.pack('>IIIHHIII16hHHhhHhh22x', count, nibbles, rate, 0, 0, 2, nibbles - 1, 2,
                       *coefficients, 0, first_header, 0, 0, 0, 0, 0)


def convert_samples(data, compression, channels, rate):
    """A permutation's samples to the DSP layout above."""
    if compression == COMPRESSION_XBOX_ADPCM:
        decoded = decode_xbox_adpcm(data, channels)
    elif compression == COMPRESSION_NONE:
        decoded = decode_pcm16(data, channels)
    else:
        raise ValueError(f'sound compression {compression} is not one the Xbox has')
    headers, bodies = bytearray(), bytearray()
    for samples in decoded:
        coefficients, frames = encode_dsp(samples)
        headers += dsp_header(len(samples), rate, coefficients, frames[0] if frames else 0)
        bodies += frames + bytes((-len(frames)) % 32)
    return headers + bodies
