"""Round-trip regressions for the offline BC texture encoder."""

import struct
import unittest

from cook_bc7 import encode_chain


def decode_bc4(block: bytes) -> list:
    high, low = block[:2]
    palette = [high, low]
    if high > low:
        palette += [((7 - i) * high + i * low) // 7 for i in range(1, 7)]
    else:
        palette += [((5 - i) * high + i * low) // 5 for i in range(1, 5)] + [0, 255]
    indices = int.from_bytes(block[2:8], 'little')
    return [palette[(indices >> (3 * i)) & 7] for i in range(16)]


class texture_cook_tests(unittest.TestCase):
    def test_bc5_keeps_rg_at_every_texel(self) -> None:
        # Distinct B/A expose accidental RGBA input to the packed-RG BC5 encoder.
        for width, height in [(4, 4), (5, 7), (1, 1), (8, 4)]:
            with self.subTest(width=width, height=height):
                encoded = encode_chain(bytes([90, 140, 250, 255]) * width * height,
                                       width, height, 1, 83)
                for offset in range(148, len(encoded), 16):
                    for channel, expected in [(0, 90), (1, 140)]:
                        decoded = decode_bc4(encoded[offset + channel * 8:offset + channel * 8 + 8])
                        self.assertLessEqual(max(abs(v - expected) for v in decoded), 1)

    def test_bc5_preserves_block_and_row_positions(self) -> None:
        colours = [(80, 120), (100, 140), (160, 180), (190, 200)]
        source = bytearray()
        for y in range(8):
            for x in range(8):
                red, green = colours[(y // 4) * 2 + x // 4]
                source.extend([red, green, 250, 255])
        encoded = encode_chain(bytes(source), 8, 8, 1, 83)
        for block_index, colour in enumerate(colours):
            for channel, expected in enumerate(colour):
                start = 148 + block_index * 16 + channel * 8
                self.assertLessEqual(max(abs(v - expected) for v in decode_bc4(encoded[start:start + 8])), 1)

    def test_mip_payload_and_format(self) -> None:
        source = bytes([128, 128, 255, 255]) * (16 + 4 + 1)
        for format_id in [83, 98, 99]:
            encoded = encode_chain(source, 4, 4, 3, format_id)
            self.assertEqual(len(encoded), 148 + 3 * 16)
            self.assertEqual(struct.unpack_from('<I', encoded, 128)[0], format_id)

    def test_invalid_source_rejected(self) -> None:
        for source in [bytes(63), bytes(65)]:
            with self.assertRaises(ValueError):
                encode_chain(source, 4, 4, 1, 83)


if __name__ == '__main__':
    unittest.main()
