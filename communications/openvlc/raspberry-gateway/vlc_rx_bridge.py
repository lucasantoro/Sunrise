#!/usr/bin/env python3
"""Bridge decoded STM32 OpenVLC IP datagrams into a Linux TUN interface."""

import argparse
import fcntl
import glob
import os
import signal
import struct
import subprocess
import sys
import time
from typing import Optional

from vlc_host_protocol import (
    HostFrameParser,
    TYPE_IP,
    TYPE_LOG,
    validate_ip_packet,
)

TUNSETIFF = 0x400454CA
IFF_TUN = 0x0001
IFF_NO_PI = 0x1000


def open_tun(device: str) -> int:
    fd = os.open("/dev/net/tun", os.O_RDWR)
    request = struct.pack("16sH", device.encode("ascii"), IFF_TUN | IFF_NO_PI)
    fcntl.ioctl(fd, TUNSETIFF, request)
    return fd


def configure_tun(
    device: str, cidr: str, source_route: str, mtu: int
) -> None:
    subprocess.run(
        ["ip", "link", "set", "dev", device, "mtu", str(mtu)],
        check=True,
    )
    subprocess.run(["ip", "link", "set", "dev", device, "up"], check=True)
    subprocess.run(
        ["ip", "address", "replace", cidr, "dev", device], check=True
    )
    if source_route:
        subprocess.run(
            ["ip", "route", "replace", source_route, "dev", device],
            check=True,
        )


def resolve_serial_port(port: str) -> str:
    if port != "auto":
        return port

    by_id = sorted(glob.glob("/dev/serial/by-id/*"))
    preferred = [
        candidate
        for candidate in by_id
        if any(
            marker in os.path.basename(candidate).lower()
            for marker in ("stlink", "st-link", "stmicro", "stm32")
        )
    ]
    candidates = preferred or by_id or sorted(glob.glob("/dev/ttyACM*"))
    if not candidates:
        raise FileNotFoundError(
            "no STM32 serial device found under /dev/serial/by-id or /dev/ttyACM*"
        )
    return candidates[0]


def print_stats(
    parser: HostFrameParser,
    ip_frames: int,
    ip_bytes: int,
    log_frames: int,
    invalid_ip: int,
    sequence_gaps: int,
    sequence_resets: int,
    tun_errors: int,
    started_at: float,
    interval_started_at: float,
    previous_ip_frames: int,
    previous_ip_bytes: int,
    previous_sequence_gaps: int,
) -> None:
    now = time.monotonic()
    elapsed = max(now - started_at, 0.001)
    interval = max(now - interval_started_at, 0.001)
    stats = parser.stats
    rate_kbps = (ip_bytes * 8.0) / elapsed / 1000.0
    interval_rate_kbps = (
        (ip_bytes - previous_ip_bytes) * 8.0 / interval / 1000.0
    )
    interval_fps = (ip_frames - previous_ip_frames) / interval
    interval_gaps = sequence_gaps - previous_sequence_gaps
    sys.stderr.write(
        "[bridge] "
        f"rate={interval_rate_kbps:.1f}kbps fps={interval_fps:.1f} "
        f"gap={interval_gaps} total_ip={ip_frames} avg={rate_kbps:.1f}kbps "
        f"log={log_frames} seq_gap={sequence_gaps} "
        f"seq_reset={sequence_resets} invalid_ip={invalid_ip} "
        f"tunerr={tun_errors} "
        f"crc={stats.crc_errors} length={stats.length_errors} "
        f"header={stats.header_errors} discarded={stats.discarded_bytes}\n"
    )


