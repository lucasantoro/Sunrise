# 50-MS/s capture validation — 2026-07-21

Inputs:

- `node_a.csv`: 742129 samples, 14.84256 ms;
- `node_b.csv`: 740001 samples, 14.80000 ms;
- sample interval: 20 ns (50 MS/s).

Both files contain two full optical frames. At 1.50 V, their main bursts are:

| Capture | Raw edges | After deglitch | Duration | Recovered timing |
|---|---:|---:|---:|---|
| node A packet 1 | 11786 | 11776 | 7.369 ms | 26/38, T=32 ticks |
| node A packet 2 | 11801 | 11787 | 7.364 ms | 37/27, T=32 ticks |
| node B packet 1 | 11804 | 11800 | 7.359 ms | 28/36, T=32 ticks |
| node B packet 2 | 11828 | 11784 | 7.359 ms | 28/36, T=32 ticks |

The polarity-specific durations always sum to 64 ticks, confirming a stable
32-tick cell at the 1-Mbit/s profile. Their separation shows substantial
duty-cycle/crossing distortion, but not a clock-rate error.

## Decoder A/B

The rejected DPLL residual carry decoded 0/4 captured packets. Independent
run quantization decoded 3/4 at 1.50 V. Applying a conservative decision bias
of `T/16` (2 TIM2 ticks here) decoded 4/4 with no Manchester bad pairs:

| Capture | Result | Payload | LQI |
|---|---|---:|---:|
| node A packet 1 | RS/CRC OK | 828 | 93 |
| node A packet 2 | RS/CRC OK | 828 | 89 |
| node B packet 1 | RS/CRC OK | 828 | 98 |
| node B packet 2 | RS/CRC OK | 828 | 96 |

An iperf UDP length of 800 bytes appears as 828 bytes at the TUN/optical layer
because the IPv4 and UDP headers add 28 bytes.

The final decoder recovered node A at every tested threshold from 1.30 through
2.30 V. Node B recovered both packets from 1.50 through 2.30 V at the tested
0.10-V points. This supports retaining the firmware's 1.50-V initial threshold
and scan-based adaptation; no fixed threshold change was required.

## Offline replay

`firmware/tests/capture_decode.c` reads a two-column analog CSV, linearly
interpolates threshold crossings, converts them to TIM2 ticks, applies the same
short-pulse cancellation as firmware, segments bursts, and invokes the real
`openvlc_rx_edges_to_packet()` implementation. Its CMake target is
`capture_decode_fast`.
