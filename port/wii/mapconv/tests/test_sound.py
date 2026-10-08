import math
import random
import struct
import unittest

from mapconv import sound as s


def snr(reference, decoded):
    n = min(len(reference), len(decoded))
    signal = sum(v * v for v in reference[:n])
    noise = sum((a - b) ** 2 for a, b in zip(reference[:n], decoded[:n]))
    return 10 * math.log10(signal / max(noise, 1))


def tone(count, rate=22050, seed=0):
    rng = random.Random(seed)
    return [int(9000 * math.sin(2 * math.pi * 330 * n / rate) + 4000 * math.sin(2 * math.pi * 1700 * n / rate)
                + rng.gauss(0, 150)) for n in range(count)]


class Sound(unittest.TestCase):
    def test_xbox_adpcm_blocks_are_64_samples_with_the_header_first(self):
        samples = tone(64 * 40)
        data = s.encode_xbox_adpcm([samples])
        self.assertEqual(len(data), 36 * 40)
        decoded = s.decode_xbox_adpcm(data, 1)[0]
        self.assertEqual(len(decoded), 64 * 40)
        self.assertEqual(decoded[64], samples[64])          # each block's header holds its first sample
        self.assertGreater(snr(samples, decoded), 20)

    def test_stereo_blocks_alternate_channels(self):
        left, right = tone(64 * 40, seed=1), [v // 3 for v in tone(64 * 40, seed=2)]
        decoded = s.decode_xbox_adpcm(s.encode_xbox_adpcm([left, right]), 2)
        self.assertGreater(snr(left, decoded[0]), 20)
        self.assertGreater(snr(right, decoded[1]), 20)

    def test_dsp_adpcm_is_close_to_what_it_encodes(self):
        samples = tone(22050)
        coefficients, frames = s.encode_dsp(samples)
        self.assertEqual(len(coefficients), 16)
        self.assertEqual(len(frames), -(-len(samples) // 14) * 8)
        self.assertGreater(snr(samples, s.decode_dsp(frames, coefficients, len(samples))), 30)

    def test_silence_stays_silent(self):
        coefficients, frames = s.encode_dsp([0] * 1000)
        self.assertEqual(set(s.decode_dsp(frames, coefficients, 1000)), {0})

    def test_the_converted_layout_is_headers_then_padded_channels(self):
        left, right = tone(640, seed=3), tone(640, seed=4)
        data = s.convert_samples(s.encode_xbox_adpcm([left, right]), s.COMPRESSION_XBOX_ADPCM, 2, 22050)
        count, nibbles, rate = struct.unpack_from('>III', data)
        self.assertEqual((count, rate), (640, 22050))
        self.assertEqual(nibbles, (640 // 14) * 16 + 640 % 14 + 2)
        frames = -(-640 // 14) * 8
        padded = frames + (-frames) % 32
        self.assertEqual(len(data), 2 * s.DSP_HEADER_SIZE + 2 * padded)
        coefficients = struct.unpack_from('>16h', data, 0x1C)
        first = data[2 * s.DSP_HEADER_SIZE:2 * s.DSP_HEADER_SIZE + frames]
        self.assertEqual(struct.unpack_from('>H', data, 0x3E)[0], first[0])
        reference = s.decode_xbox_adpcm(s.encode_xbox_adpcm([left, right]), 2)[0]
        self.assertGreater(snr(reference, s.decode_dsp(first, coefficients, count)), 25)

    def test_pcm_converts_too(self):
        samples = tone(500)
        data = s.convert_samples(struct.pack('<500h', *samples), s.COMPRESSION_NONE, 1, 44100)
        self.assertEqual(struct.unpack_from('>III', data)[::2], (500, 44100))


if __name__ == '__main__':
    unittest.main()
