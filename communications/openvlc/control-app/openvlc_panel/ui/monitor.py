"""Log streaming: start/stop a source and dispatch each parsed line.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

from typing import Optional
import time

from .. import ssh
from ..stats import parse_line


class MonitorMixin:
    """Log streaming: start/stop a source and dispatch each parsed line."""

    _MONITOR_UNITS = {"rx": "openvlc-rx", "trx": "openvlc-transceiver"}


    def _toggle_monitor(self):
        if self._streamer is None:
            self._start_monitor_if_needed()
        else:
            self._stop_monitor()


    def _stop_monitor(self) -> None:
        if self._streamer is not None:
            self._streamer.stop()
            self._streamer = None
        self._streamer_source_label = None
        self._sync_monitor_buttons(False)
        self._status("monitor stopped")


    def _monitor_sources(self):
        """(label, device, journal unit) for every node that runs a link
        service worth monitoring (RX bridge or transceiver bridge)."""
        return [(label, dev, self._MONITOR_UNITS[kind])
                for label, dev, kind in self._nodes()
                if kind in self._MONITOR_UNITS]


    def _refresh_monitor_sources(self) -> None:
        current = self.cbo_monitor_source.currentText()
        self.cbo_monitor_source.clear()
        for label, _dev, unit in self._monitor_sources():
            self.cbo_monitor_source.addItem(f"{label} ({unit})", label)
        if self.cbo_monitor_source.count() == 0:
            self.cbo_monitor_source.addItem("RX Pi (openvlc-rx)", "RX Pi")
        idx = self.cbo_monitor_source.findText(current)
        if idx >= 0:
            self.cbo_monitor_source.setCurrentIndex(idx)


    def _start_monitor_if_needed(self, source_label: Optional[str] = None):
        if source_label and hasattr(self, "cbo_monitor_source"):
            idx = self.cbo_monitor_source.findData(source_label)
            if idx >= 0:
                self.cbo_monitor_source.setCurrentIndex(idx)
        selected = self.cbo_monitor_source.currentData()
        if self._streamer is not None:
            if source_label and selected != self._streamer_source_label:
                self._stop_monitor()
            else:
                return
        device, unit = self.cfg.rx_pi, "openvlc-rx"
        for label, dev, node_unit in self._monitor_sources():
            if label == selected:
                device, unit = dev, node_unit
                break
        cmd = f"journalctl --no-pager -u {unit} -f -o cat -n 80 2>&1"
        self._streamer = ssh.LogStreamer(
            device,
            cmd,
            on_line=self._log_bridge.line.emit,
            on_error=self._log_bridge.error.emit,
        )
        self._streamer.start()
        self._streamer_source_label = selected
        if hasattr(self, "txt_journal"):
            self.txt_journal.appendPlainText(
                f"[monitor] {selected} / {unit} on {device.user}@{device.host}"
            )
        self._sync_monitor_buttons(True)
        self._status(f"monitor started ({selected}, {unit})")


    def _monitor_trx(self, attr: str) -> None:
        dev = getattr(self.cfg, attr, None)
        if dev is None or not dev.host:
            self._status(f"{attr}: host not configured (Settings)", err=True)
            return
        label = "Transceiver A" if attr == "trx_a" else "Transceiver B"
        if self._streamer is not None and self._streamer_source_label == label:
            self._stop_monitor()
            return
        self._start_monitor_if_needed(label)


    def _sync_monitor_buttons(self, running: bool) -> None:
        text = "Stop monitor" if running else "Start monitor"
        for button in self._monitor_buttons:
            button.setText(text)


    def _on_log_error(self, error: str) -> None:
        if hasattr(self, "txt_journal"):
            self.txt_journal.appendPlainText(f"[monitor error] {error}")
        self._status(f"log stream: {error}", err=True)


    def _on_log_line(self, line: str):
        if hasattr(self, "txt_journal"):
            self.txt_journal.appendPlainText(line)
        if hasattr(self, "txt_control"):
            self.txt_control.appendPlainText(f"[journal] {line}")
        if getattr(self, "_agc_armed", False):
            self._agc_ingest(line)
        kind, m = parse_line(line)
        if not kind:
            return
        self._latest[kind] = m
        self._last_log_at = time.time()
        self._latest_at[kind] = self._last_log_at
        self._stale_marked = False
        t = self._last_log_at - self._t0

        if kind == "BRIDGE":
            for key in ("seq_gap", "invalid_ip", "tunerr", "crc", "discarded"):
                if key in m:
                    self._counter_delta(kind, key, m[key])
            seq_gap = m.get("gap", self._counter_deltas.get(("BRIDGE", "seq_gap")))
            self._read("rate", m.get("rate"), ".0f")
            self._read("fps", m.get("fps"), ".0f")
            self._read("seq_gap", seq_gap, ".0f")
            self._read("invalid_ip", m.get("invalid_ip"), ".0f")
            self._read("tunerr", m.get("tunerr"), ".0f")
            self._read("crc", m.get("crc"), ".0f")
            if "rate" in m:
                self._push("rate", t, m["rate"])
            if seq_gap is not None:
                self._push("seq_gap", t, seq_gap)

        elif kind == "TRXBRIDGE":
            for key in (
                "txdrop", "txfiltered", "rxgap", "rxreset", "rxinvalid",
                "serialerr", "tunerr", "crc", "header",
            ):
                if key in m:
                    self._counter_delta(kind, key, m[key])
            active_kbps = max(m.get("tx_kbps", 0.0), m.get("rx_kbps", 0.0))
            active_fps = max(m.get("tx_fps", 0.0), m.get("rx_fps", 0.0))
            self._read("tx_kbps", m.get("tx_kbps"), ".0f")
            self._read("rx_kbps", m.get("rx_kbps"), ".0f")
            self._read("tx_fps", m.get("tx_fps"), ".0f")
            self._read("rx_fps", m.get("rx_fps"), ".0f")
            self._read("rate", active_kbps, ".0f")
            self._read("fps", active_fps, ".0f")
            self._read("backlog", m.get("backlog"), ".0f")
            for raw_key, readout_key in (
                ("txdrop", "txdrop"),
                ("rxgap", "rxgap"),
                ("serialerr", "serialerr"),
                ("tunerr", "tunerr"),
                ("crc", "crc"),
                ("header", "header"),
            ):
                self._read(readout_key, self._counter_deltas.get((kind, raw_key)), ".0f")
            rxgap = self._counter_deltas.get((kind, "rxgap"))
            if rxgap is not None:
                self._read("seq_gap", rxgap, ".0f")
                self._push("seq_gap", t, rxgap)
            if "tx_kbps" in m or "rx_kbps" in m:
                self._push("rate", t, active_kbps)

        elif kind == "COMP":
            for key in (
                "ringdrop", "hostdrop", "hosterr", "crc", "sync",
                "ft", "fn", "fp", "fx", "fc", "fl", "fo", "fi",
            ):
                if key in m:
                    self._counter_delta(kind, key, m[key])
            for key in ("ringdrop", "hostdrop", "hosterr"):
                self._read(key, self._counter_deltas.get((kind, key)), ".0f")
            for key in ("edgeps", "glitchps", "glitch_permille", "longps", "burstps"):
                self._read(key, m.get(key), ".0f")
            for key in (
                "threshold", "thr_mv", "halfcell", "dec_us", "decmax_us",
                "syncs", "pre_rej", "lock", "sfd_mode", "parse_status",
                "fc", "fl", "fo", "fi",
            ):
                self._read(key, m.get(key), ".0f")
            self._read("comp_crc", m.get("crc"), ".0f")
            self._read("comp_sync", m.get("sync"), ".0f")
            self._read("crc_delta", self._counter_deltas.get((kind, "crc")), ".0f")
            self._read("sync_delta", self._counter_deltas.get((kind, "sync")), ".0f")
            self._read("fc_delta", self._counter_deltas.get((kind, "fc")), ".0f")
            self._read("fi_delta", self._counter_deltas.get((kind, "fi")), ".0f")
            if "okps" in m:
                self._read("ok", m.get("okps"), ".0f")
                self._push("ok", t, m["okps"])
            if "seenps" in m:
                self._read("seen", m.get("seenps"), ".0f")
                seen = m.get("seenps", 0.0)
                ok = m.get("okps", 0.0)
                if seen > 0:
                    self._read("success", 100.0 * ok / seen, ".1f")
            ringdrop = self._counter_deltas.get((kind, "ringdrop"))
            if ringdrop is not None:
                self._push("ringdrop", t, ringdrop)
            if m.get("qvalid", 0.0) > 0.0 or "score" in m or "jitter" in m:
                self._read("score", m.get("score"), ".0f")
                self._read("jitter", m.get("jitter"), ".1f")
                self._read("tq", m.get("tq"), ".0f")
                self._read("rs", m.get("rs"), ".0f")
                if "score" in m:
                    self._push("score", t, m["score"])
                if "jitter" in m:
                    self._push("jitter", t, m["jitter"])
                self._record_quality_sample(kind, m)
                score_min = self._quality_stat("score", min, seconds=60)
                if score_min is not None:
                    self._read("score_min", score_min, ".0f")
            elif "qvalid" in m:
                for key in ("score", "jitter", "tq", "rs"):
                    self._clear_read(key)

        elif kind == "COMPDUTY":
            self._read("duty", m.get("duty"), ".1f")
            self._read("last_duty", m.get("last"), ".1f")
            self._read("threshold", m.get("threshold"), ".0f")
            self._read("thr_mv", m.get("thr_mv"), ".0f")

        elif kind == "COMPAUTO":
            self._read("threshold", m.get("threshold"), ".0f")
            self._read("thr_mv", m.get("thr_mv"), ".0f")

        elif kind == "RXRATE":
            self._read("ok", m.get("okps"), ".0f")
            self._read("seen", m.get("seenps"), ".0f")
            self._read("burstps", m.get("burstps"), ".0f")
            self._read("dec_us", m.get("dec_us"), ".0f")
            self._read("decmax_us", m.get("decmax_us"), ".0f")
            seen = m.get("seenps", 0.0)
            ok = m.get("okps", 0.0)
            if seen > 0:
                self._read("success", 100.0 * ok / seen, ".1f")
            if "okps" in m:
                self._push("ok", t, m["okps"])

        elif kind == "PHY":
            for key in ("payload", "len_raw", "pstat"):
                self._read(key, m.get(key), ".0f")
            self._record_quality_sample(kind, m)

        elif kind == "SFDSYNC":
            for key in ("sfderr", "lock", "syncs", "pre_rej"):
                self._read(key, m.get(key), ".0f")
            self._record_quality_sample(kind, m)

        elif kind == "PACKET":
            self._read("score", m.get("score"), ".0f")
            self._read("jitter", m.get("jitter"), ".1f")
            self._read("tq", m.get("tq"), ".0f")
            self._read("rs", m.get("rs"), ".0f")
            if "score" in m:
                self._push("score", t, m["score"])
            if "jitter" in m:
                self._push("jitter", t, m["jitter"])
            self._record_quality_sample(kind, m)
            score_min = self._quality_stat("score", min, seconds=60)
            if score_min is not None:
                self._read("score_min", score_min, ".0f")

        self._refresh_summary_panels()
        self._refresh_charts()
        self._refresh_link_health()
