# Pi HAT hardware setup

| Signal | STM32 pin | Configuration |
|---|---|---|
| optical analog RX | PB0 | COMP1 IN+, analog, no pull |
| optical TX | PE9 | TIM1 CH1, AF1 |
| Pi receives from STM32 | PB10 | USART3 TX, AF7 |
| STM32 receives from Pi | PB11 | USART3 RX, AF7 |
| threshold | internal | DAC1 CH1 -> COMP1 IN- |

Connect Pi TX to PB11 and Pi RX to PB10; TX/RX names are from the device that
drives the wire. All boards and optical front ends need a common ground.

The PB0 waveform must remain inside the STM32 input range (0..3.3 V, with
margin) and cross the configured DAC threshold in both directions. Do not
drive PC5 into PB11: COMP1 reaches TIM2 through the internal mux.

For an oscilloscope view of the digital slicer output set
`OPENVLC_COMP_DEBUG_OUTPUT=1` and probe PC5. Restore it to zero for performance
tests. Probe PE9/OWC_TX for optical TX.
