# Synthetic channel campaign

`firmware/tests/host_loopback.c` builds the same 828-byte Reed-Solomon frame
used by the link test, converts it to Manchester cells and produces comparator
timestamps without involving the STM32 hardware.

Every configured PHY profile runs a deterministic campaign containing:

- clock offsets from -1% to +1% in the boundary grid and up to +/-1.5% in the
  seeded mixed cases;
- polarity-dependent comparator crossing delay from zero to one quarter cell;
- bounded independent edge jitter;
- captures with and without the artificial timestamp at the frame origin;
- narrow extra comparator pulse pairs passed through the contextual filter;
- missing transition pairs at positions distributed across the complete frame;
- grossly truncated frames.

The acceptance rules are strict:

1. Every impairment inside the supported envelope must decode the complete
   packet byte-for-byte.
2. A destructive impairment may be corrected exactly or rejected.
3. Returning `OPENVLC_OK` with different header or payload bytes is always a
   test failure.

All pseudo-random mixed cases use deterministic seeds. A failure prints its
case number, seed and channel parameters so it can be reproduced exactly.

Run the complete host suite after configuring the CMake build directory:

```sh
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

This campaign validates the digital timing and decoding envelope. It cannot
model analog effects that never reach TIM2, such as comparator metastability,
front-end saturation or a transition completely absent from the captured
edge stream. Version-2 raw captures remain the source for adding those real
waveforms as permanent regression cases.
