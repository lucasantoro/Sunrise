# PCB_RX_V2.7 schematic analysis

Scope: latest RX design in `communications/openvlc/hardware/rx/PCB_RX_V2.7`.

Primary sources inspected:

- `PCB_RX.SchDoc`
- `PCB_RX.PcbDoc`
- `P-17464_2025-12-13_IMDEA_PCB_RX_V2.7.zip`
- STM32 RX documentation in `communications/openvlc/stm32-rx/docs/`
- BeagleBone hardware notes in
  `communications/openvlc/beaglebone-reference/docs/hardware/README.md`

## Executive conclusion

`PCB_RX_V2.7` is a capable analog optical front-end, but it is still shaped as
a BeagleBone/OpenVLC cape-era board. For the new modular architecture, it should
not be copied as-is. The main improvements are:

1. Remove BeagleBone P8/P9 dependency from the RX module.
2. Define a clean RX module connector around the actual chosen system boundary.
3. Keep the photodiode, first low-noise stage, AGC/filtering, and final
   slicer/ADC close together on the RX module.
4. Rework power so analog rails are quiet and separated from TX current loops.
5. Decide whether the final product uses the current STM32 comparator path or
   keeps legacy external ADC/SPI support.

The current STM32 receiver baseline does **not** use the external `ADS7886`
SPI ADC path. It uses a conditioned analog AGC/Vin signal into STM32 `PB2 /
COMP1_INP`, sliced against the STM32 DAC threshold and timestamped by `TIM2`.
That means the new RX module can be simpler than `PCB_RX_V2.7` if STM32 is the
target.

## Observed RX blocks

Extracted from the Altium schematic records:

| Block | Components / nets observed |
| --- | --- |
| Optical input | `PD` = `SFH213` photodiode |
| Low-noise input | `U1` = `ADA4817-2ACPZ-RL`, nets including `+IP`, `-IP`, `+VT`, `-VT`, `+VB`, `-VB`, `+eOP`, `-eOP` |
| Variable gain / AGC | `U1C` = `AD8338ACPZ-R7`, nets `VAGC`, `GAIN_OUT`, `MODE`, `OFSN`, `VREF` |
| Filter/gain stages | `U2C` = `OPA863ADBVR`, `U2`/`U?` = `AD8066ARZ-R7`, nets `LNA_P/N`, `BPF_P/N`, `LPF_IN/OUT` |
| Differential / ADC driver | `U1I` = `AD8139ARDZ-REEL` |
| External ADC | `ADC` = `ADS7886SBDBVR`, nets `ADC_IN`, `CS_PD`, `MISO_PD`, `MOSI_PD` |
| Analog rails | `+5V`, `-5V`, `VCC`, `GND`; `U1B` = `LM2663M/NOPB` charge pump; `U1A` = `TLV76150DCYR` |
| Legacy host interface | `P8`, `P9` = BeagleBone headers, plus pin-swap notes |
| Debug/tuning | Many 2-pin/3-pin headers and 50 k trimmers around bias, threshold and gain stages |

Board size from Gerber extents: approximately `85.0 mm x 54.6 mm`.

## Fit against the current STM32 receiver

The active STM32 firmware path is:

```text
photodiode / analog frontend / AGC
  -> voltage-limited AGC/Vin
  -> STM32 PB2 / COMP1_INP
  -> COMP1
  -> TIM2 input capture
  -> edge-timing decoder
```

The STM32 docs explicitly say the current comparator path does not use the
external ADC `SDO/MISO` line. Therefore, `ADS7886`, its SPI wiring, and part of
the BeagleBone header interface are legacy compatibility features, not required
for the current STM32 baseline.

## Recommended RX module boundary

### Preferred boundary for the new modular system

Keep on `RX_MODULE`:

- Photodiode and mechanical/optical alignment.
- First low-noise transimpedance/front-end amplifier.
- AGC or fixed-gain conditioning required to produce a clean waveform.
- Comparator/slicer **or** ADC, depending on final acquisition architecture.
- Local analog reference filtering and final output protection.

