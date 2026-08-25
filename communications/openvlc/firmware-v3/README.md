# OpenVLC STM32H723 full-duplex transceiver - Raspberry Pi HAT (v2)

This directory is the maintained Pi-HAT firmware variant. Its active receiver
is comparator-edge based; sampled ADC and software-timer RX paths are not used.

## v2 deltas vs the original

| Change | Where | Why |
|---|---|---|
| Packet-local timing recovery | `openvlc_phy.c` | Every burst estimates `t0`, `t1`, and `T`; independent run quantisation with a profile-scaled 1/16-cell bias avoids propagating a defective crossing. |
| Pair-aware cell timing | `openvlc_phy.c` | If two adjacent comparator intervals still total two cells, the decoder applies the only possible `1+1` decomposition instead of inserting a false cell when one crossing moves. |
| CRC-gated differential fallback | `openvlc_phy.c` | In the same edge pass, retain the independent event-timing interpretation in a compact bit stream; parse it only after a complete primary frame fails CRC (`m=51` on recovery). |
| Deadline-bound recovery | `openvlc_phy.c`, `openvlc_stm32_hal.c` | The measured packet cadence limits complete decoder passes; a damaged frame cannot consume several following 8 ms periods and collapse the capture ring. |
| Bounded timing training | `openvlc_phy.c` | Timing is estimated from the first 1536 edges, which retain the validated 37/27-tick training run without scanning the complete payload twice. |
| Fast verified Reed-Solomon | `openvlc_frame.c` | Data and parity remain correctable and are rechecked, but precomputed syndrome transitions replace per-byte GF log/modulo work. Tables are initialized before capture starts. |
| Constant-time compatibility length | `openvlc_phy.c`, `openvlc_frame.c` | Payload length is resolved from at most five possible RS block counts instead of scanning all 901 payload sizes twice per frame. |
| Length-vs-burst sanity check | `openvlc_phy.c` (after length accept) | A corrupted 16-bit length that cannot fit in the burst's remaining cell budget is rejected immediately (relock) instead of consuming thousands of cells in the wrong phase. |
| Lean compact instrumentation | `OPENVLC_DIAGNOSTIC_LEVEL` | Compact health logs no longer enable a second edge pass or per-edge residual tracking; the edge-rate counter is accumulated once per polling visit instead of with a volatile store per edge. |
| Decimated quality estimator | `OPENVLC_RX_QUALITY_DECIMATION` | Link jitter/LQI remains available without executing a 64-bit timing-square for every received interval. |
| RX idle-time burst finalization | `OPENVLC_RX_IDLE_TIMEOUT_FLUSH` | Double-checks the DMA head around the timer sample, then starts decoding at the end of the current burst instead of waiting for the first edge of the next frame. |
| Corpus-bounded RX delimiter | `OPENVLC_EDGE_GAP_US` | At 1 Mbit/s the single 6-us delimiter retains the measured 3.86-us in-frame maximum without the ring pressure caused by a speculative full decode or the frame merging observed at 8 us. |
| Bounded TX inter-frame pacing | `OPENVLC_STM32_TX_INTERFRAME_GUARD_US` | READY slots retain FIFO order but cannot be chained with only the 32-cell electrical tail; the 400 us guard smooths Linux/UART bursts while retaining capacity for 828-byte packets at 125 fps. |
| Duty-servo target trim | `openvlc_stm32_hal.c` (`comp_duty_target_trim`), `OPENVLC_COMP_DUTY_TRIM*` in `openvlc_board.h` | duty=50% is the DC mean, but the measured impossible-run minimum sits 1-2 steps off centre (asymmetric hysteresis/propagation). A slow hill-climb moves the servo setpoint to minimise `longps` - the direct measure of lost transitions. Logs `COMP TRIM ...`. |
| `.ioc` hysteresis aligned to `COMP_HYSTERESIS_LOW` | `stm32.ioc` | `comp.c` was already LOW but the `.ioc` still said HIGH: a CubeMX regeneration would have silently reverted it. (The ORIGINAL project still has this trap.) |

The earlier integer timing IIR, DPLL residual carry, and experimental local
timing grid were removed: real and live Pi-HAT traffic showed that they could
propagate a local crossing defect through otherwise valid packet timing. Run
decisions therefore remain independent.

---

This is the pin-remapped Raspberry Pi HAT variant of the validated comparator
receiver and STM32 transmitter. It preserves the OpenVLC PHY, framing,
2 Mbaud host protocol, DMA queues, symbol profiles, and 192 MHz TX timer clock.

## Hardware

Target: **custom Raspberry Pi HAT / STM32H723VET6 (LQFP100)**

For the autonomous `OWC_TX` boot waveform and fault/heartbeat LED meanings,
see [docs/pi_hat_tx_diagnostics.md](docs/pi_hat_tx_diagnostics.md).

| Function | MCU pin | Peripheral |
|---|---|---|
| RX analog input | PB0 | COMP1_INP / `COMP_INPUT_PLUS_IO1` |
| RX capture | internal | COMP1_OUT -> TIM2_TI4 mux |
| RX diagnostic output | PC5 | optional; set `OPENVLC_COMP_DEBUG_OUTPUT=1` |
| RX threshold | internal | DAC1_CH1 -> COMP1_INM |
| TX modulation | PE9 | TIM1_CH1, AF1, low slew rate |
| TX alternate-path disable | PB5 | GPIO low |
| Raspberry serial TX/RX | PB10 / PB11 | USART3_TX / USART3_RX, AF7 |

