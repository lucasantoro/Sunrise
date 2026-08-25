# OpenVLC modular main board proposal

This note defines a manufacturable direction for splitting the current OpenVLC
hardware into three boards:

- `TX_MODULE`: optical transmitter daughterboard.
- `RX_MODULE`: optical receiver daughterboard.
- `MAIN_BOARD`: power, host/control, protection, service connectors, and
  interconnect between TX/RX.

## Source revisions used

Current hardware in this repository:

| Function | Current source | Fab/output package | Notes |
| --- | --- | --- | --- |
| TX | `tx/PCB_TX_V2.2_A` | `Project Outputs for PCB_TX_V2.2_A.zip` | Current transmitter; ODB++ package present. |
| RX | `rx/PCB_RX_V2.7` | `P-17464_2025-12-13_IMDEA_PCB_RX_V2.7.zip` | Current receiver; manufactured package present. |
| Power | `power/PCB_PDB_V1.2` | `Project Outputs for PCB_PDB_V1.2.zip` | Current PDB. |
| STM32/Pi carrier | `stm32-pi-hat/STM32_Pi_HAT` | `P-18729_2026-05-29_IMDEA_STM32_Pi_HAT.zip` | Current host carrier. |

The source files are Altium `.SchDoc/.PcbDoc`. Gerbers for a new design should
be regenerated from Altium after schematic/layout work and DRC/ERC. The current
workspace does not include an EDA CLI capable of modifying Altium files or
producing verified new Gerbers directly.

## Current board facts

Measured from Gerber/ODB extents:

| Board | Approx. size |
| --- | ---: |
| `PCB_TX_V2.2_A` | 85.0 mm x 54.6 mm |
| `PCB_RX_V2.7` | 85.0 mm x 54.6 mm |
| `PCB_INTER_V1.2` | 48.0 mm x 54.6 mm |
| `PCB_PDB_V1.2` | 100.0 mm x 100.0 mm |
| `STM32_Pi_HAT` | 175.1 mm x 124.4 mm |

Important TX nets/components identified:

- Rails/signals: `+13.8V`, `+5V`, `+3.3V`, `GND`, `P8_45`, `PS_ON`,
  `CS_PD`, `MISO_PD`, `MOSI_PD`, `LED`, `HeatSink`.
- Core TX parts: `LM3409QMY/NOPB`, `LZ4-04MDCA-0000`, `SLF12575T-330M3R2-PF`,
  `PMEG3050EP`, `FDC6333C`, `DMT67M8LSS-13`, `ZXMP7A17KTC`,
  `SN74AHC1G126DBVR`, current-sense resistor `CRA2512-FZ-R075ELF`.

Important RX rails/signals identified:

- Rails/signals: `+5V`, `-5V`, `VCC`, `GND`, `VREF`, `VAGC`, `MODE`, `OFSN`,
  `AGC_P`, `AGC_N`, `LNA_P`, `LNA_N`, `BPF_P`, `BPF_N`, `LPF_IN`, `LPF_OUT`,
  `GAIN_OUT`, `ADC_IN`, `CS_PD`, `MISO_PD`, `MOSI_PD`, `+IP`, `-IP`,
  `+VT`, `-VT`, `+VB`, `-VB`, `+eOP`, `-eOP`.
- Core RX parts visible in schematic: `ADA4817-2`, `AD80xx` gain/filter
  stages, `ADS788x` ADC, `LM2661` charge pump, trim pots, analog filters and
  bias networks.

## Proposed partition

### TX_MODULE

Keep the high-current optical transmitter loop on the TX module:

- LED and optics/mechanical alignment.
- LM3409 LED driver, switch MOSFET, diode, inductor, current-sense resistor,
  compensation network, and local high-frequency decoupling.
- Gate/control buffer only if needed to preserve PWM edge quality at the
  driver.
- Local temperature/fan connector if mechanically tied to the LED/heatsink.

Move to `MAIN_BOARD`:

- BeagleBone/Pi/STM32 headers and host-specific wiring.
- Global `+13.8V`, `+5V`, `+3.3V` generation and power sequencing.
- Bulk input protection, fuse/eFuse, reverse polarity protection, power switch,
  service/test connectors.
- Any debug-only test headers not required at the optical module.

Reason: the LED switching current loop must stay physically compact. Moving the
LM3409 power stage to the main board and routing LED current over a connector
would increase EMI and optical-current ringing.

### RX_MODULE

Keep the low-noise receive chain on the RX module:

- Photodiode and optical/mechanical interface.
- First transimpedance/low-noise amplifier stage.
- Critical gain/filter stages whose nodes are `LNA_*`, `BPF_*`, `LPF_*`.
- ADC or comparator/slicer close to the analog front-end unless the main board
  can guarantee a shielded, controlled-impedance short analog path.
- Local analog decoupling, guard ring, and bias/reference filtering.

Move to `MAIN_BOARD`:

- Host MCU/Raspberry Pi/BeagleBone carrier functions.
- SPI/UART/control routing and level shifting.
- Global rails and power supervision.
- Non-critical debug headers.

