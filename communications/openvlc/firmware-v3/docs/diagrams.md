# Data paths

```text
Optical RX -> AGC/front end -> PB0 -> COMP1 <- DAC1 threshold
                                      |
                                      +-> internal TIM2 CH4 -> DMA ring
                                          -> packet timing -> SFD/Manchester
                                          -> RS/CRC -> USART3 PB10 -> Pi

Pi -> USART3 PB11 -> frame/RS/Manchester -> FIFO-by-age DMA slot
   -> TIM1 CH1/DMA2 -> PE9 -> optical TX
```
