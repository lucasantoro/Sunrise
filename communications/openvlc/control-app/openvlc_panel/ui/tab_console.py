"""Console tab: pick a node, send one command.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

from PySide6 import QtWidgets

from .. import ssh
from .widgets import HistoryLineEdit, run_async


class ConsoleTabMixin:
    """Console tab: pick a node, send one command."""

    _CONSOLE_PRESETS = [
        ("Serial",
         "ls -l /dev/serial/by-id/ 2>/dev/null; echo '--- dmesg tty ---'; "
         "dmesg 2>/dev/null | grep -i tty | tail -6"),
        ("Net", "ip -br addr; echo '--- routes ---'; ip route"),
        ("Processes",
         "ps aux | grep -E 'openvlc|iperf|socat|vlc|tx_bridge|rx_bridge' "
         "| grep -v grep"),
        ("Services",
         "systemctl list-units --type=service --state=running 2>/dev/null "
         "| grep -iE 'openvlc|vlc'; echo '--- failed ---'; "
         "systemctl --failed --no-pager 2>/dev/null"),
        ("TRX journal", "journalctl -u openvlc-transceiver -n 100 -o cat --no-pager || true"),
        ("RX journal", "journalctl -u openvlc-rx -n 100 -o cat --no-pager || true"),
        ("Journal", "journalctl -n 60 --no-pager 2>/dev/null | tail -60"),
        ("System", "uptime; echo '--- mem ---'; free -h; echo '--- disk ---'; df -h /"),
    ]


    def _build_console(self) -> QtWidgets.QWidget:
        w = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(w)
        lay.setContentsMargins(18, 16, 18, 18)
        lay.addWidget(self._page_intro(
            "Console",
            "Send an ad-hoc command to a connected board over SSH and read its output.",
        ))
        row = QtWidgets.QHBoxLayout()
        row.addWidget(QtWidgets.QLabel("Target"))
        self.cbo_console_target = QtWidgets.QComboBox()
        self._refresh_console_targets()
        row.addWidget(self.cbo_console_target)
        self.ed_console_cmd = HistoryLineEdit()
        self.ed_console_cmd.setPlaceholderText(
            "type a command, Up/Down for history - e.g. systemctl status openvlc-rx")
        self.ed_console_cmd.returnPressed.connect(lambda: self._console_send(False))
        row.addWidget(self.ed_console_cmd, stretch=1)
        b_run = QtWidgets.QPushButton("Run")
        self._set_button_role(b_run, "primary")
        b_run.clicked.connect(lambda: self._console_send(False))
        b_sudo = QtWidgets.QPushButton("Run sudo")
        b_sudo.clicked.connect(lambda: self._console_send(True))
        row.addWidget(b_run)
        row.addWidget(b_sudo)
        lay.addLayout(row)

        presets = QtWidgets.QHBoxLayout()
        presets.addWidget(QtWidgets.QLabel("Quick diagnostics:"))
        for label, cmd in self._CONSOLE_PRESETS:
            b = QtWidgets.QPushButton(label)
            self._set_button_role(b, "quiet")
            b.setToolTip(cmd)
            b.clicked.connect(lambda _=False, c=cmd: self._console_fill(c))
            presets.addWidget(b)
        presets.addStretch(1)
        lay.addLayout(presets)

        self.txt_console = QtWidgets.QPlainTextEdit()
        self.txt_console.setReadOnly(True)
        self.txt_console.setMaximumBlockCount(4000)
        lay.addLayout(self._log_header("Output", self.txt_console))
        lay.addWidget(self.txt_console, stretch=1)
        return w


    def _console_fill(self, cmd: str) -> None:
        self.ed_console_cmd.setText(cmd)
        self.ed_console_cmd.setFocus()


    def _nodes(self):
        """All known nodes as (label, device, kind); unset hosts are skipped
        so the panel only ever offers boards that are actually configured."""
        candidates = [
            ("RX Pi", self.cfg.rx_pi, "rx"),
            ("TX Pi", self.cfg.tx_pi, "tx"),
            ("BeagleBone", self.cfg.bbb, "bbb"),
            ("Transceiver A", getattr(self.cfg, "trx_a", None), "trx"),
            ("Transceiver B", getattr(self.cfg, "trx_b", None), "trx"),
        ]
        return [(label, dev, kind) for label, dev, kind in candidates
                if dev is not None and dev.host]


    def _refresh_console_targets(self) -> None:
        current = self.cbo_console_target.currentText()
        self.cbo_console_target.clear()
        labels = [label for label, _dev, _kind in self._nodes()]
        if not labels:
            labels = ["RX Pi"]
        self.cbo_console_target.addItems(labels)
        idx = self.cbo_console_target.findText(current)
        if idx >= 0:
            self.cbo_console_target.setCurrentIndex(idx)


    def _console_device(self):
        label = self.cbo_console_target.currentText()
        for node_label, dev, _kind in self._nodes():
            if node_label == label:
                return dev
        return self.cfg.rx_pi


    def _console_send(self, use_sudo: bool) -> None:
        cmd = self.ed_console_cmd.text().strip()
        if not cmd:
            return
        dev = self._console_device()
        label = self.cbo_console_target.currentText()
        self.ed_console_cmd.push(cmd)
        self.txt_console.appendPlainText(
            f"$ [{label}]{' sudo' if use_sudo else ''} {cmd}")
        self.ed_console_cmd.clear()
        runner = ssh.run_sudo if use_sudo else ssh.run

        def work():
            return runner(dev, cmd, timeout=30.0)

        def done(result, error):
            if error:
                self.txt_console.appendPlainText(f"! {error}")
                return
            status, out, err = result
            if out and out.strip():
                self.txt_console.appendPlainText(out.rstrip("\n"))
            if err and err.strip():
                self.txt_console.appendPlainText(err.rstrip("\n"))
            self.txt_console.appendPlainText(f"[exit {status}]")

        run_async(work, done)
