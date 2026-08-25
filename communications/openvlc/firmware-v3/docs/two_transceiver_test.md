# Two-transceiver full-duplex test

## Node identities

| Node | STM32 firmware | TUN address | Optical address |
|---|---|---|---|
| A | build with `OPENVLC_TRANSCEIVER_NODE=1` | `192.168.0.1/24` | `src=7`, `dst=8` |
| B | build with `OPENVLC_TRANSCEIVER_NODE=2` | `192.168.0.2/24` | `src=8`, `dst=7` |

Do not flash the same node firmware on both boards. Each receiver discards
frames carrying its own optical source address.

## Raspberry installation

On node A:

```bash
cd ~/raspberry-gateway
sudo bash ./install_transceiver_service.sh --node a
```

On node B:

```bash
cd ~/raspberry-gateway
sudo bash ./install_transceiver_service.sh --node b
```

Verify:

```bash
cat /etc/default/openvlc-transceiver
ip -br addr show tun0
systemctl status openvlc-transceiver --no-pager
```

Node A must report `192.168.0.1/24`; node B must report
`192.168.0.2/24`. No legacy `10.0.0.0/24` route is installed for this direct
two-node test.

## Optical wiring

Point node A TX LED at node B photodiode and node B TX LED at node A
photodiode. On the Pi HAT the local optical paths are PE9/OWC_TX and
PB0/COMP1_IN. Use an optical barrier between each board's own LED and receiver
to limit self-interference during simultaneous traffic.

## Connectivity

```bash
# Node A
ip route get 192.168.0.2
ping -c 5 192.168.0.2

# Node B
ip route get 192.168.0.1
ping -c 5 192.168.0.1
```

The route must use `tun0` and the local node address as source.

## Simultaneous iperf2

Start the receiver on node A:

```bash
iperf -u -s -p 10002 -i 1
```

Start the receiver on node B:

```bash
iperf -u -s -p 10001 -i 1
```

In a second terminal on node A, transmit toward B:

```bash
iperf -u -c 192.168.0.2 -b 800k -l 800 -p 10001 -t 120 -i 1
```

In a second terminal on node B, transmit toward A:

```bash
iperf -u -c 192.168.0.1 -b 800k -l 800 -p 10002 -t 120 -i 1
```

Monitor both bridges:

```bash
sudo journalctl -u openvlc-transceiver -f -o cat |
grep --line-buffered -E 'trx-bridge|COMP |TX uart'
```

At 800 kbit/s per direction, each bridge should remain near `125 fps` TX and
RX with `drop=0`, `qdrop=0`, `ringdrop=0` and stable `hwovf`.

The STM32 `TX` line must also keep `reorder=0`, `fifoerr=0`, `late=0`, and a
`maxpoll` safely below the approximately 41 ms capacity of the 8192-byte host
RX DMA ring at 2 Mbaud.
