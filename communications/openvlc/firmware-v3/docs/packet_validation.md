# STM32 Packet Validation

This document defines the packet acceptance rules used by the STM32 comparator
receiver. A packet is delivered only when every required validation layer
returns `OPENVLC_OK`.

The receiver supports two frame families:

- BeagleBone-compatible frames produced by the current Linux/PRU transmitter;
- native STM32 frames produced by `openvlc_frame_build()`.

The physical synchronization path is shared. The final integrity check differs
between the two frame families.

## Validation Pipeline

```text
COMP1 edge capture
  -> capture and burst integrity
  -> short-pulse cancellation
  -> half-cell timing estimate
  -> alternating raw-preamble gate
  -> bounded-distance SFD correlation and polarity resolution
  -> Manchester pair recovery
  -> length and buffer bounds
  -> complete-frame and byte-alignment checks
  -> frame-family detection
       -> BeagleBone: Reed-Solomon correction and validation
       -> native STM32: CRC-16/CCITT validation
  -> packet delivery
```

## 1. Capture And Burst Integrity

TIM2 CH4 timestamps both comparator edges at 64 MHz. The platform layer drains
the circular DMA ring and groups edges into bursts separated by an idle gap.

The platform rejects or records:

- DMA or burst-buffer overflow;
- partial bursts lost during capture restart;
- bursts that are too short to contain the configured preamble;
- comparator chatter represented by short pulse pairs.

`openvlc_edge_cancel_short_pulses()` removes both edges of a narrow pulse. It
does not remove only one edge, because doing so would invert every reconstructed
level after the glitch.

## 2. Timing Classification

`comp_estimate_preamble_timing()` finds the longest alternating-preamble
interval run, estimates separate one-cell durations for the two edge
polarities, and averages them into the nominal cell period. A whole-burst
histogram is used only as a fallback.

The decoder rejects a burst when it cannot obtain a plausible timing estimate
or when the nominal cell period is below
`OPENVLC_COMP_MIN_HALFCELL_TICKS`.

## 3. Preamble-Gated SFD Synchronization

The current interoperability profile uses:

```text
raw preamble: 8 bytes of 0xAA
SFD:          0xA3, Manchester-coded
```

The SFD is represented by 16 Manchester line cells. The receiver correlates
against both the expected pattern and its bitwise inverse. The inverse match is
valid because the optical polarity at the comparator output is not assumed.

An SFD match is accepted only when the immediately preceding
`OPENVLC_SFD_SYNC_PREAMBLE_CELLS` reconstructed cells alternate. The validated
default is 12 cells, while the eight-byte raw preamble provides up to 64
cells. The shorter gate tolerates distortion at the start of an AGC burst
without removing the preamble requirement.

This gate prevents an isolated `0xA3`-equivalent sequence in noise, payload
data, or an incomplete burst from opening the frame decoder. After the gate
succeeds, the SFD may contain at most
`OPENVLC_SFD_SYNC_MAX_CELL_ERRORS` mismatches, currently two.

The `SFDSYNC pre_rej` diagnostic counts SFD candidates rejected by this
preamble condition. `SFDSYNC sfderr` reports the Hamming distance of the
accepted SFD. Such a rejection is expected behavior, not a packet integrity
failure.

## 4. Manchester Validation

After SFD lock, the receiver groups reconstructed line cells into Manchester
pairs.

A valid pair contains one low and one high cell. Equal pairs indicate a damaged
cell or a phase error. The current decoder:

- emits the second cell as a best-effort bit for an isolated equal pair;
- counts every invalid pair for link quality;
- allows the later Reed-Solomon or CRC stage to validate the complete frame;
- drops synchronization when a consecutive invalid-pair run exceeds
  `OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS`.

This is intentionally tolerant for BBB frames with FEC. It does not by itself
make a packet valid; the packet must still pass all framing and integrity
checks.

## 5. Length, Bounds, And Completeness

The first 16 recovered bits after the SFD are the raw length field.

For native STM32 frames, this field is the payload length. The receiver checks:

- payload length does not exceed `OPENVLC_MAX_PAYLOAD_BYTES`;
- the expected frame size fits the frame buffer;
- the recovered frame ends on a byte boundary;
- the final byte count exactly matches the declared payload length.

For BeagleBone-compatible frames, the raw field contains the physical encoded
symbol length rather than the payload length. The receiver validates that the
value maps to:

- a supported raw preamble length;
- the configured Manchester expansion;
- a complete byte frame;
- a valid number of 200-byte Reed-Solomon data blocks;
- the required 16 parity bytes for every block.

Random or false length values therefore fail before packet delivery.

