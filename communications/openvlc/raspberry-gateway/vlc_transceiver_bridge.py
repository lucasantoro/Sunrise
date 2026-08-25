#!/usr/bin/env python3
"""Bidirectional Linux TUN <-> STM32 OpenVLC transceiver bridge."""

import argparse
import pathlib
import socket
import fcntl
import ipaddress
import os
import select
import signal
import subprocess
import sys
import time
from collections import deque
from typing import Optional

from vlc_host_protocol import (
    HostFrameParser,
    TYPE_CAPTURE,
    TYPE_IP,
    TYPE_LOG,
    encode_frame,
    validate_ip_packet,
)
from vlc_capture import CaptureAssembler
from vlc_pacing import advance_paced_deadline
from vlc_stm32_tx_bridge import configure_tun, open_tun, resolve_serial_port


def packet_destination(packet: bytes):
    version = packet[0] >> 4
    if version == 4:
        return ipaddress.ip_address(packet[16:20])
    return ipaddress.ip_address(packet[24:40])


# Logs and captures live beside this script, so they travel with the
# deployment instead of scattering into /var/log. Resolved from __file__
# rather than the working directory: systemd starts this from /.
LOG_DIR = pathlib.Path(__file__).resolve().parent / "logs"
DEFAULT_CAPTURE_DIR = LOG_DIR / "captures"


