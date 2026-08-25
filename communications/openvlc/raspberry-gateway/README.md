# Raspberry Pi OpenVLC video path

> **Full operations & testing runbook + parameter reference:**
> [`docs/operations.md`](docs/operations.md) - bring-up, end-to-end iperf,
> headless video viewing, latency tuning, every configurable parameter, and a
> troubleshooting table.

This directory contains the companion-computer software for a one-way video
link:

```text
USB webcam
  -> TX Raspberry Pi
  -> Linux TUN interface, 192.168.0.1/24
  -> framed USART3 stream, 2 Mbit/s
  -> STM32H7 OpenVLC transmitter
  -> optical Manchester link
  -> STM32H7 COMP1/TIM2 receiver
  -> 2 Mbit/s framed USART3 stream
  -> RX Raspberry Pi (tun0)
  -> ffplay or mpv
```

The STM32 outputs the original decoded IP datagram. The receiver bridge only
checks the serial record and the IP header, then writes the packet to a Linux
TUN interface. No custom video protocol is required.

The optical path is one-way. Use UDP video and iperf2 UDP. Do not use iperf3,
TCP, RTSP control sessions, or protocols that require acknowledgements from
the receiver.

## Layout

The runtime Python sits flat at the root on purpose: the modules import each
other by bare name and the installer copies them into one directory on the Pi,
so the deployed layout and this one match. Everything that is not runtime is
grouped.

```
NOTES.md          bench notes: commands, logs, every diagnostic field
README.md         this file
collect_logs.sh   save a run into logs/

vlc_transceiver_bridge.py   the v2 transceiver bridge (current)
vlc_rx_bridge.py            legacy receive-only bridge
vlc_stm32_tx_bridge.py      legacy STM32 TX bridge
vlc_host_protocol.py        host <-> STM32 framing
vlc_capture.py              RX capture validation and writing
vlc_pacing.py               transmit pacing
vlc_udp_pacer.py            UDP pacing helper

docs/       operations notes
install/    install_*.sh and setup_*.sh
systemd/    unit and default files
tools/      diagnostics, link tests, video helpers
tests/      unittest, no hardware needed
logs/       all bench output; not tracked
```

**Start with [NOTES.md](NOTES.md)** if you are at the bench: it has the
commands, how to enable and save logs, and the meaning of every field in the
`COMP`, `TX` and `trx-bridge` lines.

Run the tests with:

```bash
python -m unittest discover -s tests -t .
```


## Physical connections

Power all boards off before changing the optical-front-end wiring.

### Transmitter side

```text
USB webcam
  -> TX Raspberry Pi USB

TX Raspberry Pi USB
  -> STM32 TX ST-Link virtual COM port (USART3 PD8/PD9)

STM32 TX PB4
  -> existing OpenVLC LED-driver modulation input

STM32 TX PB5
  -> OpenVLC PCB P8_46-equivalent auxiliary LED branch
  -> hold low continuously; it is not the frame enable

TX frontend hardware-enable strap
  -> 3.3 V

STM32 TX GND
  -> TX frontend GND
```

Do not drive the LED directly from `PB4`. Keep the known-working OpenVLC
driver, current limiting, and power supply. On the current TX PCB, `PB4`
replaces BBB `P8_45` and carries all OOK data. `PB5` replaces BBB `P8_46`,
which drives a second LED-current branch; keep it low exactly like
`OPENVLC_TX_ENABLE_MODE=0`. The physically swapped `P9_26` position carries
`PS_ON` and must be held at `3.3 V` to power the frontend.

### Receiver side

```text
photodiode / AGC frontend output, limited to 0..3.3 V
  -> NUCLEO-H743ZI2 PB2 / COMP1_INP

receiver frontend GND
  -> Nucleo GND

Nucleo ST-Link USB connector
  -> RX Raspberry Pi USB

RX Raspberry Pi HDMI
  -> display
```

