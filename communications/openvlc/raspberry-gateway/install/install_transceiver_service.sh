#!/usr/bin/env bash
# Install the single full-duplex STM32 OpenVLC bridge.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPLACE_CONFIG=0
NODE=""
PEER_IP=""
PI_HAT=0

if [ "$(id -u)" -ne 0 ]; then
    exec sudo bash "$0" "$@"
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --replace-config)
            REPLACE_CONFIG=1
            ;;
        --node)
            [ "$#" -ge 2 ] || { echo "--node requires a letter" >&2; exit 2; }
            NODE=$(printf '%s' "$2" | tr '[:upper:]' '[:lower:]')
            REPLACE_CONFIG=1
            shift
            ;;
        --peer)
            [ "$#" -ge 2 ] || { echo "--peer requires an address" >&2; exit 2; }
            PEER_IP="$2"
            REPLACE_CONFIG=1
            shift
            ;;
        --pi-hat)
            PI_HAT=1
            REPLACE_CONFIG=1
            ;;
        *)
            echo "usage: $0 [--replace-config] [--node LETTER] [--peer IP] [--pi-hat]" >&2
            exit 2
            ;;
    esac
    shift
done

# Deployment root. Everything the bridge reads or writes lives under here, so
# a bench run leaves nothing scattered across the filesystem.
DEST=/opt/openvlc-raspberry

apt-get update
apt-get install -y python3-serial iproute2 iperf
install -d -m 0755 "$DEST"
install -m 0755     "$ROOT_DIR/vlc_transceiver_bridge.py"     "$ROOT_DIR/vlc_stm32_tx_bridge.py"     "$ROOT_DIR/vlc_host_protocol.py"     "$ROOT_DIR/vlc_capture.py"     "$ROOT_DIR/vlc_pacing.py"     "$ROOT_DIR/tools/vlc_link_test.sh"     "$ROOT_DIR/tools/check_pi_hat_uart.sh"     "$ROOT_DIR/collect_logs.sh"     "$DEST/"
# Logs live with the deployment, next to the code that produced them. Group
# writable so the service user and an interactive session can both add to it.
install -d -m 0775 "$DEST/logs" "$DEST/logs/captures"
install -m 0644 "$ROOT_DIR/systemd/openvlc-transceiver.service" /etc/systemd/system/openvlc-transceiver.service
# Node identity.
#
# There used to be one .default file per node, differing in three lines. That
# does not survive a third node, so the identity is derived instead: from
# --node if given, otherwise from the trailing letter of the hostname. Hosts
# are named nodeA, nodeB, ... so nodeC installs itself correctly with no
# arguments and no new file to write.
if [ -z "$NODE" ]; then
    # Strictly a node name: nodeA, node-b, nodeC. Anything else -- a Pi still
    # called raspberrypi, say -- must not silently become node 'i', which an
    # earlier version of this line happily did.
    NODE=$(hostname \
           | grep -oiE '^nod[eo][-_]?[a-z]$' \
           | grep -oiE '[a-z]$' \
           | tr '[:upper:]' '[:lower:]')
fi
if ! printf '%s' "$NODE" | grep -qE '^[a-z]$'; then
    echo "cannot derive a node letter from hostname '$(hostname)';" >&2
    echo "pass one explicitly, e.g. --node c" >&2
    exit 2
fi

# a -> 1, b -> 2, c -> 3 ... which is both the tun host address and, offset by
# 6, the optical address the firmware is built with (node A is 7).
NODE_INDEX=$(printf '%d' "$(( $(printf '%d' "'$NODE") - 96 ))")
NODE_IP="192.168.0.$NODE_INDEX"

# Point-to-point peer. On a two-node bench it is simply the other one; with
# more nodes on a shared optical medium the peer is a routing decision, so it
# has to be given explicitly.
if [ -z "$PEER_IP" ]; then
    case "$NODE" in
        a) PEER_IP=192.168.0.2 ;;
        b) PEER_IP=192.168.0.1 ;;
        *) echo "node $NODE needs an explicit --peer IP" >&2; exit 2 ;;
    esac
fi

CONFIG_SOURCE="$ROOT_DIR/systemd/openvlc-transceiver.default"
if [ ! -e /etc/default/openvlc-transceiver ] || [ "$REPLACE_CONFIG" -eq 1 ]; then
    install -m 0644 "$CONFIG_SOURCE" /etc/default/openvlc-transceiver
    # Stamp this node's identity onto the shared template.
    sed -i         -e "s|^OPENVLC_TUN_CIDR=.*|OPENVLC_TUN_CIDR=$NODE_IP/24|"         -e "s|^OPENVLC_PEER_IP=.*|OPENVLC_PEER_IP=$PEER_IP|"         -e "s|^OPENVLC_CAPTURE_NODE=.*|OPENVLC_CAPTURE_NODE=$(hostname)|"         /etc/default/openvlc-transceiver
    echo "node $NODE: $NODE_IP/24, peer $PEER_IP, captures tagged $(hostname)"
    if [ "$PI_HAT" -eq 1 ]; then
        # /dev/serial0 is unreliable on Raspberry Pi 5: it can keep pointing
        # at the dedicated debug UART (ttyAMA10) even once uart0 is enabled
        # via a dtoverlay, so target the GPIO header UART device directly.
        sed -i 's|^OPENVLC_SERIAL_PORT=.*|OPENVLC_SERIAL_PORT=/dev/ttyAMA0|' \
            /etc/default/openvlc-transceiver
    fi
else
    echo "Keeping existing /etc/default/openvlc-transceiver"
    echo "Use --node a or --node b to install an explicit node configuration."
fi

if [ "$PI_HAT" -eq 1 ]; then
    "$DEST"/check_pi_hat_uart.sh
fi

# The standalone services cannot share the same serial port or TUN device.
systemctl disable --now openvlc-rx.service openvlc-tx-stm32.service 2>/dev/null || true
systemctl daemon-reload
systemctl enable openvlc-transceiver.service
systemctl restart openvlc-transceiver.service
systemctl --no-pager --full status openvlc-transceiver.service || true
