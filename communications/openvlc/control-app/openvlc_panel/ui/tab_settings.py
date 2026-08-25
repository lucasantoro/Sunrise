"""Settings tab: endpoints, paths, ports and validated profiles.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

import os

from PySide6 import QtCore, QtWidgets

from .. import ssh
from ..config import Device
from .theme import VALID_BUDGETS
from .widgets import run_async


class SettingsTabMixin:
    """Settings tab: endpoints, paths, ports and validated profiles."""

    def _build_settings(self) -> QtWidgets.QWidget:
        w = QtWidgets.QWidget()
        page = QtWidgets.QVBoxLayout(w)
        page.setContentsMargins(18, 16, 18, 18)
        page.addWidget(self._page_intro(
            "Settings",
            "Configure lab endpoints, local playback and validated optical profiles.",
        ))
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        content = QtWidgets.QWidget()
        outer = QtWidgets.QVBoxLayout(content)
        outer.setContentsMargins(0, 0, 8, 0)
        self._dev_edits = {}

        def dev_row(title: str, dev: Device, with_jump: bool):
            box = QtWidgets.QGroupBox(title)
            fl = QtWidgets.QFormLayout(box)
            edits = {}
            edits["host"] = QtWidgets.QLineEdit(dev.host)
            edits["user"] = QtWidgets.QLineEdit(dev.user)
            edits["password"] = QtWidgets.QLineEdit(dev.password)
            edits["password"].setEchoMode(QtWidgets.QLineEdit.Password)
            fl.addRow("host", edits["host"])
            fl.addRow("user", edits["user"])
            fl.addRow("password", edits["password"])
            if with_jump:
                edits["jump_host"] = QtWidgets.QLineEdit(dev.jump_host)
                edits["jump_user"] = QtWidgets.QLineEdit(dev.jump_user)
                edits["jump_password"] = QtWidgets.QLineEdit(dev.jump_password)
                edits["jump_password"].setEchoMode(QtWidgets.QLineEdit.Password)
                fl.addRow("jump host (TX Pi)", edits["jump_host"])
                fl.addRow("jump user", edits["jump_user"])
                fl.addRow("jump password", edits["jump_password"])
            outer.addWidget(box)
            return edits

        self._dev_edits["rx_pi"] = dev_row("RX Raspberry Pi (bridge)", self.cfg.rx_pi, False)
        self._dev_edits["tx_pi"] = dev_row("TX Raspberry Pi (feeds BBB)", self.cfg.tx_pi, False)
        self._dev_edits["bbb"] = dev_row("BeagleBone TX (via TX Pi)", self.cfg.bbb, True)
        self._dev_edits["trx_a"] = dev_row(
            "Transceiver A Pi (openvlc-transceiver) - empty host hides it",
            self.cfg.trx_a, False)
        self._dev_edits["trx_b"] = dev_row(
            "Transceiver B Pi (openvlc-transceiver) - empty host hides it",
            self.cfg.trx_b, False)

        misc = QtWidgets.QGroupBox("PC and validated TX profile")
        ml = QtWidgets.QFormLayout(misc)
        self.ed_pc_ip = QtWidgets.QLineEdit(self.cfg.pc_ip)
        self.ed_vlc = QtWidgets.QLineEdit(self.cfg.vlc_path)
        self.cb_budget = QtWidgets.QComboBox()
        for budget, info in sorted(VALID_BUDGETS.items(), reverse=True):
            self.cb_budget.addItem(info["label"], budget)
        idx = self.cb_budget.findData(self.cfg.tx_budget)
        self.cb_budget.setCurrentIndex(idx if idx >= 0 else self.cb_budget.findData(50))
        ml.addRow("this PC IP (UDP target)", self.ed_pc_ip)
        ml.addRow("VLC path", self.ed_vlc)
        ml.addRow("TX budget profile", self.cb_budget)
        outer.addWidget(misc)

        adv = QtWidgets.QGroupBox("Advanced paths and ports")
        al = QtWidgets.QFormLayout(adv)
        self.ed_rx_dir = QtWidgets.QLineEdit(self.cfg.rx_gateway_dir)
        self.ed_tx_dir = QtWidgets.QLineEdit(self.cfg.tx_gateway_dir)
        self.ed_bbb_dir = QtWidgets.QLineEdit(self.cfg.bbb_tx_dir)
        self.ed_trx_a_tun_ip = QtWidgets.QLineEdit(self.cfg.trx_a_tun_ip)
        self.ed_trx_b_tun_ip = QtWidgets.QLineEdit(self.cfg.trx_b_tun_ip)
        self.sp_serial_baud = self._spin(115200, 4000000, self.cfg.serial_baud)
        self.sp_iperf_port = self._spin(1, 65535, self.cfg.iperf_port)
        self.sp_iperf_payload = self._spin(64, 8972, self.cfg.iperf_payload)
        self.sp_video_port = self._spin(1, 65535, self.cfg.video_port)
        self.sp_relay_port = self._spin(1, 65535, self.cfg.relay_port)
        al.addRow("RX gateway dir", self.ed_rx_dir)
        al.addRow("TX gateway dir", self.ed_tx_dir)
        al.addRow("BBB TX dir", self.ed_bbb_dir)
        al.addRow("Transceiver A TUN IP", self.ed_trx_a_tun_ip)
        al.addRow("Transceiver B TUN IP", self.ed_trx_b_tun_ip)
        al.addRow("serial baud", self.sp_serial_baud)
        al.addRow("iperf UDP port", self.sp_iperf_port)
        al.addRow("iperf UDP payload bytes", self.sp_iperf_payload)
        al.addRow("video UDP port", self.sp_video_port)
        al.addRow("PC relay port", self.sp_relay_port)
        outer.addWidget(adv)

        btns = QtWidgets.QHBoxLayout()
        b_save = QtWidgets.QPushButton("Save")
        b_save.clicked.connect(self._save_settings)
        b_test = QtWidgets.QPushButton("Test connections")
        b_test.clicked.connect(self._test_connections)
        b_check = QtWidgets.QPushButton("Check system")
        b_check.clicked.connect(self._run_preflight)
        btns.addWidget(b_save)
        btns.addWidget(b_test)
        btns.addWidget(b_check)
        btns.addStretch(1)
        outer.addLayout(btns)
        outer.addStretch(1)
        scroll.setWidget(content)
        page.addWidget(scroll, stretch=1)
        return w


    def _save_settings(self, silent: bool = False):
        for name, edits in self._dev_edits.items():
            dev: Device = getattr(self.cfg, name)
            for key, ed in edits.items():
                setattr(dev, key, ed.text())
        self.cfg.pc_ip = self.ed_pc_ip.text().strip()
        self.cfg.vlc_path = self.ed_vlc.text().strip()
        self.cfg.tx_budget = int(self.cb_budget.currentData())
        self.cfg.rx_gateway_dir = self.ed_rx_dir.text().strip()
        self.cfg.tx_gateway_dir = self.ed_tx_dir.text().strip()
        self.cfg.bbb_tx_dir = self.ed_bbb_dir.text().strip()
        self.cfg.trx_a_tun_ip = self.ed_trx_a_tun_ip.text().strip() or "192.168.0.1"
        self.cfg.trx_b_tun_ip = self.ed_trx_b_tun_ip.text().strip() or "192.168.0.2"
        if hasattr(self, "cb_video_camera_node"):
            route = self.cb_video_camera_node.currentData()
            self.cfg.video_camera_node = (
                route if route in ("a", "b", "legacy") else "b"
            )
        self.cfg.serial_baud = self.sp_serial_baud.value()
        self.cfg.iperf_port = self.sp_iperf_port.value()
        self.cfg.iperf_payload = self.sp_iperf_payload.value()
        self.cfg.iperf_duration = self.sp_iperf_duration.value()
        self.cfg.video_port = self.sp_video_port.value()
        self.cfg.relay_port = self.sp_relay_port.value()
        self.cfg.save()
        self._video_camera_route_changed()
        self._update_budget_button()
        self._apply_video_capacity_hint()
        if hasattr(self, "cbo_console_target"):
            self._refresh_console_targets()
        if hasattr(self, "cbo_monitor_source"):
            self._refresh_monitor_sources()
        if not silent:
            self._status("settings saved")


    def _test_connections(self):
        self._save_settings(silent=True)

        def task():
            lines = []
            for name in ("rx_pi", "tx_pi", "bbb", "trx_a", "trx_b"):
                dev = getattr(self.cfg, name)
                if not dev.host:
                    lines.append(f"{name}: skipped (empty host)")
                    continue
                ok, msg = ssh.ping(dev)
                lines.append(f"{name}: {'OK' if ok else 'FAIL'} ({msg})")
            return "\n".join(lines)

        run_async(task, lambda r, e: QtWidgets.QMessageBox.information(
            self, "Connection test", str(e) if e else r))


    def _probe_device(self, title: str, dev: Device, commands: list[tuple[str, str]]) -> list[str]:
        lines = [f"## {title}"]
        if not dev.host:
            lines.append("SKIP: empty host")
            return lines
        ok, msg = ssh.ping(dev)
        lines.append(f"ssh={'OK' if ok else 'FAIL'} {msg}")
        if not ok:
            if "getaddrinfo" in msg:
                lines.append(
                    "hint: host name did not resolve. Use a numeric IP address "
                    "or fix DNS. For the BBB, the usual direct USB-gadget "
                    "target is 192.168.7.2; if it is reachable only through "
                    "the TX Pi, set that Pi as the jump host in Settings."
                )
            return lines
        for label, cmd in commands:
            try:
                st, out, err = ssh.run(dev, cmd, timeout=20.0)
                body = (out + err).strip()
                lines.append(f"$ {label}: {cmd}\n[exit {st}]\n{body}")
            except Exception as exc:  # noqa: BLE001
                lines.append(f"$ {label}: {cmd}\nERROR: {exc}")
        return lines


    def _local_summary(self) -> str:
        budget = self.cfg.tx_budget
        budget_ok = "OK" if budget in VALID_BUDGETS else "INVALID"
        vlc_ok = "OK" if os.path.exists(self.cfg.vlc_path) else "MISSING"
        video_tx, video_rx, video_dest, video_label = self._video_route()
        return (
            "## Local config\n"
            f"pc_ip={self.cfg.pc_ip}\n"
            f"vlc_path={self.cfg.vlc_path} ({vlc_ok})\n"
            f"tx_budget={budget} ({budget_ok})\n"
            f"rx_pi={self._device_summary(self.cfg.rx_pi)}\n"
            f"tx_pi={self._device_summary(self.cfg.tx_pi)}\n"
            f"bbb={self._device_summary(self.cfg.bbb)}\n"
            f"trx_a={self._device_summary(self.cfg.trx_a)} tun={self.cfg.trx_a_tun_ip}\n"
            f"trx_b={self._device_summary(self.cfg.trx_b)} tun={self.cfg.trx_b_tun_ip}\n"
            f"video_route={self.cfg.video_camera_node} ({video_label}) "
            f"camera={self._device_summary(video_tx)} "
            f"receiver={self._device_summary(video_rx)} dest={video_dest}\n"
            f"rx_gateway_dir={self.cfg.rx_gateway_dir}\n"
            f"tx_gateway_dir={self.cfg.tx_gateway_dir}\n"
            f"bbb_tx_dir={self.cfg.bbb_tx_dir}\n"
            f"ports iperf={self.cfg.iperf_port} payload={self.cfg.iperf_payload} "
            f"video={self.cfg.video_port} relay={self.cfg.relay_port}\n"
            f"serial_baud={self.cfg.serial_baud}"
        )


    @staticmethod
    def _device_summary(dev: Device) -> str:
        target = f"{dev.user + '@' if dev.user else ''}{dev.host or '(empty)'}:{dev.port}"
        if dev.jump_host:
            jump = (
                f"{dev.jump_user + '@' if dev.jump_user else ''}"
                f"{dev.jump_host}:{dev.jump_port}"
            )
            return f"{target} via {jump}"
        return target


    def _update_budget_button(self):
        if hasattr(self, "btn_bbb_tx"):
            self.btn_bbb_tx.setText(f"Start BBB TX (budget {self.cfg.tx_budget})")
