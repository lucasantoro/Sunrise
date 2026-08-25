# Link test plan

1. Flash the v2 ELF and confirm `COMP1_PB0`, the intended `rate`, and 2-Mbaud
   USART3 in the init log.
2. With optical TX absent, confirm no packet rate and no sustained false burst
   rate. A raw edge counter may still show isolated noise.
3. Run one direction at a conservative UDP rate. Require increasing valid
   packet count, `ringdrop=0`, `hwovf=0`, and stable `t0/t1/tn`.
4. Repeat in the opposite direction.
5. Run full duplex and increase load gradually. Check TX `qdrop`, RX ring peak,
   CRC/RS, bad-pair run, and `trq` together.
6. If PC5 was enabled for scope correlation, disable it and repeat the final
   throughput test.

`trq / 256` is the largest packet run residual in TIM2 ticks. Treat it as a
diagnostic of the worst crossing, not as a feedback-loop state.