The ST-Link USB connection powers the Nucleo if its jumper configuration is
unchanged and exposes USART3 as `/dev/ttyACM*`. No separate UART wires are
required. PC5 carries the external COMP1 output and may be connected to an
oscilloscope probe for diagnostics; it is not connected to the Raspberry Pi.

Never apply the unscaled frontend voltage to PB2. Verify the waveform remains
inside the STM32 input range before connecting it.

## Components

| File | Host | Purpose |
|---|---|---|
| `vlc_stm32_tx_bridge.py` | TX Raspberry Pi | Read IP datagrams from `tun0`, frame them, and write them to STM32 TX over USART |
| `vlc_stm32_tx_serial_test.py` | TX Raspberry Pi | Send valid frames directly over USART, bypassing TUN/routes |
| `install/install_tx_stm32_service.sh` | TX Raspberry Pi | Install and start the STM32 TX bridge as a systemd service |
| `setup_stm32_tx_pi.sh` | TX Raspberry Pi | Run the STM32 TX bridge in the foreground for debug |
| `tools/vlc_tx_video.sh` | TX Raspberry Pi | Capture and encode the webcam into a bounded UDP MPEG-TS stream |
| `vlc_rx_bridge.py` | RX Raspberry Pi | Parse STM32 records, validate IP packets, and inject them into `tun0` |
| `install/install_rx_service.sh` | RX Raspberry Pi | Install and start the bridge as a systemd service |
| `vlc_rx_view.sh` | RX Raspberry Pi | Display the UDP video stream |
| `tools/vlc_link_test.sh` | both sides | Measure one-way UDP goodput and loss with iperf2 |

The legacy BBB TX scripts are kept for comparison and fallback only. Do not use
them in the final STM32-TX path.

## Network plan

| Interface | Address | MTU |
|---|---:|---:|
| TX Pi `tun0` | `192.168.0.1/24` | `900` |
| RX Pi `tun0` | `192.168.0.2/24` | kernel TUN |

The OpenVLC STM32 payload limit is 900 bytes. The video scripts use a 752-byte
UDP payload, which produces a 780-byte IPv4 datagram and avoids fragmentation.
The validated budget-50 link has delivered roughly 0.8-1.0 Mbit/s under iperf2
with approximately zero steady-state loss after startup when the optical path is
clean. The stable video profile intentionally uses a 650 kbit/s MPEG-TS rate to
retain margin for scheduling jitter and protocol overhead.

## 1. STM32 receiver profile

The desktop GUI in `../control-app` can run the same setup scripts, monitor the
RX bridge, start iperf, and display the STM32 link-quality metric. Use the
commands below when operating manually or when debugging the GUI's remote
actions.

The companion profile in `Core/Inc/openvlc_board.h` uses:

```c
#define OPENVLC_RX_HOST_FORWARD 1
#define OPENVLC_HOST_UART_BAUD 2000000u
```

USART3 is the NUCLEO ST-Link virtual COM port on PD8/PD9. Connect the NUCLEO
USB debug port to the RX Raspberry Pi and identify the device:

```bash
ls -l /dev/ttyACM*
```

The decoder remains COMP1 plus TIM2 input capture. Host records are queued
during packet decoding. TIM2/DMA is restarted before USART transmission, so a
large IP packet no longer extends the receiver blind interval.

Flash `communications/openvlc/stm32-rx/STM32CubeIDE/Debug/stm32.elf` with
STM32CubeIDE or the ST-Link programmer before installing the Raspberry bridge.

## 2. STM32 transmitter

Build and flash:

```text
communications/openvlc/stm32-tx/STM32CubeIDE/Debug/stm32-tx.elf
```

The default firmware profile is budget 40, with USART3 host mode at 2 Mbit/s.
The Raspberry TX bridge sends IP datagrams over that serial link using the same
host-frame CRC format used by the STM32 RX bridge.

Install the bridge on the TX Raspberry Pi:

