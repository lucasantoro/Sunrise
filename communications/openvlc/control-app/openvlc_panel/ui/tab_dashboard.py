"""Link Quality tab: live channel health, summary cards and charts.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

from typing import Callable, Optional
import time

from PySide6 import QtWidgets

from .deps import HAVE_PG, pg
from .text import _rate_to_kbps


class DashboardTabMixin:
    """Link Quality tab: live channel health, summary cards and charts."""

    def _build_dashboard(self) -> QtWidgets.QWidget:
        w = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(w)
        lay.setContentsMargins(18, 16, 18, 18)
        lay.addWidget(self._page_intro(
            "Link quality",
            "Monitor live receiver health, decoder quality and delivery throughput.",
        ))

        bar = QtWidgets.QHBoxLayout()
        bar.addWidget(QtWidgets.QLabel("Source"))
        self.cbo_monitor_source = QtWidgets.QComboBox()
        self.cbo_monitor_source.setToolTip(
            "Which node's service journal the monitor streams")
        self._refresh_monitor_sources()
        bar.addWidget(self.cbo_monitor_source)
        self.btn_monitor = QtWidgets.QPushButton("Start monitor")
        self._monitor_buttons.append(self.btn_monitor)
        self._set_button_role(self.btn_monitor, "primary")
        self.btn_monitor.clicked.connect(self._toggle_monitor)
        bar.addWidget(self.btn_monitor)

        btn_check = QtWidgets.QPushButton("Check system")
        btn_check.clicked.connect(self._run_preflight)
        bar.addWidget(btn_check)

        btn_diag = QtWidgets.QPushButton("Collect diagnostics")
        btn_diag.clicked.connect(self._collect_diagnostics)
        bar.addWidget(btn_diag)

        self.lbl_link = QtWidgets.QLabel("link: unknown")
        self.lbl_link.setStyleSheet("font-weight: bold;")
        bar.addWidget(self.lbl_link)
        bar.addStretch(1)
        lay.addLayout(bar)

        grid = QtWidgets.QGridLayout()
        self.readouts: dict[str, QtWidgets.QLabel] = {}
        fields = [
            ("rate", "IP rate (kbps)"),
            ("fps", "frames/s"),
            ("tx_kbps", "TRX TX kbps"),
            ("rx_kbps", "TRX RX kbps"),
            ("tx_fps", "TRX TX fps"),
            ("rx_fps", "TRX RX fps"),
            ("goodput", "iperf RX goodput"),
            ("ok", "ok/s"),
            ("seen", "seen/s"),
            ("burstps", "burst/s"),
            ("success", "ok/seen diag (%)"),
            ("seq_gap", "seq_gap interval"),
            ("ringdrop", "ringdrop interval"),
            ("hostdrop", "hostdrop interval"),
            ("hosterr", "hosterr interval"),
            ("txdrop", "TRX tx drop interval"),
            ("rxgap", "TRX rx gap interval"),
            ("invalid_ip", "invalid_ip"),
            ("tunerr", "tunerr"),
            ("serialerr", "serialerr interval"),
            ("backlog", "serial backlog"),
            ("edgeps", "raw COMP edge/s"),
            ("glitchps", "raw COMP glitch/s"),
            ("glitch_permille", "glitch permille"),
            ("longps", "long edge/s"),
            ("duty", "COMP duty (%)"),
            ("last_duty", "last duty (%)"),
            ("threshold", "COMP threshold DAC"),
            ("thr_mv", "COMP threshold mV"),
            ("halfcell", "half-cell ticks"),
            ("dec_us", "decode us"),
            ("decmax_us", "decode max us"),
            ("crc", "serial crc"),
            ("header", "serial header"),
            ("comp_crc", "COMP crc total"),
            ("comp_sync", "COMP sync total"),
            ("crc_delta", "COMP crc interval"),
            ("sync_delta", "COMP sync interval"),
            ("fc_delta", "frame CRC interval"),
            ("fi_delta", "frame invalid interval"),
            ("score", "quality score"),
            ("score_min", "score min"),
            ("jitter", "jitter (%)"),
            ("tq", "timing quality"),
            ("rs", "RS corrections"),
            ("sfderr", "SFD errors"),
            ("lock", "SFD lock"),
            ("syncs", "SFD syncs"),
            ("pre_rej", "SFD pre reject"),
            ("sfd_mode", "SFD mode"),
            ("parse_status", "parse status"),
            ("fc", "frame crc total"),
            ("fl", "frame len total"),
            ("fo", "frame overflow total"),
            ("fi", "frame invalid total"),
            ("payload", "payload bytes"),
            ("len_raw", "raw length"),
            ("pstat", "PHY pstat"),
        ]
        for i, (key, label) in enumerate(fields):
            grid.addWidget(QtWidgets.QLabel(label + ":"), i // 3, (i % 3) * 2)
            val = QtWidgets.QLabel("-")
            val.setStyleSheet("font-family: monospace; font-size: 14px;")
            self.readouts[key] = val
            grid.addWidget(val, i // 3, (i % 3) * 2 + 1)
        lay.addLayout(grid)

        body = QtWidgets.QHBoxLayout()
        chart_col = QtWidgets.QVBoxLayout()
        if HAVE_PG:
            self.plot_rate = pg.PlotWidget(title="IP throughput (kbps)")
            self.plot_loss = pg.PlotWidget(title="seq_gap / ringdrop per log interval")
            for p in (self.plot_rate, self.plot_loss):
                p.showGrid(x=True, y=True, alpha=0.3)
                p.addLegend()
            self.curve_rate = self.plot_rate.plot(pen="y", name="rate")
            self.curve_seq = self.plot_loss.plot(pen="r", name="seq_gap")
            self.curve_ring = self.plot_loss.plot(pen="c", name="ringdrop")
            self.plot_quality = pg.PlotWidget(title="Link quality score / jitter")
            self.plot_quality.showGrid(x=True, y=True, alpha=0.3)
            self.plot_quality.addLegend()
            self.curve_score = self.plot_quality.plot(pen="g", name="score")
            self.curve_jitter = self.plot_quality.plot(pen="m", name="jitter %")
            chart_col.addWidget(self.plot_rate)
            chart_col.addWidget(self.plot_loss)
            chart_col.addWidget(self.plot_quality)
        else:
            chart_col.addWidget(QtWidgets.QLabel(
                "pyqtgraph not installed - charts disabled (numbers still update)."))
        body.addLayout(chart_col, stretch=3)
        body.addWidget(self._build_link_summary(), stretch=2)
        lay.addLayout(body, stretch=1)

        self.txt_journal = QtWidgets.QPlainTextEdit()
        self.txt_journal.setReadOnly(True)
        self.txt_journal.setMaximumBlockCount(1500)
        self.txt_journal.setMaximumHeight(180)
        self.txt_journal.setPlaceholderText(
            "Live journal output appears here after starting the monitor."
        )
        lay.addLayout(self._log_header("Live service journal", self.txt_journal))
        lay.addWidget(self.txt_journal)
        return w


    def _build_link_summary(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget()
        panel.setMinimumWidth(330)
        outer = QtWidgets.QVBoxLayout(panel)
        outer.setContentsMargins(0, 0, 0, 0)

        live = QtWidgets.QGroupBox("Live metrics")
        live_grid = QtWidgets.QGridLayout(live)
        self.summary_cards: dict[str, QtWidgets.QLabel] = {}
        cards = [
            ("score", "score"),
            ("score_min", "score min"),
            ("jitter", "jitter"),
            ("throughput", "IP rate"),
            ("ok", "ok/s"),
            ("drops", "seq/ring drops"),
        ]
        for i, (key, title) in enumerate(cards):
            card = QtWidgets.QFrame()
            card.setFrameShape(QtWidgets.QFrame.StyledPanel)
            card.setStyleSheet(
                "QFrame { border:1px solid palette(mid); border-radius:4px; }"
                "QLabel { border:0; }"
            )
            card_lay = QtWidgets.QVBoxLayout(card)
            card_lay.setContentsMargins(10, 8, 10, 8)
            label = QtWidgets.QLabel(title)
            label.setStyleSheet("font-size:12px;")
            value = QtWidgets.QLabel("-")
            value.setStyleSheet("font-family:monospace; font-size:24px; font-weight:bold;")
            card_lay.addWidget(label)
            card_lay.addWidget(value)
            self.summary_cards[key] = value
            live_grid.addWidget(card, i // 2, i % 2)
        outer.addWidget(live)

        dec = QtWidgets.QGroupBox("Decoder status")
        dec_grid = QtWidgets.QGridLayout(dec)
        self.decoder_status: dict[str, QtWidgets.QLabel] = {}
        for row, key in enumerate(("PHY", "SFDSYNC", "COMP", "HOST", "VIDEO")):
            name = QtWidgets.QLabel(key)
            name.setStyleSheet("font-weight:bold;")
            value = QtWidgets.QLabel("-")
            value.setWordWrap(True)
            value.setStyleSheet("font-family:monospace;")
            self.decoder_status[key] = value
            dec_grid.addWidget(name, row, 0)
            dec_grid.addWidget(value, row, 1)
        dec_grid.setColumnStretch(1, 1)
        outer.addWidget(dec)
        outer.addStretch(1)
        return panel


    def _push(self, key, t, v):
        xs, ys = self._series[key]
        xs.append(t)
        ys.append(v)


    def _readout_text(self, key: str) -> str:
        label = self.readouts.get(key) if hasattr(self, "readouts") else None
        if not label:
            return "-"
        text = label.text().strip()
        return text if text else "-"


    def _set_summary_card(self, key: str, text: str, color: str = "#111827"):
        if not hasattr(self, "summary_cards") or key not in self.summary_cards:
            return
        self.summary_cards[key].setText(text)
        self.summary_cards[key].setStyleSheet(
            f"font-family:monospace; font-size:24px; font-weight:bold; color:{color};"
        )


    def _metric_color(self, value: str, good_min: float,
                      warn_min: Optional[float] = None,
                      invert: bool = False) -> str:
        try:
            v = float(value.replace("%", "").replace("k", ""))
        except ValueError:
            return "#667085"
        if invert:
            if v <= good_min:
                return "green"
            if warn_min is not None and v <= warn_min:
                return "#b87500"
            return "red"
        if v >= good_min:
            return "green"
        if warn_min is not None and v >= warn_min:
            return "#b87500"
        return "red"


    def _metric(self, metrics: dict, key: str, fmt: str = ".0f") -> str:
        if key not in metrics:
            return "-"
        return format(metrics[key], fmt)


    def _fresh_metric_text(
        self, kind: str, key: str, fmt: str = ".0f", max_age: float = 8.0
    ) -> str:
        if time.time() - self._latest_at.get(kind, 0.0) > max_age:
            return "-"
        metrics = self._latest.get(kind, {})
        if key not in metrics:
            return "-"
        return format(metrics[key], fmt)


    def _active_rate_text(self) -> str:
        rate = self._readout_text("rate")
        if rate != "-":
            return rate
        trx = self._latest.get("TRXBRIDGE", {})
        if trx and time.time() - self._latest_at.get("TRXBRIDGE", 0.0) <= 8.0:
            return f"{max(trx.get('tx_kbps', 0.0), trx.get('rx_kbps', 0.0)):.0f}"
        bridge = self._latest.get("BRIDGE", {})
        if bridge and time.time() - self._latest_at.get("BRIDGE", 0.0) <= 8.0:
            return self._metric(bridge, "rate")
        return "-"


    def _refresh_summary_panels(self):
        if not hasattr(self, "summary_cards") or not hasattr(self, "decoder_status"):
            return

        score = self._fresh_metric_text("COMP", "score")
        if score == "-":
            score = self._fresh_metric_text("PACKET", "score")
        score_min_value = self._quality_stat("score", min, seconds=60)
        score_min = f"{score_min_value:.0f}" if score_min_value is not None else "-"
        if score_min_value is not None:
            self._read("score_min", score_min_value, ".0f")
        else:
            self._clear_read("score_min")
        jitter = self._fresh_metric_text("COMP", "jitter", ".1f")
        if jitter == "-":
            jitter = self._fresh_metric_text("PACKET", "jitter", ".1f")
        rate = self._active_rate_text()
        goodput = self._readout_text("goodput")
        ok = self._readout_text("ok")
        if ok == "-":
            ok = self._fresh_metric_text("COMP", "okps")
        if ok == "-":
            ok = self._fresh_metric_text("RXRATE", "okps")
        seq_gap = self._readout_text("seq_gap")
        ringdrop = self._readout_text("ringdrop")

        self._set_summary_card("score", score, self._metric_color(score, 85.0, 70.0))
        self._set_summary_card("score_min", score_min, self._metric_color(score_min, 80.0, 65.0))
        jitter_text = f"{jitter}%" if jitter != "-" and not jitter.endswith("%") else jitter
        self._set_summary_card("jitter", jitter_text, self._metric_color(jitter, 8.0, 15.0, invert=True))
        throughput = rate if rate != "-" else goodput
        throughput_text = f"{throughput}k" if throughput != "-" and not throughput.endswith("k") else throughput
        throughput_color = "#667085"
        try:
            throughput_color = "green" if float(throughput.replace("k", "")) > 0.0 else "#667085"
        except ValueError:
            pass
        self._set_summary_card("throughput", throughput_text, throughput_color)
        self._set_summary_card("ok", ok, "green" if ok not in ("-", "0") else "#667085")
        drops = f"{seq_gap}/{ringdrop}" if seq_gap != "-" or ringdrop != "-" else "-"
        drop_color = "green" if seq_gap in ("-", "0") and ringdrop in ("-", "0") else "#b87500"
        self._set_summary_card("drops", drops, drop_color)

        phy = self._latest.get("PHY", {})
        self.decoder_status["PHY"].setText(
            f"status={self._metric(phy, 'status')} stage={self._metric(phy, 'stage')} "
            f"pstat={self._metric(phy, 'pstat')} payload={self._metric(phy, 'payload')}"
            if phy else "-"
        )

        sfd = self._latest.get("SFDSYNC", {})
        self.decoder_status["SFDSYNC"].setText(
            f"lock={self._metric(sfd, 'lock')} sfderr={self._metric(sfd, 'sfderr')} "
            f"syncs={self._metric(sfd, 'syncs')} pre_rej={self._metric(sfd, 'pre_rej')}"
            if sfd else "-"
        )

        comp = self._latest.get("COMP", {})
        comp_duty = self._latest.get("COMPDUTY", {})
        comp_auto = self._latest.get("COMPAUTO", {})
        if comp:
            if "okps" in comp or "threshold" in comp or "fc" in comp:
                self.decoder_status["COMP"].setText(
                    f"okp={self._metric(comp, 'okps')} seenp={self._metric(comp, 'seenps')} "
                    f"crcD={self._readout_text('crc_delta')} syncD={self._readout_text('sync_delta')} "
                    f"fcD={self._readout_text('fc_delta')} fiD={self._readout_text('fi_delta')} "
                    f"thr={self._metric(comp, 'threshold')}({self._metric(comp, 'thr_mv')}mV) "
                    f"hc={self._metric(comp, 'halfcell')} m={self._metric(comp, 'sfd_mode')} "
                    f"ps={self._metric(comp, 'parse_status')} du/dm={self._metric(comp, 'dec_us')}/"
                    f"{self._metric(comp, 'decmax_us')} score={self._metric(comp, 'score')} "
                    f"jit={self._metric(comp, 'jitter', '.1f')}"
                )
            else:
                self.decoder_status["COMP"].setText(
                    f"qvalid={self._metric(comp, 'qvalid')} score={self._metric(comp, 'score')} "
                    f"jitter={self._metric(comp, 'jitter', '.1f')} tq={self._metric(comp, 'tq')} "
                    f"rs={self._metric(comp, 'rs')}"
                )
        elif comp_duty:
            self.decoder_status["COMP"].setText(
                f"duty={self._metric(comp_duty, 'duty', '.1f')} "
                f"last={self._metric(comp_duty, 'last', '.1f')} "
                f"thr={self._metric(comp_duty, 'threshold')}({self._metric(comp_duty, 'thr_mv')}mV)"
            )
        elif comp_auto:
            self.decoder_status["COMP"].setText(
                f"auto thr={self._metric(comp_auto, 'threshold')}({self._metric(comp_auto, 'thr_mv')}mV) "
                f"bursts={self._metric(comp_auto, 'bursts')} ok={self._metric(comp_auto, 'ok')} "
                f"crc={self._metric(comp_auto, 'crc')} sync={self._metric(comp_auto, 'sync')}"
            )
        else:
            self.decoder_status["COMP"].setText("-")

        host = (
            f"seq_gap={seq_gap} ringdrop={ringdrop} "
            f"hostdrop={self._readout_text('hostdrop')} crc={self._readout_text('crc')}"
        )
        trx = self._latest.get("TRXBRIDGE", {})
        if trx:
            host += (
                f" | trx tx/rx={self._metric(trx, 'tx_kbps')}/"
                f"{self._metric(trx, 'rx_kbps')}kbps "
                f"dropD={self._readout_text('txdrop')} gapD={self._readout_text('rxgap')} "
                f"serD={self._readout_text('serialerr')} hdrD={self._readout_text('header')} "
                f"backlog={self._metric(trx, 'backlog')}"
            )
        self.decoder_status["HOST"].setText(host)

        mux = _rate_to_kbps(self.ed_muxrate.text().strip()) if hasattr(self, "ed_muxrate") else 0.0
        if self._last_goodput_kbps and mux:
            margin = 100.0 * (self._last_goodput_kbps - mux) / mux
            self.decoder_status["VIDEO"].setText(
                f"muxrate={mux:.0f}k margin={margin:.1f}% vs RX goodput"
            )
        elif self._last_goodput_kbps:
            self.decoder_status["VIDEO"].setText(
                f"RX goodput={self._last_goodput_kbps:.0f}k measured"
            )
        else:
            self.decoder_status["VIDEO"].setText("run iperf to estimate video margin")


    def _refresh_charts(self):
        if not HAVE_PG:
            return
        xs, ys = self._series["rate"]
        self.curve_rate.setData(list(xs), list(ys))
        xs, ys = self._series["seq_gap"]
        self.curve_seq.setData(list(xs), list(ys))
        xs, ys = self._series["ringdrop"]
        self.curve_ring.setData(list(xs), list(ys))
        xs, ys = self._series["score"]
        self.curve_score.setData(list(xs), list(ys))
        xs, ys = self._series["jitter"]
        self.curve_jitter.setData(list(xs), list(ys))


    def _refresh_link_health(self):
        if self._last_log_at <= 0:
            self._set_link("link: unknown", "#666")
            return
        age = time.time() - self._last_log_at
        if age > 8.0:
            self._set_link(f"link: stale ({age:.0f}s)", "#b87500")
            self._mark_stale_metrics()
            return

        problems = []
        bridge = self._latest.get("BRIDGE", {})
        trxbridge = self._latest.get("TRXBRIDGE", {})
        rxrate = self._latest.get("RXRATE", {})
        sfdsync = self._latest.get("SFDSYNC", {})
        comp = self._latest.get("COMP", {})
        if comp:
            packet = comp if (
                comp.get("qvalid", 0.0) > 0.0 or "score" in comp or "jitter" in comp
            ) else {}
        else:
            packet = self._latest.get("PACKET", {})

        for key in ("seq_gap", "invalid_ip", "tunerr", "crc", "discarded"):
            bridge_delta = bridge.get("gap", 0.0) if key == "seq_gap" else 0.0
            if max(bridge_delta, self._counter_deltas.get(("BRIDGE", key), 0.0)) > 0:
                problems.append(key)
        for key in ("ringdrop", "hostdrop", "hosterr"):
            if self._counter_deltas.get(("COMP", key), 0.0) > 0:
                problems.append(key)
        for key in ("crc", "sync", "fc", "fi"):
            if self._counter_deltas.get(("COMP", key), 0.0) > 0:
                problems.append(f"comp_{key}")
        for key, shown in (
            ("txdrop", "txdrop"),
            ("rxgap", "rxgap"),
            ("serialerr", "serialerr"),
            ("crc", "serial_crc"),
            ("header", "serial_header"),
        ):
            if self._counter_deltas.get(("TRXBRIDGE", key), 0.0) > 0:
                problems.append(shown)
        if trxbridge.get("backlog", 0.0) > 8.0:
            problems.append("serial backlog")
        seenps = rxrate.get("seenps", comp.get("seenps", 0.0))
        okps = rxrate.get("okps", comp.get("okps", 0.0))
        rx_kbps = trxbridge.get("rx_kbps", 0.0)
        rx_fps = trxbridge.get("rx_fps", 0.0)
        now = time.time()
        packet_seen = self._latest_at.get("PACKET", 0.0) >= now - 8.0
        bridge_active = bridge.get("rate", 0.0) > 0.0 or bridge.get("fps", 0.0) > 0.0
        valid_rx = okps > 0 or rx_kbps > 0.0 or rx_fps > 0.0 or packet_seen or bridge_active
        if seenps > 0 and okps <= 0:
            problems.append("no valid packets")
        if comp.get("parse_status", 0.0) != 0.0 and okps <= 0:
            problems.append("parser")
        if sfdsync.get("sfderr", 0.0) > 0:
            problems.append("sfd")
        if packet.get("jitter", 0.0) > 15.0:
            problems.append("jitter")

        if not valid_rx:
            if self._iperf_running:
                self._set_link("link: degraded (no valid RX packets)", "#b87500")
            else:
                self._set_link("link: idle (no valid RX packets)", "#666")
            return

        if problems:
            shown = ", ".join(dict.fromkeys(problems))
            self._set_link(f"link: degraded ({shown})", "#b87500")
        else:
            self._set_link("link: OK", "green")


    def _set_link(self, text: str, color: str):
        self.lbl_link.setText(text)
        self.lbl_link.setStyleSheet(f"font-weight:bold; color:{color};")
        if hasattr(self, "lbl_header_link"):
            compact = text.replace("link:", "").strip().upper()
            self.lbl_header_link.setText(f"LINK  {compact}")
            bg = {
                "green": "#0F6B48",
                "#b87500": "#7B5715",
                "#666": "#263B5F",
            }.get(color, "#7B3030")
            self.lbl_header_link.setStyleSheet(
                f"color:#FFFFFF; background:{bg}; border-radius:14px;"
                "padding:7px 12px; font-size:9pt; font-weight:700;"
            )


    def _counter_delta(self, kind: str, key: str, value: float):
        ident = (kind, key)
        prev = self._counter_values.get(ident)
        self._counter_values[ident] = value
        self._counter_deltas[ident] = 0.0 if prev is None else max(0.0, value - prev)


    def _record_quality_sample(self, kind: str, metrics: dict):
        sample = {"t": time.time(), "kind": kind}
        sample.update(metrics)
        self._quality_samples.append(sample)


    def _quality_stat(self, key: str, fn: Callable[[list], float],
                      seconds: Optional[float] = None) -> Optional[float]:
        now = time.time()
        values = [
            float(s[key])
            for s in self._quality_samples
            if key in s and (seconds is None or now - float(s["t"]) <= seconds)
        ]
        if not values:
            return None
        return float(fn(values))


    def _quality_summary(self, since: Optional[float] = None) -> str:
        samples = [
            s for s in self._quality_samples
            if since is None or float(s["t"]) >= since
        ]
        if not samples:
            return "link quality: unavailable (start the monitor before running iperf)"

        def values(key: str) -> list[float]:
            return [float(s[key]) for s in samples if key in s]

        scores = values("score")
        jitters = values("jitter")
        rs_values = values("rs")
        tq_values = values("tq")
        okps_values = values("okps")
        seenps_values = values("seenps")
        thr_values = values("threshold")
        halfcell_values = values("halfcell")
        dec_values = values("dec_us")
        sfderr_values = values("sfderr")
        locks = values("lock")
        pstats = values("pstat")
        parse_status = values("parse_status")

        problems = []
        if jitters and max(jitters) > 15.0:
            problems.append("high jitter")
        if sfderr_values and max(sfderr_values) > 0:
            problems.append("SFD errors")
        if seenps_values and max(seenps_values) > 0 and okps_values and max(okps_values) <= 0:
            problems.append("no valid packets")
        if parse_status and max(parse_status) != 0.0 and okps_values and max(okps_values) <= 0:
            problems.append("parser status nonzero")
        verdict = "PASS" if not problems else "WARN: " + ", ".join(problems)

        lines = [f"link quality: {verdict}", f"samples={len(samples)}"]
        if scores:
            lines.append(
                f"score avg/min/max={sum(scores) / len(scores):.0f}/"
                f"{min(scores):.0f}/{max(scores):.0f}"
            )
        if jitters:
            lines.append(
                f"jitter avg/max={sum(jitters) / len(jitters):.1f}%/"
                f"{max(jitters):.1f}%"
            )
        if tq_values:
            lines.append(f"timing_quality avg={sum(tq_values) / len(tq_values):.0f}")
        if okps_values or seenps_values:
            ok_avg = sum(okps_values) / len(okps_values) if okps_values else 0.0
            seen_avg = sum(seenps_values) / len(seenps_values) if seenps_values else 0.0
            lines.append(f"okps/seenps avg={ok_avg:.1f}/{seen_avg:.1f}")
        if thr_values:
            lines.append(
                f"threshold avg/min/max={sum(thr_values) / len(thr_values):.0f}/"
                f"{min(thr_values):.0f}/{max(thr_values):.0f}"
            )
        if halfcell_values:
            lines.append(
                f"halfcell avg/min/max={sum(halfcell_values) / len(halfcell_values):.0f}/"
                f"{min(halfcell_values):.0f}/{max(halfcell_values):.0f}"
            )
        if dec_values:
            lines.append(f"decode_us avg/max={sum(dec_values) / len(dec_values):.0f}/{max(dec_values):.0f}")
        if rs_values:
            lines.append(
                f"rs avg/max={sum(rs_values) / len(rs_values):.2f}/"
                f"{max(rs_values):.0f}"
            )
        if locks:
            lines.append(f"sfd_lock avg/min={sum(locks) / len(locks):.0f}/{min(locks):.0f}")
        if sfderr_values:
            lines.append(f"sfderr max={max(sfderr_values):.0f}")
        if pstats:
            rejects = sum(1 for v in pstats if v != 0.0)
            lines.append(f"phy_reject_samples={rejects}/{len(pstats)}")
        return "\n".join(lines)


    def _read(self, key: str, value: Optional[float], fmt: str = ".0f"):
        if value is None or key not in self.readouts:
            return
        self.readouts[key].setText(format(value, fmt))


    def _clear_read(self, key: str):
        if key in self.readouts:
            self.readouts[key].setText("-")


    def _mark_stale_metrics(self):
        if self._stale_marked:
            return
        self._stale_marked = True
        t = time.time() - self._t0
        for key in ("rate", "seq_gap", "ringdrop", "ok"):
            self._push(key, t, 0.0)
        for key in (
            "rate", "fps", "goodput", "ok", "seen", "success",
            "tx_kbps", "rx_kbps", "tx_fps", "rx_fps",
            "seq_gap", "ringdrop", "hostdrop", "hosterr", "txdrop", "rxgap",
            "invalid_ip", "tunerr", "serialerr", "backlog",
            "edgeps", "glitchps", "glitch_permille", "longps",
            "duty", "last_duty", "threshold", "thr_mv", "halfcell", "dec_us", "decmax_us",
            "crc", "header", "comp_crc", "comp_sync", "crc_delta",
            "sync_delta", "fc_delta", "fi_delta",
            "score", "score_min", "jitter", "tq", "rs",
            "sfderr", "lock", "syncs", "pre_rej", "sfd_mode", "parse_status",
            "fc", "fl", "fo", "fi",
        ):
            self._clear_read(key)
        self._refresh_summary_panels()
        self._refresh_charts()
