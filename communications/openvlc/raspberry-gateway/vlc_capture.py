#!/usr/bin/env python3
"""Assemble and persist STM32 OpenVLC RX edge captures."""

import json
import os
import struct
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

MAGIC = b"OVCT"
VERSION = 2
SUPPORTED_VERSIONS = (1, 2)
KIND_BEGIN = 1
KIND_DATA = 2
KIND_END = 3
BEGIN_LEN = 88
DATA_HEADER_LEN = 16
END_LEN = 20

TRIGGERS = {
    0: "ok",
    1: "crc",
    2: "sync",
    3: "length",
    4: "decode",
}


def fnv1a32(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def _signed32(value: int) -> int:
    return value - 0x100000000 if value & 0x80000000 else value


def parse_begin(payload: bytes) -> dict:
    if (
        len(payload) != BEGIN_LEN
        or payload[:4] != MAGIC
        or payload[4] not in SUPPORTED_VERSIONS
        or payload[5] != KIND_BEGIN
    ):
        raise ValueError("invalid capture BEGIN record")
    values = struct.unpack_from(">20I", payload, 8)
    fields = (
        "capture_id",
        "tick_hz",
        "interval_count",
        "edge_count",
        "status_raw",
        "parse_status_raw",
        "phy_rate_kbps",
        "threshold_dac",
        "threshold_mv",
        "cell0_ticks",
        "cell1_ticks",
        "nominal_ticks",
        "residual_q8",
        "syncs",
        "mode",
        "hypothesis_budget",
        "lock_cell",
        "length_raw",
        "snapshot_ms",
        "hash32",
    )
    metadata = dict(zip(fields, values))
    metadata["status"] = _signed32(metadata.pop("status_raw"))
    metadata["parse_status"] = _signed32(metadata.pop("parse_status_raw"))
    metadata["trigger_code"] = payload[6]
    metadata["trigger"] = TRIGGERS.get(payload[6], "unknown")
    metadata["clipped"] = bool(payload[7] & 1)
    metadata["capture_version"] = payload[4]
    metadata["input_domain"] = (
        "raw_pre_filter"
        if payload[4] >= 2 and payload[7] & 2
        else "filtered_edges"
    )
    if metadata["edge_count"] != metadata["interval_count"] + 1:
        raise ValueError("capture BEGIN has inconsistent edge count")
    return metadata


@dataclass(frozen=True)
class CaptureResult:
    binary_path: Optional[Path]
    metadata_path: Optional[Path]
    metadata: dict


@dataclass
class _Capture:
    begin: bytes
    metadata: dict
    intervals: bytearray


class CaptureAssembler:
    """Validate BEGIN/DATA/END records and atomically save complete captures."""

    def __init__(
        self, output_dir: str, node: str = "node", persist: bool = True
    ):
        self.output_dir = Path(output_dir)
        self.persist = persist
        self.node = "".join(
            character if character.isalnum() or character in "-_" else "_"
            for character in node
        )
        self.current: Optional[_Capture] = None
        self.errors = 0

    def feed(self, payload: bytes) -> Optional[CaptureResult]:
        try:
            return self._feed(payload)
        except (OSError, ValueError):
            self.errors += 1
            self.current = None
            raise

    def _feed(self, payload: bytes) -> Optional[CaptureResult]:
        if (
            len(payload) < 6
            or payload[:4] != MAGIC
            or payload[4] not in SUPPORTED_VERSIONS
        ):
            raise ValueError("invalid capture record prefix")
        kind = payload[5]
        if kind == KIND_BEGIN:
            metadata = parse_begin(payload)
            if metadata["clipped"]:
                raise ValueError("refusing clipped RX capture")
            self.current = _Capture(payload, metadata, bytearray())
            return None
        if self.current is None:
            raise ValueError("capture DATA/END without BEGIN")
        if payload[4] != self.current.metadata["capture_version"]:
            raise ValueError("capture protocol version changed before END")

        capture_id = struct.unpack_from(">I", payload, 8)[0] if len(payload) >= 12 else -1
        if capture_id != self.current.metadata["capture_id"]:
            raise ValueError("capture id changed before END")

        if kind == KIND_DATA:
            if len(payload) < DATA_HEADER_LEN:
                raise ValueError("short capture DATA record")
            count = struct.unpack_from(">H", payload, 6)[0]
            offset = struct.unpack_from(">I", payload, 12)[0]
            if len(payload) != DATA_HEADER_LEN + 2 * count:
                raise ValueError("capture DATA length mismatch")
            if offset != len(self.current.intervals) // 2:
                raise ValueError("non-contiguous capture DATA record")
            if offset + count > self.current.metadata["interval_count"]:
                raise ValueError("capture DATA exceeds declared size")
            self.current.intervals.extend(payload[DATA_HEADER_LEN:])
            return None

        if kind != KIND_END or len(payload) != END_LEN:
            raise ValueError("invalid capture record kind or END length")
        flags = payload[6]
        total, end_hash = struct.unpack_from(">II", payload, 12)
        raw = bytes(self.current.intervals)
        metadata = self.current.metadata
        if flags & 1:
            raise ValueError("refusing clipped RX capture")
        end_is_raw = bool(payload[4] >= 2 and flags & 2)
        if end_is_raw != (metadata["input_domain"] == "raw_pre_filter"):
            raise ValueError("RX capture domain changed before END")
        if total != metadata["interval_count"] or len(raw) != total * 2:
            raise ValueError("incomplete RX capture")
        actual_hash = fnv1a32(raw)
        if actual_hash != end_hash or actual_hash != metadata["hash32"]:
            raise ValueError("RX capture hash mismatch")

        result = self._persist(self.current.begin, raw, metadata)
        self.current = None
        return result

    def _persist(
        self, begin: bytes, raw_intervals: bytes, metadata: dict
    ) -> CaptureResult:
        self.output_dir.mkdir(parents=True, exist_ok=True)
        now = datetime.now(timezone.utc)
        stamp = now.strftime("%Y%m%dT%H%M%S.%fZ")
        base = (
            f"{self.node}-{stamp}-id{metadata['capture_id']:08x}-"
            f"{metadata['trigger']}"
        )
        binary_path = self.output_dir / f"{base}.bin"
        metadata_path = self.output_dir / f"{base}.json"
        document = dict(metadata)
        document.update(
            {
                "schema": f"openvlc-rx-capture-v{metadata['capture_version']}",
                "node": self.node,
                "saved_utc": now.isoformat(),
                "binary_file": binary_path.name,
                "binary_layout": "88-byte OVCT BEGIN followed by big-endian uint16 intervals",
            }
        )
        if not self.persist:
            return CaptureResult(None, None, document)
        self._atomic_write(binary_path, begin + raw_intervals)
        try:
            self._atomic_write(
                metadata_path,
                (json.dumps(document, indent=2, sort_keys=True) + "\n").encode(),
            )
        except OSError:
            binary_path.unlink(missing_ok=True)
            raise
        return CaptureResult(binary_path, metadata_path, document)

    @staticmethod
    def _atomic_write(path: Path, data: bytes) -> None:
        descriptor, temporary = tempfile.mkstemp(
            prefix=f".{path.name}.", dir=str(path.parent)
        )
        try:
            with os.fdopen(descriptor, "wb") as handle:
                handle.write(data)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, path)
        except BaseException:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
            raise
