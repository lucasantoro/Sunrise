#!/usr/bin/env bash
#
# vlc_tx_video.sh - TX side: capture a USB webcam and stream it over the
# OpenVLC link toward the receiving node.
#
# Runs on the TX-side Raspberry Pi (the one with the webcam). The encoded UDP
# stream is sent to DEST (the RX node's VLC IP). In the final STM32-TX setup,
# the route to DEST goes through tun0 and vlc_stm32_tx_bridge.py frames packets
# onto the STM32 USART.
#
# Override parameters through environment variables, for example:
#   INPUT_FORMAT=mjpeg SIZE=640x360 OUT_SIZE=640x360 FPS=20 \
#     RATE_MODE=capped-crf CRF=21 PRESET=veryfast \
#     BITRATE=720k MUXRATE=900k ./vlc_tx_video.sh
#
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

DEV=${DEV:-/dev/video0}      # webcam device
DEST=${DEST:-192.168.0.2}    # RX node IP (the Pi tun0 address)
PORT=${PORT:-5000}
SIZE=${SIZE:-640x360}        # native C270 MJPEG mode; avoids a scaling stage
FPS=${FPS:-15}
BITRATE=${BITRATE:-500k}     # H.264 elementary-stream rate
MUXRATE=${MUXRATE:-650k}     # paced MPEG-TS rate, including TS padding
PKT=${PKT:-752}              # four 188-byte MPEG-TS packets, IP total 780 bytes
CODEC=${CODEC:-x264}         # x264 (better compression) or mjpeg (simpler)
INPUT_FORMAT=${INPUT_FORMAT:-mjpeg}
OUT_SIZE=${OUT_SIZE:-}
OUT_FPS=${OUT_FPS:-}
BUFSIZE=${BUFSIZE:-150k}
RATE_MODE=${RATE_MODE:-cbr}  # cbr or capped-crf
CRF=${CRF:-21}               # lower is sharper; used by capped-crf
PRESET=${PRESET:-veryfast}   # slower presets improve quality per bit
H264_PROFILE=${H264_PROFILE:-main}
H264_LEVEL=${H264_LEVEL:-3.0}
GOP=${GOP:-${OUT_FPS:-$FPS}} # one IDR per second by default
SLICE_BYTES=${SLICE_BYTES:-600}
SEND_BUFFER=${SEND_BUFFER:-262144}
BURST_PACKETS=${BURST_PACKETS:-1}
PACER=${PACER:-1}
PACER_PORT=${PACER_PORT:-14915}
MUXDELAY=${MUXDELAY:-0.60}
LOGLEVEL=${LOGLEVEL:-info}
THREAD_QUEUE_SIZE=${THREAD_QUEUE_SIZE:-256}
CAMERA_TUNE=${CAMERA_TUNE:-1}
POWER_LINE_FREQUENCY=${POWER_LINE_FREQUENCY:-1} # 1=50 Hz, 2=60 Hz
EXPOSURE_AUTO_PRIORITY=${EXPOSURE_AUTO_PRIORITY:-0}

echo "[tx] capture=$DEV ${SIZE}@${FPS} codec=$CODEC rate_mode=$RATE_MODE video_cap=$BITRATE mux=$MUXRATE" >&2
echo "[tx] output=${OUT_SIZE:-$SIZE}@${OUT_FPS:-$FPS} gop=$GOP packet=$PKT burst=$BURST_PACKETS muxdelay=$MUXDELAY -> $DEST:$PORT" >&2
if [ "$CODEC" != "mjpeg" ]; then
    echo "[tx] x264 preset=$PRESET profile=$H264_PROFILE level=$H264_LEVEL crf=$CRF bufsize=$BUFSIZE" >&2
fi

command -v ffmpeg >/dev/null || {
    echo "ffmpeg is required" >&2
    exit 1
}
[ -e "$DEV" ] || {
    echo "camera device not found: $DEV" >&2
    exit 1
}
if (( THREAD_QUEUE_SIZE <= 0 )); then
    echo "THREAD_QUEUE_SIZE must be positive" >&2
    exit 1
fi
if (( PKT <= 0 || PKT % 188 != 0 )); then
    echo "PKT must be a positive multiple of 188; recommended value: 752" >&2
    exit 1
