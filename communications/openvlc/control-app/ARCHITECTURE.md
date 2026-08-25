# Control panel architecture

Target bench: `firmware-v2`. v3 is still in development and the
panel does not model its streaming-mode counters yet.

## Why this changed

The panel grew around one bench with exactly two transceivers plus a legacy
BeagleBone chain, and that assumption ended up in the data:

```python
trx_a: Device        trx_a_tun_ip: str
trx_b: Device        trx_b_tun_ip: str
video_camera_node: str   # "a" | "b" | "legacy"
```

Every feature that touched a node then had to branch on that pair, so a third
node meant editing the UI rather than the configuration. `app.py` also grew to
3488 lines in a single `MainWindow` class holding all eight tabs, and there was
no test anywhere in the app.

## Layers

```
model.py         data only: SshTarget, Node, Link, settings groups, NodeRegistry
config.py        persistence, versioning, v1 -> v2 migration
ssh.py           paramiko: connect (optionally via jump), run, detach, stream
stats.py         log line -> {metric: value}
diagnostics.py   {metric: value} -> named findings with an action
app.py           the window: assembly, shared state, teardown
ui/              one module per tab and per subsystem (see ui/__init__.py)
tests/           unittest, no Qt required
```

The rule is one direction only: `app.py` may import anything, `diagnostics`
imports nothing but the standard library, and `model.py` imports nothing at all.
That is what makes everything below the UI testable without a display.

## The node model

A node owns one optical address; a **link** is an ordered pair of node ids. This
matches the firmware, where a receiver discards frames carrying its own address
as source, so a destination belongs to a hop and not to a node. The old layout
stored `src`/`dst` per side, which only works for exactly two nodes.

```python
Node(node_id="a", label="Node A", role=ROLE_TRANSCEIVER,
     ssh=SshTarget(host="10.0.0.11", user="vlctrx"),
     tun_ip="192.168.0.1", optical_addr=7)
Link(src_id="a", dst_id="b")
```

`Config.add_node()` allocates a free id, optical address and tun IP, so adding a
third node is a button, not a code change. `Config.remove_node()` also drops the
links and video routes that referenced it, because stale ids in saved settings
were a real source of confusing behaviour.

Roles (`transceiver`, `rx-bridge`, `tx-host`, `tx-optical`) decide which
controls a node offers, so the UI can be generated from the node list.

## Configuration versioning

`~/.openvlc_panel.json` carries a `version`. Version 1 files are migrated on
load and rewritten on the next save; an existing bench keeps its settings
without being reconfigured. Migration is covered by tests against a
representative version-1 document.

`Config` still exposes the version-1 attribute names (`cfg.trx_a`,
`cfg.iperf_rate`, `cfg.video_camera_node`, ...) as properties backed by the node
list. **That is scaffolding, not API.** It exists so the UI can migrate tab by
tab instead of in one unverifiable sweep, and the whole block is meant to be
deleted once `app.py` stops using it. Nothing new should be written against it.

## Diagnostics

`diagnostics.py` turns counters into named conditions. Each rule carries what
was measured, why it means what it means, and what to do next — the panel used
to render sixty raw counters and leave the reading to whoever was at the bench.

Current rules and where they came from:

| Rule | Signature | Reading |
|---|---|---|
| `ring-pressure` | `rd>0` or `ovf>0` | the ring is dropping, every other counter in the line is measuring a truncated input — read this first |
| `duty-fatal` | `1T-wide` and `2T-narrow` within 20 ticks | the 1T/2T decision margin is collapsing; this is the new-RX-board failure |
| `seen-never-ok` | `bp == sp`, `okp == 0` | one burst per frame at the right rate, all attempted, none surviving: bit level, not acquisition |
| `sfd-length-zero` | `syncs>0`, `len_raw==0` | SFD survives by correlation while the bits are already wrong |
| `duty-warn` | halves sum to `2 x nominal`, imbalance >10% | clock correct, crossing point off centre |
| `decode-saturation` | `du` > 60% of the 8000 µs frame period | any parallel decoder or capture path will push this over |
| `tx-queue` | `qdrop>0` | send period at or below the host frame period |
| `invisible-loss` | `sp` short of the 125 fps pace | loss that never reaches a counter |
| `fail-crc` / `fail-sync` | which failure counter dominates | payload corruption vs framing — they lead to different fixes |