No PC5-to-PB11 jumper is required. The firmware selects the internal
TIM2_TI4_COMP1 route with `HAL_TIMEx_TISelection()`, leaving PB11 available for
USART3_RX. RX and TX optical front ends must share ground with the STM32 and
Raspberry Pi. See [`docs/pi_hat_port.md`](docs/pi_hat_port.md) for wiring and
bring-up checks. Pin references in inherited architecture documents are
superseded by the table above.

## Runtime architecture

- RX edges: COMP1 -> TIM2_CH4 -> DMA1 Stream1 -> 128 KB circular ring.
- TX cells: TIM1_CH1, with TIM1_CH4 requesting DMA2 Stream0 updates.
- Raspberry input: USART3 RX -> DMA1 Stream2 circular buffer.
- Raspberry output: USART3 TX interrupt queue.
DMA1 is intentionally reserved for the latency-sensitive comparator capture
path. Putting the high-rate TIM1 cell stream on DMA1 causes TIM2 overcapture
and deterministic RX loss during simultaneous TX/RX operation.
TIM1 belongs exclusively to the hardware TX generator. ADC1 and TIM6 are not
used for sampling; TIM6 is reserved as the 1 MHz one-shot that releases a
queued TX slot after the inter-frame guard.

The serial interface is electrically full duplex. At 2 Mbaud, each direction
has its own 2 Mbaud line capacity; RX and TX traffic are not added onto one
half-duplex budget.

## Runtime diagnostics

Set only `OPENVLC_DIAGNOSTIC_LEVEL` in `Core/Inc/openvlc_board.h`:

| Level | Intended use | Runtime cost |
|---:|---|---|
| `0` | deployment and iperf | no periodic STM32 logs, no diagnostic burst scan, no TX hardware monitor |
| `1` | link health | one-second RX/TX summaries and heartbeat text |
| `2` | fault investigation | register snapshots, decoder detail, TX pin probe and 10 kHz TX monitor |

Level `1` is the current validation default; select level `0` for the final
minimum-overhead deployment image. Comparator auto-threshold and duty-servo control are
independent of logging: if either feature is enabled, its control loop still
runs with diagnostic text disabled.

At diagnostic levels 1 and 2, the compact `COMP` record also reports the raw
TIM2 interval rates before software deglitching: `r07`, `r811`, `r1215`,
`r1619`, `r2023`, and `r24p`. The names denote tick ranges (for example,
`r1619` is the number of consecutive raw edges per second separated by 16--19
ticks); `r24p` is the remainder at 24 ticks or longer. These bins distinguish
comparator/filter pulse loss from pulses rejected by the software gate without
changing the deployed level-0 hot path.

`OPENVLC_RX_CAPTURE` is also independent of periodic diagnostics. When
enabled, it builds a spaced, balanced good/failure population. Version-2 files
preserve raw pre-filter comparator intervals, and the Raspberry bridge saves a
validated `.bin` plus `.json` for deterministic offline replay.
See [RX signal capture and offline replay](docs/rx_failure_trace.md).
See [bounded RX list decoding](docs/rx_list_decoder.md) for the CRC-gated
multi-slip recovery used by the 1-Mbit/s Pi-HAT profile.
The deterministic [synthetic channel campaign](docs/synthetic_channel_tests.md)
tests the decoder against timing drift, jitter, duty-cycle distortion, extra
and missing edges, glitches, clipped captures, and truncated frames.

## Build

Import STM32CubeIDE/ as project stm32-transceiver-pi-hat and build Debug.
The build is fixed to STM32H723xx; selecting an H743 target fails at compile
time.

On Windows, import the project through a local or mapped-drive path such as
`Z:\Projects\Sunrise\...`. STM32CubeIDE's generated makefiles do not resolve
linked resources correctly when the project is imported directly from a UNC
path such as `\\server\share\...`.

The linker enforces the real H723 memory limits:

- Flash: 512 KB
- DTCM: 128 KB
- AXI SRAM: 320 KB
- D2 SRAM: 32 KB

Current Debug allocation is approximately 55 KB Flash, 76 KB DTCM, 287 KB AXI
SRAM, and 8 KB D2 SRAM.

## Raspberry bridge

Use the single bidirectional bridge. Do not run the standalone RX and TX
services against the same serial port.

On each Raspberry Pi, first disable the serial login console and enable the
hardware UART with `sudo raspi-config`, then reboot. Install the GPIO-UART
profile from `communications/openvlc/raspberry-gateway`:

~~~bash
# Node A (.1, peer .2)
sudo ./install_transceiver_service.sh --node a --pi-hat

# Or node B (.2, peer .1)
sudo ./install_transceiver_service.sh --node b --pi-hat
~~~

The installer selects `/dev/serial0`, requires 2 Mbaud, and runs
`check_pi_hat_uart.sh`. It deliberately rejects the mini-UART (`ttyS*`) and an
active serial console. This avoids `auto` selecting the connected ST-LINK VCP.

~~~bash
sudo python3 ./vlc_transceiver_bridge.py \
  --port /dev/serial0 \
  --baud 2000000 \
  --dev tun0 \
  --ip 192.168.0.2/24 \
  --peer-ip 192.168.0.1 \
  --source-route 10.0.0.0/24 \
  --mtu 900
~~~

A service template is provided as openvlc-transceiver.service. These defaults
configure the current transceiver as node `.2`, interoperable with the legacy
BBB TX path. A second transceiver at the opposite end must use `.1/24`, peer
`.2`, and a source route appropriate to its local network.

By default the bridge forwards TUN egress to the STM32 TX only when the IP
destination is the configured optical peer (`192.168.0.1/32` on node `.2`).
This prevents iperf server feedback and kernel ICMP traffic addressed to the
legacy `10.0.0.0/24` source network from activating the LED while receiving.
Use repeated `--tx-allow CIDR` options for additional remote networks, or
`--forward-all` only when the optical link is intended to be a general router.
