#!/usr/bin/env python3
"""Bridge Linux IP datagrams from a TUN interface into an STM32 OpenVLC TX."""

import argparse
import fcntl
import glob
import ipaddress
import os
import select
import signal
import struct
import subprocess
import sys
import time

from vlc_host_protocol import TYPE_IP, encode_frame, validate_ip_packet

TUNSETIFF = 0x400454CA
IFF_TUN = 0x0001
IFF_NO_PI = 0x1000


def open_tun(device: str) -> int:
    fd = os.open("/dev/net/tun", os.O_RDWR | os.O_NONBLOCK)
    request = struct.pack("16sH", device.encode("ascii"), IFF_TUN | IFF_NO_PI)
    fcntl.ioctl(fd, TUNSETIFF, request)
    return fd


def configure_tun(device: str, cidr: str, peer_ip: str, mtu: int) -> None:
    local_ip = str(ipaddress.ip_interface(cidr).ip)
    subprocess.run(["ip", "link", "set", "dev", device, "mtu", str(mtu)], check=True)
    subprocess.run(["ip", "address", "replace", cidr, "dev", device], check=True)
    subprocess.run(["ip", "link", "set", "dev", device, "up"], check=True)
    # A legacy BBB setup may have left a more-specific /32 route through eth0.
    # Replace it so traffic cannot bypass this TUN bridge.
    subprocess.run(
        [
            "ip",
            "route",
            "replace",
            f"{peer_ip}/32",
            "dev",
            device,
            "src",
            local_ip,
            "mtu",
            str(mtu),
        ],
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
    frames: int,
    bytes_sent: int,
    drops: int,
    serial_errors: int,
    stm32_log_bytes: int,
    started_at: float,
    interval_started_at: float,
    previous_frames: int,
    previous_bytes: int,
) -> None:
    now = time.monotonic()
    elapsed = max(now - started_at, 0.001)
    interval = max(now - interval_started_at, 0.001)
    rate_kbps = (bytes_sent * 8.0) / elapsed / 1000.0
    interval_rate_kbps = ((bytes_sent - previous_bytes) * 8.0) / interval / 1000.0
    interval_fps = (frames - previous_frames) / interval
    sys.stderr.write(
        "[tx-bridge] "
        f"rate={interval_rate_kbps:.1f}kbps fps={interval_fps:.1f} "
        f"total={frames} avg={rate_kbps:.1f}kbps "
        f"drop={drops} serialerr={serial_errors} stm32log={stm32_log_bytes}\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Linux TUN to STM32 OpenVLC transmitter bridge"
    )
    parser.add_argument("--port", default="auto")
    parser.add_argument("--baud", type=int, default=2_000_000)
    parser.add_argument("--dev", default="tun0")
    parser.add_argument("--ip", default="192.168.0.1/24")
    parser.add_argument("--peer-ip", default="192.168.0.2")
    parser.add_argument("--mtu", type=int, default=900)
    parser.add_argument("--max-payload", type=int, default=900)
    parser.add_argument("--stats-interval", type=float, default=5.0)
    parser.add_argument("--quiet-stm32", action="store_true")
    parser.add_argument("--no-configure", action="store_true")
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        sys.stderr.write("pyserial is required: sudo apt install python3-serial\n")
        return 2

    serial_path = resolve_serial_port(args.port)
    tun = open_tun(args.dev)
    if not args.no_configure:
        configure_tun(args.dev, args.ip, args.peer_ip, args.mtu)

    serial_port = serial.Serial(
        serial_path,
        args.baud,
        timeout=0,
        write_timeout=1.0,
        rtscts=False,
        dsrdtr=False,
    )

    stopped = False

    def stop_handler(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)

    seq = 0
    frames = 0
    bytes_sent = 0
    drops = 0
    serial_errors = 0
    stm32_log_bytes = 0
    started_at = time.monotonic()
    interval_started_at = started_at
    previous_frames = 0
    previous_bytes = 0
    next_stats = started_at + args.stats_interval
    sys.stderr.write(
        f"[tx-bridge] {args.dev} {args.ip} mtu={args.mtu} -> "
        f"{serial_path}@{args.baud}\n"
    )
    if not args.no_configure:
        route = subprocess.run(
            ["ip", "route", "get", args.peer_ip],
            check=True,
            capture_output=True,
            text=True,
        )
        sys.stderr.write(f"[tx-bridge] route: {route.stdout.strip()}\n")

    try:
        while not stopped:
            readable, _, _ = select.select([tun, serial_port.fileno()], [], [], 0.05)
            if tun in readable:
                try:
                    packet = os.read(tun, args.mtu + 128)
                except BlockingIOError:
                    packet = b""
                ip_packet = validate_ip_packet(packet)
                if ip_packet is None or len(ip_packet) > args.max_payload:
                    drops += 1
                else:
                    frame = encode_frame(TYPE_IP, ip_packet, seq)
                    try:
                        written = serial_port.write(frame)
                    except serial.SerialException:
                        serial_errors += 1
                    else:
                        if written != len(frame):
                            serial_errors += 1
                            continue
                        frames += 1
                        bytes_sent += len(ip_packet)
                        seq = (seq + 1) & 0xFFFF

            if serial_port.fileno() in readable:
                try:
                    data = os.read(serial_port.fileno(), 4096)
                except BlockingIOError:
                    data = b""
                if data:
                    stm32_log_bytes += len(data)
                    if not args.quiet_stm32:
                        sys.stderr.write(
                            data.decode("utf-8", errors="replace")
                        )

            now = time.monotonic()
            if args.stats_interval > 0 and now >= next_stats:
                print_stats(
                    frames,
                    bytes_sent,
                    drops,
                    serial_errors,
                    stm32_log_bytes,
                    started_at,
                    interval_started_at,
                    previous_frames,
                    previous_bytes,
                )
                interval_started_at = now
                previous_frames = frames
                previous_bytes = bytes_sent
                next_stats = now + args.stats_interval
    finally:
        serial_port.close()
        os.close(tun)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