An empty result means "no signature I know about", never "healthy". Rules are
deliberately conservative.

Sources: `docs/comparator_rx.md`, `docs/capture_validation_2026-07-21.md`,
`docs/rx_board_b_duty_2026-08-21.md` in the v2 firmware tree.

## Tests

```bash
.venv/Scripts/python.exe -m unittest discover -s tests -v
```

36 tests. Most need neither Qt nor hardware; the UI ones run headless under
`QT_QPA_PLATFORM=offscreen`, which the snapshot module sets for itself.

The diagnostic cases use verbatim bench output rather than synthesised lines, so
a rule that stops recognising the real hardware fails the suite.
`test_ui_structure.py` compares the built window against `ui_baseline.txt`,
captured before the split; regenerate it only when a UI change is intended:

```bash
.venv/Scripts/python.exe tests/ui_snapshot.py > tests/ui_baseline.txt
```

## Status and what is left

Done and verified:

- `model.py`, `config.py` with v1 migration, `diagnostics.py`
- **`app.py` split**: 3488 lines in one class -> a 180-line window plus sixteen
  `ui/` modules, the largest 660 lines. No method lost, no name defined twice,
  MRO checked, and the built window is widget-for-widget identical to the
  pre-split baseline.
- 36 tests: config and diagnostics need nothing; the UI ones run headless, and
  the startup one spawns a fresh process

Five defects the split introduced. The first two were caught by the tests
already in place; the last three were not, and needed new ones:

- `ASSET_DIR` resolved against its own file, so moving `theme.py` into `ui/`
  would have pointed it at a directory that does not exist -- a missing window
  icon and nothing else to show for it.
- Two class-level attributes (`_CONSOLE_PRESETS`, `_MONITOR_UNITS`) were not
  `FunctionDef` nodes and the first extraction pass dropped them.
- `main()` lost its `QtGui` import. The fingerprint test builds `MainWindow`
  directly and never goes through the entry point, so the app failed to start
  while every test passed.
- `ui/text.py` lost `from .. import ssh`, used by `_remote_cd` and `_env`.
- `ui/widgets.py` lost the module-level `_PENDING` set, an `AnnAssign` the
  extractor did not collect. It keeps the `_Signals` object alive until its
  queued connection fires, so without it **every async action in the panel**
  would have raised `NameError` on first use.

The common shape: code reached only on paths no test exercised. The fix is
`test_startup.py`, which runs the real entry point in a subprocess and runs
pyflakes over the package -- that second check finds all three at once, and
`tools/split_app.py` has been hardened so a regeneration cannot reintroduce
them.

Not done:

1. **Make the shared state explicit.** The mixins still reach into attributes
   `MainWindow.__init__` sets up (`self._series`, `self._latest`,
   `self._streamer`, every widget attribute). That coupling is now *visible*
   rather than buried; turning it into an object passed to each tab is the
   next refactor and is a much smaller job from here.
2. **Generate node controls from the list.** Service buttons, the console
   target picker, monitor sources and video route selectors are still written
   out per side. All of them become loops over `cfg.registry`. This is now a
   change inside single-purpose modules instead of inside a 3488-line class.
3. **Retire the compatibility block** in `config.py` once (2) lands.
4. **iperf: TCP.** `IperfSettings` carries `protocol`, `mss`, `parallel` and
   `bidirectional`; `_iperf_plan()` in `ui/iperf.py` still builds UDP only.
5. **Surface `diagnostics.evaluate()`** in the dashboard, beside the raw
   counters rather than instead of them.

## Tools

    .venv/Scripts/python.exe tools/split_app.py      # one-shot: app.py -> ui/
    .venv/Scripts/python.exe tools/tidy_imports.py   # per-module explicit imports

`tidy_imports.py` is idempotent and safe to re-run at any time.

`split_app.py` is **not**: it reads methods out of `app.py`, which no longer has
them, and refuses with the list it could not find rather than writing anything.
It is kept because it documents the decomposition as executable code -- the
`PLAN` list *is* the module map. To move a method between modules now, move the
method; to record where it belongs, edit `PLAN`.
