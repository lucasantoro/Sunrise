# Pi HAT port

The active port is PB0 COMP1 input, PE9 optical TX, and USART3 on PB10/PB11.
COMP1 output is routed internally to TIM2 TI4. PC5 is an optional scope-only
output controlled by `OPENVLC_COMP_DEBUG_OUTPUT` and is off by default.

Before traffic, verify the boot log contains `COMP1_PB0`, `baud=2000000`, the
expected PHY profile, and enabled caches. Then test RX-only, TX-only, and
finally full duplex while requiring `ringdrop=0`, `hwovf=0`, and `qdrop=0`.