```bash
cd communications/openvlc/raspberry-gateway
sudo bash ./install/install_tx_stm32_service.sh
sudo systemctl status openvlc-tx-stm32
```

For foreground debug instead of systemd:

```bash
cd communications/openvlc/raspberry-gateway
sudo bash ./install/setup_stm32_tx_pi.sh
```

After the bridge starts, traffic sent to `192.168.0.2` leaves through `tun0`,
is framed on USART3, and is transmitted by the STM32.

The bridge installs an explicit `192.168.0.2/32` route on `tun0`. This replaces
any stale route through `eth0` left by the legacy BBB topology. Verify:

```bash
ip route get 192.168.0.2
```

The first line must contain `dev tun0 src 192.168.0.1`.

## 2b. Legacy BeagleBone transmitter

The former BBB path is still useful as a reference transmitter. Copy or clone
the repository on the BBB, then run:

```bash
cd communications/openvlc/raspberry-gateway
bash ./install/setup_bbb_tx_router.sh
```

The script supports both repository layouts:

- `communications/openvlc/{raspberry-gateway,beaglebone-reference}`;
- `~/beaglebone-reference/raspberry-gateway`.

It writes the PRU build profile to `/run/openvlc-tx-profile`. The statistics
script reports a warning when this marker is missing, because the kernel module
parameters do not contain the PRU symbol wait budget.

The script loads this known TX profile:

```text
OPENVLC_PREAMBLE_LEN=8
OPENVLC_PREAMBLE_MODE=0
OPENVLC_TX_LINE_CODE=1
OPENVLC_TX_ENABLE_MODE=0
OPENVLC_TX_SYMBOL_WAIT_BUDGET=50
rx=0 self_id=7 dst_id=8 pool_size=50
```

Override `PI_IF` if the Ethernet interface connected to the TX Pi is not
`eth0`.

This must be paired with `OPENVLC_PHY_RATE_KBPS=1000` in the STM32 firmware.
The STM32 boot line is the authority: verify it prints `rate=1000k` before
testing this budget-50 profile.
The setup script also replaces any TBF left by capacity experiments with a
small FIFO. Do not leave a manual `650kbit` TBF active: FFmpeg now performs
constant-rate MPEG-TS pacing itself.

The experimental budget-40 profile uses
`beaglebone-tx/TX_setup_budget40.sh` and must be paired with
`OPENVLC_PHY_RATE_KBPS=1250` on the STM32. The corresponding boot line is
`rate=1250k`. Keep video on the validated budget-50 profile until the
budget-40 iperf procedure passes without growing BBB queues, STM32
`ringdrop`/`hostdrop`, or bridge sequence gaps.

## 3. TX Raspberry Pi traffic generation

Install the runtime and verify the STM32 TX TUN route:

```bash
sudo apt install ffmpeg iperf v4l-utils iproute2
cd communications/openvlc/raspberry-gateway
sudo systemctl status openvlc-tx-stm32
ip route get 192.168.0.2
v4l2-ctl --list-formats-ext -d /dev/video0
```

Start the stable profile:

```bash
INPUT_FORMAT=mjpeg SIZE=640x360 FPS=15 \
  OUT_SIZE=640x360 OUT_FPS=15 \
  BITRATE=500k MUXRATE=650k \
  bash ./tools/vlc_tx_video.sh
```

For the Logitech C270 this is a native mode, so FFmpeg does not need a scaling
stage. For more detail at the same frame rate:

```bash
INPUT_FORMAT=mjpeg SIZE=864x480 FPS=15 \
  OUT_SIZE=864x480 OUT_FPS=15 \
  RATE_MODE=capped-crf CRF=24 \
  BITRATE=680k BUFSIZE=220k MUXRATE=800k \
  bash ./tools/vlc_tx_video.sh
```

