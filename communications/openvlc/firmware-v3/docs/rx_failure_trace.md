# RX signal capture and offline replay

`OPENVLC_RX_CAPTURE` builds a balanced corpus of comparator bursts accepted and
rejected by the live decoder. The default quotas are 32 successful and 32
failed frames. Version-2 captures contain the TIM2 intervals before software
edge filtering, allowing legacy and timing-aware filters to be compared from
the same physical signal. It reuses one snapshot buffer and streams each
completed capture to the Pi before rearming, so increasing the quotas does not
allocate more STM32 RAM.

The recorder qualifies once after 20 consecutive successful packets, excluding
reset, AGC-settling and startup transients. Captures are then spaced by decoded
frames: a qualifying failure has priority after 20 frames, while a successful
frame is sampled after 64. Each snapshot contains up to 14000 adjacent TIM2 edge
intervals and requires a burst with at least 11000 edges. The spacing restarts
only after the previous binary capture has been completely queued, preventing
adjacent packets or a single failure cascade from dominating the population.
The snapshot also contains the decoder status, SFD/timing estimates, threshold,
PHY profile and timestamp. It does not alter the threshold, timing recovery,
Manchester decisions, Reed-Solomon processing or packet delivery.

Enable the recorder in `Core/Inc/openvlc_board.h`:

```c
#define OPENVLC_RX_CAPTURE 1u
#define OPENVLC_RX_CAPTURE_RAW 1u
```

The normal production default is `OPENVLC_RX_CAPTURE=0u`. Keep
`OPENVLC_RX_CAPTURE_RAW=1u` when collecting a corpus; set capture to `0u` for
an absolute minimum-memory production image. The snapshot
uses at most 28 KB of CPU-only RAM.

The two population sizes and their spacing are independently configurable:

```c
#define OPENVLC_RX_CAPTURE_MAX_FAILURES 32u
#define OPENVLC_RX_CAPTURE_MAX_SUCCESSES 32u
#define OPENVLC_RX_CAPTURE_MIN_FRAME_GAP 20u
#define OPENVLC_RX_CAPTURE_OK_FRAME_GAP 64u
```

Increasing this number does not allocate more STM32 RAM; it only allows the
same buffer to be reused for more events.

## Collection on Raspberry Pi

Install/update the bridge:

```sh
cd ~/Sunrise/communications/openvlc/raspberry-gateway
sudo ./install_transceiver_service.sh
```

Set a unique label in `/etc/default/openvlc-transceiver`:

```sh
OPENVLC_CAPTURE_NODE=node-a
OPENVLC_CAPTURE_DIR=/var/log/openvlc/captures
```

After flashing and resetting the STM32, run the normal traffic test. The
bridge validates every binary chunk and saves only a complete capture:

```sh
sudo journalctl -u openvlc-transceiver -f -o cat
sudo ls -lh /var/log/openvlc/captures
```

Track the two populations independently:

```sh
find /var/log/openvlc/captures -maxdepth 1 -name '*-ok.bin' | wc -l
find /var/log/openvlc/captures -maxdepth 1 -name '*.bin' ! -name '*-ok.bin' | wc -l
```

Completion is reported as:

```text
[rx-capture] saved /var/log/openvlc/captures/node-a-...-crc.bin
[rx-capture] saved /var/log/openvlc/captures/node-a-...-ok.bin
```

Each event produces:

- `.bin`: the exact 88-byte metadata header followed by big-endian `uint16`
  raw timer intervals;
- `.json`: human-readable decoder and acquisition metadata.

ID, offsets, interval count and FNV-1a hash are checked before the atomic
rename. Interrupted or corrupt captures are never exposed as valid files.
The bridge statistic `cap=records/saved/errors` reports collection health.

Create the archive to copy back to the development machine:

```sh
sudo tar -C /var/log/openvlc/captures -czf /tmp/openvlc-rx-captures.tar.gz .
sudo chown "$USER":"$USER" /tmp/openvlc-rx-captures.tar.gz
```

## Replay the firmware decoder

Copy the `.bin` file to the development machine and run the same decoder used
by the STM32 firmware:

```sh
cmake -S firmware -B firmware/build-host
cmake --build firmware/build-host --target failure_replay_fast
firmware/build-host/failure_replay_fast capture.bin
```

Version-2 raw captures automatically use the filter policy compiled into the
selected replay target. The profile-1000 realtime target mirrors the deployed
12-tick physical gate. Force a legacy threshold, or preserve every raw edge,
with:

```sh
firmware/build-host/failure_replay_fast_realtime capture.bin --deglitch=12
firmware/build-host/failure_replay_fast capture.bin --deglitch=0
```

For offline experiments only, a timing-aware filter can be selected explicitly
as `candidate,hard,margin`. This overrides the filter compiled into the replay
executable but never changes the STM32 image:

```sh
firmware/build-host/failure_replay_fast_realtime capture.bin --contextual=13,8,2
```

Add `--search` to run the existing diagnostic cell-override search:

```sh
firmware/build-host/failure_replay_fast capture.bin --search
```

The replay output reports `domain=raw|filtered`, `filter=...`, the active gate
and the number of removed edges. The tool uses the same decoder for both
`-ok.bin` and failure captures, and remains compatible with version-1 `.bin`,
older `RXFAIL_*` journal dumps and extracted CSV files.
