# Simulations

Use this folder for simulations related to the drone-mounted VLC link.

Possible topics:

- optical link budget;
- LED/photodiode geometry;
- drone motion and pointing error;
- channel noise and ambient light;
- BER/PER simulations;
- network behavior between drones.

Suggested structure:

```text
simulations/
  optical_link/
  drone_motion/
  channel_model/
  notebooks/
  results_sample/
```

## What is here

| file | |
|---|---|
| `Carclo10391_CREE_XHP35_White.zmx` | Zemax model: Carclo 10391 lens over a CREE XHP35 White emitter |

The matching `.ZDA` ray database is **not** in the repository. It is 1.4 GB of
traced rays generated from the `.zmx` above, which exceeds GitHub's 100 MB
per-file limit; Zemax regenerates it from the source model, and the emitter ray
file it starts from comes from the LED vendor.

Keep heavy generated results outside Git or store them as release artifacts.