`BITRATE` controls the H.264 elementary stream. `MUXRATE` controls the actual
constant-rate MPEG-TS stream and must be higher than `BITRATE`. The remaining
rate carries TS headers, repeated PAT/PMT, PCR, and null packets. Constant-rate
muxing avoids delivering a whole video frame to the BBB as a short packet
burst. The FFmpeg UDP output is additionally rate-limited to `MUXRATE` with a
one-datagram burst allowance, so the 752-byte packets are distributed across
time instead of being released together. This keeps driver queues and the
optical AGC operating steadily.

With `MUXRATE=650k` and `PKT=752`, the sender emits approximately 108 UDP
datagrams per second. Including the 28-byte IPv4/UDP header, the BBB receives
about 674 kbit/s of network data. The STM32 host record adds 11 bytes and UART
8N1 framing, consuming about 855 kbit/s on the 2 Mbaud companion link. Both
figures remain below their measured limits.

The encoder uses:

- H.264 Baseline without B-frames;
- one IDR per second;
- repeated codec and MPEG-TS headers;
- slices bounded to approximately 600 bytes;
- 752-byte UDP payloads;
- a 250 kbit VBV buffer;
- a 600 ms MPEG-TS decode delay so DTS remains ahead of PCR during IDR
  bursts;
- square output pixels, preventing camera sample-aspect-ratio metadata from
  turning a 640x360 stream into a 4:3 display image.

These settings limit the visible effect of a lost datagram and allow the
decoder to recover at the next IDR without restarting the stream.
The decode delay is deliberately larger than `BUFSIZE / MUXRATE`
(`250 kbit / 650 kbit/s`, approximately 385 ms). A smaller delay can make the
constant-rate PCR overtake an IDR packet DTS, producing FFmpeg's
`dts < pcr, TS is invalid` warning. Override `MUXDELAY` only together with
`BUFSIZE`; lowering it reduces latency but also reduces the permitted encoder
burst margin.

## 4. RX Raspberry Pi

Install and start the bridge:

```bash
cd communications/openvlc/raspberry-gateway
sudo bash ./install/install_rx_service.sh
journalctl -u openvlc-rx -f
ip address show tun0
```

The default `OPENVLC_SERIAL_PORT=auto` prefers the stable ST-Link path under
`/dev/serial/by-id`. Existing installations may still contain
`/dev/ttyACM0`; change them to `auto` or the exact by-id path:

```bash
sudo sed -i 's|^OPENVLC_SERIAL_PORT=.*|OPENVLC_SERIAL_PORT=auto|' \
  /etc/default/openvlc-rx
sudo systemctl restart openvlc-rx
```

For the `stm32-transceiver-pi-hat` GPIO-UART variant, do not use `auto`: an
attached ST-LINK can otherwise be selected instead of the HAT UART. Enable the
hardware serial port, disable the serial console, reboot, then install with:

```bash
sudo ./install/install_transceiver_service.sh --pi-hat            # node derived from the hostname
# or, if the host is not named nodeX: --node b --peer 192.168.0.1
```

This sets `OPENVLC_SERIAL_PORT=/dev/serial0` and runs the 2 Mbaud PL011
preflight in `tools/check_pi_hat_uart.sh` before starting the service.

The full-duplex bridge also sets `OPENVLC_TX_MAX_FPS=125`. This is a maximum,
not a traffic generator: lower-rate traffic passes immediately, while a burst
of queued iperf datagrams is released to the STM32 at one record every 8 ms.
Late Linux scheduling never causes a catch-up burst. Set the value to `0` only
for an explicit unpaced stress test; normal Pi-HAT operation should retain
`125`.

The bridge also installs `10.0.0.0/24 dev tun0`. This makes Linux reverse-path
validation consistent with packets whose source is the TX Raspberry Pi. Change
`OPENVLC_SOURCE_ROUTE` if the Ethernet subnet between the TX Pi and BBB differs.

For foreground diagnostics:

```bash
sudo systemctl stop openvlc-rx
sudo python3 ./vlc_rx_bridge.py --port /dev/ttyACM0 --baud 2000000
```

Start the viewer in another terminal:

