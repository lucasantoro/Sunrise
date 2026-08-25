"""Video tab layout, camera routing and presets.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

from PySide6 import QtCore, QtWidgets

from ..config import Device
from .deps import HAVE_VLC
from .text import _rate_to_kbps
from .theme import VIDEO_PRESETS, VIDEO_CAMERA_ROUTES


class VideoTabMixin:
    """Video tab layout, camera routing and presets."""

    def _build_video(self) -> QtWidgets.QWidget:
        w = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(w)
        lay.setContentsMargins(18, 16, 18, 18)
        lay.setSpacing(12)

        top = QtWidgets.QHBoxLayout()
        top.addWidget(self._page_intro(
            "Live video",
            "One command starts the RX relay/player and the selected TX camera profile.",
        ), stretch=1)
        self.lbl_video_state = QtWidgets.QLabel("SESSION  STOPPED")
        self.lbl_video_state.setAlignment(QtCore.Qt.AlignCenter)
        self.lbl_video_state.setStyleSheet(
            "color:#66758F; background:#E7ECF3; border-radius:14px;"
            "padding:7px 13px; font-weight:700;"
        )
        top.addWidget(self.lbl_video_state)
        self.btn_video_session = QtWidgets.QPushButton("Start video session")
        self.btn_video_session.setMinimumWidth(180)
        self._set_button_role(self.btn_video_session, "primary")
        self.btn_video_session.clicked.connect(self._toggle_video_session)
        top.addWidget(self.btn_video_session)
        lay.addLayout(top)

        content = QtWidgets.QHBoxLayout()
        content.setSpacing(12)

        viewer_card = QtWidgets.QFrame()
        viewer_card.setObjectName("card")
        viewer_lay = QtWidgets.QVBoxLayout(viewer_card)
        viewer_lay.setContentsMargins(12, 12, 12, 12)
        viewer_title = QtWidgets.QLabel("Receiver preview")
        viewer_title.setStyleSheet("font-size:12pt; font-weight:700;")
        viewer_lay.addWidget(viewer_title)

        self.video_frame = QtWidgets.QFrame()
        self.video_frame.setMinimumSize(580, 360)
        self.video_frame.setAttribute(QtCore.Qt.WA_NativeWindow, True)
        self.video_frame.setStyleSheet("background:#080D16; border-radius:8px;")
        frame_lay = QtWidgets.QVBoxLayout(self.video_frame)
        self.lbl_video_placeholder = QtWidgets.QLabel(
            "Video preview\nStart the session when the optical link is ready."
        )
        self.lbl_video_placeholder.setAlignment(QtCore.Qt.AlignCenter)
        self.lbl_video_placeholder.setStyleSheet(
            "color:#7F91AD; font-size:12pt; background:transparent;"
        )
        frame_lay.addWidget(self.lbl_video_placeholder)
        viewer_lay.addWidget(self.video_frame, stretch=1)

        self.lbl_preview_status = QtWidgets.QLabel("Preview: stopped")
        self.lbl_preview_status.setWordWrap(True)
        self.lbl_preview_status.setStyleSheet(
            "color:#52637D; background:#EDF1F6; border-radius:6px; padding:7px 9px;"
        )
        viewer_lay.addWidget(self.lbl_preview_status)

        rx_options = QtWidgets.QHBoxLayout()
        rx_options.addWidget(QtWidgets.QLabel("Transport"))
        self.rb_tcp = QtWidgets.QRadioButton("TCP (clean)")
        self.rb_udp = QtWidgets.QRadioButton("UDP (low latency)")
        self.rb_tcp.setChecked(True)
        rx_options.addWidget(self.rb_tcp)
        rx_options.addWidget(self.rb_udp)
        rx_options.addSpacing(12)
        rx_options.addWidget(QtWidgets.QLabel("Cache"))
        self.sp_cache = QtWidgets.QSpinBox()
        self.sp_cache.setRange(0, 3000)
        self.sp_cache.setValue(800)
        self.sp_cache.setSuffix(" ms")
        rx_options.addWidget(self.sp_cache)
        self.cb_embed = QtWidgets.QCheckBox("embed in window")
        self.cb_embed.setChecked(HAVE_VLC)
        self.cb_embed.setEnabled(HAVE_VLC)
        self.cb_embed.setToolTip("Uncheck to open a separate VLC window.")
        rx_options.addWidget(self.cb_embed)
        rx_options.addStretch(1)
        viewer_lay.addLayout(rx_options)
        if not HAVE_VLC:
            unavailable = QtWidgets.QLabel(
                "Embedded preview unavailable: VLC will open in a separate window."
            )
            unavailable.setObjectName("sectionSubtitle")
            viewer_lay.addWidget(unavailable)
        content.addWidget(viewer_card, stretch=3)

        side = QtWidgets.QVBoxLayout()
        profile_card = QtWidgets.QGroupBox("Transmitter profile")
        profile_lay = QtWidgets.QVBoxLayout(profile_card)

        self.cb_video_camera_node = QtWidgets.QComboBox()
        for label, value in VIDEO_CAMERA_ROUTES:
            self.cb_video_camera_node.addItem(label, value)
        route_index = self.cb_video_camera_node.findData(
            self.cfg.video_camera_node
        )
        self.cb_video_camera_node.setCurrentIndex(
            route_index if route_index >= 0 else 0
        )
        self.cb_video_camera_node.currentIndexChanged.connect(
            self._video_camera_route_changed
        )
        profile_lay.addWidget(QtWidgets.QLabel("Camera location"))
        profile_lay.addWidget(self.cb_video_camera_node)
        self.lbl_video_route = QtWidgets.QLabel()
        self.lbl_video_route.setWordWrap(True)
        self.lbl_video_route.setStyleSheet(
            "color:#52637D; background:#EDF1F6; border-radius:6px; padding:7px 9px;"
        )
        profile_lay.addWidget(self.lbl_video_route)

        self.cb_video_preset = QtWidgets.QComboBox()
        for name in VIDEO_PRESETS:
            self.cb_video_preset.addItem(name)
        self.cb_video_preset.currentTextChanged.connect(self._apply_video_preset)
        profile_lay.addWidget(QtWidgets.QLabel("Preset"))
        profile_lay.addWidget(self.cb_video_preset)
        self.lbl_capacity = QtWidgets.QLabel("capacity: run iperf first")
        self.lbl_capacity.setWordWrap(True)
        self.lbl_capacity.setStyleSheet(
            "font-weight:700; background:#F2F5F9; border-radius:7px; padding:9px;"
        )
        profile_lay.addWidget(self.lbl_capacity)
        side.addWidget(profile_card)

        self.ed_capture_size = QtWidgets.QLineEdit()
        self.ed_outsize = QtWidgets.QLineEdit()
        self.ed_fps = QtWidgets.QLineEdit()
        self.ed_outfps = QtWidgets.QLineEdit()
        self.ed_bitrate = QtWidgets.QLineEdit()
        self.ed_muxrate = QtWidgets.QLineEdit()
        self.cb_mode = QtWidgets.QComboBox()
        self.cb_mode.addItems(["cbr", "capped-crf"])
        self.ed_crf = QtWidgets.QLineEdit()
        self.ed_preset = QtWidgets.QLineEdit()
        self.ed_bufsize = QtWidgets.QLineEdit()
        self.ed_muxdelay = QtWidgets.QLineEdit()
        self.ed_input_format = QtWidgets.QLineEdit("mjpeg")

        rows = [
            ("capture size", self.ed_capture_size),
            ("output size", self.ed_outsize),
            ("capture fps", self.ed_fps),
            ("output fps", self.ed_outfps),
            ("bitrate", self.ed_bitrate),
            ("muxrate", self.ed_muxrate),
            ("rate mode", self.cb_mode),
            ("crf", self.ed_crf),
            ("x264 preset", self.ed_preset),
            ("VBV buffer", self.ed_bufsize),
            ("mux delay", self.ed_muxdelay),
            ("input format", self.ed_input_format),
        ]
        self.gb_video_advanced = QtWidgets.QGroupBox("Advanced encoding settings")
        self.gb_video_advanced.setCheckable(True)
        self.gb_video_advanced.setChecked(False)
        advanced = QtWidgets.QFormLayout(self.gb_video_advanced)
        for lbl, wdg in rows:
            advanced.addRow(lbl, wdg)
        self.gb_video_advanced.toggled.connect(
            lambda checked: self._set_advanced_video_visible(advanced, checked)
        )
        side.addWidget(self.gb_video_advanced)

        capture = QtWidgets.QGroupBox("Capture utility")
        capture_lay = QtWidgets.QVBoxLayout(capture)
        capture_lay.addWidget(QtWidgets.QLabel(
            "Record 15 seconds at the receiver, download the stream and open it in VLC."
        ))
        b_rec = QtWidgets.QPushButton("Record 15 s and play")
        b_rec.clicked.connect(self._record_fetch)
        capture_lay.addWidget(b_rec)
        side.addWidget(capture)
        side.addStretch(1)
        content.addLayout(side, stretch=2)
        lay.addLayout(content, stretch=1)

        self._apply_video_preset(self.cb_video_preset.currentText())
        self._video_camera_route_changed()
        self._set_advanced_video_visible(advanced, False)
        return w


    def _video_route(self) -> tuple[Device, Device, str, str]:
        """Return camera device, receiver device, destination TUN IP and label."""
        route = self.cfg.video_camera_node
        if route == "a":
            return (
                self.cfg.trx_a,
                self.cfg.trx_b,
                self.cfg.trx_b_tun_ip,
                "Node A camera → Node B receiver",
            )
        if route == "legacy":
            return (
                self.cfg.tx_pi,
                self.cfg.rx_pi,
                self.cfg.trx_b_tun_ip,
                "Legacy TX Pi camera → RX Pi",
            )
        return (
            self.cfg.trx_b,
            self.cfg.trx_a,
            self.cfg.trx_a_tun_ip,
            "Node B camera → Node A receiver",
        )


    def _current_video_route(self) -> tuple[Device, Device, str, str]:
        return self._video_route_devices or self._video_route()


    def _video_camera_route_changed(self, _index: int = -1) -> None:
        if hasattr(self, "cb_video_camera_node"):
            value = self.cb_video_camera_node.currentData()
            self.cfg.video_camera_node = (
                value if value in ("a", "b", "legacy") else "b"
            )
        tx_dev, rx_dev, destination, label = self._video_route()
        if hasattr(self, "lbl_video_route"):
            self.lbl_video_route.setText(
                f"{label}\n"
                f"camera SSH: {tx_dev.host or 'not configured'}  •  "
                f"receiver SSH: {rx_dev.host or 'not configured'}  •  "
                f"optical destination: {destination}"
            )


    @staticmethod
    def _set_advanced_video_visible(
        layout: QtWidgets.QFormLayout, visible: bool
    ) -> None:
        for index in range(layout.count()):
            item = layout.itemAt(index)
            if item.widget():
                item.widget().setVisible(visible)


    def _apply_video_preset(self, name: str):
        preset = VIDEO_PRESETS.get(name)
        if not preset:
            return
        self.ed_capture_size.setText(preset["size"])
        self.ed_outsize.setText(preset["outsize"])
        self.ed_fps.setText(preset["fps"])
        self.ed_outfps.setText(preset["outfps"])
        self.ed_bitrate.setText(preset["bitrate"])
        self.ed_muxrate.setText(preset["muxrate"])
        self.cb_mode.setCurrentText(preset["mode"])
        self.ed_crf.setText(preset["crf"])
        self.ed_preset.setText(preset["preset"])
        self.ed_bufsize.setText(preset["bufsize"])
        self.ed_muxdelay.setText(preset["muxdelay"])
        self._apply_video_capacity_hint()


    def _apply_video_capacity_hint(self):
        if not hasattr(self, "lbl_capacity"):
            return
        mux = _rate_to_kbps(self.ed_muxrate.text().strip()) if hasattr(self, "ed_muxrate") else 0.0
        if not self._last_goodput_kbps:
            self.lbl_capacity.setText("capacity: run iperf first")
            self.lbl_capacity.setStyleSheet("font-weight:bold; color:#666;")
            return
        if not mux:
            self.lbl_capacity.setText(f"capacity: {self._last_goodput_kbps:.0f} kbps measured")
            self.lbl_capacity.setStyleSheet("font-weight:bold; color:#666;")
            return
        margin = 100.0 * (self._last_goodput_kbps - mux) / mux
        self.lbl_capacity.setText(
            f"capacity: {self._last_goodput_kbps:.0f} kbps, mux margin {margin:.1f}%"
        )
        color = "green" if margin >= 15.0 else ("#b87500" if margin >= 0 else "red")
        self.lbl_capacity.setStyleSheet(f"font-weight:bold; color:{color};")
        self._refresh_summary_panels()
