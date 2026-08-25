# STM32H723 Pi HAT transceiver architecture

## Active hardware data paths

| Resource | Purpose |
|---|---|
| PB0 -> COMP1 IN+ | optical analog input |
| DAC1 CH1 -> COMP1 IN- | programmable slicing threshold |
| COMP1 -> internal TIM2 TI4 | comparator edge routing; no external jumper |
| TIM2 CH4 -> DMA1 Stream1 | both-edge timestamps into the circular RX ring |
| PE9 / TIM1 CH1 + DMA2 Stream0 | optical TX cell waveform |
| PB10 / PB11, USART3 | Raspberry Pi TX / RX at 2 Mbaud |

PC5 can expose COMP1_OUT only when `OPENVLC_COMP_DEBUG_OUTPUT=1`; it is off in
production to avoid needless switching beside the analog input. ADC1 and TIM6
are not part of the active receiver.

## RX pipeline

1. COMP1 slices PB0 against the DAC threshold in high-speed, low-hysteresis
   mode.
2. TIM2 captures both edges at 64 MHz (16 MHz in the 500-kbit/s profile) and
   DMA writes timestamps to a 128-KB non-cacheable circular ring.
3. The foreground poller segments bursts on the inter-frame gap and removes
   pulses shorter than the hardware/profile limit.
4. Every burst obtains its own robust preamble estimates: low-cell duration
   `t0`, high-cell duration `t1`, and average cell period `T`.
5. Each interval is quantized independently against those packet-local
   estimates. A conservative 1/16-cell boundary bias, validated on real
   captures, prevents borderline filter/comparator timing from inserting a
   false cell. Residual timing is measured but never fed into the next edge.
6. Strict preamble/SFD correlation fixes polarity and phase. If the normal
   decoder fails, a bounded fallback detects sustained Manchester pair errors
   and realigns by one cell. Isolated defects are left to RS, the declared
   length must fit the burst, and every recovered frame still requires full
   RS/CRC validation.
7. Timing anchors are learned across packets only from validated frames.

A missing comparator transition is lost information and cannot always be
reconstructed uniquely. The decoder tolerates timing movement and isolated
defects, but the analog chain must still produce a usable edge for nearly every
cell transition.

## TX and scheduling

Packets arriving from the Pi are parsed from the USART3 RX DMA ring, framed and
Manchester encoded directly into one of four TIM1 DMA slots. READY slots are
served by enqueue age, never by array index, preserving FIFO order under
full-duplex backlog. TIM1/DMA2 generates the PE9 waveform independently of CPU
scheduling; the 3-second boot waveform is disabled.

RX processing runs before bounded TX encoding in each foreground iteration.
The TIM2 ring is polled through DMA NDTR, so high-rate capture does not create a
DMA interrupt storm. DMA-written RX and UART rings are MPU non-cacheable; the
cacheable TX slots are cleaned before DMA starts.

## Host frame

Both serial directions use:

```text
A5 5A C3 | version | type | sequence | length | payload | CRC16-CCITT
```

One Raspberry bridge owns both the serial descriptor and TUN interface. Do not
run separate RX and TX services on the same UART.

## Protocol and modem

- [docs/wire_format.md](docs/wire_format.md) - line code, frame layout,
  addressing, Reed-Solomon, and the airtime budget that governs the rest.
- [docs/modem_design.md](docs/modem_design.md) - how the transmitter and
  receiver actually work, and why the comparator threshold decides whether
  the decoder has any margin to work with.