fi
if (( GOP <= 0 || SLICE_BYTES <= 0 || BURST_PACKETS <= 0 )); then
    echo "GOP, SLICE_BYTES, and BURST_PACKETS must be positive integers" >&2
    exit 1
fi
if [[ "$PACER" != "0" && "$PACER" != "1" ]]; then
    echo "PACER must be 0 or 1" >&2
    exit 1
fi
if (( PACER_PORT <= 0 || PACER_PORT > 65535 )); then
    echo "PACER_PORT must be between 1 and 65535" >&2
    exit 1
fi
if [[ "$RATE_MODE" != "cbr" && "$RATE_MODE" != "capped-crf" ]]; then
    echo "RATE_MODE must be cbr or capped-crf" >&2
    exit 1
fi
if ! [[ "$CRF" =~ ^[0-9]+$ ]] || (( CRF < 0 || CRF > 51 )); then
    echo "CRF must be an integer from 0 to 51" >&2
    exit 1
fi

rate_to_bps() {
    local rate=$1
    local value

    case "$rate" in
        *[kK])
            value=${rate%?}
            [[ "$value" =~ ^[0-9]+$ ]] || return 1
            echo $((value * 1000))
            ;;
        *[mM])
            value=${rate%?}
            [[ "$value" =~ ^[0-9]+$ ]] || return 1
            echo $((value * 1000000))
            ;;
        *)
            [[ "$rate" =~ ^[0-9]+$ ]] || return 1
            echo "$rate"
            ;;
    esac
}

if ! muxrate_bps=$(rate_to_bps "$MUXRATE"); then
    echo "MUXRATE must be an integer rate such as 650k or 650000" >&2
    exit 1
fi
if ! bitrate_bps=$(rate_to_bps "$BITRATE"); then
    echo "BITRATE must be an integer rate such as 720k or 720000" >&2
    exit 1
fi
if (( bitrate_bps >= muxrate_bps )); then
    echo "BITRATE must remain below MUXRATE to leave room for MPEG-TS overhead" >&2
    exit 1
fi
burst_bits=$((PKT * 8 * BURST_PACKETS))

run_stream() {
    if [[ "$PACER" == "0" ]]; then
        exec "$@"
    fi

    command -v python3 >/dev/null || {
        echo "python3 is required for UDP pacing" >&2
        return 1
    }
    [ -f "$SCRIPT_DIR/vlc_udp_pacer.py" ] || {
        echo "missing $SCRIPT_DIR/vlc_udp_pacer.py" >&2
        return 1
    }

    local pacer_pid=""
    local stream_pid=""
    cleanup_stream() {
        trap - TERM INT
        [ -z "$stream_pid" ] || kill "$stream_pid" 2>/dev/null || true
        [ -z "$pacer_pid" ] || kill "$pacer_pid" 2>/dev/null || true
        [ -z "$stream_pid" ] || wait "$stream_pid" 2>/dev/null || true
        [ -z "$pacer_pid" ] || wait "$pacer_pid" 2>/dev/null || true
    }
    trap 'cleanup_stream; exit 143' TERM INT

    python3 "$SCRIPT_DIR/vlc_udp_pacer.py" \
        --listen-port "$PACER_PORT" \
        --destination "$DEST" \
        --destination-port "$PORT" \
        --rate "$muxrate_bps" \
        --packet-size "$PKT" &
    pacer_pid=$!
    sleep 0.2
    if ! kill -0 "$pacer_pid" 2>/dev/null; then
        wait "$pacer_pid"
        return $?
    fi

    "$@" &
    stream_pid=$!
    set +e
    wait "$stream_pid"
    local status=$?
    set -e
    cleanup_stream
    return "$status"
}

