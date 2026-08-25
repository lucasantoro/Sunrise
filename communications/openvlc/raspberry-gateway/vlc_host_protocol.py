#!/usr/bin/env python3
"""Parser for the STM32-to-companion OpenVLC serial protocol."""

from dataclasses import dataclass
from typing import List, Optional

MAGIC = b"\xA5\x5A\xC3"
LEGACY_MAGIC = b"\xA5\x5A"
VERSION = 1
TYPE_IP = 0x01
TYPE_LOG = 0x02
TYPE_CAPTURE = 0x03
VALID_TYPES = (TYPE_IP, TYPE_LOG, TYPE_CAPTURE)


def _crc16_build_table() -> tuple:
    table = []
    for byte in range(256):
        crc = byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
        table.append(crc)
    return tuple(table)


_CRC16_TABLE = _crc16_build_table()


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: init 0xffff, polynomial 0x1021.

    Table-driven: the previous per-bit loop cost ~10 us/byte in CPython, which
    at full-duplex frame rates (~210 kB/s of encode+parse) saturated a whole Pi
    core inside the single-threaded bridge and caused burst RX loss whenever
    the TX direction was also active. One lookup per byte is ~15x faster.
    """
    crc = 0xFFFF
    table = _CRC16_TABLE
    for byte in data:
        crc = ((crc << 8) & 0xFFFF) ^ table[(crc >> 8) ^ byte]
    return crc


@dataclass(frozen=True)
class HostFrame:
    frame_type: int
    payload: bytes
    sequence: Optional[int]
    legacy: bool = False


@dataclass
class ParserStats:
    frames: int = 0
    crc_errors: int = 0
    length_errors: int = 0
    header_errors: int = 0
    discarded_bytes: int = 0
    legacy_frames: int = 0


class HostFrameParser:
    """Incremental, self-synchronizing parser for arbitrary serial chunks."""

    # Unframed bytes are normally line noise, but the firmware's fault handler
    # prints its HardFault dump as RAW text (no host framing) — silently
    # discarding it hides the only evidence of a crash. Keep a bounded copy of
    # discarded bytes so the bridge can surface printable lines.
    RAW_TEXT_MAX = 8192

    def __init__(self, max_payload: int = 900, accept_legacy: bool = False):
        if max_payload <= 0 or max_payload > 0xFFFF:
            raise ValueError("max_payload must be between 1 and 65535")
        self.max_payload = max_payload
        self.accept_legacy = accept_legacy
        self.buffer = bytearray()
        self.stats = ParserStats()
        self._raw = bytearray()

    def _capture_raw(self, chunk) -> None:
        room = self.RAW_TEXT_MAX - len(self._raw)
        if room > 0:
            self._raw.extend(chunk[:room])

    def drain_raw_lines(self) -> List[str]:
        """Complete printable text lines seen between frames (fault dumps)."""
        if b"\n" not in self._raw:
            return []
        *complete, tail = self._raw.split(b"\n")
        self._raw = bytearray(tail[-1024:])
        lines: List[str] = []
        for raw in complete:
            text = raw.replace(b"\r", b"")
            printable = bytes(c for c in text if 32 <= c <= 126 or c == 9)
            if len(printable) >= 4 and len(printable) * 10 >= len(text) * 7:
                lines.append(printable.decode("ascii", "replace"))
        return lines

    def _discard_before_magic(self) -> bool:
        index = self.buffer.find(LEGACY_MAGIC)
        if index >= 0:
            if index:
                self.stats.discarded_bytes += index
                self._capture_raw(self.buffer[:index])
                del self.buffer[:index]
            return True

        keep = 1 if self.buffer.endswith(LEGACY_MAGIC[:1]) else 0
        discard = len(self.buffer) - keep
        if discard:
            self.stats.discarded_bytes += discard
            self._capture_raw(self.buffer[:discard])
            del self.buffer[:discard]
        return False

    def feed(self, data: bytes) -> List[HostFrame]:
        if data:
            self.buffer.extend(data)
        frames: List[HostFrame] = []

        while True:
            if not self._discard_before_magic():
                break
            if len(self.buffer) < 3:
                break

            if self.buffer.startswith(MAGIC):
                if len(self.buffer) < 9:
                    break
                version = self.buffer[3]
                frame_type = self.buffer[4]
                sequence = (self.buffer[5] << 8) | self.buffer[6]
                length = (self.buffer[7] << 8) | self.buffer[8]
                if version != VERSION or frame_type not in VALID_TYPES:
                    self.stats.header_errors += 1
                    del self.buffer[0]
                    continue
                if length > self.max_payload:
                    self.stats.length_errors += 1
                    del self.buffer[0]
                    continue
                total = 11 + length
                if len(self.buffer) < total:
                    break
                payload_end = 9 + length
                crc_received = (
                    (self.buffer[payload_end] << 8)
                    | self.buffer[payload_end + 1]
                )
                crc_expected = crc16_ccitt(bytes(self.buffer[3:payload_end]))
                if crc_received != crc_expected:
                    self.stats.crc_errors += 1
                    del self.buffer[0]
                    continue
                payload = bytes(self.buffer[9:payload_end])
                del self.buffer[:total]
                frames.append(HostFrame(frame_type, payload, sequence))
                self.stats.frames += 1
                continue

            if not self.accept_legacy:
                self.stats.header_errors += 1
                del self.buffer[0]
                continue
            if len(self.buffer) < 5:
                break
            frame_type = self.buffer[2]
            length = (self.buffer[3] << 8) | self.buffer[4]
            if frame_type not in VALID_TYPES:
                self.stats.header_errors += 1
                del self.buffer[0]
                continue
            if length > self.max_payload:
                self.stats.length_errors += 1
                del self.buffer[0]
                continue
            total = 7 + length
            if len(self.buffer) < total:
                break
            payload_end = 5 + length
            crc_received = (
                (self.buffer[payload_end] << 8)
                | self.buffer[payload_end + 1]
            )
            payload = bytes(self.buffer[5:payload_end])
            if crc16_ccitt(payload) != crc_received:
                self.stats.crc_errors += 1
                del self.buffer[0]
                continue
            del self.buffer[:total]
            frames.append(HostFrame(frame_type, payload, None, legacy=True))
            self.stats.frames += 1
            self.stats.legacy_frames += 1

        return frames


def encode_frame(
    frame_type: int, payload: bytes, sequence: int, version: int = VERSION
) -> bytes:
    """Test/helper encoder matching the STM32 wire format."""
    if frame_type not in VALID_TYPES:
        raise ValueError("invalid frame type")
    if len(payload) > 0xFFFF:
        raise ValueError("payload too large")
    body = bytes(
        (
            version,
            frame_type,
            (sequence >> 8) & 0xFF,
            sequence & 0xFF,
            (len(payload) >> 8) & 0xFF,
            len(payload) & 0xFF,
        )
    ) + payload
    crc = crc16_ccitt(body)
    return MAGIC + body + bytes((crc >> 8, crc & 0xFF))


def validate_ip_packet(payload: bytes) -> Optional[bytes]:
    """Return an exact IPv4/IPv6 datagram, or None for malformed input."""
    if not payload:
        return None
    version = payload[0] >> 4
    if version == 4:
        if len(payload) < 20:
            return None
        header_length = (payload[0] & 0x0F) * 4
        total_length = (payload[2] << 8) | payload[3]
        if header_length < 20 or total_length < header_length:
            return None
        if total_length > len(payload):
            return None
        return payload[:total_length]
    if version == 6:
        if len(payload) < 40:
            return None
        total_length = 40 + ((payload[4] << 8) | payload[5])
        if total_length > len(payload):
            return None
        return payload[:total_length]
    return None