```bash
bash ./vlc_rx_view.sh
```

The default `MODE=stable` allows approximately 500 ms of receiver buffering.
This absorbs normal packet and scheduling jitter. For latency experiments:

```bash
MODE=low-latency bash ./vlc_rx_view.sh
```

Use the low-latency mode only after the stable profile is clean; zero-buffer
playback makes a single late datagram visible even when no packet was lost.

The bridge reports:

- current five-second bitrate and packet rate;
- cumulative accepted IP frames and average bitrate;
- STM32 record sequence gaps;
- serial CRC failures;
- malformed or oversized records;
- invalid IP datagrams and TUN write errors.
- RX capture records, saved captures and rejected captures as
  `cap=records/saved/errors`.

The STM32 `COMP` diagnostic also reports `hostq`, `hostsent`, `hostdrop`, and
`hosterr`. Nonzero `hostdrop` or `hosterr` identifies the companion transport
rather than the optical decoder as the loss point.

With `OPENVLC_RX_CAPTURE=1` in the STM32 firmware, the bridge saves qualified
successful and failed comparator bursts under `logs/captures`.
The default firmware quotas are 32 `-ok` files and 32 classified failure files
(`-crc`, `-sync`, `-length` or `-decode`). Set
`OPENVLC_CAPTURE_NODE=node-a` or `node-b` in
`/etc/default/openvlc-transceiver` so files from the two receivers remain
distinct.

## 5. Measure capacity before video

On the RX Pi:

```bash
ROLE=rx bash ./tools/vlc_link_test.sh
```

On the TX Pi:

```bash
RATE=100k bash ./tools/vlc_link_test.sh
RATE=150k bash ./tools/vlc_link_test.sh
RATE=200k bash ./tools/vlc_link_test.sh
```

Increase further only after the previous step is stable. The current
budget-50 profile has been validated up to roughly a 1 Mbit/s offered iperf2
stream on clean hardware, but the stable capacity is a measured property of the
current optical alignment, AGC/threshold point, and host load. The first iperf
interval can report old sequence numbers if the receiver was started after the
sender; assess steady-state intervals after both bridge and server are already
running.

For budget 40, begin at `800k` and step through `900k`, `950k`, `1000k`, and
`1050k`. A valid step requires steady-state UDP loss below 1%, BBB
`pru_completed` tracking enqueue, and no increase in STM32 `hostdrop`,
`hosterr`, or `ringdrop`. Stop at the first sustained failure; do not infer
capacity from a one-second interval.

While a test is running, inspect the BBB in another terminal:

```bash
WINDOW=10 bash ./vlc_bbb_tx_stats.sh
```

The updated driver exports read-only TX service counters under
`/sys/module/vlc/parameters/`. The script reports:

```text
enqueued_by_driver     packets accepted into the OpenVLC MAC queue
encoded                frames prepared by the Linux worker
pru_started            frames submitted to PRU1
pru_completed          frames whose final optical symbol was emitted
optical_completed      completed network bytes per second
queue_depth            current software backlog
backpressure_events    netdev stops caused by an exhausted packet pool
```

For a sustainable offered load, `pru_completed` should approximately equal
`tx_packets`, queue depth must not trend upward, `backlog_delta` should stay
near zero, and `backpressure_events` should remain zero. Compare
`optical_completed` with the RX bridge rate.

- low BBB rate or increasing `tx_dropped`: BBB queue/CPU/TX service bottleneck;
- `new_queue_warnings` above zero: the PRU path is applying backpressure during
  the measurement; do not interpret `enqueued_by_driver` as air goodput;
- high BBB enqueue rate, no new queue warnings, but low STM32 `ok/seen`: optical
  acquisition/demodulation loss;
- STM32 `ok` rises but bridge `seq_gap`, `hostdrop`, or `hosterr` rises:
  companion UART/queue loss.
- enqueue exceeds `optical_completed` and queue depth grows: BBB TX service is
  saturated before the optical receiver.

