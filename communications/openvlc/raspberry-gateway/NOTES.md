# Bench notes

Everything needed to run an OpenVLC link from a Raspberry Pi and read what it
tells you. Target: `firmware-v2`.

- [Layout](#layout)
- [Running a node](#running-a-node)
- [Logs](#logs)
- [Traffic tests](#traffic-tests)
- [Diagnostic reference](#diagnostic-reference)
- [Reading recipes](#reading-recipes)

---

## Layout

Each node is one Raspberry Pi plus one STM32H723 Pi HAT. The Pi runs a bridge
that owns a TUN interface and speaks a framed protocol over UART to the HAT;
the HAT does the optical work (comparator RX + TIM1 TX).

```
  tun0 192.168.0.1/24          tun0 192.168.0.2/24
        Pi node A                     Pi node B
           |  UART 2 Mbaud                |
        H723 HAT   ~~~ optical ~~~    H723 HAT
        addr 7                        addr 8
```

A receiver discards frames carrying its own optical address as source, so the
two nodes must be flashed with different `OPENVLC_TRANSCEIVER_NODE` values.
Never flash the same node firmware on both boards.

Hosts are named `nodeA`, `nodeB`, ... and everything on the Pi side derives
from that: node letter, tun address, capture filenames. Adding `nodeC` means
naming the host and running the installer — no new file, no edit.

| path on the Pi | what |
|---|---|
| `/opt/openvlc-raspberry/` | deployed code |
| `/opt/openvlc-raspberry/logs/` | **all bench output** |
| `/opt/openvlc-raspberry/logs/captures/` | STM32 RX captures |
| `/etc/default/openvlc-transceiver` | this node's settings |

---

## Running a node

Install once, per node. The installer reads the node identity from the
**hostname** — hosts are named `nodeA`, `nodeB`, `nodeC`, ... — so on a
correctly named Pi there is nothing to pass:

```bash
sudo bash ./install/install_transceiver_service.sh
```

`nodeA` becomes node `a` on `192.168.0.1`, `nodeB` node `b` on `192.168.0.2`,
and so on: the letter is the host address. Captures are tagged with the
hostname, so a `.bin` from `nodeC` says so in its filename.

A host not named that way is **rejected** rather than guessed at — an earlier
version of this derivation would have turned `raspberrypi` into node `i`. Pass
it explicitly instead:

```bash
sudo bash ./install/install_transceiver_service.sh --node c --peer 192.168.0.1
```

Beyond two nodes `--peer` is required: with two, "the other one" is
unambiguous; with three or more on a shared optical medium, which peer the tun
points at is a routing decision and the installer will not invent it.

Then check what it wrote:

```bash
cat /etc/default/openvlc-transceiver && ip -br addr show tun0
```

Node A must report `192.168.0.1/24`, node B `192.168.0.2/24`.

Start, stop, restart:

```bash
sudo systemctl start openvlc-transceiver
```

```bash
sudo systemctl restart openvlc-transceiver && systemctl status openvlc-transceiver --no-pager
```

Reachability, and an MTU check in one command — `900 - 28 = 872`:

```bash
ping -c 5 -M do -s 872 192.168.0.2
```

If that fragments or fails, nothing below is worth reading.

### Changing a setting

Everything the bridge takes is an environment variable in
`/etc/default/openvlc-transceiver`. Edit, then restart.

| variable | default | what it does |
|---|---|---|
| `OPENVLC_SERIAL_PORT` | `auto` | UART to the HAT; `auto` picks the only one present |
| `OPENVLC_SERIAL_BAUD` | `2000000` | host link speed, not the optical rate |
| `OPENVLC_TUN_CIDR` | per node | this node's address |
| `OPENVLC_PEER_IP` | per node | the other end |
| `OPENVLC_MTU` | `900` | tun MTU; the optical frame carries 828 B of payload |
| `OPENVLC_TX_MAX_FPS` | `125` | frames/s handed to the HAT; `0` disables pacing |
| `OPENVLC_STATS_INTERVAL` | `5` | seconds between `[trx-bridge]` lines |
| `OPENVLC_CAPTURE_DIR` | `logs/captures` | where RX captures land |
| `OPENVLC_CAPTURE_NODE` | hostname | label written into capture filenames |

`OPENVLC_TX_MAX_FPS` is the one worth knowing: lowering it on one node while
the other stays at 125 is how you tell whether a counter follows the *local*
transmitter or the *received* traffic. See
[self-interference](#is-my-own-transmitter-blinding-my-receiver).

### Running it by hand

Stop the service first or the two fight over the serial port:

```bash
sudo systemctl stop openvlc-transceiver
```

```bash
sudo python3 /opt/openvlc-raspberry/vlc_transceiver_bridge.py --port auto --baud 2000000 --dev tun0 --ip 192.168.0.2/24 --peer-ip 192.168.0.1 --source-route="" --mtu 900 --stats-interval 5 --tx-max-fps 125 --capture-node "$(hostname)"
```

---

## Logs

The bridge prints to stdout; under systemd that becomes the journal.

**Watch live**, only the lines that carry numbers:

```bash
journalctl -u openvlc-transceiver -f | grep -E "COMP|TX uart|trx-bridge"
```

**Watch just the RX health line:**

```bash
journalctl -u openvlc-transceiver -f | grep --line-buffered "COMP "
```

**Save a run.** `collect_logs.sh` writes into `logs/` beside the captures, with
a context file recording which node, which settings and which build produced
the numbers — a log without that is hard to interpret a month later:

```bash
./collect_logs.sh -s "30 min ago" -t baffle-test
```

**Record a run as it happens:**

```bash
./collect_logs.sh -f -t long-run
```

**Hand the whole thing to someone else:**

```bash
tar czf run.tar.gz -C /opt/openvlc-raspberry logs
```

### RX captures

When the firmware hits a failure trigger it emits an interval trace, which the
bridge validates and writes to `logs/captures/` as a `.bin` plus a `.json` of
metadata. Those replay offline through `failure_replay` in the firmware tree,
so a decoder problem can be worked without the optical bench running at all:

```bash
for f in logs/captures/*.bin; do ./failure_replay_fast "$f"; done
```

Captures are disabled with `--no-capture-save` when you only want the
validation, not the files.

---

## Traffic tests

Both directions of the link are independent. Test them one at a time before
testing them together — if one direction is already bad, the simultaneous
result tells you nothing.

### UDP

Server on B:

```bash
iperf -u -s -p 10001 -i 1
```

Client on A, at the link's nominal rate:

```bash
iperf -u -c 192.168.0.2 -b 800k -l 800 -p 10001 -t 60 -i 1
```

`-l 800` because 800 B of UDP payload plus 28 B of IPv4+UDP headers is the
828 B the optical frame carries. Read the loss column, not the bandwidth: at a
fixed offered rate the bandwidth just mirrors the loss.

### TCP

```bash
iperf -s -p 5201 -i 1
```

```bash
iperf -c 192.168.0.2 -p 5201 -t 60 -i 1 -M 860
```

`-M 860` fixes the MSS at `900 - 40`. Without it TCP tries 1460 and relies on
path-MTU discovery, which adds noise to the measurement.

Expect TCP to come out well below the UDP number. Congestion control reads
optical loss as congestion and backs off; that is TCP behaving normally on a
lossy link, not a fault. `-r` runs the two directions in sequence, `-d` runs
them simultaneously — start with `-r`.

### Both directions at once

Only after each direction is clean on its own. Simultaneous traffic is when
the local transmitter and the local receiver are both active, which is exactly
the condition that exposes optical self-interference.

---

## Diagnostic reference

Three lines matter. Every field below is mapped to its firmware variable, so
the meaning is the source's, not a guess.

### `[stm32] COMP` — receive health

Rates are per logging interval; totals are cumulative since boot. Mixing the
two up is the single most common misreading: `ovf=172` standing still means the
ring overflowed 172 times *at some point*, not that it is overflowing now.
**Only a growing total is evidence.**

**Rates** — recomputed every interval:

| field | meaning |
|---|---|
| `ep` | comparator edges per second. ~1.0-1.5M on a live link |
| `bp` | bursts detected per second. Should equal the frame rate |
| `sp` | frames *seen* per second — a burst that produced an SFD hit |
| `okp` | frames *delivered* per second. This is the one that matters |
| `lp` | over-long intervals per second |
| `gpm` | glitches per mille of all edges |

**Interval histogram** — how the edge widths are distributed, in 64 MHz ticks.
A legitimate half-cell is 32 ticks and a full cell 64, so a healthy link puts
almost everything in `r24p`:

| field | bucket |
|---|---|
| `r07` | 0-7 ticks — pure noise |
| `r811` | 8-11 — glitches. Watch this one: it is the noise floor |
| `r1215` | 12-15 |
| `r1619` | 16-19 |
| `r2023` | 20-23 |
| `r24p` | 24 and above — where the signal should be |

**Timing recovery** — the most diagnostic group on the line:

| field | meaning |
|---|---|
| `hc` | recovered half-cell, ticks |
| `t0` / `t1` | recovered half-cell **per polarity**. See below |
| `tn` | nominal half-cell the decoder is working against (32) |
| `trq` | peak timing residual |
| `thr` | comparator threshold, DAC code. `mV = dac * 3300 / 4095` |

`t0` and `t1` are worth more than the rest of the line put together. They must
**sum to `2 x tn`** — if they do, the clock is correct, whatever else is wrong.
And they should be roughly **equal**: the gap between them is the duty-cycle
distortion, and it eats the margin the decoder needs to tell a 1-cell interval
from a 2-cell one. See [duty](#the-duty-is-off-centre).

**Frame counters** — cumulative:

| field | meaning |
|---|---|
| `seen` | frames attempted |
| `ok` | frames delivered to the host |
| `crc` | failed the CRC — framed correctly, payload corrupt |
| `sync` | failed framing — never reached a CRC |

`crc` against `sync` says *where* frames die, and the two lead to completely
different fixes. Payload corruption is an analog problem; framing failure is
acquisition or timing.

**Buffers and overruns** — cumulative, read as deltas:

| field | meaning |
|---|---|
| `hq` / `hd` | frames queued to the host / dropped there |
| `rp` | peak edge-ring occupancy |
| `rd` | edge-ring drops. **If this grows, stop reading the rest of the line** |
| `hwo` | timer hardware overcaptures |
| `ovf` | edge buffer overflows |

**Burst segmentation:**

| field | meaning |
|---|---|
| `fr` | bursts fragmented — a gap appeared *inside* a frame |
| `fa` | edge index where the last fragmentation happened |
| `fg` | the gap that caused it, in microseconds |
| `sg` | soft-gap probes / completed / bridged / reused |
| `br` `bl` `bok` `bf` `bsh` `bst` | last raw / segmented / ok / failed / short burst length, and the last decode status |

`fr` growing at the frame rate means every frame is being cut in two. Compare
`fg` against `OPENVLC_EDGE_GAP_US` (14 µs): a gap just over that threshold is
the segmenter mistaking a dead stretch inside a frame for a frame boundary.

**Decode cost:**

| field | meaning |
|---|---|
| `du` | last decode, microseconds |
| `dm` | worst decode, microseconds |

The frame period is 8000 µs. `du` above ~5000 means anything else added to the
CPU will push it over.

**SFD synchronisation** — why acquisition failed:

| field | meaning |
|---|---|
| `sf` | last SFD-sync result |
| `lock` | cell index the SFD locked at |
| `ss` | successful syncs |
| `pre` `pbad` `pse` | preamble rejects, bad-max, sfd-min |
| `se` | SFD bit errors |
| `m` | SFD sync mode |
| `pe` | phase edits applied |
| `ps` | parse status of the last frame (negative = failure) |

**Failure attribution** — which stage rejected the frame:

| field | rejected at |
|---|---|
| `ft` | timing |
| `fn` | no SFD found |
| `fp` | preamble |
| `fx` | parse |
| `fc` | CRC |
| `fl` | length |
| `fo` | overflow |
| `fi` | incomplete |

**Quality**, present only when a packet was actually delivered in the interval:

| field | meaning |
|---|---|
| `sc` | link quality score, 0-100 |
| `jit` | timing jitter, percent |

### `[stm32] TX` — transmit health

| field | meaning |
|---|---|
| `frames` / `fps` | frames handed to the optical transmitter |
| `uart` / `kbytes` | bytes received from the Pi |
| `q` / `pipe` | queue depth and pipeline slots in use |
| `qdrop` | **queue overflow** — the host is offering faster than the link sends |
| `seqgap` / `reorder` | sequence gaps and reordering from the host |
| `crc` `len` `hdr` `ip` | host frames rejected, by reason |
| `started` / `done` | transmissions begun and completed |
| `tx_us` | airtime of one frame, microseconds |
| `words` `hi` `lo` | Manchester half-cells in the last frame, and their split |
| `enc_us` / `encmax_us` | encode cost |
| `late` / `txlate` / `latemax_us` | transmissions that missed their slot |
| `guard` | guard-interval events |
| `idle_us` `idlemin_us` `idlemax_us` | gap between frames |
| `hash` | payload hash — changes every frame on live traffic |

`tx_us` against the frame period is the **optical duty cycle**, and it decides
whether the receiver ever gets a dark window. At 125 fps with `tx_us=7264` the
LED is modulating 93% of the time.

`hash` frozen across lines means the same payload is being resent — usually
keep-alive traffic rather than real data.

### `[trx-bridge]` — the Pi side

| field | meaning |
|---|---|
| `tx=` / `rx=` | kbps and fps in each direction |
| `wiretx` | frames actually put on the wire after pacing |
| `total=` | host frames sent / received, cumulative |
| `drop` | frames the pacer dropped |
| `gap` / `reset` | sequence gaps and link resets |
| `backlog` | frames waiting |
| `pace` | the configured `--tx-max-fps` |
| `crc` / `header` | frames the bridge rejected |

`rx=` here is the end-to-end truth. If `okp` on the STM32 looks fine and `rx=`
does not, the problem is between the HAT and the Pi, not optical.

---

## Reading recipes

Signatures that have cost bench time, so they can be recognised at a glance.

### Read these first, in order

1. **`rd` or `ovf` growing** — the edge ring is dropping. Every other counter
   on the line is measuring a truncated input. Fix this before reading on.
2. **`t0 + t1` far from `2 x tn`** — the clock is wrong, not the threshold.
3. **`t0` vs `t1` unbalanced** — the threshold is off centre. Most common fault.
4. **`crc` vs `sync`** — where frames die decides which fix applies.

### The duty is off centre

`t0=22 t1=42` sums to 64 = `2 x 32`, so the clock is exact — but the duty is
34/66 instead of 50/50.

Why it is fatal rather than cosmetic: the decoder separates a 1-cell interval
from a 2-cell one at 1.5 cells, 48 ticks. Centred, the neighbouring classes sit
at 32 and 64 — 32 ticks apart. At 34/66 the "1-cell wide" class is at 42 and
the "2-cell narrow" class at `22 + 32 = 54`: **12 ticks apart**, with a real
spread of ±5. The tails overlap and the payload is lost.

Causes seen so far, both real:

- an asymmetric receiver front end (unequal rise and fall slew), which no
  threshold fully corrects
- the local transmitter adding light to the local photodiode, which moves the
  baseline under a fixed threshold

`OPENVLC_COMP_DUTY_SERVO` exists to chase the threshold toward 50/50 and is
**off by default** — its header asks you to first confirm on the bench that the
duty moves monotonically with the DAC. `OPENVLC_COMP_THRESHOLD_SWEEP` does that
confirmation in one run and logs `COMP SWEEP` with the duty at each step.

### Every frame attempted, none delivered

`bp == sp` with `okp = 0`: one burst per frame at the right rate, all attempted,
none surviving. Acquisition and segmentation are working — the failure is at
the bit level. Go straight to `t0`/`t1`.

If `du` is also small, the CPU is idle and is not the cause.

### Frames lost without being counted

`sp` short of the frame rate means frames that never produced an SFD hit at all.
They appear in no failure counter. Always compare `sp` against the *offered*
rate, never `ok` against `seen`.

### Is my own transmitter blinding my receiver?

The decisive test needs no hardware change. **Stop the remote node**, leave the
local one transmitting, and read the local `bp` and `sp`:

| local node shows | conclusion |
|---|---|
| `bp` ≈ frame rate | the photodiode sees its own LED. No software fix exists |
| `bp` ≈ 0 | it does not; look elsewhere |

There is no third outcome — nothing else is transmitting.

A softer version, useful while both nodes must stay up: set
`OPENVLC_TX_MAX_FPS=60` on one node and watch whether its own counters follow
60 or stay at the remote's 125.

If confirmed, the fix is physical: an opaque baffle between LED and photodiode,
and a hood restricting the photodiode's field of view. Distance helps only the
direct path — reflections do not fall off with it. The acceptance criterion is
the test above returning `bp = 0`, not "it looks better".

Note that at 125 fps the transmitter occupies ~93% of the time, so there is no
dark window for the receiver to work in. Full duplex at that rate is only
reachable once the optical isolation is good.
