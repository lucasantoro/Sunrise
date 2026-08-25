# Bring-up stages

`OPENVLC_BOOT_STAGE` selects:

1. host UART and OpenVLC initialization;
2. initialization plus COMP1/TIM2 capture start;
3. full RX/TX polling (production default).

The active receiver starts DAC1 CH1, COMP1 on PB0, the internal COMP1-to-TIM2
TI4 route, and TIM2 CH4 circular DMA. The active transmitter uses PE9 TIM1 CH1
and DMA2. ADC1 and TIM6 are not part of any stage.
