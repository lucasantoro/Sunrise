#!/usr/bin/env python3
"""Send valid host frames directly to STM32 TX, bypassing TUN and routing."""

import argparse
import glob
import os
import sys
import time

from vlc_host_protocol import TYPE_IP, encode_frame


def resolve_serial_port(port: str) -> str:
    if port != "auto":
        return port
    by_id = sorted(glob.glob("/dev/serial/by-id/*"))
    preferred = [
        item
        for item in by_id
        if any(
            marker in os.path.basename(item).lower()
            for marker in ("stlink", "st-link", "stmicro", "stm32")
        )
    ]
    candidates = preferred or by_id or sorted(glob.glob("/dev/ttyACM*"))
    if not candidates:
        raise FileNotFoundError("no STM32 serial device found")
    return candidates[0]


def test_ipv4_packet(sequence: int) -> bytes:
    # Minimal valid IPv4 datagram. The STM32 validates version, IHL and length.
    packet = bytearray(64)
    packet[0] = 0x45
    packet[2] = 0
    packet[3] = len(packet)
    packet[8] = 64
    packet[9] = 17
    packet[12:16] = bytes((192, 168, 0, 1))
    packet[16:20] = bytes((192, 168, 0, 2))
    packet[20:] = bytes(((sequence + index) & 0xFF for index in range(44)))
    return bytes(packet)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="auto")
    parser.add_argument("--baud", type=int, default=2_000_000)
    parser.add_argument("--frames", type=int, default=32)
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        sys.stderr.write("pyserial is required: sudo apt install python3-serial\n")
        return 2

    path = resolve_serial_port(args.port)
    sys.stderr.write(
        f"[serial-test] opening {path} at {args.baud} baud; "
        "stop openvlc-tx-stm32 first\n"
    )
    with serial.Serial(
        path,
        args.baud,
        timeout=0.1,
        write_timeout=1.0,
        rtscts=False,
        dsrdtr=False,
        exclusive=True,
    ) as port:
        port.reset_input_buffer()
        for sequence in range(args.frames):
            frame = encode_frame(
                TYPE_IP, test_ipv4_packet(sequence), sequence
            )
            written = port.write(frame)
            if written != len(frame):
                raise RuntimeError(
                    f"partial serial write: {written}/{len(frame)}"
                )
            time.sleep(0.01)

        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            data = port.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()

    sys.stderr.write(
        f"[serial-test] sent {args.frames} valid host frames\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
