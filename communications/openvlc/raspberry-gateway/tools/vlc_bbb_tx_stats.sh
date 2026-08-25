#!/usr/bin/env bash
# Show whether loss occurs before the optical receiver.
set -euo pipefail

VLC_IF=${VLC_IF:-vlc0}
WINDOW=${WINDOW:-10}
PARAM_DIR=${PARAM_DIR:-/sys/module/vlc/parameters}

read_counter() {
    local name=$1
    cat "/sys/class/net/${VLC_IF}/statistics/${name}"
}

warning_count() {
    dmesg 2>/dev/null |
        grep -E -c "MAC layer queue is full|VLC_TX: dropping" || true
}

read_module_counter() {
    local name=$1

    if [ -r "${PARAM_DIR}/${name}" ]; then
        cat "${PARAM_DIR}/${name}"
    else
        echo 0
    fi
}

if [ ! -d "/sys/class/net/${VLC_IF}" ]; then
    echo "${VLC_IF} does not exist; load the OpenVLC TX first" >&2
    exit 1
fi

echo "[bbb] module profile:"
dmesg | grep "VLC: params" | tail -1 || true
if [ -r /run/openvlc-tx-profile ]; then
    echo "[bbb] loaded PRU profile:"
    cat /run/openvlc-tx-profile
else
    echo "[bbb] WARNING: PRU timing profile is unverified; rerun setup_bbb_tx_router.sh"
fi
echo "[bbb] sampling ${VLC_IF} for ${WINDOW}s"

txp0=$(read_counter tx_packets)
txb0=$(read_counter tx_bytes)
txd0=$(read_counter tx_dropped)
warn0=$(warning_count)
have_pru_stats=0
if [ -r "${PARAM_DIR}/tx_pru_completed" ]; then
    have_pru_stats=1
    encoded0=$(read_module_counter tx_encoded)
    started0=$(read_module_counter tx_pru_started)
    completed0=$(read_module_counter tx_pru_completed)
    completed_bytes0=$(read_module_counter tx_pru_completed_bytes)
    backpressure0=$(read_module_counter tx_backpressure)
    queue0=$(read_module_counter tx_queue_depth)
fi
sleep "$WINDOW"
txp1=$(read_counter tx_packets)
txb1=$(read_counter tx_bytes)
txd1=$(read_counter tx_dropped)
warn1=$(warning_count)
if [ "$have_pru_stats" -eq 1 ]; then
    encoded1=$(read_module_counter tx_encoded)
    started1=$(read_module_counter tx_pru_started)
    completed1=$(read_module_counter tx_pru_completed)
    completed_bytes1=$(read_module_counter tx_pru_completed_bytes)
    backpressure1=$(read_module_counter tx_backpressure)
    queue1=$(read_module_counter tx_queue_depth)
    queue_max=$(read_module_counter tx_queue_max_depth)
fi

packets=$((txp1 - txp0))
bytes=$((txb1 - txb0))
drops=$((txd1 - txd0))
warnings=$((warn1 - warn0))

echo "[bbb] tx_packets=${packets} tx_bytes=${bytes} tx_dropped=${drops} new_queue_warnings=${warnings}"
awk -v packets="$packets" -v bytes="$bytes" -v seconds="$WINDOW" \
    'BEGIN {
        printf "[bbb] enqueued_by_driver=%.1f kbps packet_rate=%.1f pps",
               bytes * 8 / seconds / 1000, packets / seconds
        if (packets > 0)
            printf " mean_packet=%.1f bytes", bytes / packets
        printf "\n"
    }'
echo "[bbb] note: vlc0 TX counters advance at MAC enqueue, before PRU/optical completion"
if [ "$have_pru_stats" -eq 1 ]; then
    encoded=$((encoded1 - encoded0))
    started=$((started1 - started0))
    completed=$((completed1 - completed0))
    completed_bytes=$((completed_bytes1 - completed_bytes0))
    backpressure=$((backpressure1 - backpressure0))
    backlog_delta=$((packets - completed))

    echo "[bbb] encoded=${encoded} pru_started=${started} pru_completed=${completed} completed_bytes=${completed_bytes}"
    awk -v frames="$completed" -v bytes="$completed_bytes" -v seconds="$WINDOW" \
        'BEGIN {
            printf "[bbb] optical_completed=%.1f kbps completion_rate=%.1f fps",
                   bytes * 8 / seconds / 1000, frames / seconds
            if (frames > 0)
                printf " mean_completed_packet=%.1f bytes", bytes / frames
            printf "\n"
        }'
    echo "[bbb] queue_depth=${queue0}->${queue1} lifetime_queue_max=${queue_max} backlog_delta=${backlog_delta} backpressure_events=${backpressure}"
    echo "[bbb] interpretation: stable service requires pru_completed ~= tx_packets, queue depth not trending upward, and backpressure_events=0"
else
    echo "[bbb] PRU completion counters unavailable; rebuild/reload the updated vlc.ko to measure optical completion"
fi
ip -s link show dev "$VLC_IF"
if [ "$warnings" -gt 0 ]; then
    echo "[bbb] queue warnings generated during this sample:"
    dmesg | grep -E "MAC layer queue is full|VLC_TX: dropping" | tail -10 || true
fi
