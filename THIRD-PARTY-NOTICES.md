# Third-party components

The MIT licence in [`LICENSE`](LICENSE) covers the work in this repository.
It does **not** cover the vendor components redistributed alongside it, which
keep the licences their authors granted. Each one carries its own `LICENSE.txt`
in place; those files are authoritative.

| component | in | licence |
|---|---|---|
| Arm CMSIS (Core, DSP, Device headers) | `communications/openvlc/firmware-v*/Drivers/CMSIS/` | Apache-2.0, Arm Limited |
| STM32H7xx HAL and LL drivers | `communications/openvlc/firmware-v*/Drivers/STM32H7xx_HAL_Driver/` | BSD-3-Clause, STMicroelectronics |
| STM32H7xx CMSIS device support | `communications/openvlc/firmware-v*/Drivers/CMSIS/Device/ST/STM32H7xx/` | Apache-2.0, STMicroelectronics |

These are kept in the tree deliberately, so the firmware builds from a fresh
clone without chasing versions. Redistributing this repository means honouring
their terms as well as the MIT licence above.

## Protocol lineage

The frame layout and the Reed-Solomon scheme the firmware speaks come from the
upstream OpenVLC BeagleBone protocol, selected in the firmware by
`OPENVLC_BEAGLEBONE_COMPAT`. The implementation here is original; the wire
format it interoperates with is not.

## Hardware

`hardware/` holds PCB design files, not software. The MIT licence is a software
licence and is a poor fit for board designs — if these are to be reused, a
hardware licence such as CERN-OHL states the intent far better.
