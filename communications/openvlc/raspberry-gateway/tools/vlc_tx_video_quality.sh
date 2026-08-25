#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

export INPUT_FORMAT="${INPUT_FORMAT:-mjpeg}"
export SIZE="${SIZE:-640x360}"
export FPS="${FPS:-20}"
export OUT_SIZE="${OUT_SIZE:-640x360}"
export OUT_FPS="${OUT_FPS:-20}"

export RATE_MODE="${RATE_MODE:-capped-crf}"
export CRF="${CRF:-21}"
export PRESET="${PRESET:-veryfast}"
export H264_PROFILE="${H264_PROFILE:-main}"

export BITRATE="${BITRATE:-720k}"
export BUFSIZE="${BUFSIZE:-220k}"
export MUXRATE="${MUXRATE:-900k}"

exec bash ./vlc_tx_video.sh