Preferred RX interface is digital after ADC/comparator. Exporting `ADC_IN` as
an analog signal across a board-to-board connector is possible but should be
treated as the fallback option because it is the most noise-sensitive boundary.

### MAIN_BOARD

Integrate the current PDB and STM32/Pi carrier roles:

- Input power connector, fuse/eFuse, reverse polarity, TVS, current monitoring.
- Buck rail generation: `+13.8V` distribution, `+5V`, `+3.3V`.
- Quiet analog rail generation/filtering for RX: `+5VA`, `-5VA`, `VREF`.
- MCU/host connector and firmware/debug access.
- Board-to-board connectors for one or more TX/RX modules.
- Star-ground strategy and physical zoning for power, digital, and analog.

## Proposed module interfaces

Use locking board-to-board or cable connectors with ground pins interleaved
between noisy or sensitive signals. Do not reuse generic 2.54 mm headers for
final flight/field hardware unless mechanical retention is added.

### TX connector

Minimum signals:

| Pin group | Signal |
| --- | --- |
| Power | `+13.8V_LED`, multiple pins |
| Power return | `PGND_LED`, multiple pins adjacent to `+13.8V_LED` |
| Logic | `TX_PWM` / current modulation input |
| Control | `TX_EN` / `PS_ON` |
| Telemetry | `TX_FAULT`, `TX_TEMP` optional |
| Service | `SCL/SDA` or spare GPIO optional |
| Shield | Chassis/shield if cable is used |

Keep `+13.8V_LED` and `PGND_LED` connector current rating at least 2x the
expected continuous LED current, with derating for temperature.

### RX connector

Preferred digital boundary:

| Pin group | Signal |
| --- | --- |
| Analog power | `+5VA`, `-5VA`, `AGND` |
| Digital power | `+3V3D` or `+5VD`, `DGND` |
| Data | `RX_SPI_SCK`, `RX_SPI_MISO`, `RX_SPI_MOSI`, `RX_CS` |
| Timing | `RX_SAMPLE_CLK` or `RX_DATA_READY` |
| Control | `RX_MODE`, `RX_GAIN`, `RX_EN` |
| Reference/monitor | `VREF_MON`, `RSSI/GAIN_OUT` optional |

Fallback analog boundary:

- `ADC_IN` must be surrounded by ground pins, routed short, shielded, and kept
  away from TX power routing.
- Add an RC input filter and clamp at the main-board ADC input.

## EMC, noise, and layout constraints

- Use at least a 4-layer stackup for the main board: signal, solid GND, power,
  signal. Prefer 6 layers if TX current and RX analog coexist on the same PCB.
- Physically separate zones: TX power stage, digital host/MCU, RX analog.
- Put RX analog at the quiet edge of the board. Put TX power at the opposite
  edge. Do not route LED current below the RX module.
- Use a single low-impedance ground system, but route high-current LED return
  directly back to the input/power stage so it does not share narrow copper with
  RX analog returns.
- Add ferrite beads or small LC filters at RX module power entry:
  `+5V_MAIN -> +5VA_RX`, `-5V_MAIN -> -5VA_RX`.
- Keep switching converters, charge pumps, inductors, and fast LED nodes away
  from photodiode/TIA input. If `LM2661` remains on RX, place it in its own
  noisy island with filtering and guard spacing from the photodiode input.
- Route SPI/control to RX with series damping resistors near the driver and
  ground reference pins beside the clock/chip-select.
- Route TX PWM/enable with series damping near the MCU/buffer. Avoid long,
  unterminated fast edges into the LED driver.
- Add TVS/ESD at external connectors, not at the sensitive photodiode node.
- Place test points for `+13.8V`, `+5V`, `+3.3V`, `+5VA`, `-5VA`, `TX_PWM`,
  `TX_EN`, `RX_CS`, `RX_MISO`, `ADC_IN`/digital RX output, and `GND`.

## Gerber/schematic generation path

1. Clone `PCB_TX_V2.2_A` and `PCB_RX_V2.7` into new Altium projects:
   `TX_MODULE_V1`, `RX_MODULE_V1`, `MAIN_BOARD_V1`.
2. Remove host/header/global-power circuitry from TX/RX according to the
   partition above.
3. Add explicit board-to-board connector symbols with locked pinout tables.
4. Move PDB and STM32/Pi carrier functions into `MAIN_BOARD_V1`.
5. Compile all three projects and run ERC.
6. Push ECO from schematic to PCB for each board.
7. Layout with the EMC constraints above; run DRC.
8. Generate Gerber X2, NC drill, ODB++, pick/place, BOM, and STEP.
9. Before fabrication, review Gerbers in an independent viewer and check:
   board outline, drill registration, polarity, solder mask openings,
   connector pin-1 orientation, and TX/RX mating orientation.

## Open decisions before layout

- Number of TX/RX module slots: single link, dual link, or multi-TX array.
- Host architecture: keep STM32 + Raspberry Pi HAT, or replace with a custom
  MCU section on the main board.
- RX boundary: digital after ADC/comparator, or analog `ADC_IN` to main board.
- Mechanical stack: mezzanine modules, cabled modules, or edge-mounted modules.
- LED thermal solution and whether fan control is local to TX or on main board.
