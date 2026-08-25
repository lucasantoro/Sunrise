# Sunrise

Drone-mounted visible-light communication. The goal is an optical link that a
drone can carry: modulated light instead of RF, with the mechanical, optical,
electronic and firmware work kept as separate areas that reference each other.

The link itself works and is documented in
[`communications/openvlc/`](communications/openvlc/): two nodes, each a
Raspberry Pi with an STM32H723 Pi HAT, carrying IP traffic over Manchester OOK
at 1 Mbit/s in both directions.

## Structure

```text
Sunrise/
  communications/     the optical link — firmware, gateways, control software
  hardware/           PCBs — LED driver, photodiode front end, power, Pi HAT
  mechanical_design/  CAD: mounts, optical housing
  simulations/        link, optical, channel and integration studies
  drone_software/     flight stack and companion-computer integration
  docs/               cross-area architecture and integration notes
  outputs/            generated artefacts
```

`hardware/` and `mechanical_design/` sit beside each other deliberately: the
optical housing and the board geometry are one problem. The current open issue
on the link — isolating a node's transmitter from its own receiver — needs
both.

## Where to start

| you want to | read |
|---|---|
| run the link at the bench | [`communications/openvlc/raspberry-gateway/NOTES.md`](communications/openvlc/raspberry-gateway/NOTES.md) |
| understand how it works | [`communications/openvlc/firmware-v2/docs/modem_design.md`](communications/openvlc/firmware-v2/docs/modem_design.md) |
| know what goes over the light | [`communications/openvlc/firmware-v2/docs/wire_format.md`](communications/openvlc/firmware-v2/docs/wire_format.md) |
| see the boards | [`hardware/README.md`](hardware/README.md) |

## Status

The link is in service and measured: about 830 kbit/s of delivered UDP each
way, 124.6 of 125 frames per second, ~0.30% loss after CRC and Reed-Solomon.

Its open problem is physical rather than software. With both nodes
transmitting, a node's own LED couples into its own photodiode strongly enough
to destroy reception — the transmitter occupies roughly 90% of the frame
period, so there is no dark window to receive in. The fix is optical isolation
between the two, which is where this area meets `hardware/` and
`mechanical_design/`.

## Licence

MIT — see [`LICENSE`](LICENSE).

The Arm CMSIS and STMicroelectronics driver trees under
`communications/openvlc/firmware-v*/Drivers/` are redistributed under their own
Apache-2.0 and BSD-3-Clause terms, and `hardware/` is board design data rather
than software. [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) sets out what
the MIT licence does and does not cover.
