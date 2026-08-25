#!/usr/bin/env bash
#
# vlc_rx_view.sh - RX side: display the video stream coming out of the OpenVLC
# link. Run on the receiving Raspberry Pi AFTER vlc_rx_bridge.py is up (so the
# IP packets are flowing into tun0).
#
#   PORT=5000 ./vlc_rx_view.sh
#
set -euo pipefail
PORT=${PORT:-5000}
PLAYER=${PLAYER:-ffplay}     # ffplay or mpv
MODE=${MODE:-stable}         # stable or low-latency
FIFO_SIZE=${FIFO_SIZE:-65536}

url="udp://0.0.0.0:$PORT?fifo_size=$FIFO_SIZE&overrun_nonfatal=1"
echo "[rx] listening on UDP $PORT with $PLAYER mode=$MODE" >&2

if [ "$PLAYER" = "mpv" ]; then
    if [ "$MODE" = "low-latency" ]; then
        exec mpv --no-cache --untimed --profile=low-latency "$url"
    fi
    exec mpv --profile=low-latency --cache=yes --cache-secs=0.5 \
        --demuxer-readahead-secs=0.5 --cache-pause=no "$url"
else
    if [ "$MODE" = "low-latency" ]; then
        exec ffplay -hide_banner -loglevel warning -fflags nobuffer \
            -flags low_delay -probesize 32768 -analyzeduration 0 -framedrop \
            "$url"
    fi
    exec ffplay -hide_banner -loglevel warning \
        -fflags +discardcorrupt+genpts -flags low_delay \
        -probesize 262144 -analyzeduration 500000 -max_delay 500000 \
        -framedrop -sync ext "$url"
fi