## 6. Structural Frame Checks

`openvlc_frame_parse()` checks the canonical frame representation:

- expected preamble bytes;
- expected SFD byte;
- minimum frame size;
- exact native frame size or a valid BBB-compatible frame size;
- data and parity placement within the supplied frame.

In the comparator path, the canonical preamble and SFD bytes are seeded after
the physical preamble-gated SFD lock. Their parser checks are structural
consistency checks, not a second independent observation of the optical
preamble.

## 7. BeagleBone Reed-Solomon Validation

The current BBB transmitter protects:

```text
destination(2) | source(2) | protocol(2) | payload
```

Data is divided into blocks of at most 200 bytes. Each block has 16
Reed-Solomon parity bytes, allowing correction of up to eight unknown byte
errors per block.

The STM32 parser:

1. reconstructs the BBB payload length from the complete frame size;
2. verifies the number and position of parity blocks;
3. runs Reed-Solomon decoding on every block;
4. applies successful byte corrections before extracting the packet;
5. rejects the frame if any block is uncorrectable.

The current BBB frame does not transmit a separate CRC-16. Reed-Solomon is the
final link-layer integrity and correction mechanism for this frame family.

For compatibility with the existing counters, an uncorrectable RS block
returns `OPENVLC_ERR_CRC` and increments `crc_failed`. The `crc` log field
therefore means "final integrity failure" for BBB traffic, not a literal CRC
mismatch.

## 8. Native STM32 CRC Validation

Native STM32 frames append a two-byte CRC-16/CCITT. The CRC covers:

```text
length | destination | source | protocol | payload
```

The received CRC must exactly match the calculated CRC. A mismatch returns
`OPENVLC_ERR_CRC`; no native packet is delivered after a CRC failure.

Native STM32 frames currently use CRC but do not add the BBB Reed-Solomon
parity layout.

## 9. Delivery And Counters

`openvlc_app_rx_edges()` delivers a packet only when the complete decoder returns
`OPENVLC_OK`.

The main counters mean:

| Counter | Meaning |
| --- | --- |
| `seen` | Edge bursts submitted to the decoder |
| `ok` | Packets that passed every required validation layer |
| `crc` | Native CRC mismatch or uncorrectable BBB Reed-Solomon block |
| `sync` | Timing, preamble, SFD, Manchester, length, bounds, or completeness failure |
| `quality` | Reserved quality-policy rejection counter |

For delivered packets, the comparator profile reports the measured normalized
timing residual (`tjit`, shown as `jitter` by some tools), invalid Manchester
pairs (`bad`), longest invalid-pair run (`badrun`), large timing outliers
(`tout`), Reed-Solomon corrections (`rs`), and a combined link-quality
indicator (`lqi`, shown as `score` by some tools). These values are calculated
after synchronization and remain diagnostic only. None replaces the final RS
or CRC integrity decision.

## Checks Not Currently Used As Acceptance Gates

The parser extracts `dst`, `src`, and `protocol`, but it does not currently
reject a packet because:

- the destination does not match `OPENVLC_ADDR_SELF_DEFAULT`;
- the protocol is unknown;
- the packet duplicates a previously delivered STM32 packet.

These are MAC or application policy checks, not PHY integrity checks. They can
be added above `openvlc_platform_on_packet()` without changing synchronization
or FEC behavior.

## Validation Test Matrix

| Test | Expected result |
| --- | --- |
| Idle receiver, no TX | No increase in `ok` |
| SFD-like sequence without alternating preamble | `pre_rej` may increase; no packet delivery |
| Damaged Manchester pairs within RS capability | BBB packet may be corrected and delivered with `rs > 0` |
| More than eight byte errors in one BBB RS block | `crc` increases; packet rejected |
| Native frame with one modified protected byte | CRC mismatch; packet rejected |
| Invalid or random length | `sync` increases; packet rejected |
| Frame truncated before all data/parity bytes | `sync` increases; packet rejected |
| Valid BBB frame | `ok` increases; host-forward builds expose quality on `COMP qvalid=1`, debug builds may also print `VLC_RX packet` |

## Relevant Source Files

| File | Responsibility |
| --- | --- |
| `firmware/platform/stm32h7/openvlc_stm32_hal.c` | DMA drain, burst splitting, capture diagnostics |
| `firmware/openvlc/src/openvlc_phy.c` | Timing recovery, preamble gate, SFD lock, Manchester and length state machine |
| `firmware/openvlc/src/openvlc_frame.c` | Frame-size validation, BBB RS correction, native CRC-16 validation |
| `firmware/openvlc/src/openvlc_app.c` | Final counters, quality reporting, packet delivery |
