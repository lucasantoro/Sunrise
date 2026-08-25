#!/usr/bin/env bash
# One-shot setup helper for a Raspberry Pi directly connected to STM32 TX.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SERIAL_PORT=${OPENVLC_SERIAL_PORT:-auto}
SERIAL_BAUD=${OPENVLC_SERIAL_BAUD:-2000000}
TUN_DEVICE=${OPENVLC_TUN_DEVICE:-tun0}
TUN_CIDR=${OPENVLC_TUN_CIDR:-192.168.0.1/24}
PEER_IP=${OPENVLC_PEER_IP:-192.168.0.2}
VLC_MTU=${OPENVLC_MTU:-900}
STATS_INTERVAL=${OPENVLC_STATS_INTERVAL:-5}

if [ "$(id -u)" -ne 0 ]; then
    exec sudo --preserve-env=OPENVLC_SERIAL_PORT,OPENVLC_SERIAL_BAUD,OPENVLC_TUN_DEVICE,OPENVLC_TUN_CIDR,OPENVLC_PEER_IP,OPENVLC_MTU,OPENVLC_STATS_INTERVAL \
        bash "$0" "$@"
fi

modprobe tun || true
python3 "$ROOT_DIR/vlc_stm32_tx_bridge.py" \
    --port "$SERIAL_PORT" \
    --baud "$SERIAL_BAUD" \
    --dev "$TUN_DEVICE" \
    --ip "$TUN_CIDR" \
    --peer-ip "$PEER_IP" \
    --mtu "$VLC_MTU" \
    --stats-interval "$STATS_INTERVAL"
