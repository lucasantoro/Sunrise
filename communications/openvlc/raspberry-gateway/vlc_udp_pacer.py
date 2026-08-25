#!/usr/bin/env python3
"""Normalize an MPEG-TS UDP stream into fixed, evenly paced datagrams."""

from __future__ import annotations

import argparse
import signal
import socket
import sys
import time
from typing import Iterator


class FixedPacketizer:
    def __init__(self, packet_size: int):
        if packet_size <= 0 or packet_size % 188:
            raise ValueError("packet size must be a positive multiple of 188")
        self.packet_size = packet_size
        self.buffer = bytearray()

    def feed(self, data: bytes) -> Iterator[bytes]:
        self.buffer.extend(data)
        while len(self.buffer) >= self.packet_size:
            packet = bytes(self.buffer[: self.packet_size])
            del self.buffer[: self.packet_size]
            yield packet


class DatagramPacer:
    def __init__(self, rate_bps: int, packet_size: int):
        if rate_bps <= 0:
            raise ValueError("rate must be positive")
        self.interval_ns = packet_size * 8 * 1_000_000_000 // rate_bps
        self.next_send_ns = 0

    def wait(self) -> None:
        now = time.monotonic_ns()
        if self.next_send_ns > now:
            time.sleep((self.next_send_ns - now) / 1_000_000_000)
            now = time.monotonic_ns()
        # Never catch up by emitting a burst after scheduler latency.
        base = max(self.next_send_ns, now)
        self.next_send_ns = base + self.interval_ns


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--destination", required=True)
    parser.add_argument("--destination-port", type=int, required=True)
    parser.add_argument("--rate", type=int, required=True, help="TS payload bits/s")
    parser.add_argument("--packet-size", type=int, default=752)
    parser.add_argument("--stats-interval", type=float, default=5.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    packetizer = FixedPacketizer(args.packet_size)
    pacer = DatagramPacer(args.rate, args.packet_size)
    source = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    source.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    source.bind((args.bind, args.listen_port))
    source.settimeout(0.2)
    target = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    stopped = False

    def stop_handler(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)

    received_datagrams = 0
    received_bytes = 0
    sent_datagrams = 0
    sent_bytes = 0
    next_stats = time.monotonic() + args.stats_interval
    sys.stderr.write(
        f"[pacer] {args.bind}:{args.listen_port} -> "
        f"{args.destination}:{args.destination_port} rate={args.rate} "
        f"packet={args.packet_size}\n"
    )

    try:
        while not stopped:
            try:
                data, _peer = source.recvfrom(65535)
            except socket.timeout:
                data = b""
            if data:
                received_datagrams += 1
                received_bytes += len(data)
                for packet in packetizer.feed(data):
                    pacer.wait()
                    target.sendto(
                        packet, (args.destination, args.destination_port)
                    )
                    sent_datagrams += 1
                    sent_bytes += len(packet)

            now = time.monotonic()
            if args.stats_interval > 0 and now >= next_stats:
                sys.stderr.write(
                    f"[pacer] in={received_datagrams}/{received_bytes} "
                    f"out={sent_datagrams}/{sent_bytes} "
                    f"pending={len(packetizer.buffer)}\n"
                )
                next_stats = now + args.stats_interval
    finally:
        source.close()
        target.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
