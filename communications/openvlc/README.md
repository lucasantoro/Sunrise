# OpenVLC

A visible-light IP link. Two nodes, each a Raspberry Pi carrying an STM32H723
Pi HAT, exchange Linux IP traffic over modulated light — Manchester OOK at
1 Mbit/s, full duplex, presented to Linux as an ordinary `tun0` interface.

```text
   Pi node A                                   Pi node B
   tun0 192.168.0.1                            tun0 192.168.0.2
        |                                            |
   UART 2 Mbaud                                 UART 2 Mbaud
        |                                            |
   H723 Pi HAT      ~~~~~ modulated light ~~~~~  H723 Pi HAT
   optical addr 7      TIM1 TX / COMP1 RX        optical addr 8
```

Anything with an IP address on one Pi can reach the other. The bench runs
`iperf` and streams video over it.

## State

| | |
|---|---|
| **In service** | [`firmware-v2/`](firmware-v2/) |
| Best measured | 124.6 of 125 frames/s delivered, ~0.30% loss, ~830 kbit/s each way |
| **In development** | [`firmware-v3/`](firmware-v3/) — not deployed |
| Superseded | five earlier STM32 stages and the BeagleBone chain — not published |

**Open issue, physical:** with both nodes transmitting, a node's own LED
couples into its own photodiode strongly enough to destroy reception. The
transmitter occupies ~90% of the frame period, so there is no dark window to
hide in. This is being fixed with optical isolation, not in software — see
`firmware-v2/docs/two_transceiver_test.md`.

## Layout

| folder | runs on | what |
|---|---|---|
| [`firmware-v2/`](firmware-v2/) | STM32H723 Pi HAT | **the firmware in service** — comparator RX, TIM1 TX, framing, RS, CRC |
| [`firmware-v3/`](firmware-v3/) | STM32H723 Pi HAT | streaming-decoder experiment; carries a faithful port of the reference implementation for A/B work |
| [`raspberry-gateway/`](raspberry-gateway/) | Raspberry Pi | the bridge: `tun0` ↔ UART, pacing, capture collection, install scripts |
| [`control-app/`](control-app/) | lab PC | PySide6 panel driving the nodes over SSH, live telemetry, diagnostics |



The link went through five earlier STM32 stages and a BeagleBone-transmitter
chain before this one. They are not published: they are superseded, and none of
them builds against the current hardware. One thing from that lineage does
survive here — the frame layout and Reed-Solomon scheme are the upstream
OpenVLC BeagleBone protocol, which `OPENVLC_BEAGLEBONE_COMPAT` selects.

The boards these run on are one level up, in [`../../hardware/`](../../hardware/):
LED driver, photodiode front end, interconnect, power and the STM32 Pi HAT
carrier. They live beside `mechanical_design/` because the optical housing and
the PCB geometry are one problem, not two.

## Start here

**At the bench**, on a Pi:
[`raspberry-gateway/NOTES.md`](raspberry-gateway/NOTES.md) — the commands, how
to enable and save logs, and the meaning of every field in the `COMP`, `TX` and
`trx-bridge` diagnostic lines, plus the failure signatures that are worth
recognising on sight.

**Understanding the link:**

- [`firmware-v2/docs/wire_format.md`](firmware-v2/docs/wire_format.md)
  — line code, frame layout, addressing, Reed-Solomon, and the airtime budget
  that governs most of the system's behaviour
- [`firmware-v2/docs/modem_design.md`](firmware-v2/docs/modem_design.md)
  — how the transmitter and receiver work, and why the comparator threshold
  decides whether the decoder has any margin at all
- [`firmware-v2/docs/comparison_reference_impl.md`](firmware-v2/docs/comparison_reference_impl.md)
  — v2 against a colleague's 884-line implementation of the same link, measured
  side by side

**Bringing up two nodes:**
[`firmware-v2/docs/two_transceiver_test.md`](firmware-v2/docs/two_transceiver_test.md)

## Node naming

Hosts are named `nodeA`, `nodeB`, … and the Pi side derives everything from
that: node letter, tun address (`nodeC` → `192.168.0.3`), and the tag written
into capture filenames. Adding a node means naming the host and running the
installer.

The optical address is separate and set at firmware build time
(`OPENVLC_TRANSCEIVER_NODE`). A receiver discards frames carrying its own
address as source, so **the two boards must never be flashed with the same node
firmware**.

## Quick start

Install the bridge on each Pi:

```bash
cd raspberry-gateway && sudo bash ./install/install_transceiver_service.sh
```

Check the interface came up and the path carries a full-size frame:

```bash
ip -br addr show tun0 && ping -c 5 -M do -s 872 192.168.0.2
```

Measure one direction:

```bash
iperf -u -c 192.168.0.2 -b 800k -l 800 -p 10001 -t 60 -i 1
```

Watch what the modem is doing while it runs:

```bash
journalctl -u openvlc-transceiver -f | grep -E "COMP|TX uart|trx-bridge"
```

Reading those numbers is [`raspberry-gateway/NOTES.md`](raspberry-gateway/NOTES.md).

## A note on the numbers

Loss figures in this repository are counted **after** CRC and Reed-Solomon, so
a "lost" frame is one that failed integrity, not one that arrived corrupt and
was passed on. Implementations without an integrity check report a different
quantity under the same name; the comparison document above goes into why that
makes throughput numbers across implementations incomparable.