def main() -> int:
    cli = argparse.ArgumentParser(
        description="Bidirectional STM32 OpenVLC transceiver bridge"
    )
    cli.add_argument("--port", default="auto")
    cli.add_argument("--baud", type=int, default=2_000_000)
    cli.add_argument("--dev", default="tun0")
    cli.add_argument("--ip", default="192.168.0.2/24")
    cli.add_argument("--peer-ip", default="192.168.0.1")
    cli.add_argument(
        "--source-route",
        default="10.0.0.0/24",
        help="return route for sources behind a legacy BBB; empty disables it",
    )
    cli.add_argument("--mtu", type=int, default=900)
    cli.add_argument("--max-payload", type=int, default=900)
    cli.add_argument("--stats-interval", type=float, default=5.0)
    cli.add_argument(
        "--tx-max-fps",
        type=float,
        default=125.0,
        help=(
            "maximum host records sent to the STM32 per second; "
            "0 disables pacing (default: 125)"
        ),
    )
    cli.add_argument("--quiet-stm32", action="store_true")
    cli.add_argument(
        "--capture-dir",
        default=str(DEFAULT_CAPTURE_DIR),
        help="directory for validated STM32 RX captures",
    )
    cli.add_argument(
        "--capture-node",
        default=socket.gethostname(),
        help="node label in capture filenames (default: this hostname)",
    )
    cli.add_argument(
        "--no-capture-save",
        action="store_true",
        help="validate but do not persist STM32 RX captures",
    )
    cli.add_argument("--no-configure", action="store_true")
    cli.add_argument(
        "--tx-allow",
        action="append",
        default=[],
        metavar="CIDR",
        help="TX destination prefix; repeat as needed (default: peer /32 only)",
    )
    cli.add_argument(
        "--forward-all",
        action="store_true",
        help="forward every TUN egress packet, including kernel feedback",
    )
    args = cli.parse_args()
    if args.tx_max_fps < 0:
        cli.error("--tx-max-fps must be >= 0")

    local_ip = str(ipaddress.ip_interface(args.ip).ip)
    if local_ip == args.peer_ip:
        cli.error("--ip and --peer-ip must identify different nodes")
    tx_allow = [ipaddress.ip_network(value, strict=False) for value in args.tx_allow]
    if not tx_allow:
        peer = ipaddress.ip_address(args.peer_ip)
        tx_allow = [ipaddress.ip_network(f"{peer}/{peer.max_prefixlen}")]

    try:
        import serial
    except ImportError:
        sys.stderr.write("pyserial is required: sudo apt install python3-serial\n")
        return 2

    serial_path = resolve_serial_port(args.port)
    tun = open_tun(args.dev)
    if not args.no_configure:
        configure_tun(args.dev, args.ip, args.peer_ip, args.mtu)
        if args.source_route:
            subprocess.run(
                [
                    "ip",
                    "route",
                    "replace",
                    args.source_route,
                    "dev",
                    args.dev,
                ],
                check=True,
            )
    port = serial.Serial(
        serial_path,
        args.baud,
        timeout=0,
        write_timeout=0,
        rtscts=False,
        dsrdtr=False,
    )
    # A blocking serial write used to stall the WHOLE pump (both directions)
    # for up to 1 s whenever the ST-Link VCP back-pressured under bidirectional
    # load - the STM32 TX queue then drained and the LED gapped. The fd is now
    # non-blocking and downlink frames wait in a bounded backlog drained via
    # select's write set, so uplink congestion can never freeze the downlink.
    port_fd = port.fileno()
    fcntl.fcntl(port_fd, fcntl.F_SETFL,
                fcntl.fcntl(port_fd, fcntl.F_GETFL) | os.O_NONBLOCK)
    serial_backlog: deque = deque()
    # ~0.5 s of downlink at 125 fps; beyond this the serial link is genuinely
    # saturated and dropping (UDP semantics) beats unbounded latency.
    serial_backlog_max = 64
    serial_interval_ns = (
        int(1_000_000_000 / args.tx_max_fps)
        if args.tx_max_fps > 0
        else 0
    )
    next_serial_frame_ns = 0
    serial_frame_in_progress = False
    parser = HostFrameParser(args.max_payload)
    capture_assembler = CaptureAssembler(
        args.capture_dir,
        args.capture_node,
        persist=not args.no_capture_save,
    )
    stopped = False

    def stop_handler(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)

    tx_sequence = 0
    expected_rx_sequence: Optional[int] = None
    tx_frames = tx_bytes = tx_drops = tx_filtered = 0
    serial_frames_completed = 0
    rx_frames = rx_bytes = rx_invalid = rx_gaps = rx_resets = 0
    log_frames = capture_frames = captures_saved = capture_errors = 0
    serial_errors = tun_errors = 0
    previous = (0, 0, 0, 0, 0)
    interval_start = time.monotonic()
    next_stats = interval_start + args.stats_interval

    sys.stderr.write(
        f"[trx-bridge] {args.dev} {args.ip} <-> "
        f"{serial_path}@{args.baud}, peer={args.peer_ip}, mtu={args.mtu}, "
        f"source_route={args.source_route or 'off'}, "
        f"tx_allow={'all' if args.forward_all else ','.join(map(str, tx_allow))}, "
        f"tx_max_fps={args.tx_max_fps:g}\n"
    )

    try:
        while not stopped:
            now_ns = time.monotonic_ns()
            serial_due = (
                serial_frame_in_progress
                or serial_interval_ns == 0
                or now_ns >= next_serial_frame_ns
            )
            wait_write = [port_fd] if serial_backlog and serial_due else []
            select_timeout = 0.05
            if serial_backlog and not serial_due:
                select_timeout = min(
                    select_timeout,
                    max((next_serial_frame_ns - now_ns) / 1_000_000_000, 0),
                )
            readable, writable, _ = select.select(
                [tun, port_fd], wait_write, [], select_timeout
            )

            if writable:
                # Finish one framed record without blocking. When pacing is
                # active, advance an absolute deadline so ordinary Linux
                # wake-up jitter does not accumulate. Re-anchor only after a
                # complete missed slot, which also prevents catch-up bursts.
                while serial_backlog:
                    chunk = serial_backlog[0]
                    try:
                        written = os.write(port_fd, chunk)
                    except BlockingIOError:
                        break
                    except OSError:
                        serial_errors += 1
                        serial_backlog.popleft()
                        serial_frame_in_progress = False
                        if serial_interval_ns:
                            next_serial_frame_ns = advance_paced_deadline(
                                next_serial_frame_ns,
                                time.monotonic_ns(),
                                serial_interval_ns,
                            )
                            break
                        continue
                    if written < len(chunk):
                        serial_backlog[0] = chunk[written:]
                        serial_frame_in_progress = True
                        break
                    serial_backlog.popleft()
                    serial_frames_completed += 1
                    serial_frame_in_progress = False
                    if serial_interval_ns:
                        next_serial_frame_ns = advance_paced_deadline(
                            next_serial_frame_ns,
                            time.monotonic_ns(),
                            serial_interval_ns,
                        )
                        break

            if tun in readable:
                # Drain several packets per round so a busy uplink burst does
                # not ration the downlink to one packet per select cycle.
                for _ in range(8):
                    try:
                        packet = os.read(tun, args.mtu + 128)
                    except BlockingIOError:
                        break
                    ip_packet = validate_ip_packet(packet)
                    if ip_packet is None or len(ip_packet) > args.max_payload:
                        tx_drops += 1
                    elif not args.forward_all and not any(
                        packet_destination(ip_packet) in network
                        for network in tx_allow
                    ):
                        tx_filtered += 1
                    elif len(serial_backlog) >= serial_backlog_max:
                        tx_drops += 1
                    else:
                        serial_backlog.append(
                            encode_frame(TYPE_IP, ip_packet, tx_sequence)
                        )
                        tx_frames += 1
                        tx_bytes += len(ip_packet)
                        tx_sequence = (tx_sequence + 1) & 0xFFFF

            if port_fd in readable:
                try:
                    data = os.read(port.fileno(), 16384)
                except BlockingIOError:
                    data = b""
                for frame in parser.feed(data):
                    if frame.sequence is not None:
                        if (
                            expected_rx_sequence is not None
                            and frame.sequence != expected_rx_sequence
                        ):
                            delta = (
                                frame.sequence - expected_rx_sequence
                            ) & 0xFFFF
                            if delta < 0x8000:
                                rx_gaps += delta
                            else:
                                rx_resets += 1
                        expected_rx_sequence = (frame.sequence + 1) & 0xFFFF

                    if frame.frame_type == TYPE_IP:
                        packet = validate_ip_packet(frame.payload)
                        if packet is None:
                            rx_invalid += 1
                            continue
                        try:
                            written = os.write(tun, packet)
                        except OSError:
                            tun_errors += 1
                            continue
                        if written != len(packet):
                            tun_errors += 1
                            continue
                        rx_frames += 1
                        rx_bytes += len(packet)
                    elif frame.frame_type == TYPE_LOG:
                        log_frames += 1
                        if not args.quiet_stm32:
                            sys.stderr.write(
                                "[stm32] "
                                + frame.payload.decode(
                                    "utf-8", errors="replace"
                                )
                            )
                    elif frame.frame_type == TYPE_CAPTURE:
                        capture_frames += 1
                        try:
                            result = capture_assembler.feed(frame.payload)
                        except (OSError, ValueError) as error:
                            capture_errors += 1
                            # A malformed capture can otherwise flood the
                            # journal once per 20-ms chunk. Log the first four
                            # events and then powers of two, including enough
                            # wire evidence to distinguish a stale protocol
                            # from an STM32 payload construction defect.
                            if (
                                capture_errors <= 4
                                or capture_errors
                                & (capture_errors - 1)
                                == 0
                            ):
                                prefix = frame.payload[:16].hex(" ")
                                sys.stderr.write(
                                    "[rx-capture] rejected "
                                    f"count={capture_errors} "
                                    f"len={len(frame.payload)} "
                                    f"head={prefix}: {error}\n"
                                )
                        else:
                            if result is not None:
                                if args.no_capture_save:
                                    sys.stderr.write(
                                        "[rx-capture] complete (not saved)\n"
                                    )
                                else:
                                    captures_saved += 1
                                    sys.stderr.write(
                                        f"[rx-capture] saved {result.binary_path}\n"
                                    )
                # Unframed printable text = the firmware's raw fault dump
                # (*** HARDFAULT *** CFSR=...). Always surface it, even with
                # --quiet-stm32: it is the only evidence of a crash.
                for raw_line in parser.drain_raw_lines():
                    sys.stderr.write("[stm32-raw] " + raw_line + "\n")

            now = time.monotonic()
            if args.stats_interval > 0 and now >= next_stats:
                elapsed = max(now - interval_start, 0.001)
                (
                    prev_tx_frames,
                    prev_tx_bytes,
                    prev_serial_frames,
                    prev_rx_frames,
                    prev_rx_bytes,
                ) = previous
                sys.stderr.write(
                    "[trx-bridge] "
                    f"tx={((tx_bytes-prev_tx_bytes)*8/elapsed/1000):.1f}kbps/"
                    f"{((tx_frames-prev_tx_frames)/elapsed):.1f}fps "
                    f"wiretx={((serial_frames_completed-prev_serial_frames)/elapsed):.1f}fps "
                    f"rx={((rx_bytes-prev_rx_bytes)*8/elapsed/1000):.1f}kbps/"
                    f"{((rx_frames-prev_rx_frames)/elapsed):.1f}fps "
                    f"total={tx_frames}/{rx_frames} "
                    f"drop={tx_drops} filtered={tx_filtered} "
                    f"gap={rx_gaps} reset={rx_resets} "
                    f"invalid={rx_invalid} serialerr={serial_errors} "
                    f"backlog={len(serial_backlog)} "
                    f"pace={args.tx_max_fps:g}fps "
                    f"tunerr={tun_errors} log={log_frames} "
                    f"cap={capture_frames}/{captures_saved}/{capture_errors} "
                    f"crc={parser.stats.crc_errors} "
                    f"header={parser.stats.header_errors}\n"
                )
                previous = (
                    tx_frames,
                    tx_bytes,
                    serial_frames_completed,
                    rx_frames,
                    rx_bytes,
                )
                interval_start = now
                next_stats = now + args.stats_interval
    finally:
        port.close()
        os.close(tun)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
