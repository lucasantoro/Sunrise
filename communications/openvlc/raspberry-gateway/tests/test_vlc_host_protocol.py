#!/usr/bin/env python3
import unittest

from vlc_host_protocol import (
    HostFrameParser,
    TYPE_IP,
    TYPE_LOG,
    crc16_ccitt,
    encode_frame,
    validate_ip_packet,
)


def ipv4_packet(payload: bytes = b"test") -> bytes:
    total = 20 + len(payload)
    header = bytes(
        (
            0x45,
            0,
            total >> 8,
            total & 0xFF,
            0,
            1,
            0,
            0,
            64,
            17,
            0,
            0,
            192,
            168,
            0,
            1,
            192,
            168,
            0,
            2,
        )
    )
    return header + payload


class ProtocolTests(unittest.TestCase):
    def test_known_crc(self):
        self.assertEqual(crc16_ccitt(b"123456789"), 0x29B1)

    def test_fragmented_frame(self):
        encoded = encode_frame(TYPE_IP, ipv4_packet(), 42)
        parser = HostFrameParser()
        frames = []
        for byte in encoded:
            frames.extend(parser.feed(bytes((byte,))))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].sequence, 42)
        self.assertEqual(frames[0].payload, ipv4_packet())

    def test_noise_and_multiple_frames(self):
        parser = HostFrameParser()
        stream = (
            b"BOOT text\r\n"
            + encode_frame(TYPE_LOG, b"ready\n", 7)
            + encode_frame(TYPE_IP, ipv4_packet(b"video"), 8)
        )
        frames = parser.feed(stream)
        self.assertEqual([frame.frame_type for frame in frames], [TYPE_LOG, TYPE_IP])
        self.assertGreater(parser.stats.discarded_bytes, 0)

    def test_crc_error_resynchronizes(self):
        first = bytearray(encode_frame(TYPE_LOG, b"bad", 1))
        first[-1] ^= 0x01
        parser = HostFrameParser()
        frames = parser.feed(bytes(first) + encode_frame(TYPE_LOG, b"good", 2))
        self.assertEqual([frame.payload for frame in frames], [b"good"])
        self.assertEqual(parser.stats.crc_errors, 1)

    def test_oversized_header_resynchronizes(self):
        bad = b"\xA5\x5A\xC3\x01\x01\x00\x01\xff\xff"
        parser = HostFrameParser(max_payload=900)
        frames = parser.feed(bad + encode_frame(TYPE_LOG, b"ok", 2))
        self.assertEqual([frame.payload for frame in frames], [b"ok"])
        self.assertEqual(parser.stats.length_errors, 1)

    def test_ip_validation(self):
        packet = ipv4_packet(b"abc")
        self.assertEqual(validate_ip_packet(packet + b"padding"), packet)
        self.assertIsNone(validate_ip_packet(b"\x45\x00"))
        self.assertIsNone(validate_ip_packet(b"\x10" + b"\x00" * 40))


if __name__ == "__main__":
    unittest.main()
