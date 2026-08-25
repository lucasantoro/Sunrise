"""Diagnostics tab: preflight checks and report collection.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

from datetime import datetime
import os

from PySide6 import QtWidgets

from .. import ssh
from .theme import RELAY_PID, RELAY_LOG, TX_VIDEO_LOG
from .widgets import run_async


class DiagnosticsTabMixin:
    """Diagnostics tab: preflight checks and report collection."""

    def _build_diagnostics(self) -> QtWidgets.QWidget:
        w = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(w)
        lay.setContentsMargins(18, 16, 18, 18)
        lay.addWidget(self._page_intro(
            "Diagnostics",
            "Run read-only preflight checks, verify SSH access and collect a support report.",
        ))

        bar = QtWidgets.QHBoxLayout()
        b_check = QtWidgets.QPushButton("Run preflight")
        b_check.clicked.connect(self._run_preflight)
        b_collect = QtWidgets.QPushButton("Collect report")
        b_collect.clicked.connect(self._collect_diagnostics)
        b_test = QtWidgets.QPushButton("Test SSH connections")
        b_test.clicked.connect(self._test_connections)
        bar.addWidget(b_check)
        bar.addWidget(b_collect)
        bar.addWidget(b_test)
        bar.addStretch(1)
        lay.addLayout(bar)

        self.txt_diagnostics = QtWidgets.QPlainTextEdit()
        self.txt_diagnostics.setReadOnly(True)
        self.txt_diagnostics.setMaximumBlockCount(1200)
        lay.addLayout(self._log_header("Diagnostics log", self.txt_diagnostics))
        lay.addWidget(self.txt_diagnostics, stretch=1)
        return w


    def _run_preflight(self):
        self._save_settings(silent=True)
        self._control_log("system check ...")
        video_tx, video_rx, video_dest, video_label = self._video_route()

        def task():
            lines = ["OpenVLC system check", f"timestamp={datetime.now().isoformat()}"]
            lines.append(self._local_summary())
            lines.extend(self._probe_device(
                f"Selected video camera ({video_label})", video_tx, [
                    ("camera", "test -e /dev/video0 && echo /dev/video0-ok || echo missing:/dev/video0"),
                    ("formats", "v4l2-ctl --list-formats-ext -d /dev/video0 2>/dev/null | head -80 || true"),
                    ("route receiver", f"ip route get {ssh.shell_quote(video_dest)} || true"),
                    ("media", "ps -ef | grep -E 'vlc_tx_video|ffmpeg' | grep -v grep || true"),
                ]
            ))
            lines.extend(self._probe_device(
                f"Selected video receiver ({video_label})", video_rx, [
                    ("tun0", "ip -br addr show tun0 || true"),
                    ("listeners", f"ss -lunp | grep ':{self.cfg.video_port}' || true"),
                    ("relay", ssh.pidfile_status_command(RELAY_PID)),
                ]
            ))
            lines.extend(self._probe_device("RX Pi", self.cfg.rx_pi, [
                ("service", "systemctl is-active openvlc-rx || true"),
                ("tun0", "ip -br addr show tun0 || true"),
                ("listeners", f"ss -lunp | grep ':{self.cfg.video_port}' || true"),
                ("tools", "for t in python3 ffmpeg socat iperf; do command -v $t || echo missing:$t; done"),
                ("bridge config", "test -r /etc/default/openvlc-rx && cat /etc/default/openvlc-rx || true"),
            ]))
            lines.extend(self._probe_device("TX Pi", self.cfg.tx_pi, [
                ("route", "ip route get 192.168.0.2 || true"),
                ("tools", "for t in ffmpeg iperf v4l2-ctl; do command -v $t || echo missing:$t; done"),
                ("camera", "test -e /dev/video0 && echo /dev/video0-ok || echo missing:/dev/video0"),
                ("formats", "v4l2-ctl --list-formats-ext -d /dev/video0 2>/dev/null | head -80 || true"),
            ]))
            lines.extend(self._probe_device("BBB TX", self.cfg.bbb, [
                ("profile", "cat /run/openvlc-tx-profile 2>/dev/null || true"),
                ("params", "dmesg | grep 'VLC: params' | tail -1 || true"),
                ("vlc0", "ip -s link show dev vlc0 || true"),
                ("qdisc", "tc -s qdisc show dev vlc0 || true"),
            ]))
            for label, dev, peer_ip in (
                ("Transceiver A", self.cfg.trx_a, self.cfg.trx_b_tun_ip),
                ("Transceiver B", self.cfg.trx_b, self.cfg.trx_a_tun_ip),
            ):
                lines.extend(self._probe_device(label, dev, [
                    ("service", "systemctl is-active openvlc-transceiver || true"),
                    ("tun0", "ip -br addr show tun0 || true"),
                    ("route peer", f"ip route get {ssh.shell_quote(peer_ip)} || true"),
                    ("serial", "ls -l /dev/serial/by-id/ 2>/dev/null || true"),
                    ("tools", "for t in python3 iperf journalctl; do command -v $t || echo missing:$t; done"),
                    ("config", "test -r /etc/default/openvlc-transceiver && cat /etc/default/openvlc-transceiver || true"),
                    ("processes", "ps -ef | grep -E 'vlc_transceiver|openvlc-transceiver|iperf' | grep -v grep || true"),
                ]))
            return "\n".join(lines)

        run_async(task, lambda r, e: self._control_log(str(e) if e else r))


    def _collect_diagnostics(self):
        self._save_settings(silent=True)
        self._control_log("collecting diagnostics ...")
        video_tx, video_rx, video_dest, video_label = self._video_route()

        def task():
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            path = os.path.join(os.path.expanduser("~"), "Downloads", f"openvlc_diag_{stamp}.txt")
            sections = [
                "OpenVLC diagnostics",
                f"timestamp={datetime.now().isoformat()}",
                self._local_summary(),
            ]
            sections.extend(self._probe_device(
                f"Selected video camera ({video_label})", video_tx, [
                    ("routes", "ip route || true"),
                    ("route receiver", f"ip route get {ssh.shell_quote(video_dest)} || true"),
                    ("camera", "v4l2-ctl --list-formats-ext -d /dev/video0 2>/dev/null || true"),
                    ("media", "ps -ef | grep -E 'vlc_tx_video|ffmpeg' | grep -v grep || true"),
                    ("tx video log", f"tail -120 {TX_VIDEO_LOG} 2>/dev/null || true"),
                ]
            ))
            sections.extend(self._probe_device(
                f"Selected video receiver ({video_label})", video_rx, [
                    ("tun0", "ip -s addr show tun0 || true"),
                    ("listeners", f"ss -lunp | grep ':{self.cfg.video_port}' || true"),
                    ("relay", ssh.pidfile_status_command(RELAY_PID)),
                    ("relay log", f"tail -120 {RELAY_LOG} 2>/dev/null || true"),
                ]
            ))
            sections.extend(self._probe_device("RX Pi", self.cfg.rx_pi, [
                ("journal", "journalctl -u openvlc-rx -n 180 -o cat || true"),
                ("service", "systemctl status openvlc-rx --no-pager || true"),
                ("tun0", "ip -s addr show tun0 || true"),
                ("routes", "ip route || true"),
                ("media", "ps -ef | grep -E 'vlc_rx_bridge|socat|ffmpeg' | grep -v grep || true"),
            ]))
            sections.extend(self._probe_device("TX Pi", self.cfg.tx_pi, [
                ("routes", "ip route || true"),
                ("camera", "v4l2-ctl --list-formats-ext -d /dev/video0 2>/dev/null || true"),
                ("media", "ps -ef | grep -E 'vlc_tx_video|ffmpeg' | grep -v grep || true"),
                ("tx video log", f"tail -120 {TX_VIDEO_LOG} 2>/dev/null || true"),
            ]))
            sections.extend(self._probe_device("BBB TX", self.cfg.bbb, [
                ("profile", "cat /run/openvlc-tx-profile 2>/dev/null || true"),
                ("params", "dmesg | grep 'VLC: params' | tail -5 || true"),
                ("vlc0", "ip -s link show dev vlc0 || true"),
                ("stats", f"test -x {self.cfg.bbb_tx_dir}/raspberry/vlc_bbb_tx_stats.sh && WINDOW=10 bash {self.cfg.bbb_tx_dir}/raspberry/vlc_bbb_tx_stats.sh || true"),
            ]))
            for label, dev, peer_ip in (
                ("Transceiver A", self.cfg.trx_a, self.cfg.trx_b_tun_ip),
                ("Transceiver B", self.cfg.trx_b, self.cfg.trx_a_tun_ip),
            ):
                sections.extend(self._probe_device(label, dev, [
                    ("journal", "journalctl -u openvlc-transceiver -n 240 -o cat || true"),
                    ("service", "systemctl status openvlc-transceiver --no-pager || true"),
                    ("config", "test -r /etc/default/openvlc-transceiver && cat /etc/default/openvlc-transceiver || true"),
                    ("tun0", "ip -s addr show tun0 || true"),
                    ("routes", "ip route || true"),
                    ("route peer", f"ip route get {ssh.shell_quote(peer_ip)} || true"),
                    ("serial", "ls -l /dev/serial/by-id/ 2>/dev/null; dmesg 2>/dev/null | grep -i tty | tail -20"),
                    ("processes", "ps -ef | grep -E 'vlc_transceiver|openvlc-transceiver|iperf' | grep -v grep || true"),
                ]))
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("\n\n".join(sections))
            return path

        run_async(task, lambda r, e: self._control_log(f"diagnostics failed: {e}" if e else f"diagnostics saved: {r}"))