Move to `MAIN_BOARD`:

- BeagleBone P8/P9 headers.
- STM32/Pi/host interface.
- Bulk power conversion and protection.
- Service/debug connectors that do not need to live beside the photodiode.

For the STM32 comparator baseline, the best module output is either:

```text
RX_ANALOG_OUT_0V3V3 -> STM32 PB2 / COMP1_INP
```

or:

```text
RX_DIGITAL_EDGE -> STM32 timer/capture-capable GPIO
```

The first option keeps threshold control in STM32 DAC/COMP1. The second option
puts the comparator on the RX module and exports a digital edge stream.

### Compatibility boundary

If compatibility with BeagleBone ADC/SPI receive is still needed, keep the ADC
on the RX module and export digital SPI:

```text
RX_SPI_SCK
RX_SPI_MISO
RX_SPI_MOSI
RX_CS
RX_DRDY or RX_SAMPLE_CLK if added
```

Do **not** export a high-impedance analog photodiode/TIA node across the module
connector.

## Concrete improvements

### 1. Remove direct BeagleBone cape dependency

Current `P8`/`P9` headers and pin-swap notes are a source of confusion and
miswiring risk. Replace them with a purpose-built RX connector:

- Explicit signal names.
- Ground pins interleaved with fast or sensitive signals.
- Separate analog and digital return pins.
- Keyed/locking connector, not ambiguous 2.54 mm cape header orientation.

This is the highest-value schematic cleanup for modularity.

### 2. Decide ADC vs comparator now

There are two credible product directions:

| Direction | Keep | Remove/simplify |
| --- | --- | --- |
| STM32 comparator baseline | analog conditioning to `0..3.3 V` or module comparator output | external `ADS7886` path, BeagleBone SPI header dependency |
| Legacy/diagnostic ADC support | `ADS7886` and SPI boundary | BeagleBone P8/P9 physical header; still use modular SPI connector |

For the current firmware, the comparator baseline is simpler, lower latency, and
already validated in software.

### 3. Rework negative rail generation

`LM2663` charge pump on the RX board is convenient, but it is a switching noise
source close to sensitive analog circuitry. Better options:

- Provide `+5VA` and `-5VA` from the main board through filtered analog power
  pins, then add local ferrite/RC/LC filtering on RX.
- Or keep `LM2663` on RX but put it in a physically isolated power island with
  local return, shield copper/guard spacing, and post-filtering before the
  amplifier rails.

For the modular main board, the first option is cleaner if the connector and
layout can keep analog rails quiet.

### 4. Add a proper STM32-safe output stage

The current docs require scaling AGC/Vin before STM32 `PB2`:

```text
frontend AGC/Vin -> divider or buffer -> PB2 / COMP1_INP
```

Make this a defined part of the new RX schematic, not a bring-up workaround:

- Output range target: normally below `3.0 V`, absolute normal-use below
  `3.3 V`.
- Add series resistor near the RX module output.
- Add clamp/protection compatible with STM32 analog input leakage and bandwidth.
- Add optional RC footprint for edge/noise shaping.
- Label this net clearly as `RX_COMP_IN_0V3V3` or `RX_ANALOG_OUT_0V3V3`.

### 5. Reduce manual trimmers

The schematic contains several 50 k trimmers and debug headers around bias,
gain, and threshold points. They are useful on a lab board but not ideal for a
repeatable modular design.

Recommended approach:

- Keep footprints for one bring-up revision, but add fixed resistor population
  options.
- Move threshold control to STM32 DAC where possible.
- For gain/AGC control, prefer a DAC or digitally controlled setting from the
  main board if runtime adaptation is desired.
- Document default BOM values and DNP variants.

### 6. Clean annotation and BOM semantics

Items to fix before layout release:

- `U?` exists for an `AD8066ARZ-R7` block. This must be annotated.
- Multiple `U1`/`U2` entries appear as multipart symbols; verify Altium
  packaging maps exactly to the intended physical ICs.
