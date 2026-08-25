# Communications

Communication systems for Sunrise. At present that is one thing: the
visible-light link.

```text
communications/
  openvlc/       the VLC link — firmware, Raspberry Pi gateway, control panel
```

## openvlc

An OpenVLC-derived optical link carrying ordinary IP traffic. Two nodes, each a
Raspberry Pi with an STM32H723 Pi HAT, exchange packets over modulated light
and present the result to Linux as a `tun0` interface — so anything with an IP
address on one Pi reaches the other.

Manchester OOK at 1 Mbit/s, full duplex, framed with Reed-Solomon and a CRC.

| | |
|---|---|
| in service | `openvlc/firmware-v2/` |
| in development | `openvlc/firmware-v3/` |
| superseded | five earlier STM32 stages and a BeagleBone chain — not published |

Start at [`openvlc/README.md`](openvlc/README.md).

The boards are not here: they are in [`../hardware/`](../hardware/), beside the
mechanical design, because the PCB geometry and the optical housing are one
problem.
