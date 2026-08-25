#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

export INPUT_FORMAT="${INPUT_FORMAT:-mjpeg}"
export SIZE="${SIZE:-640x360}"
export FPS="${FPS:-20}"
export OUT_SIZE="${OUT_SIZE:-640x360}"
export OUT_FPS="${OUT_FPS:-20}"

export RATE_MODE="${RATE_MODE:-capped-crf}"
export CRF="${CRF:-22}"
export PRESET="${PRESET:-veryfast}"
export H264_PROFILE="${H264_PROFILE:-main}"

# Budget-40 ADC carries about 125 packets/s. PKT=752 at 600 kbit/s produces
# about 100 packets/s, leaving margin for decoder recovery and serial jitter.
export BITRATE="${BITRATE:-460k}"
export BUFSIZE="${BUFSIZE:-120k}"
export MUXRATE="${MUXRATE:-600k}"
export GOP="${GOP:-10}"
export SLICE_BYTES="${SLICE_BYTES:-400}"

exec bash ./vlc_tx_video.sh
