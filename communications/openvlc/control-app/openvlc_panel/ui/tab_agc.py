"""AGC tuning tab: threshold sweep capture and export.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

import re

from PySide6 import QtCore, QtWidgets

from .deps import HAVE_PG, pg
from .theme import AGC_FEATURE_READY


class AgcTabMixin:
    """AGC tuning tab: threshold sweep capture and export."""

    def _build_agc(self) -> QtWidgets.QWidget:
        w = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(w)
        lay.setContentsMargins(18, 16, 18, 18)
        lay.addWidget(self._page_intro(
            "AGC signal / gain tuning",
            "Capture and plot the raw AGC/ADC waveform to tune the front-end gain: "
            "aim for a full swing that touches neither 0 nor full-scale (no clipping).",
        ))
        if not AGC_FEATURE_READY:
            banner = QtWidgets.QLabel(
                "Coming soon - the STM32 firmware does not sample the VAGC "
                "signal yet. Once the ADC capture is implemented (ADCRAW/"
                "ADCPHASE log lines), this tab will light up automatically: "
                "set AGC_FEATURE_READY = True in app.py.")
            banner.setWordWrap(True)
            banner.setStyleSheet(
                "background:#FFF7E0; border:1px solid #E5C858; border-radius:8px;"
                "padding:10px 12px; color:#6B5410; font-weight:600;")
            lay.addWidget(banner)

        bar = QtWidgets.QHBoxLayout()
        self.btn_agc_capture = QtWidgets.QPushButton("Arm capture")
        self._set_button_role(self.btn_agc_capture, "primary")
        self.btn_agc_capture.setCheckable(True)
        self.btn_agc_capture.toggled.connect(self._agc_toggle)
        bar.addWidget(self.btn_agc_capture)
        b_agc_save = QtWidgets.QPushButton("Save CSV")
        self._set_button_role(b_agc_save, "quiet")
        b_agc_save.clicked.connect(self._agc_save_csv)
        bar.addWidget(b_agc_save)
        b_agc_clear = QtWidgets.QPushButton("Clear")
        self._set_button_role(b_agc_clear, "quiet")
        b_agc_clear.clicked.connect(self._agc_clear)
        bar.addWidget(b_agc_clear)
        note = QtWidgets.QLabel(
            "Enable OPENVLC_ADC_RAW_CAPTURE on the RX firmware so it emits "
            "'ADCRAW n=.. d=..' lines; keep the monitor running. ADCPHASE lines "
            "update the envelope even without raw capture.")
        note.setObjectName("sectionSubtitle")
        note.setWordWrap(True)
        bar.addWidget(note, stretch=1)
        lay.addLayout(bar)
        if not AGC_FEATURE_READY:
            for control in (self.btn_agc_capture, b_agc_save, b_agc_clear):
                control.setEnabled(False)

        stats = QtWidgets.QGroupBox("Envelope (ADC counts, 0-255)")
        sg = QtWidgets.QHBoxLayout(stats)
        self._agc_stat: dict = {}
        for key, label in [("min", "min"), ("max", "max"), ("mean", "mean"),
                           ("span", "span"), ("clip", "clip %")]:
            col = QtWidgets.QVBoxLayout()
            cap = QtWidgets.QLabel(label)
            cap.setStyleSheet("font-size:12px;")
            val = QtWidgets.QLabel("-")
            val.setStyleSheet(
                "font-family:monospace; font-size:22px; font-weight:bold;")
            col.addWidget(cap)
            col.addWidget(val)
            sg.addLayout(col)
            self._agc_stat[key] = val
        lay.addWidget(stats)

        if HAVE_PG:
            self.plot_agc = pg.PlotWidget(title="AGC waveform")
            self.plot_agc.showGrid(x=True, y=True, alpha=0.3)
            self.plot_agc.setYRange(0, 255)
            self.plot_agc.addLine(
                y=255, pen=pg.mkPen("r", style=QtCore.Qt.DashLine))
            self.plot_agc.addLine(
                y=0, pen=pg.mkPen("r", style=QtCore.Qt.DashLine))
            self.curve_agc = self.plot_agc.plot(pen="c")
            lay.addWidget(self.plot_agc, stretch=1)
        else:
            lay.addWidget(QtWidgets.QLabel(
                "pyqtgraph not installed - waveform plot disabled."))
        return w


    def _agc_toggle(self, on: bool) -> None:
        self._agc_armed = on
        self.btn_agc_capture.setText(
            "Capturing... (click to stop)" if on else "Arm capture")
        self._status("AGC capture armed" if on else "AGC capture stopped")


    def _agc_ingest(self, line: str) -> None:
        """Parse ADCRAW (waveform) / ADCPHASE (envelope) from a live log line."""
        if "ADCRAW" in line and "d=" in line:
            try:
                data = line.split("d=", 1)[1].strip().split()[0]
                samples = [int(x) for x in data.split(",") if x != ""]
            except (ValueError, IndexError):
                return
            if samples:
                self._agc_render(samples)
        elif "ADCPHASE" in line:
            m = re.search(r"p0=(\d+)/(\d+)/(\d+)", line)
            if m:
                lo, hi, mean = (int(m.group(i)) for i in (1, 2, 3))
                self._agc_stat["min"].setText(str(lo))
                self._agc_stat["max"].setText(str(hi))
                self._agc_stat["mean"].setText(str(mean))
                self._agc_stat["span"].setText(str(hi - lo))


    def _agc_render(self, samples: list) -> None:
        self._agc_last_samples = list(samples)
        lo, hi = min(samples), max(samples)
        mean = sum(samples) / len(samples)
        clip = sum(1 for s in samples if s <= 0 or s >= 255) / len(samples) * 100.0
        self._agc_stat["min"].setText(str(lo))
        self._agc_stat["max"].setText(str(hi))
        self._agc_stat["mean"].setText(f"{mean:.0f}")
        self._agc_stat["span"].setText(str(hi - lo))
        self._agc_stat["clip"].setText(f"{clip:.1f}")
        if HAVE_PG:
            self.curve_agc.setData(list(range(len(samples))), samples)


    def _agc_clear(self) -> None:
        self._agc_last_samples = []
        if HAVE_PG:
            self.curve_agc.setData([], [])
        for v in self._agc_stat.values():
            v.setText("-")


    def _agc_save_csv(self) -> None:
        samples = getattr(self, "_agc_last_samples", None)
        if not samples:
            self._status("AGC: no samples captured yet", err=True)
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Save AGC samples", "agc_samples.csv", "CSV (*.csv)")
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write("index,adc_count\n")
                for i, s in enumerate(samples):
                    f.write(f"{i},{s}\n")
            self._status(f"AGC samples saved to {path}")
        except OSError as e:
            self._status(f"AGC save failed: {e}", err=True)
