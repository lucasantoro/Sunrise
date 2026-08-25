# Raspberry Pi companion link

## Purpose

The STM32 receiver terminates the optical PHY and OpenVLC frame validation.
After a frame passes synchronization, length checks, Reed-Solomon processing,
or native CRC validation, its payload is the original Linux IP datagram
generated on the BeagleBone `vlc0` interface. The parser extracts source,
destination, and protocol fields, but those fields are not currently packet
acceptance gates.

The companion link transfers that datagram to a Raspberry Pi, where
`vlc_rx_bridge.py` injects it into `tun0`.

## Why 115200 baud was not sufficient

An 828-byte IP datagram occupies about 72 ms on an 8N1 UART at 115200 baud.
The old implementation performed three blocking UART writes inside
`openvlc_platform_on_packet()`, while TIM2 input capture was stopped. This
created an avoidable blind interval after every valid optical packet and
limited payload throughput to less than 100 kbit/s.

The companion profile now uses:

- USART3 at 2,000,000 baud;
- the validated BBB TX budget-50 / STM32 1000-kcell/s timing profile;
- a fixed-depth record queue;
- one contiguous serial record per queued payload;
- transmission from `openvlc_stm32_host_poll()` after TIM2/DMA restarts;
- record sequence numbers and CRC over the complete record body.

An 828-byte packet occupies about 4.2 ms at 2 Mbit/s. During that time TIM2 DMA
continues recording comparator edges. The main loop services the edge ring
before sending the next host record.

## Data ownership

```text
COMP1/TIM2 DMA
  -> edge burst
  -> OpenVLC decoder
  -> validated openvlc_packet_t
  -> host queue copy
  -> TIM2/DMA restart
  -> USART3 record
  -> Raspberry parser
  -> validated IPv4/IPv6 datagram
  -> tun0
```

The queue copies payload data because `openvlc_packet_t` is stack-owned by the
decoder. Queue overflow is represented by a sequence gap at the companion and
by the STM32 host-drop counter. Diagnostic logs never consume the final free
queue slot, so the decoded IP datagram following a success log has priority.

The periodic `COMP` record exposes `hostq`, `hostsent`, `hostdrop`, and
`hosterr`. A healthy companion path keeps `hostdrop=0` and `hosterr=0`.

## Operational limits

The UART is not the optical PHY and does not improve optical goodput. It must
only remain comfortably faster than the validated optical payload rate.
Datagrams must remain at or below `OPENVLC_MAX_PAYLOAD_BYTES` (900 bytes).
The recommended MPEG-TS UDP payload is 752 bytes, producing a 780-byte IPv4
packet.

Start capacity tests at 100 kbit/s and increase the offered UDP rate in small
steps. A high `iperf -b` value does not configure the optical PHY; it only
controls how quickly Linux fills the BBB transmit queue.

See `communications/openvlc/raspberry-gateway/README.md` for deployment.
