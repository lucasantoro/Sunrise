# Pi HAT TX diagnostics

The firmware target is the custom HAT MCU `STM32H723VET6` (LQFP100).

## Observable boot sequence

1. Reset or power-cycle the HAT.
2. If `OPENVLC_TX_BOOT_PIN_TEST_MS` is non-zero, `OWC_TX` (connector pin 1,
   measured against connector pin 2/GND) outputs a 100 kHz, approximately 50%
   duty-cycle square wave for the configured duration. Production defaults to
   zero, so normal OpenVLC/UART operation starts immediately.
3. Normal OpenVLC/UART operation starts after the optional test.
4. `FLAG_1` / green LED toggles once per second while the main loop is alive.
5. `FLAG_2` / blue LED turns on continuously on NMI, HardFault, MemManage,
   BusFault, UsageFault, or `Error_Handler`.

The boot waveform does not depend on Raspberry Pi traffic, USART3/PB11, frame
parsing, or TX DMA. If it is absent, debug PE9/TIM1, flashing, power/reset, or
the PCB path. If it is present but `iperf` produces no later waveform, the
problem is before optical TX in the Pi UART or STM32 host parser path.

## Continuous TX integrity fields

The one-second `TX` record verifies both the encoded stream and the TIM1/DMA2
schedule:

- `words`: halfwords in the active DMA stream.
- `hi` / `lo`: exact HIGH/LOW cell counts. Manchester traffic should remain
  close to 50%, with `lo` higher only by the configured low guard/gap.
- `hash`: ordered FNV-1a checksum of the active cell stream. It may change with
  packet data, but must never remain stale while different traffic is sent.
- `tx_us`: measured start-to-DMA-completion time.
- `txlate`: completions more than one cell later than the theoretical CH4
  schedule. It must remain zero.
- `latemax_us`: maximum measured completion lateness including IRQ entry.
- `cfg`: invalid TIM1/DMA2 configuration observations. It must remain zero.
- `fcr`: DMA2 FIFO-control register snapshot. FIFO mode must remain enabled.
- `derr` / `dma`: HAL DMA error state and cumulative DMA errors. Both must
  remain zero.

For an 800-byte UDP payload at the 1000 kbit/s profile, `tx_us` should remain
approximately constant between TX-only and full-duplex operation. A rising
`txlate`, `cfg`, `derr`, or `dma` identifies an STM32 scheduling/configuration
fault. If all remain clean while PE9 is correct but LED current falls, the
fault is downstream of the Pi HAT.

## Physical TX register monitor

Diagnostic revision `txhwdiag1` adds three records:

- `TXCFG` is emitted once at boot and identifies the flashed node, source and
  destination addresses, `silent` mode, PHY profile, timer clock, line-cell
  rate, slot count and DMA memory mode. Normal optical transmission requires
  `silent=0`.
- `TXHW` is a live snapshot of TIM1 and the TX state machine.
- `TXDMA` is the matching DMA2, DMAMUX and GPIOE snapshot. `first`, `first_gen`
  and `first_ndtr` preserve the first detected hardware inconsistency.
- `TXFIRST` appears after the first inconsistency and repeats its complete
  frozen register snapshot, even if the live peripheral subsequently recovers.

The monitor samples every 100 us and never restarts a peripheral. A DMA/TIM1
stall is declared only after 500 us without `NDTR` progress. The short normal
window in which DMA has reached zero but its completion IRQ is pending is not
reported as a fault.

`fault` describes the current sample and `latched` is the OR of every fault
seen since reset:

| Bit | Hex | Meaning |
|---:|---:|---|
| 0 | `0x00000001` | active slot is invalid |
| 1 | `0x00000002` | TIM1 stopped while TX is busy |
| 2 | `0x00000004` | DMA2 stream stopped while TX is busy |
| 3 | `0x00000008` | TIM1 CC4 DMA request disabled |
| 4 | `0x00000010` | CH1 output gate/state inconsistent |
| 5 | `0x00000020` | TIM1 main-output-enable missing |
| 6 | `0x00000040` | ARR or CCR4 timing mismatch |
| 7 | `0x00000080` | CCR1 is neither zero nor one full cell |
| 8 | `0x00000100` | PE9 is not AF1/TIM1 |
| 9 | `0x00000200` | impossible DMA `NDTR` |
| 10 | `0x00000400` | DMA peripheral or memory address mismatch |
| 11 | `0x00000800` | DMA FIFO/direct/transfer error flag |
| 12 | `0x00001000` | DMAMUX synchronization overrun |
| 13 | `0x00002000` | no DMA progress for at least 500 us |

During a healthy 1000-profile frame the important values are:

- `busy=1`, `arr=95`, `ccr4=12`;
- `cr1 & 1`, `dier & TIM_DIER_CC4DE`, `ccer & TIM_CCER_CC1E` and
  `bdtr & TIM_BDTR_MOE` set;
- `ccr1` equal to `0` or `96`;
- DMA `ndtr` decreasing, `par` pointing at `TIM1_CCR1`;
- `fault=00000000` and `latched=00000000`.

The monitor still cannot measure LED current. During the faint-output event,
capture the journal without restarting iperf and probe OWC_TX/PE9:

```sh
sudo journalctl -u openvlc-transceiver.service -f -o cat |
  tee ~/openvlc-tx-faint.log
```

If `TXHW`/`TXDMA` stay clean and PE9 remains a 0--3.3 V, 2 Mcell/s waveform
while LED current falls, the failure is downstream in the external driver,
power rail or protection circuitry.

## Fault inspection over SWD

Start a Debug session in STM32CubeIDE without resetting the target, suspend the
CPU, and add these expressions to **Live Expressions**:

- `openvlc_fault_record.magic`: `0xFA17xxxx` means a captured fault.
- `openvlc_fault_record.kind`: 1=NMI, 2=HardFault, 3=MemManage, 4=BusFault,
  5=UsageFault, 6=Error_Handler.
- `openvlc_fault_record.pc`, `cfsr`, `hfsr`, `bfar`, and `mmfar` locate the
  exception.
- `openvlc_boot_step`, `openvlc_main_phase`, and `openvlc_main_loop_count`
  show how far execution progressed.

Do not reset after the blue LED turns on until the record has been read: the
record describes the current halted fault loop.
