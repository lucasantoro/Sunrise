"""Control tab: verify, prepare the link, run a performance test.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

from PySide6 import QtWidgets

from .theme import IPERF_DIRECTIONS


class ControlTabMixin:
    """Control tab: verify, prepare the link, run a performance test."""

    def _build_control(self) -> QtWidgets.QWidget:
        w = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(w)
        lay.setContentsMargins(18, 16, 18, 18)
        lay.setSpacing(12)
        lay.addWidget(self._page_intro(
            "Link control",
            "Prepare the optical chain first, then run a measured performance test. "
            "Live video is managed entirely from the Video tab.",
        ))

        stages = QtWidgets.QHBoxLayout()
        stages.setSpacing(12)

        gb_check = QtWidgets.QGroupBox("1  Verify")
        check_lay = QtWidgets.QVBoxLayout(gb_check)
        check_lay.addWidget(QtWidgets.QLabel(
            "Read-only checks for SSH, routes, services, tools and camera."
        ))
        b_check = QtWidgets.QPushButton("Run system check")
        self._set_button_role(b_check, "primary")
        b_check.clicked.connect(self._run_preflight)
        check_lay.addWidget(b_check)
        check_lay.addStretch(1)
        stages.addWidget(gb_check, stretch=1)

        gb_prepare = QtWidgets.QGroupBox("2  Prepare link")
        prepare_lay = QtWidgets.QGridLayout(gb_prepare)
        b_monitor = QtWidgets.QPushButton("Open selected journal")
        b_monitor.setToolTip("Start/stop the live journal monitor selected in the Link Quality tab.")
        self._monitor_buttons.append(b_monitor)
        b_monitor.clicked.connect(self._toggle_monitor)
        b_bridge = QtWidgets.QPushButton("Restart legacy RX bridge")
        b_bridge.setToolTip("Legacy chain only: restart systemd service openvlc-rx on the RX Pi.")
        b_bridge.clicked.connect(lambda: self._bridge_ctl("restart"))
        b_route = QtWidgets.QPushButton("Configure legacy TX route")
        b_route.setToolTip("Legacy chain only: configure routing on the TX Pi toward the optical receiver.")
        b_route.clicked.connect(self._setup_tx_pi)
        self.btn_bbb_tx = QtWidgets.QPushButton()
        self.btn_bbb_tx.setToolTip("Legacy chain only: start the BeagleBone optical transmitter profile.")
        self._update_budget_button()
        self.btn_bbb_tx.clicked.connect(self._start_bbb_tx)
        prepare_lay.addWidget(b_monitor, 0, 0)
        prepare_lay.addWidget(b_bridge, 0, 1)
        prepare_lay.addWidget(b_route, 1, 0)
        prepare_lay.addWidget(self.btn_bbb_tx, 1, 1)
        stages.addWidget(gb_prepare, stretch=2)

        gb_manage = QtWidgets.QGroupBox("Link services / journals")
        manage_lay = QtWidgets.QVBoxLayout(gb_manage)
        manage_lay.addWidget(QtWidgets.QLabel("RX bridge (openvlc-rx)"))
        service_row = QtWidgets.QHBoxLayout()
        for label, cmd in [
            ("Start legacy RX service", "start"),
            ("Stop legacy RX service", "stop"),
            ("Restart legacy RX service", "restart"),
        ]:
            b = QtWidgets.QPushButton(label)
            b.setProperty("role", "quiet")
            b.setToolTip(f"Run 'systemctl {cmd} openvlc-rx' on the legacy RX Pi.")
            b.clicked.connect(lambda _=False, c=cmd: self._bridge_ctl(c))
            service_row.addWidget(b)
        manage_lay.addLayout(service_row)
        manage_lay.addWidget(QtWidgets.QLabel("Transceivers (openvlc-transceiver)"))
        both_row = QtWidgets.QHBoxLayout()
        for label, cmd in [
            ("Start both TRX services", "start"),
            ("Stop both TRX services", "stop"),
            ("Restart both TRX services", "restart"),
        ]:
            b = QtWidgets.QPushButton(label)
            b.setProperty("role", "quiet")
            b.setToolTip(f"Run 'systemctl {cmd} openvlc-transceiver' on configured nodes A and B.")
            b.clicked.connect(lambda _=False, c=cmd: self._trx_ctl_all(c))
            both_row.addWidget(b)
        manage_lay.addLayout(both_row)
        for attr, short in (("trx_a", "TRX A"), ("trx_b", "TRX B")):
            trx_row = QtWidgets.QHBoxLayout()
            for label, cmd in [(f"Start {short} service", "start"),
                               (f"Stop {short} service", "stop"),
                               (f"Restart {short} service", "restart"),
                               (f"Show {short} journal", "journal")]:
                b = QtWidgets.QPushButton(label)
                b.setProperty("role", "quiet")
                if cmd == "journal":
                    b.setToolTip(f"Show the live openvlc-transceiver journal for {short}.")
                    b.clicked.connect(lambda _=False, a=attr: self._monitor_trx(a))
                else:
                    b.setToolTip(f"Run 'systemctl {cmd} openvlc-transceiver' on {short}.")
                    b.clicked.connect(
                        lambda _=False, a=attr, c=cmd: self._trx_ctl(a, c))
                trx_row.addWidget(b)
            manage_lay.addLayout(trx_row)
        stop_journal = QtWidgets.QPushButton("Stop journal monitor")
        stop_journal.setProperty("role", "quiet")
        stop_journal.setToolTip("Stop the active live journal stream without stopping the transceiver service.")
        stop_journal.clicked.connect(self._stop_monitor)
        manage_lay.addWidget(stop_journal)
        note = QtWidgets.QLabel(
            "The transceiver service is full-duplex. Choose the iperf direction "
            "to test TX or RX separately; do not stop half of the daemon."
        )
        note.setObjectName("sectionSubtitle")
        note.setWordWrap(True)
        manage_lay.addWidget(note)
        manage_lay.addStretch(1)
        stages.addWidget(gb_manage, stretch=1)
        lay.addLayout(stages)

        gb_ip = QtWidgets.QGroupBox("3  Performance test - iperf UDP")
        hi = QtWidgets.QHBoxLayout(gb_ip)
        hi.addWidget(QtWidgets.QLabel("Direction"))
        self.cbo_iperf_direction = QtWidgets.QComboBox()
        for label, key in IPERF_DIRECTIONS:
            self.cbo_iperf_direction.addItem(label, key)
        self.cbo_iperf_direction.currentIndexChanged.connect(self._update_iperf_button)
        hi.addWidget(self.cbo_iperf_direction)
        hi.addWidget(QtWidgets.QLabel("Target rate"))
        self.ed_rate = QtWidgets.QLineEdit(self.cfg.iperf_rate)
        self.ed_rate.setFixedWidth(92)
        hi.addWidget(self.ed_rate)
        hi.addWidget(QtWidgets.QLabel("Duration"))
        self.sp_iperf_duration = QtWidgets.QSpinBox()
        self.sp_iperf_duration.setRange(1, 3600)
        self.sp_iperf_duration.setValue(max(1, int(self.cfg.iperf_duration)))
        self.sp_iperf_duration.setSuffix(" s")
        self.sp_iperf_duration.setFixedWidth(96)
        self.sp_iperf_duration.valueChanged.connect(self._update_iperf_button)
        hi.addWidget(self.sp_iperf_duration)
        self.btn_iperf = QtWidgets.QPushButton()
        self._set_button_role(self.btn_iperf, "primary")
        self.btn_iperf.setToolTip(
            "Start receiver-side iperf server(s), then transmitter-side client(s), "
            "and collect authoritative receiver results."
        )
        self.btn_iperf.clicked.connect(self._run_iperf)
        self._update_iperf_button()
        hi.addWidget(self.btn_iperf)
        self.btn_stop_iperf = QtWidgets.QPushButton("Stop iperf")
        self.btn_stop_iperf.setEnabled(False)
        self.btn_stop_iperf.setToolTip("Stop any iperf server/client process started by this app.")
        self.btn_stop_iperf.clicked.connect(self._stop_iperf)
        hi.addWidget(self.btn_stop_iperf)
        note = QtWidgets.QLabel(
            "Receiver-side iperf output is authoritative. The journal monitor "
            "follows the selected receive node when possible."
        )
        note.setObjectName("sectionSubtitle")
        hi.addWidget(note)
        hi.addStretch(1)
        lay.addWidget(gb_ip)

        self.txt_control = QtWidgets.QPlainTextEdit()
        self.txt_control.setReadOnly(True)
        self.txt_control.setMaximumBlockCount(800)
        lay.addLayout(self._log_header("Activity log", self.txt_control))
        lay.addWidget(self.txt_control, stretch=1)
        return w
