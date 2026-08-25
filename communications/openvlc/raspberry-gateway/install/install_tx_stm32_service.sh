#!/usr/bin/env bash
# Install the Raspberry Pi bridge for STM32 OpenVLC TX.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [ "$(id -u)" -ne 0 ]; then
    exec sudo bash "$0" "$@"
fi

apt-get update
apt-get install -y python3-serial iproute2 iperf ffmpeg v4l-utils
install -d -m 0755 /opt/openvlc-raspberry
install -m 0755 \
    "$ROOT_DIR/vlc_stm32_tx_bridge.py" \
    "$ROOT_DIR/tools/vlc_stm32_tx_serial_test.py" \
    "$ROOT_DIR/vlc_host_protocol.py" \
    "$ROOT_DIR/vlc_udp_pacer.py" \
    "$ROOT_DIR/tools/vlc_tx_video.sh" \
    "$ROOT_DIR/tools/vlc_tx_video_adc.sh" \
    "$ROOT_DIR/tools/vlc_tx_video_quality.sh" \
    "$ROOT_DIR/tools/vlc_link_test.sh" \
    /opt/openvlc-raspberry/
install -m 0644 "$ROOT_DIR/systemd/openvlc-tx-stm32.service" \
    /etc/systemd/system/openvlc-tx-stm32.service
if [ ! -e /etc/default/openvlc-tx-stm32 ]; then
    install -m 0644 "$ROOT_DIR/systemd/openvlc-tx-stm32.default" \
        /etc/default/openvlc-tx-stm32
fi

systemctl daemon-reload
systemctl enable --now openvlc-tx-stm32.service
systemctl --no-pager --full status openvlc-tx-stm32.service || true
