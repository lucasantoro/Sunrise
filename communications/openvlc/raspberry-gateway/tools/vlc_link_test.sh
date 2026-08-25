#!/usr/bin/env bash
# Run a one-way iperf2 link test. Use ROLE=rx on the receiver Pi.
set -euo pipefail

ROLE=${ROLE:-tx}
DEST=${DEST:-192.168.0.2}
RATE=${RATE:-100k}
DURATION=${DURATION:-30}
PAYLOAD=${PAYLOAD:-752}
PORT=${PORT:-10001}
INTERVAL=${INTERVAL:-1}

command -v iperf >/dev/null || {
    echo "iperf2 is required: sudo apt install iperf" >&2
    exit 1
}

if [ "$ROLE" = "rx" ]; then
    exec stdbuf -oL -eL iperf -u -s -p "$PORT" -i "$INTERVAL"
fi

exec iperf -u -c "$DEST" -b "$RATE" -l "$PAYLOAD" \
    -p "$PORT" -t "$DURATION" -i "$INTERVAL"
