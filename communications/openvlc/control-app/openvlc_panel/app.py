"""OpenVLC desktop control panel.

Tabs:
  Control      - link preparation and configurable performance tests
  Link Quality - live channel health, score, and charts
  AGC tuning   - comparator threshold sweep capture
  Video        - live view and TX video presets
  Diagnostics  - preflight and report collection
  Console      - one-shot commands against any configured node
  Settings     - nodes, paths, ports, and validated profiles

All device work happens over SSH on worker threads so the UI stays responsive.

This module is now only the window itself: assembly, shared state and teardown.
Every tab and subsystem lives in ``openvlc_panel.ui`` -- see that package's
docstring for the layering and for why the split is by mixin.
"""

from __future__ import annotations

import subprocess
import threading
import time
from collections import deque
from typing import Optional

from PySide6 import QtCore, QtGui, QtWidgets

from . import ssh
from .config import Config, Device
from .ui import PANEL_MIXINS
from .ui.theme import (AGC_FEATURE_READY, APP_MARK, APP_STYLE, ASSET_DIR,
                       HISTORY, POLL_SECONDS, VALID_BUDGETS)
from .ui.widgets import LogBridge


class MainWindow(*PANEL_MIXINS, QtWidgets.QMainWindow):
    """The panel window.

    Holds the configuration and the state every tab reads, and nothing else;
    the behaviour comes from the mixins in ``openvlc_panel.ui``. Those mixins
    reach into the attributes set up here -- making that reach explicit is the
    next refactor, and is far easier now that the readers are separated.
    """

    def __init__(self):
        super().__init__()
        self.cfg = Config.load()
        if self.cfg.tx_budget not in VALID_BUDGETS:
            self.cfg.tx_budget = 50
        if self.cfg.video_camera_node not in ("a", "b", "legacy"):
            self.cfg.video_camera_node = "b"

        self.setWindowTitle("OpenVLC Control Panel")
        self.resize(1120, 760)

        self._streamer: Optional[ssh.LogStreamer] = None
        self._log_bridge = LogBridge()
        self._log_bridge.line.connect(self._on_log_line)
        self._log_bridge.error.connect(self._on_log_error)
        self._iperf_bridge = LogBridge()
        self._iperf_bridge.line.connect(self._control_log)
        self._vlc_proc: Optional[subprocess.Popen] = None
        self._vlc_instance = None
        self._vlc_player = None

        self._t0 = time.time()
        self._series = {
            k: (deque(maxlen=HISTORY), deque(maxlen=HISTORY))
            for k in ("rate", "seq_gap", "ringdrop", "ok", "score", "jitter")
        }
        self._latest: dict[str, dict] = {}
        self._latest_at: dict[str, float] = {}
        self._quality_samples: deque = deque(maxlen=600)
        self._counter_values: dict[tuple[str, str], float] = {}
        self._counter_deltas: dict[tuple[str, str], float] = {}
        self._last_log_at = 0.0
        self._last_goodput_kbps = 0.0
        self._stale_marked = False
        self._video_session_starting = False
        self._video_session_active = False
        self._video_session_generation = 0
        self._video_url = ""
        self._video_route_devices: Optional[tuple[Device, Device, str, str]] = None
        self._monitor_buttons: list[QtWidgets.QPushButton] = []
        self._streamer_source_label: Optional[str] = None
        self._iperf_running = False
        self._iperf_stop_event = threading.Event()
        self._iperf_active_devices: list[Device] = []

        self.tabs = QtWidgets.QTabWidget()
        self.tabs.setDocumentMode(True)
        self.tabs.addTab(self._build_control(), "Control")
        self.tabs.addTab(self._build_dashboard(), "Link Quality")
        self.tabs.addTab(self._build_agc(),
                         "AGC tuning" if AGC_FEATURE_READY else "AGC tuning (soon)")
        self.video_tab_index = self.tabs.addTab(self._build_video(), "Video")
        self.tabs.addTab(self._build_diagnostics(), "Diagnostics")
        self.tabs.addTab(self._build_console(), "Console")
        self.tabs.addTab(self._build_settings(), "Settings")

        shell = QtWidgets.QWidget()
        shell_lay = QtWidgets.QVBoxLayout(shell)
        shell_lay.setContentsMargins(0, 0, 0, 0)
        shell_lay.setSpacing(0)
        shell_lay.addWidget(self._build_app_header())
        shell_lay.addWidget(self.tabs, stretch=1)
        self.setCentralWidget(shell)

        self._health_timer = QtCore.QTimer(self)
        self._health_timer.timeout.connect(self._refresh_link_health)
        self._health_timer.start(int(POLL_SECONDS * 1000))

        self._apply_video_capacity_hint()
        self.statusBar().showMessage("Ready - set devices in Settings, then Check system.")

    def closeEvent(self, event):
        if self._streamer is not None:
            self._streamer.stop()
        self._stop_embedded()
        self._stop_vlc()
        super().closeEvent(event)


def main():
    app = QtWidgets.QApplication([])
    app.setApplicationName("OpenVLC Control Panel")
    app.setOrganizationName("OpenVLC")
    app.setStyle("Fusion")
    app.setStyleSheet(APP_STYLE)
    app.setWindowIcon(QtGui.QIcon(str(APP_MARK)))

    splash_pixmap = QtGui.QPixmap(620, 330)
    splash_pixmap.fill(QtCore.Qt.transparent)
    painter = QtGui.QPainter(splash_pixmap)
    painter.setRenderHint(QtGui.QPainter.Antialiasing)
    background = QtGui.QLinearGradient(0, 0, 620, 330)
    background.setColorAt(0.0, QtGui.QColor("#101C32"))
    background.setColorAt(1.0, QtGui.QColor("#1A3154"))
    painter.setBrush(background)
    painter.setPen(QtCore.Qt.NoPen)
    painter.drawRoundedRect(0, 0, 620, 330, 18, 18)
    mark = QtGui.QIcon(str(APP_MARK)).pixmap(132, 132)
    painter.drawPixmap(244, 34, mark)
    painter.setPen(QtGui.QColor("#FFFFFF"))
    title_font = QtGui.QFont("Segoe UI", 24, QtGui.QFont.Bold)
    painter.setFont(title_font)
    painter.drawText(
        QtCore.QRect(0, 174, 620, 50),
        QtCore.Qt.AlignHCenter | QtCore.Qt.AlignVCenter,
        "OpenVLC Control Panel",
    )
    painter.setPen(QtGui.QColor("#AFC1DE"))
    painter.setFont(QtGui.QFont("Segoe UI", 11))
    painter.drawText(
        QtCore.QRect(0, 222, 620, 32),
        QtCore.Qt.AlignHCenter | QtCore.Qt.AlignVCenter,
        "Preparing optical link controls and live telemetry",
    )
    painter.setBrush(QtGui.QColor("#39D9FF"))
    painter.drawRoundedRect(154, 285, 312, 4, 2, 2)
    painter.end()

    splash = QtWidgets.QSplashScreen(splash_pixmap)
    splash.setWindowFlag(QtCore.Qt.WindowStaysOnTopHint)
    splash.show()
    app.processEvents()

    win = MainWindow()

    def reveal():
        win.show()
        splash.finish(win)

    QtCore.QTimer.singleShot(700, reveal)
    app.exec()


if __name__ == "__main__":
    main()