def main() -> int:
    parser_cli = argparse.ArgumentParser(
        description="STM32 OpenVLC receiver to Linux TUN bridge"
    )
    parser_cli.add_argument("--port", default="/dev/ttyACM0")
    parser_cli.add_argument("--baud", type=int, default=2_000_000)
    parser_cli.add_argument("--dev", default="tun0")
    parser_cli.add_argument("--ip", default="192.168.0.2/24")
    parser_cli.add_argument("--mtu", type=int, default=900)
    parser_cli.add_argument(
        "--source-route",
        default="10.0.0.0/24",
        help="route for source addresses behind the BBB; empty disables it",
    )
    parser_cli.add_argument("--max-payload", type=int, default=900)
    parser_cli.add_argument("--stats-interval", type=float, default=5.0)
    parser_cli.add_argument("--quiet", action="store_true")
    parser_cli.add_argument("--no-configure", action="store_true")
    parser_cli.add_argument(
        "--accept-legacy",
        action="store_true",
        help="also accept the old A5 5A/type/length/payload/CRC format",
    )
    args = parser_cli.parse_args()

    try:
        import serial
    except ImportError:
        sys.stderr.write("pyserial is required: sudo apt install python3-serial\n")
        return 2

    serial_path = resolve_serial_port(args.port)
    tun = open_tun(args.dev)
    if not args.no_configure:
        configure_tun(args.dev, args.ip, args.source_route, args.mtu)
    serial_port = serial.Serial(
        serial_path,
        args.baud,
        timeout=0.05,
        rtscts=False,
        dsrdtr=False,
    )
    frame_parser = HostFrameParser(args.max_payload, args.accept_legacy)
    stopped = False

    def stop_handler(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)

    ip_frames = 0
    ip_bytes = 0
    log_frames = 0
    invalid_ip = 0
    sequence_gaps = 0
    sequence_resets = 0
    tun_errors = 0
    expected_sequence: Optional[int] = None
    started_at = time.monotonic()
    interval_started_at = started_at
    previous_ip_frames = 0
    previous_ip_bytes = 0
    previous_sequence_gaps = 0
    next_stats = started_at + args.stats_interval
    sys.stderr.write(
        f"[bridge] {serial_path}@{args.baud} -> {args.dev} {args.ip} mtu={args.mtu}\n"
    )

    try:
        while not stopped:
            waiting = serial_port.in_waiting
            chunk = serial_port.read(min(max(waiting, 1), 16384))
            for frame in frame_parser.feed(chunk):
                if frame.sequence is not None:
                    if (
                        expected_sequence is not None
                        and frame.sequence != expected_sequence
                    ):
                        delta = (frame.sequence - expected_sequence) & 0xFFFF
                        if delta < 0x8000:
                            sequence_gaps += delta
                        else:
                            sequence_resets += 1
                    expected_sequence = (frame.sequence + 1) & 0xFFFF

                if frame.frame_type == TYPE_IP:
                    packet = validate_ip_packet(frame.payload)
                    if packet is None:
                        invalid_ip += 1
                        continue
                    try:
                        written = os.write(tun, packet)
                    except OSError:
                        tun_errors += 1
                        continue
                    if written != len(packet):
                        tun_errors += 1
                        continue
                    ip_frames += 1
                    ip_bytes += len(packet)
                elif frame.frame_type == TYPE_LOG:
                    log_frames += 1
                    if not args.quiet:
                        sys.stderr.write(
                            "[stm32] "
                            + frame.payload.decode("utf-8", errors="replace")
                        )

            now = time.monotonic()
            if args.stats_interval > 0 and now >= next_stats:
                print_stats(
                    frame_parser,
                    ip_frames,
                    ip_bytes,
                    log_frames,
                    invalid_ip,
                    sequence_gaps,
                    sequence_resets,
                    tun_errors,
                    started_at,
                    interval_started_at,
                    previous_ip_frames,
                    previous_ip_bytes,
                    previous_sequence_gaps,
                )
                interval_started_at = now
                previous_ip_frames = ip_frames
                previous_ip_bytes = ip_bytes
                previous_sequence_gaps = sequence_gaps
                next_stats = now + args.stats_interval
    finally:
        serial_port.close()
        os.close(tun)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
