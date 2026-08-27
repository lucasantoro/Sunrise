#!/usr/bin/env bash
# Install the STM32 serial-to-TUN bridge as a Raspberry Pi systemd service.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# These scripts live in install/; everything they read lives one level up.
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)

if [ "$(id -u)" -ne 0 ]; then
    exec sudo bash "$0" "$@"
fi

apt-get update
apt-get install -y python3-serial iproute2 iperf ffmpeg
install -d -m 0755 /opt/openvlc-raspberry
install -m 0755 \
    "$ROOT_DIR/vlc_rx_bridge.py" \
    "$ROOT_DIR/vlc_host_protocol.py" \
    "$ROOT_DIR/tools/vlc_rx_view.sh" \
    "$ROOT_DIR/tools/vlc_link_test.sh" \
    /opt/openvlc-raspberry/
install -m 0644 "$ROOT_DIR/systemd/openvlc-rx.service" \
    /etc/systemd/system/openvlc-rx.service
if [ ! -e /etc/default/openvlc-rx ]; then
    install -m 0644 "$ROOT_DIR/systemd/openvlc-rx.default" \
        /etc/default/openvlc-rx
fi

systemctl daemon-reload
systemctl enable --now openvlc-rx.service
systemctl --no-pager --full status openvlc-rx.service || true