For a clean run, compare rates over the same time window:

```text
BBB packet_rate ~= STM32 delta(seen)/seconds
STM32 delta(ok)/delta(seen) = optical frame success ratio
bridge delta(ip)/seconds ~= STM32 delta(ok)/seconds
```

If the first equality fails, frames are being delayed before the receiver or
multiple frames are merging into one capture burst. If the second ratio is
poor, the loss is in edge acquisition/PHY decode. If the third equality fails,
the loss is in the STM32 host queue, serial protocol, or Pi bridge.

The STM32 budget-50 profile uses a `4 us` edge-idle delimiter. This is much
longer than the maximum normal Manchester edge interval but shorter than the
BBB software service gap between queued frames. Keep this value matched to the
current timing profile; a delimiter that is too long merges adjacent frames
and creates a load-dependent goodput plateau even when individual packets
decode correctly.

For stable video, keep `MUXRATE=650k` until a complete video run shows all of:

```text
BBB queue_depth remains near zero
BBB backpressure_events remains zero
STM32 hostdrop and hosterr remain zero
bridge gap, crc, invalid_ip, and tunerr remain zero
```

The H.264 `BITRATE` is not the link rate. Keep it below `MUXRATE`; keep
`MUXRATE` below the measured UDP goodput.

### Higher-quality 1 Mbit/s link profile

After the optical link sustains a `1000k` iperf2 stream without internal
queue or receiver drops, use this as the first quality upgrade:

```bash
cd communications/openvlc/raspberry-gateway
bash ./tools/vlc_tx_video_quality.sh
```

The launcher is equivalent to:

```bash
INPUT_FORMAT=mjpeg SIZE=640x360 FPS=20 \
  OUT_SIZE=640x360 OUT_FPS=20 \
  RATE_MODE=capped-crf CRF=21 PRESET=veryfast H264_PROFILE=main \
  BITRATE=720k BUFSIZE=220k MUXRATE=900k \
  bash ./tools/vlc_tx_video.sh
```

`RATE_MODE=capped-crf` lets x264 use lower quantization on frames that benefit
from it while `BITRATE` remains a hard maximum. Lower `CRF` means less
quantization and a sharper image; it does not override the maximum bitrate.
Values below about 19 are usually ineffective on this link because complex
scenes immediately reach the `720k` cap.

`PRESET=veryfast` spends more Pi CPU to obtain better quality per transmitted
bit than the original `ultrafast` profile. `H264_PROFILE=main` enables CABAC,
which also improves compression efficiency; B-frames remain disabled, so the
encoder does not add frame-reordering latency. Confirm FFmpeg reports
`speed` close to or above `1.0x`. If encoding cannot run in real time, use
`PRESET=superfast` or return to `OUT_FPS=15`.

At `MUXRATE=900k` and `PKT=752`, UDP/IP traffic entering the BBB is about
934 kbit/s. Use this only after iperf shows a clean 1 Mbit/s-class link. It
also remains within the 2 Mbaud STM32 companion link.

For more spatial resolution instead of more motion resolution, try:

```bash
INPUT_FORMAT=mjpeg SIZE=1280x720 FPS=15 \
  OUT_SIZE=854x480 OUT_FPS=15 \
  RATE_MODE=capped-crf CRF=22 PRESET=veryfast H264_PROFILE=main \
  BITRATE=740k BUFSIZE=370k MUXRATE=900k \
  bash ./tools/vlc_tx_video.sh
```

Use this second profile only if the camera advertises the requested MJPEG
capture mode. At this link rate, `854x480@15` and `640x360@20` are reasonable
alternatives; attempting 720p together with higher frame rate generally
increases quantization and looks softer despite the larger frame dimensions.

## Recommended startup order

