#!/usr/bin/env python3

import json
import struct
import tempfile
import unittest
from pathlib import Path

from vlc_capture import CaptureAssembler, fnv1a32


def begin_record(
    capture_id: int,
    intervals: bytes,
    trigger: int = 1,
    version: int = 1,
    raw: bool = False,
) -> bytes:
    values = (
        capture_id,
        64_000_000,
        len(intervals) // 2,
        len(intervals) // 2 + 1,
        0xFFFFFFFC,
        0xFFFFFFFC,
        1000,
        1861,
        1500,
        40,
        24,
        32,
        7,
        1,
        0,
        1,
        16,
        800,
        1234,
        fnv1a32(intervals),
    )
    flags = 2 if raw else 0
    return b"OVCT" + bytes((version, 1, trigger, flags)) + struct.pack(">20I", *values)


def data_record(
    capture_id: int, offset: int, intervals: bytes, version: int = 1
) -> bytes:
    return (
        b"OVCT"
        + bytes((version, 2))
        + struct.pack(">HII", len(intervals) // 2, capture_id, offset)
        + intervals
    )


def end_record(
    capture_id: int, intervals: bytes, version: int = 1, raw: bool = False
) -> bytes:
    return (
        b"OVCT"
        + bytes((version, 3, 2 if raw else 0, 0))
        + struct.pack(">III", capture_id, len(intervals) // 2, fnv1a32(intervals))
    )


class CaptureAssemblerTests(unittest.TestCase):
    def test_complete_capture_is_saved_and_self_contained(self):
        intervals = struct.pack(">5H", 24, 40, 64, 24, 32)
        with tempfile.TemporaryDirectory() as directory:
            assembler = CaptureAssembler(directory, "node b")
            self.assertIsNone(assembler.feed(begin_record(7, intervals)))
            self.assertIsNone(
                assembler.feed(data_record(7, 0, intervals[:4]))
            )
            result = assembler.feed(data_record(7, 2, intervals[4:]))
            self.assertIsNone(result)
            result = assembler.feed(end_record(7, intervals))

            self.assertEqual(result.binary_path.read_bytes(),
                             begin_record(7, intervals) + intervals)
            metadata = json.loads(result.metadata_path.read_text())
            self.assertEqual(metadata["capture_id"], 7)
            self.assertEqual(metadata["trigger"], "crc")
            self.assertEqual(metadata["node"], "node_b")

    def test_gap_rejects_entire_capture(self):
        intervals = struct.pack(">2H", 24, 40)
        with tempfile.TemporaryDirectory() as directory:
            assembler = CaptureAssembler(directory)
            assembler.feed(begin_record(1, intervals))
            with self.assertRaisesRegex(ValueError, "non-contiguous"):
                assembler.feed(data_record(1, 1, intervals))
            self.assertIsNone(assembler.current)
            self.assertEqual(assembler.errors, 1)

    def test_bad_hash_is_not_saved(self):
        intervals = struct.pack(">2H", 24, 40)
        with tempfile.TemporaryDirectory() as directory:
            assembler = CaptureAssembler(directory)
            assembler.feed(begin_record(1, intervals))
            assembler.feed(data_record(1, 0, intervals))
            bad_end = end_record(1, intervals)[:-1] + b"\0"
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                assembler.feed(bad_end)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_validation_without_persistence(self):
        intervals = struct.pack(">2H", 24, 40)
        with tempfile.TemporaryDirectory() as directory:
            assembler = CaptureAssembler(directory, persist=False)
            assembler.feed(begin_record(3, intervals))
            assembler.feed(data_record(3, 0, intervals))
            result = assembler.feed(end_record(3, intervals))
            self.assertIsNone(result.binary_path)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_multiple_sequential_captures_are_saved(self):
        first = struct.pack(">2H", 24, 40)
        second = struct.pack(">3H", 25, 39, 64)
        with tempfile.TemporaryDirectory() as directory:
            assembler = CaptureAssembler(directory, "node-a")
            for capture_id, intervals in ((1, first), (2, second)):
                assembler.feed(begin_record(capture_id, intervals))
                assembler.feed(data_record(capture_id, 0, intervals))
                result = assembler.feed(
                    end_record(capture_id, intervals)
                )
                self.assertEqual(
                    result.metadata["capture_id"], capture_id
                )

            binaries = sorted(Path(directory).glob("*.bin"))
            self.assertEqual(len(binaries), 2)
            self.assertTrue(binaries[0].name.endswith("-id00000001-crc.bin"))
            self.assertTrue(binaries[1].name.endswith("-id00000002-crc.bin"))

    def test_successful_capture_is_labelled_ok(self):
        intervals = struct.pack(">3H", 28, 36, 64)
        with tempfile.TemporaryDirectory() as directory:
            assembler = CaptureAssembler(directory, "node-a")
            assembler.feed(begin_record(9, intervals, trigger=0))
            assembler.feed(data_record(9, 0, intervals))
            result = assembler.feed(end_record(9, intervals))

            self.assertEqual(result.metadata["trigger"], "ok")
            self.assertTrue(result.binary_path.name.endswith(
                "-id00000009-ok.bin"
            ))

    def test_v2_raw_capture_records_input_domain(self):
        intervals = struct.pack(">3H", 7, 20, 37)
        with tempfile.TemporaryDirectory() as directory:
            assembler = CaptureAssembler(directory, "node-a")
            assembler.feed(begin_record(10, intervals, version=2, raw=True))
            assembler.feed(data_record(10, 0, intervals, version=2))
            result = assembler.feed(
                end_record(10, intervals, version=2, raw=True)
            )

            self.assertEqual(result.metadata["capture_version"], 2)
            self.assertEqual(result.metadata["input_domain"], "raw_pre_filter")
            self.assertEqual(result.metadata["schema"], "openvlc-rx-capture-v2")

    def test_protocol_version_cannot_change_mid_capture(self):
        intervals = struct.pack(">2H", 24, 40)
        with tempfile.TemporaryDirectory() as directory:
            assembler = CaptureAssembler(directory)
            assembler.feed(begin_record(11, intervals, version=2, raw=True))
            with self.assertRaisesRegex(ValueError, "version changed"):
                assembler.feed(data_record(11, 0, intervals, version=1))
            self.assertIsNone(assembler.current)


if __name__ == "__main__":
    unittest.main()
