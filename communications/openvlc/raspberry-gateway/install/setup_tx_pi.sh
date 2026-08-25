#!/usr/bin/env bash
# Configure the webcam Raspberry Pi route through the BeagleBone transmitter.
set -euo pipefail

BBB_IF=${BBB_IF:-eth0}
TX_PI_CIDR=${TX_PI_CIDR:-10.0.0.2/24}
TX_PI_IP=${TX_PI_CIDR%/*}
BBB_IP=${BBB_IP:-10.0.0.1}
VLC_SUBNET=${VLC_SUBNET:-192.168.0.0/24}
VLC_DEST=${VLC_DEST:-192.168.0.2}
VLC_MTU=${VLC_MTU:-900}
TX_QUEUE_LEN=${TX_QUEUE_LEN:-64}

if [ "$(id -u)" -ne 0 ]; then
    exec sudo --preserve-env=BBB_IF,TX_PI_CIDR,BBB_IP,VLC_SUBNET,VLC_DEST,VLC_MTU,TX_QUEUE_LEN \
        bash "$0" "$@"
fi

ip link set dev "$BBB_IF" up
ip link set dev "$BBB_IF" txqueuelen "$TX_QUEUE_LEN"

# The BBB gateway address must never be assigned locally. This can remain on
# the Pi if setup_bbb_tx_router.sh was accidentally run here; in that case
# Linux treats the next hop as local and bypasses the gateway.
while IFS= read -r local_cidr; do
    if [ "${local_cidr%/*}" = "$BBB_IP" ]; then
        echo "[tx-pi] removing conflicting local gateway address $local_cidr" >&2
        ip address del "$local_cidr" dev "$BBB_IF"
    fi
done < <(ip -4 -o address show dev "$BBB_IF" | awk '{print $4}')

ip address replace "$TX_PI_CIDR" dev "$BBB_IF"
sysctl -w "net.ipv4.conf.${BBB_IF}.rp_filter=0" >/dev/null
ip route replace "$VLC_SUBNET" via "$BBB_IP" dev "$BBB_IF" \
    src "$TX_PI_IP" mtu "$VLC_MTU"
# Prefer an explicit host route. It wins over DHCP/VPN routes for the same
# 192.168.0.0/24 subnet and makes the one-way destination unambiguous.
ip route replace "${VLC_DEST}/32" via "$BBB_IP" dev "$BBB_IF" \
    src "$TX_PI_IP" mtu "$VLC_MTU"
ip route flush cache
tc qdisc replace dev "$BBB_IF" root fq_codel limit "$TX_QUEUE_LEN" \
    target 5ms interval 100ms quantum "$VLC_MTU"

echo "[tx-pi] route configured:"
ip route show "$VLC_SUBNET"
ip route show "${VLC_DEST}/32"
ip route get "$VLC_DEST"
if ping -I "$TX_PI_IP" -c 1 -W 2 "$BBB_IP" >/dev/null 2>&1; then
    echo "[tx-pi] BBB gateway $BBB_IP is reachable from $TX_PI_IP"
else
    echo "[tx-pi] WARNING: BBB gateway $BBB_IP did not answer on $BBB_IF" >&2
fi
tc -s qdisc show dev "$BBB_IF"
