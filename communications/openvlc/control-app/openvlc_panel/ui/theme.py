"""Constants, presets and the application stylesheet.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

import os
from pathlib import Path


POLL_SECONDS = 1.0


HISTORY = 120


VALID_BUDGETS = {
    50: {
        "script": "TX_setup_budget50.sh",
        "label": "50 - validated profile (~1.0 Mbps optical service)",
    },
    40: {
        "script": "TX_setup_budget40.sh",
        "label": "40 - experimental faster profile",
    },
}


VIDEO_PRESETS = {
    "Stable 360p15": {
        "size": "640x360",
        "outsize": "640x360",
        "fps": "15",
        "outfps": "15",
        "bitrate": "450k",
        "muxrate": "580k",
        "mode": "cbr",
        "crf": "28",
        "preset": "veryfast",
        "profile": "main",
        "bufsize": "120k",
        "muxdelay": "0.6",
        "gop": "5",
        "slice_bytes": "350",
    },
    "Sharp 480p15": {
        "size": "864x480",
        "outsize": "864x480",
        "fps": "15",
        "outfps": "15",
        "bitrate": "680k",
        "muxrate": "800k",
        "mode": "capped-crf",
        "crf": "24",
        "preset": "veryfast",
        "bufsize": "220k",
        "muxdelay": "0.6",
    },
    "Low latency 360p20": {
        "size": "640x360",
        "outsize": "640x360",
        "fps": "20",
        "outfps": "20",
        "bitrate": "560k",
        "muxrate": "760k",
        "mode": "cbr",
        "crf": "28",
        "preset": "ultrafast",
        "bufsize": "140k",
        "muxdelay": "0.45",
    },
}


RELAY_PID = "/tmp/openvlc_panel_relay.pid"


RELAY_LOG = "/tmp/openvlc_panel_relay.log"


TX_VIDEO_PID = "/tmp/openvlc_panel_tx_video.pid"


TX_VIDEO_LOG = "/tmp/openvlc_panel_tx_video.log"


IPERF_SERVER_PID = "/tmp/openvlc_panel_iperf_server.pid"


IPERF_SERVER_LOG = "/tmp/openvlc_panel_iperf_server.log"


IPERF_CLIENT_PID = "/tmp/openvlc_panel_iperf_client.pid"


IPERF_CLIENT_LOG = "/tmp/openvlc_panel_iperf_client.log"


IPERF_DIRECTIONS = [
    ("Transceiver A -> B", "trx_a_to_b"),
    ("Transceiver B -> A", "trx_b_to_a"),
    ("Transceiver full duplex", "trx_full"),
    ("Legacy TX Pi -> RX Pi", "legacy_tx_to_rx"),
]


VIDEO_CAMERA_ROUTES = [
    ("Camera on Node B → receive on Node A", "b"),
    ("Camera on Node A → receive on Node B", "a"),
    ("Legacy TX Pi → RX Pi", "legacy"),
]


AGC_FEATURE_READY = False


# Anchored to the PACKAGE root, not this file: theme.py lives in ui/
# while assets/ stayed beside __init__.py.
ASSET_DIR = Path(__file__).resolve().parent.parent / "assets"


APP_MARK = ASSET_DIR / "openvlc-mark.svg"


APP_STYLE = """
QMainWindow, QWidget {
    background: #F3F6FA;
    color: #172033;
    font-family: "Segoe UI";
    font-size: 10pt;
}
QLabel, QRadioButton, QCheckBox {
    background: transparent;
}
/* Checkboxes/radios were invisible on the white cards: give the indicator an
 * explicit border + fill and a clear checked state. */
QCheckBox::indicator, QRadioButton::indicator {
    width: 18px;
    height: 18px;
    border: 2px solid #8C9BB5;
    background: #FFFFFF;
}
QCheckBox::indicator {
    border-radius: 4px;
}
QRadioButton::indicator {
    border-radius: 10px;
}
QCheckBox::indicator:hover, QRadioButton::indicator:hover {
    border-color: #1677E8;
}
QCheckBox::indicator:checked {
    background: #1677E8;
    border-color: #1677E8;
    image: url(none);
}
QRadioButton::indicator:checked {
    background: #1677E8;
    border: 5px solid #FFFFFF;
    outline: 2px solid #1677E8;
}
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {
    border-color: #C8D2E1;
    background: #EEF2F7;
}
QWidget#appHeader {
    background: #13213A;
    border-bottom: 1px solid #263B5F;
}
QLabel#appTitle {
    color: #FFFFFF;
    font-size: 20pt;
    font-weight: 700;
}
QLabel#appSubtitle {
    color: #AFC1DE;
    font-size: 9.5pt;
}
QLabel#sectionTitle {
    color: #172033;
    font-size: 17pt;
    font-weight: 700;
}
QLabel#sectionSubtitle {
    color: #66758F;
}
QFrame#card, QGroupBox {
    background: #FFFFFF;
    border: 1px solid #DCE3ED;
    border-radius: 10px;
}
QGroupBox {
    margin-top: 12px;
    padding: 16px 12px 12px 12px;
    font-weight: 650;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 14px;
    padding: 0 5px;
    color: #31415E;
}
QPushButton {
    min-height: 32px;
    padding: 0 14px;
    border: 1px solid #C8D2E1;
    border-radius: 7px;
    background: #FFFFFF;
    color: #20304C;
    font-weight: 600;
}
QPushButton:hover {
    background: #EDF5FF;
    border-color: #79AFFF;
}
QPushButton:pressed {
    background: #DDEBFF;
}
QPushButton[role="primary"] {
    color: #FFFFFF;
    background: #1677E8;
    border-color: #1677E8;
}
QPushButton[role="primary"]:hover {
    background: #0D68D1;
}
QPushButton[role="danger"] {
    color: #A92D2D;
    background: #FFF5F5;
    border-color: #F0BBBB;
}
QPushButton[role="quiet"] {
    background: #F6F8FB;
}
QLineEdit, QSpinBox, QComboBox {
    min-height: 30px;
    padding: 0 8px;
    background: #FFFFFF;
    border: 1px solid #C8D2E1;
    border-radius: 6px;
    selection-background-color: #1677E8;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
    border-color: #1677E8;
}
QPlainTextEdit {
    background: #101A2B;
    color: #D9E5F5;
    border: 1px solid #263B5F;
    border-radius: 8px;
    padding: 8px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9pt;
}
QTabWidget::pane {
    border: 0;
    background: #F3F6FA;
}
QTabBar::tab {
    min-width: 110px;
    min-height: 36px;
    padding: 0 14px;
    background: #E6EBF2;
    color: #52627C;
    border: 0;
    border-bottom: 3px solid transparent;
}
QTabBar::tab:selected {
    color: #1677E8;
    background: #FFFFFF;
    border-bottom-color: #1677E8;
    font-weight: 700;
}
QStatusBar {
    background: #13213A;
    color: #D9E5F5;
}
QScrollArea {
    border: 0;
    background: transparent;
}
"""