- Connector/testpoint designators such as `5V`, `GND`, `DIFF` are readable on
  the schematic, but they are not ideal BOM designators. Prefer `Jx`/`TPx`.
- Many passives have comments `Cap` or `Res1`; ensure actual values are stored
  in visible BOM parameters before producing procurement data.

### 7. Make analog/digital grounding explicit

The RX module should have one controlled ground strategy:

- Continuous ground plane under the analog chain.
- No high-current TX return under or through the RX module.
- Digital return for SPI/control tied near ADC/comparator boundary.
- Guard/keepout around the photodiode and TIA summing node.
- Connector pinout should include several adjacent grounds near `RX_ANALOG_OUT`,
  `RX_SPI_SCK`, and `RX_CS`.

Avoid hard split planes that force return currents through narrow bridges, but
do keep placement/routing zones physically separate.

### 8. Add measurement points without adding antennas

Keep testability, but reduce stubs on sensitive nodes:

- Use high-impedance or coax-compatible test points for `RX_ANALOG_OUT`.
- Keep direct test points off the photodiode/TIA summing node unless guarded and
  very short.
- Add safe low-bandwidth monitor outputs for `VAGC`, `VREF`, `GAIN_OUT`, and
  module rail health.

## Proposed RX connector for STM32 comparator baseline

Suggested minimum pin groups:

| Group | Signals |
| --- | --- |
| Analog power | `+5VA`, `-5VA`, `AGND` |
| Digital/control power | `+3V3D` or `+5VD`, `DGND` |
| Output | `RX_ANALOG_OUT_0V3V3` or `RX_DIGITAL_EDGE` |
| Control | `RX_EN`, `RX_GAIN_CTRL` or `VAGC_DAC`, optional `RX_MODE` |
| Reference/monitor | `VREF_MON`, `GAIN_OUT_MON`, optional `RSSI` |
| Service | `I2C` or spare GPIO only if needed |

Use ground pins around the output and any control clocks.

## Proposed RX connector for ADC-compatible module

| Group | Signals |
| --- | --- |
| Analog power | `+5VA`, `-5VA`, `AGND` |
| Digital power | `+3V3D`, `DGND` |
| SPI | `RX_SPI_SCK`, `RX_SPI_MISO`, `RX_SPI_MOSI`, `RX_CS` |
| Timing | optional `RX_DRDY`, `RX_SAMPLE_CLK` |
| Monitor | `RX_ANALOG_OUT_0V3V3`, `VREF_MON`, `GAIN_OUT_MON` |
| Control | `RX_EN`, `RX_MODE`, gain/AGC control |

This keeps analog acquisition local to the RX module and exports digital data
to the main board.

## Recommended architecture choice

For the system described by the current STM32 docs, choose this:

```text
RX_MODULE:
  photodiode -> TIA/LNA -> AGC/filter -> protected 0..3.3 V analog output

MAIN_BOARD:
  STM32 PB2/COMP1 + DAC threshold + TIM2 capture
```

Add an optional comparator footprint on the RX module only if field testing
shows that the analog cable/module connector degrades edge timing.

Keep `ADS7886` only if you explicitly need BeagleBone PRU ADC compatibility or
want sampled diagnostic data. Otherwise it is extra complexity for the current
receiver.

## Validation checklist for the next revision

- RX module output never exceeds STM32 input limits under dark, ambient, aligned
  TX, saturated TX, and hot/cold conditions.
- Comparator threshold has a stable plateau; best DAC code does not sit at an
  endpoint.
- `glitchps` stays low with TX off and with ambient-light changes.
- Link score improves or remains stable versus `PCB_RX_V2.7`.
- TX high-current switching does not move RX baseline measurably.
- `+5VA`, `-5VA`, and `VREF` ripple are measured with TX active.
- Module connector orientation and pin 1 are unambiguous in schematic, PCB, and
  silkscreen.
