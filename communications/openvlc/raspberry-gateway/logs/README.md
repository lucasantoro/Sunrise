# logs/

Bench output. Nothing here is tracked by git.

| what | written by |
|---|---|
| `openvlc-transceiver-*.log` | `./collect_logs.sh` — journal snapshot of a run |
| `openvlc-transceiver-*.context.txt` | `./collect_logs.sh` — node config, interfaces, serial, service state |
| `captures/*.bin` + `.json` | the bridge, when the STM32 emits a validated RX capture |

The captures are the interval traces the firmware saves on a failure trigger.
They replay offline through `failure_replay` in the v2 firmware tree, which
means a decoder problem can be worked on without the optical bench running —
see `docs/rx_board_b_duty_2026-08-21.md` there for a worked example.

To keep a run permanently, move it out of here: this directory is meant to be
disposable, and a result worth keeping belongs in a doc.