if [[ "$CAMERA_TUNE" == "1" ]] && command -v v4l2-ctl >/dev/null; then
    camera_controls=$(v4l2-ctl -d "$DEV" --list-ctrls 2>/dev/null || true)
    set_camera_control() {
        local name=$1
        local value=$2

        if grep -qE "^[[:space:]]*${name}[[:space:]]" <<<"$camera_controls"; then
            if v4l2-ctl -d "$DEV" --set-ctrl="${name}=${value}" >/dev/null 2>&1; then
                echo "[tx] camera control ${name}=${value}" >&2
            else
                echo "[tx] warning: camera rejected ${name}=${value}" >&2
            fi
        fi
    }

    # Keep the requested frame cadence under low light. Otherwise the C270 can
    # lengthen exposure and silently reduce fps, which appears as video jitter.
    set_camera_control exposure_auto_priority "$EXPOSURE_AUTO_PRIORITY"
    set_camera_control power_line_frequency "$POWER_LINE_FREQUENCY"
fi

input_args=(-thread_queue_size "$THREAD_QUEUE_SIZE" -f v4l2 -framerate "$FPS" -video_size "$SIZE")
if [ -n "$INPUT_FORMAT" ]; then
    input_args+=(-input_format "$INPUT_FORMAT")
fi

# Optional output scaling/decimation. Do not insert filters when the C270
# already provides the requested native MJPEG mode and frame rate.
filters=()
[ -n "$OUT_SIZE" ] && [ "$OUT_SIZE" != "$SIZE" ] &&
    filters+=("scale=${OUT_SIZE/x/:}")
[ -n "$OUT_FPS" ] && [ "$OUT_FPS" != "$FPS" ] &&
    filters+=("fps=$OUT_FPS")
# V4L2 modes may carry a non-square source SAR. Network video uses square
# pixels so 640x360 remains 16:9 rather than inheriting a 4:3 display ratio.
filters+=("setsar=1")
vf_args=()
if [ ${#filters[@]} -gt 0 ]; then
    vf_str=$(IFS=,; echo "${filters[*]}")
    vf_args=(-vf "$vf_str")
fi

mux_args=(
    -muxrate "$MUXRATE"
    -pcr_period 20
    -pat_period 0.2
    -sdt_period 1
    -mpegts_flags +resend_headers
    # Keep enough DTS/PCR distance for the configured encoder VBV burst.
    -muxdelay "$MUXDELAY"
    -muxpreload 0
    -flush_packets 1
    -f mpegts
)

if [[ "$PACER" == "1" ]]; then
    udp_url="udp://127.0.0.1:$PACER_PORT?pkt_size=$PKT&buffer_size=$SEND_BUFFER"
    echo "[tx] UDP pacer enabled: localhost:$PACER_PORT -> $DEST:$PORT" >&2
else
    udp_url="udp://$DEST:$PORT?pkt_size=$PKT&buffer_size=$SEND_BUFFER&bitrate=$muxrate_bps&burst_bits=$burst_bits"
fi

if [ "$CODEC" = "mjpeg" ]; then
    run_stream ffmpeg -hide_banner -loglevel "$LOGLEVEL" \
        "${input_args[@]}" -i "$DEV" \
        "${vf_args[@]}" \
        -c:v mjpeg -q:v 20 \
        -an \
        "${mux_args[@]}" \
        "$udp_url"
else
    rate_args=()
    if [ "$RATE_MODE" = "capped-crf" ]; then
        rate_args=(-crf "$CRF" -maxrate "$BITRATE" -bufsize "$BUFSIZE")
        hrd_mode=vbr
    else
        rate_args=(-b:v "$BITRATE" -minrate "$BITRATE" -maxrate "$BITRATE" \
            -bufsize "$BUFSIZE")
        hrd_mode=cbr
    fi
    x264_params="nal-hrd=$hrd_mode:force-cfr=1:repeat-headers=1:aud=1:sliced-threads=1:slice-max-size=$SLICE_BYTES"
    run_stream ffmpeg -hide_banner -loglevel "$LOGLEVEL" \
        "${input_args[@]}" -i "$DEV" \
        "${vf_args[@]}" \
        -c:v libx264 -preset "$PRESET" -tune zerolatency \
        -profile:v "$H264_PROFILE" -level:v "$H264_LEVEL" \
        "${rate_args[@]}" \
        -pix_fmt yuv420p -g "$GOP" -keyint_min "$GOP" \
        -sc_threshold 0 -bf 0 -refs 1 \
        -x264-params "$x264_params" \
        -an \
        "${mux_args[@]}" \
        "$udp_url"
fi
