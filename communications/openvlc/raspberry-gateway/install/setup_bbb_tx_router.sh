#!/usr/bin/env bash
# Configure the BeagleBone as the Ethernet-to-OpenVLC one-way router.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PI_IF=${PI_IF:-eth0}
BBB_ETH_CIDR=${BBB_ETH_CIDR:-10.0.0.1/24}
VLC_MTU=${VLC_MTU:-900}
VLC_QDISC_LIMIT=${VLC_QDISC_LIMIT:-24}

if [ -z "${OPENVLC_BBB_DIR:-}" ]; then
    if [ -f "$SCRIPT_DIR/../TX_setup.sh" ]; then
        # raspberry/ was copied inside the BeagleBone repository.
        OPENVLC_BBB_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
    else
        # Monorepo layout: raspberry-gateway/ and the BeagleBone folders are
        # siblings. Prefer the recommended optimized TX, fall back to the
        # baseline reference node.
        for cand in beaglebone-tx beaglebone-reference; do
            if [ -f "$SCRIPT_DIR/../$cand/TX_setup.sh" ]; then
                OPENVLC_BBB_DIR=$(cd -- "$SCRIPT_DIR/../$cand" && pwd)
                break
            fi
        done
    fi
fi

if [ ! -x "$OPENVLC_BBB_DIR/TX_setup.sh" ] &&
   [ ! -f "$OPENVLC_BBB_DIR/TX_setup.sh" ]; then
    echo "TX_setup.sh not found under $OPENVLC_BBB_DIR" >&2
    exit 1
fi

sudo ip link set dev "$PI_IF" up
sudo ip address replace "$BBB_ETH_CIDR" dev "$PI_IF"
sudo sysctl -w net.ipv4.ip_forward=1 >/dev/null
sudo sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null
sudo sysctl -w net.ipv4.conf.default.rp_filter=0 >/dev/null
sudo sysctl -w "net.ipv4.conf.${PI_IF}.rp_filter=0" >/dev/null
sudo sysctl -w "net.ipv4.conf.${PI_IF}.forwarding=1" >/dev/null

cd "$OPENVLC_BBB_DIR"
# Optical TX profile - baked into the PRU rebuild (deploy.sh make). These must
# match the STM32 RX. The current validated companion profile is budget 50 /
# OPENVLC_PHY_RATE_KBPS=1000.
# ENABLE_MODE drives the auxiliary P8_46 LED-current branch: 0=held low,
# 1=high in idle, 2=high during TX. The validated PCB requires mode 0; this is
# not the frame enable. If a different front-end needs this branch, 0 leaves the
# LED dark and nothing is transmitted - set this to what your hardware needs.
TX_BUDGET=${OPENVLC_TX_SYMBOL_WAIT_BUDGET:-50}
TX_ENABLE_MODE=${OPENVLC_TX_ENABLE_MODE:-0}
TX_LINE_CODE=${OPENVLC_TX_LINE_CODE:-1}
TX_WARMUP_BITS=${OPENVLC_TX_WARMUP_BITS:-0}
OPENVLC_PREAMBLE_LEN=${OPENVLC_PREAMBLE_LEN:-8} \
OPENVLC_PREAMBLE_MODE=${OPENVLC_PREAMBLE_MODE:-0} \
OPENVLC_TX_LINE_CODE="$TX_LINE_CODE" \
OPENVLC_TX_ENABLE_MODE="$TX_ENABLE_MODE" \
OPENVLC_TX_WARMUP_BITS="$TX_WARMUP_BITS" \
OPENVLC_TX_SYMBOL_WAIT_BUDGET="$TX_BUDGET" \
    bash TX_setup.sh rx=0 self_id=7 dst_id=8 pool_size=50
echo "[bbb] TX profile: budget=$TX_BUDGET enable_mode=$TX_ENABLE_MODE line_code=$TX_LINE_CODE warmup=$TX_WARMUP_BITS"
printf '%s\n' \
    "budget=$TX_BUDGET" \
    "preamble_len=${OPENVLC_PREAMBLE_LEN:-8}" \
    "preamble_mode=${OPENVLC_PREAMBLE_MODE:-0}" \
    "line_code=$TX_LINE_CODE" \
    "enable_mode=$TX_ENABLE_MODE" \
    "warmup_bits=$TX_WARMUP_BITS" |
    sudo tee /run/openvlc-tx-profile >/dev/null

sudo ip link set dev vlc0 mtu "$VLC_MTU"
# Clear any TBF left by capacity experiments. Video pacing is performed by the
# constant-rate MPEG-TS muxer; this small FIFO only absorbs scheduler jitter.
sudo ip link set dev vlc0 txqueuelen "$VLC_QDISC_LIMIT"
sudo tc qdisc replace dev vlc0 root pfifo limit "$VLC_QDISC_LIMIT"
sudo sysctl -w net.ipv4.conf.vlc0.rp_filter=0 >/dev/null
sudo sysctl -w net.ipv4.conf.vlc0.forwarding=1 >/dev/null

if command -v iptables >/dev/null 2>&1; then
    # A matching rule appended after an existing DROP is ineffective. Remove
    # stale copies and install this one at the head of the forwarding chain.
    while sudo iptables -D FORWARD -i "$PI_IF" -o vlc0 -j ACCEPT \
        2>/dev/null; do
        :
    done
    sudo iptables -I FORWARD 1 -i "$PI_IF" -o vlc0 -j ACCEPT
fi

echo "[bbb] forwarding $PI_IF -> vlc0"
ip address show dev "$PI_IF"
ip address show dev vlc0
cat /run/openvlc-tx-profile
echo "[bbb] verify profile and TX counters with:"
echo "  dmesg | grep 'VLC: params' | tail -1"
echo "  ip -s link show dev vlc0"
echo "  tc -s qdisc show dev vlc0"
echo "[bbb] forwarding state:"
sysctl net.ipv4.ip_forward
ip route get 192.168.0.2 from 10.0.0.2 iif "$PI_IF" 2>/dev/null || true
if command -v iptables >/dev/null 2>&1; then
    sudo iptables -nvL FORWARD --line-numbers
fi
