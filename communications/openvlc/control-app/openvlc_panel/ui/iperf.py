"""Performance tests: plan the flows, run them, stop them.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

import time

from .. import ssh
from ..config import Device
from .text import _extract_iperf_kbps, _extract_iperf_loss
from .theme import IPERF_SERVER_PID, IPERF_SERVER_LOG, IPERF_CLIENT_PID, IPERF_CLIENT_LOG
from .widgets import run_async


class IperfMixin:
    """Performance tests: plan the flows, run them, stop them."""

    def _iperf_plan(self, direction: str) -> tuple[list[dict], str]:
        if direction == "legacy_tx_to_rx":
            if not self.cfg.rx_pi.host or not self.cfg.tx_pi.host:
                raise RuntimeError("Legacy test requires RX Pi and TX Pi hosts in Settings.")
            return [
                {
                    "name": "Legacy TX Pi -> RX Pi",
                    "server_label": "RX Pi",
                    "server_dev": self.cfg.rx_pi,
                    "client_label": "TX Pi",
                    "client_dev": self.cfg.tx_pi,
                    "dst_ip": "192.168.0.2",
                }
            ], "RX Pi"

        if not self.cfg.trx_a.host or not self.cfg.trx_b.host:
            raise RuntimeError(
                "Transceiver tests require both Transceiver A and Transceiver B hosts in Settings."
            )

        a_to_b = {
            "name": "Transceiver A -> B",
            "server_label": "Transceiver B",
            "server_dev": self.cfg.trx_b,
            "client_label": "Transceiver A",
            "client_dev": self.cfg.trx_a,
            "dst_ip": self.cfg.trx_b_tun_ip,
        }
        b_to_a = {
            "name": "Transceiver B -> A",
            "server_label": "Transceiver A",
            "server_dev": self.cfg.trx_a,
            "client_label": "Transceiver B",
            "client_dev": self.cfg.trx_b,
            "dst_ip": self.cfg.trx_a_tun_ip,
        }
        if direction == "trx_a_to_b":
            return [a_to_b], "Transceiver B"
        if direction == "trx_b_to_a":
            return [b_to_a], "Transceiver A"
        if direction == "trx_full":
            return [a_to_b, b_to_a], "Transceiver A"
        raise RuntimeError(f"Unknown iperf direction: {direction}")


    @staticmethod
    def _iperf_cleanup_command() -> str:
        return "; ".join([
            ssh.kill_pidfile_command(IPERF_CLIENT_PID),
            ssh.kill_pidfile_command(IPERF_SERVER_PID),
        ])


    @staticmethod
    def _unique_devices(flows: list[dict]) -> list[Device]:
        devices: list[Device] = []
        seen: set[tuple[str, int, str]] = set()
        for flow in flows:
            for key in ("server_dev", "client_dev"):
                dev = flow[key]
                ident = (dev.host, dev.port, dev.user)
                if dev.host and ident not in seen:
                    seen.add(ident)
                    devices.append(dev)
        return devices


    def _set_iperf_running(self, running: bool) -> None:
        self._iperf_running = running
        if hasattr(self, "btn_iperf"):
            self.btn_iperf.setEnabled(not running)
        if hasattr(self, "btn_stop_iperf"):
            self.btn_stop_iperf.setEnabled(running)
        if hasattr(self, "cbo_iperf_direction"):
            self.cbo_iperf_direction.setEnabled(not running)


    def _stop_iperf(self):
        if not self._iperf_running:
            return
        self._iperf_stop_event.set()
        devices = list(self._iperf_active_devices)
        self._control_log("Stopping iperf ...")

        def task():
            lines = []
            for dev in devices:
                try:
                    st, out, err = ssh.run(
                        dev, self._iperf_cleanup_command(), timeout=8.0
                    )
                    target = f"{dev.user}@{dev.host}" if dev.user else dev.host
                    lines.append(f"{target}: [exit {st}] {(out + err).strip()}")
                except Exception as exc:  # noqa: BLE001
                    target = f"{dev.user}@{dev.host}" if dev.user else dev.host
                    lines.append(f"{target}: stop failed: {exc}")
            return "\n".join(lines)

        run_async(task, lambda r, e: self._control_log(f"iperf stop: {e if e else r}"))


    def _run_iperf(self):
        if self._iperf_running:
            self._control_log("iperf is already running. Use Stop iperf first.")
            return
        self._save_settings(silent=True)
        rate = self.ed_rate.text().strip() or "600k"
        self.cfg.iperf_rate = rate
        self.cfg.iperf_duration = self.sp_iperf_duration.value()
        self.cfg.save()
        direction = (
            self.cbo_iperf_direction.currentData()
            if hasattr(self, "cbo_iperf_direction")
            else "legacy_tx_to_rx"
        )
        port = self.cfg.iperf_port
        payload = self.cfg.iperf_payload
        seconds = self.cfg.iperf_duration
        try:
            flows, monitor_label = self._iperf_plan(direction)
        except RuntimeError as exc:
            self._control_log(f"iperf not started: {exc}")
            return

        self._iperf_stop_event.clear()
        self._iperf_active_devices = self._unique_devices(flows)
        self._set_iperf_running(True)
        self._start_monitor_if_needed(monitor_label)
        quality_start = time.time()

        def read_log(dev: Device, path: str) -> str:
            st, out, err = ssh.run(
                dev,
                f"test -r {ssh.shell_quote(path)} && cat {ssh.shell_quote(path)} || true",
                timeout=10.0,
            )
            text = out + err
            if st != 0 and not text.strip():
                return f"[log read exit {st}]"
            return text

        def task():
            stop_requested = False
            sections = []
            flow_stats = []
            devices = self._unique_devices(flows)

            for dev in devices:
                ssh.run(
                    dev,
                    self._iperf_cleanup_command()
                    + f"; rm -f {ssh.shell_quote(IPERF_SERVER_LOG)} {ssh.shell_quote(IPERF_CLIENT_LOG)}",
                    timeout=10.0,
                )

            # Start receiver/server processes first and verify they are alive.
            for flow in flows:
                # iperf2 fully buffers stdout when redirected to a file.
                # Force line buffering so the control panel can display the
                # authoritative receiver intervals while the test is running.
                server_cmd = (
                    f"exec stdbuf -oL -eL iperf -u -s -p {port} -i 1"
                )
                st, out, err = ssh.run_detached_pidfile(
                    flow["server_dev"], server_cmd, IPERF_SERVER_PID, IPERF_SERVER_LOG
                )
                if st != 0:
                    raise RuntimeError(
                        f"{flow['name']}: failed to start receiver iperf server "
                        f"on {flow['server_label']}: {(out + err).strip()}"
                    )
            time.sleep(0.8)
            for flow in flows:
                st, out, err = ssh.run(
                    flow["server_dev"],
                    ssh.pidfile_status_command(IPERF_SERVER_PID)
                    + f"; tail -20 {ssh.shell_quote(IPERF_SERVER_LOG)} 2>/dev/null || true",
                    timeout=8.0,
                )
                if st != 0:
                    raise RuntimeError(
                        f"{flow['name']}: receiver iperf server is not running "
                        f"on {flow['server_label']}:\n{(out + err).strip()}"
                    )
            self._iperf_bridge.line.emit(
                "[iperf] receiver server ready; starting transmitter"
            )

            # Start transmitter/client processes after servers are confirmed up.
            for flow in flows:
                client_cmd = (
                    f"exec iperf -u -c {ssh.shell_quote(flow['dst_ip'])} "
                    f"-b {ssh.shell_quote(rate)} -l {payload} "
                    f"-p {port} -t {seconds} -i 1"
                )
                st, out, err = ssh.run_detached_pidfile(
                    flow["client_dev"], client_cmd, IPERF_CLIENT_PID, IPERF_CLIENT_LOG
                )
                if st != 0:
                    raise RuntimeError(
                        f"{flow['name']}: failed to start transmitter iperf client "
                        f"on {flow['client_label']}: {(out + err).strip()}"
                    )
            self._iperf_bridge.line.emit(
                "[iperf] transmitter started; waiting for live RX intervals"
            )

            deadline = time.monotonic() + seconds + 2.0
            next_rx_poll = time.monotonic()
            last_rx_lines: dict[str, str] = {}
            while time.monotonic() < deadline:
                if self._iperf_stop_event.is_set():
                    stop_requested = True
                    break
                now = time.monotonic()
                if now >= next_rx_poll:
                    next_rx_poll = now + 1.0
                    for flow in flows:
                        st, out, _err = ssh.run(
                            flow["server_dev"],
                            f"tail -n 12 {ssh.shell_quote(IPERF_SERVER_LOG)} "
                            "2>/dev/null || true",
                            timeout=5.0,
                        )
                        if st != 0:
                            continue
                        intervals = [
                            line.strip() for line in out.splitlines()
                            if "bits/sec" in line and " sec " in line
                        ]
                        if not intervals:
                            continue
                        latest = intervals[-1]
                        if last_rx_lines.get(flow["name"]) == latest:
                            continue
                        last_rx_lines[flow["name"]] = latest
                        self._iperf_bridge.line.emit(
                            f"[iperf RX {flow['server_label']}] {latest}"
                        )
                time.sleep(0.25)

            for dev in devices:
                ssh.run(dev, self._iperf_cleanup_command(), timeout=8.0)

            for flow in flows:
                server_text = read_log(flow["server_dev"], IPERF_SERVER_LOG)
                client_text = read_log(flow["client_dev"], IPERF_CLIENT_LOG)
                goodput = _extract_iperf_kbps(server_text)
                loss = _extract_iperf_loss(server_text)
                flow_stats.append({"name": flow["name"], "goodput": goodput, "loss": loss})
                sections.extend([
                    f"=== {flow['name']} ===",
                    f"receiver/server={flow['server_label']} dst={flow['dst_ip']} port={port}",
                    "iperf receiver/server output (authoritative):",
                    server_text.strip() or "(empty)",
                    "",
                    f"transmitter/client={flow['client_label']}",
                    "iperf transmitter/client output:",
                    client_text.strip() or "(empty)",
                    "",
                ])

            goodputs = [float(s["goodput"]) for s in flow_stats if s["goodput"]]
            goodput = min(goodputs) if len(goodputs) > 1 else (goodputs[0] if goodputs else 0.0)
            return {
                "text": "\n".join(sections),
                "goodput": goodput,
                "flows": flow_stats,
                "stopped": stop_requested,
            }

        self._control_log(
            f"iperf {self.cbo_iperf_direction.currentText()} @ {rate}, "
            f"payload={payload}, port={port}, duration={seconds}s ..."
        )

        def done(result, error):
            devices = list(self._iperf_active_devices)
            self._set_iperf_running(False)
            self._iperf_active_devices = []
            if error:
                self._control_log(f"iperf failed: {error}")
                def cleanup():
                    for dev in devices:
                        try:
                            ssh.run(dev, self._iperf_cleanup_command(), timeout=8.0)
                        except Exception:  # noqa: BLE001
                            pass
                    return "remote iperf cleanup attempted"

                run_async(cleanup, lambda r, e: self._control_log(str(e) if e else r))
                return
            assert isinstance(result, dict)
            goodput = result.get("goodput") or 0.0
            if goodput:
                self._last_goodput_kbps = float(goodput)
                self._read("goodput", self._last_goodput_kbps, ".0f")
                self._apply_video_capacity_hint()
            flows = result.get("flows") or []
            headline = ["iperf stopped by user" if result.get("stopped") else "iperf completed"]
            if goodput:
                headline.append(f"iperf goodput: {float(goodput):.0f} kbps")
            for flow in flows:
                text = f"{flow['name']}: {float(flow.get('goodput') or 0.0):.0f} kbps"
                loss = flow.get("loss")
                if loss:
                    lost, total, pct = loss
                    text += f", loss {lost}/{total} ({pct:.2f}%)"
                headline.append(text)
            headline.append("quality during iperf:")
            headline.append(self._quality_summary(since=quality_start))
            headline.append("")
            headline.append(result["text"])
            self._control_log("\n".join(headline))

        run_async(task, done)


    def _update_iperf_button(self) -> None:
        if hasattr(self, "btn_iperf") and hasattr(self, "sp_iperf_duration"):
            direction = ""
            if hasattr(self, "cbo_iperf_direction"):
                direction = " " + self.cbo_iperf_direction.currentText()
            self.btn_iperf.setText(
                f"Run {self.sp_iperf_duration.value()} s{direction}"
            )