1. Power the optical TX and RX frontends and verify alignment.
2. Connect and boot the RX Nucleo and RX Raspberry Pi.
3. Start `openvlc-rx.service` and verify that `tun0` exists.
4. Boot the BBB and run `install/setup_bbb_tx_router.sh`.
5. Boot the TX Raspberry Pi and run `install/setup_tx_pi.sh`.
6. Run the one-way iperf2 test before starting video.
7. Start `vlc_rx_view.sh` on the RX Pi and leave it waiting.
8. Start `tools/vlc_tx_video.sh` on the TX Pi.

## Stable video acceptance test

Run the following profile for at least ten minutes:

```bash
# RX Pi
journalctl -u openvlc-rx -f
```

```bash
# RX Pi, second terminal
MODE=stable bash ./vlc_rx_view.sh
```

```bash
# BBB
WINDOW=600 bash ~/raspberry/vlc_bbb_tx_stats.sh
```

```bash
# TX Pi
INPUT_FORMAT=mjpeg SIZE=640x360 FPS=15 \
  OUT_SIZE=640x360 OUT_FPS=15 \
  BITRATE=500k MUXRATE=650k \
  bash ./tools/vlc_tx_video.sh
```

Accept the profile only when the image remains continuous and the counters
listed above stay at zero. If the bridge is clean but the display still
stutters, the problem is player/decoder scheduling; increase receiver margin
with `FIFO_SIZE=131072`. If bridge gaps rise, fix the serial/STM32 path. If BBB
backpressure rises, lower `MUXRATE` to `600k`.

## STM32 host record format

All multibyte fields are big-endian:

```text
A5 5A C3
version      1 byte, currently 1
type         1 byte: 1 = IP datagram, 2 = UTF-8 log
sequence     2 bytes
length       2 bytes
payload      length bytes
CRC-16       2 bytes
```

CRC-16/CCITT-FALSE covers `version` through the end of `payload`. Sequence
numbers make queue drops, UART losses, and parser resynchronization visible.
The bridge can accept the previous payload-only-CRC format with
`--accept-legacy`, but the current firmware uses the format above.

## Failure isolation

1. `tun0` missing: inspect `systemctl status openvlc-rx`.
2. Serial CRC or sequence gaps rising: verify both sides use 2,000,000 baud and
   test a shorter USB cable or a direct UART designed for that baud rate.
3. Valid serial frames but no IP frames: inspect `invalid_ip` and confirm the
   BBB sends datagrams no larger than 900 bytes.
4. IP packets arrive but applications see nothing: verify
   `ip route get 10.0.0.2` selects `tun0` and check the RX Pi firewall.
5. IP packets arrive but video does not start: lower the encoder bitrate, wait
   for the next keyframe, and inspect the UDP stream with `tcpdump -ni tun0`.
6. Optical `sync` failures rise: solve the COMP1/AGC/timing issue before tuning
   ffmpeg; the companion path cannot repair an undecoded optical frame.
7. Direct BBB traffic turns on the LED but TX Pi traffic does not: this is a
   BBB forwarding problem, not an optical problem. Re-run both setup scripts,
   then inspect the first `FORWARD` rule and both interfaces:

   ```bash
   # TX Pi
   ip route get 192.168.0.2
   ping -c 2 10.0.0.1

   # BBB
   sysctl net.ipv4.ip_forward
   sudo iptables -nvL FORWARD --line-numbers
   sudo tcpdump -ni eth0 udp port 10001
   sudo tcpdump -ni vlc0 udp port 10001
   ```

   The TX Pi route must explicitly contain `via 10.0.0.1`. The setup script
   installs a `192.168.0.2/32` host route so it takes priority over any
   DHCP/VPN route using the same `192.168.0.0/24` subnet. It also removes a
   stale local `10.0.0.1` address, which can remain if the BBB router setup was
   accidentally executed on the TX Pi.

   The packet counter on the first `eth0 -> vlc0 ACCEPT` rule must increase.
   Packets visible on BBB `eth0` but absent from `vlc0` indicate routing or
   firewall rejection. Packets absent from BBB `eth0` indicate the TX Pi route,
   cable, or interface name is wrong.
