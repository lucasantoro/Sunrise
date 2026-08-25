"""Video pipeline: relay, embedded player and the TX camera.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

from typing import Callable, Optional
import os
import subprocess
import time

from PySide6 import QtCore

from .. import ssh
from .deps import HAVE_VLC, vlc
from .text import _remote_cd, _env, _rate_to_kbps
from .theme import VIDEO_PRESETS, RELAY_PID, RELAY_LOG, TX_VIDEO_PID, TX_VIDEO_LOG
from .widgets import run_async


class VideoSessionMixin:
    """Video pipeline: relay, embedded player and the TX camera."""

    def _record_fetch(self):
        remote = "~/openvlc_panel_capture.ts"
        local = os.path.join(os.path.expanduser("~"), "Downloads", "openvlc_capture.ts")
        _tx_dev, rx_dev, _destination, route_label = self._current_video_route()

        def task():
            ssh.run(rx_dev, ssh.kill_pidfile_command(RELAY_PID), timeout=10.0)
            ssh.run(
                rx_dev,
                f"timeout --signal=INT --kill-after=3s 25s "
                f"ffmpeg -nostdin -hide_banner -y -i "
                f"'udp://0.0.0.0:{self.cfg.video_port}?fifo_size=1000000&overrun_nonfatal=1' "
                f"-c copy -t 15 {remote}",
                timeout=35.0,
            )
            ssh.sftp_get(rx_dev, remote, local)
            return local

        self._control_log(f"recording 15s on {route_label} + downloading ...")

        def done(result, error):
            if self._video_session_active:
                # Recording owns the RX UDP port, so the previous live relay
                # no longer exists. End that session instead of leaving the UI
                # and player in a misleading frozen-LIVE state.
                self._stop_video_session()
            if error:
                self._control_log(f"record failed: {error}")
                return
            self._control_log(f"saved {result} - opening in VLC")
            self._launch_vlc([result], hw_none=False)

        run_async(task, done)


    def _start_live(
        self, on_done: Optional[Callable[[bool, object], None]] = None
    ):
        self._save_settings(silent=True)
        self._stop_vlc()
        udp = self.rb_udp.isChecked()
        rport = self.cfg.relay_port
        vport = self.cfg.video_port
        _tx_dev, rx_dev, _destination, route_label = self._current_video_route()
        if udp:
            relay = f"exec socat -u UDP-RECV:{vport} UDP-SENDTO:{self.cfg.pc_ip}:{rport}"
            url = f"udp://@:{rport}"
        else:
            relay = f"exec socat -u UDP-RECV:{vport} TCP-LISTEN:{rport},reuseaddr"
            url = f"tcp://{rx_dev.host}:{rport}"

        def task():
            ssh.run(rx_dev, ssh.kill_pidfile_command(RELAY_PID), timeout=10.0)
            time.sleep(0.3)
            checks = [
                "command -v socat >/dev/null || "
                "{ echo 'missing:socat (install with: sudo apt install socat)'; exit 127; }",
                f"busy=$(ss -H -lunp 2>/dev/null | grep -E ':{vport}[[:space:]]' || true); "
                f"[ -z \"$busy\" ] || {{ echo 'UDP/{vport} already in use:'; echo \"$busy\"; exit 4; }}",
            ]
            if not udp:
                checks.append(
                    f"busy=$(ss -H -ltnp 2>/dev/null | grep -E ':{rport}[[:space:]]' || true); "
                    f"[ -z \"$busy\" ] || {{ echo 'TCP/{rport} already in use:'; echo \"$busy\"; exit 4; }}"
                )
            check_status, check_out, check_error = ssh.run(
                rx_dev, "; ".join(checks), timeout=10.0
            )
            if check_status != 0:
                detail = (check_out + check_error).strip()
                raise RuntimeError(detail or "relay preflight failed")
            status, _out, error = ssh.run_detached_pidfile(
                rx_dev, relay, RELAY_PID, RELAY_LOG
            )
            if status != 0:
                raise RuntimeError(f"relay launch failed: {error.strip()}")
            time.sleep(0.6)
            status, out, error = ssh.run(
                rx_dev,
                f"tail -20 {ssh.shell_quote(RELAY_LOG)} 2>/dev/null || true; "
                f"echo relay-status:; {ssh.pidfile_status_command(RELAY_PID)}",
                timeout=10.0,
            )
            if status != 0:
                detail = (out + error).strip()
                raise RuntimeError(f"relay exited during startup: {detail}")
            return url

        self._control_log(
            f"starting relay on {route_label} ({'UDP' if udp else 'TCP'}) ..."
        )

        def done(result, error):
            if error:
                self._control_log(f"relay failed: {error}")
                if on_done:
                    on_done(False, error)
                return
            cache = self.sp_cache.value()
            embed = HAVE_VLC and self.cb_embed.isChecked()
            self._control_log(f"relay up -> {result}  (embed={'yes' if embed else 'no'})")
            self._video_url = str(result)
            if embed:
                if not self._play_embedded(result, cache):
                    self._do(
                        rx_dev,
                        ssh.kill_pidfile_command(RELAY_PID),
                        "cleanup failed relay",
                    )
                    if on_done:
                        on_done(False, "embedded VLC failed to start")
                    return
            else:
                if not self._launch_vlc(
                    [f"--network-caching={cache}", result], hw_none=True
                ):
                    self._do(
                        rx_dev,
                        ssh.kill_pidfile_command(RELAY_PID),
                        "cleanup failed relay",
                    )
                    if on_done:
                        on_done(False, "external VLC failed to start")
                    return
            self._set_preview_status("Preview: player opening, waiting for MPEG-TS")
            self._status("live started")
            if on_done:
                on_done(True, result)

        run_async(task, done)


    def _start_video_session(self):
        if self._video_session_starting or self._video_session_active:
            return
        self._save_settings(silent=True)
        tx_dev, rx_dev, destination, route_label = self._video_route()
        if not tx_dev.host or not rx_dev.host:
            self._set_video_state("CONFIG ERROR", "#A92D2D", "#FFF0F0")
            self._set_preview_status(
                "Camera or receiver SSH host is missing in Settings.", error=True
            )
            self._control_log(
                f"video configuration error: {route_label}; "
                f"camera={tx_dev.host or '(empty)'}, "
                f"receiver={rx_dev.host or '(empty)'}"
            )
            return
        self._video_route_devices = (tx_dev, rx_dev, destination, route_label)
        self._start_monitor_if_needed()
        self.tabs.setCurrentIndex(self.video_tab_index)
        self._video_session_generation += 1
        self._video_session_starting = True
        self._set_video_state("STARTING", "#9A6500", "#FFF3D3")
        self._set_preview_status("Preview: starting RX relay")
        self._control_log(
            f"video session: {route_label}; destination={destination}:"
            f"{self.cfg.video_port}. Starting receiver relay, then camera stream ..."
        )
        self._start_live(self._after_live_started)


    def _after_live_started(self, ok: bool, detail: object) -> None:
        if not ok:
            self._video_session_starting = False
            self._video_route_devices = None
            self._set_video_state("RELAY ERROR", "#A92D2D", "#FFF0F0")
            self._set_preview_status(f"Preview unavailable: {detail}", error=True)
            return
        self._start_tx_video(self._after_tx_video_started)


    def _after_tx_video_started(self, ok: bool, detail: object) -> None:
        self._video_session_starting = False
        self._video_session_active = ok
        if ok:
            self._set_video_state("BUFFERING", "#9A6500", "#FFF3D3")
            self._set_preview_status(
                "Preview: TX encoder and RX relay are running; waiting for VLC decode"
            )
            generation = self._video_session_generation
            QtCore.QTimer.singleShot(
                1800, lambda: self._check_video_pipeline(generation, final=False)
            )
            QtCore.QTimer.singleShot(
                6000, lambda: self._check_video_pipeline(generation, final=True)
            )
        else:
            self._stop_live()
            self._video_route_devices = None
            self._set_video_state("TX ERROR", "#A92D2D", "#FFF0F0")
            self._set_preview_status(f"Preview unavailable: {detail}", error=True)


    def _toggle_video_session(self):
        if self._video_session_starting:
            return
        if self._video_session_active:
            self._stop_video_session()
        else:
            self._start_video_session()


    def _stop_video_session(self):
        self._control_log("video session: stopping TX stream and RX relay ...")
        self._stop_tx_video()
        self._stop_live()
        self._video_route_devices = None
        self._video_session_starting = False
        self._video_session_active = False
        self._video_session_generation += 1
        self._set_video_state("STOPPED", "#66758F", "#E7ECF3")
        self._set_preview_status("Preview: stopped")
        if hasattr(self, "lbl_video_placeholder"):
            self.lbl_video_placeholder.show()


    def _set_video_state(self, label: str, color: str, background: str) -> None:
        if not hasattr(self, "lbl_video_state"):
            return
        self.lbl_video_state.setText(f"SESSION  {label}")
        self.lbl_video_state.setStyleSheet(
            f"color:{color}; background:{background}; border-radius:14px;"
            "padding:7px 13px; font-weight:700;"
        )
        active_or_starting = label in (
            "LIVE", "BUFFERING", "STARTING", "VIDEO ERROR", "NO VIDEO"
        )
        self.btn_video_session.setText(
            "Stop video session" if active_or_starting else "Start video session"
        )
        self.btn_video_session.setEnabled(label != "STARTING")
        self.btn_video_session.setProperty(
            "role", "danger" if active_or_starting else "primary"
        )
        self.btn_video_session.style().unpolish(self.btn_video_session)
        self.btn_video_session.style().polish(self.btn_video_session)
        if hasattr(self, "cb_video_camera_node"):
            self.cb_video_camera_node.setEnabled(not active_or_starting)


    def _set_preview_status(self, text: str, error: bool = False) -> None:
        if not hasattr(self, "lbl_preview_status"):
            return
        self.lbl_preview_status.setText(text)
        if error:
            self.lbl_preview_status.setStyleSheet(
                "color:#A92D2D; background:#FFF0F0; border-radius:6px; padding:7px 9px;"
            )
        else:
            self.lbl_preview_status.setStyleSheet(
                "color:#52637D; background:#EDF1F6; border-radius:6px; padding:7px 9px;"
            )


    def _play_embedded(self, url: str, cache_ms: int) -> bool:
        self._stop_embedded()
        try:
            vlc_dir = os.path.dirname(self.cfg.vlc_path)
            plugin_dir = os.path.join(vlc_dir, "plugins")
            if os.path.isdir(plugin_dir):
                os.environ["VLC_PLUGIN_PATH"] = plugin_dir
            self._vlc_instance = vlc.Instance(
                "--avcodec-hw=none", "--no-video-title-show", "--quiet"
            )
            if self._vlc_instance is None:
                self._control_log("libvlc init failed - check VLC installation.")
                return False
            self._vlc_player = self._vlc_instance.media_player_new()
            media = self._vlc_instance.media_new(
                url, f":network-caching={cache_ms}", ":avcodec-hw=none"
            )
            self._vlc_player.set_media(media)
            self._vlc_player.set_hwnd(int(self.video_frame.winId()))
            rc = self._vlc_player.play()
            self._control_log(f"embedded player: play() returned {rc}")
            return rc != -1
        except Exception as exc:  # noqa: BLE001
            self._control_log(f"embedded video error: {exc}")
            return False


    def _stop_embedded(self):
        if self._vlc_player is not None:
            try:
                self._vlc_player.stop()
                self._vlc_player.release()
            except Exception:  # noqa: BLE001
                pass
            self._vlc_player = None
        if self._vlc_instance is not None:
            try:
                self._vlc_instance.release()
            except Exception:  # noqa: BLE001
                pass
            self._vlc_instance = None


    def _stop_live(self):
        self._stop_embedded()
        self._stop_vlc()
        _tx_dev, rx_dev, _destination, route_label = self._current_video_route()
        self._do(
            rx_dev,
            ssh.kill_pidfile_command(RELAY_PID),
            f"stop relay ({route_label})",
        )


    def _local_player_state(self) -> tuple[bool, str]:
        if self._vlc_player is not None:
            try:
                state = self._vlc_player.get_state()
                name = str(state)
                playing = state == vlc.State.Playing
                if playing:
                    width, height = self._vlc_player.video_get_size(0)
                    suffix = f" {width}x{height}" if width and height else ""
                    return True, f"VLC {name}{suffix}"
                return False, f"VLC {name}"
            except Exception as exc:  # noqa: BLE001
                return False, f"VLC state error: {exc}"
        if self._vlc_proc is not None:
            code = self._vlc_proc.poll()
            return code is None, (
                "external VLC running" if code is None else f"external VLC exited ({code})"
            )
        return False, "no player process"


    def _check_video_pipeline(self, generation: int, final: bool) -> None:
        if (
            generation != self._video_session_generation
            or not self._video_session_active
        ):
            return

        tx_dev, rx_dev, _destination, route_label = self._current_video_route()

        def task():
            rx_cmd = (
                f"{ssh.pidfile_status_command(RELAY_PID)}; "
                f"echo listeners:; "
                f"ss -H -ltnp 2>/dev/null | grep ':{self.cfg.relay_port} ' || true; "
                f"ss -H -lunp 2>/dev/null | grep ':{self.cfg.video_port} ' || true; "
                f"echo log:; tail -25 {ssh.shell_quote(RELAY_LOG)} 2>/dev/null || true"
            )
            tx_cmd = (
                f"{ssh.pidfile_status_command(TX_VIDEO_PID)}; "
                f"echo log:; tail -30 {ssh.shell_quote(TX_VIDEO_LOG)} 2>/dev/null || true"
            )
            rx_status, rx_out, rx_err = ssh.run(rx_dev, rx_cmd, timeout=10.0)
            tx_status, tx_out, tx_err = ssh.run(tx_dev, tx_cmd, timeout=10.0)
            return {
                "rx_ok": rx_status == 0,
                "rx": (rx_out + rx_err).strip(),
                "tx_ok": tx_status == 0,
                "tx": (tx_out + tx_err).strip(),
                "route": route_label,
            }

        def done(result, error):
            if (
                generation != self._video_session_generation
                or not self._video_session_active
            ):
                return
            player_ok, player_detail = self._local_player_state()
            if error:
                self._set_video_state("VIDEO ERROR", "#A92D2D", "#FFF0F0")
                self._set_preview_status(f"Preview check failed: {error}", error=True)
                return
            assert isinstance(result, dict)
            if not result["rx_ok"] or not result["tx_ok"]:
                failed = "RX relay" if not result["rx_ok"] else "TX encoder"
                detail = result["rx"] if not result["rx_ok"] else result["tx"]
                self._set_video_state("VIDEO ERROR", "#A92D2D", "#FFF0F0")
                self._set_preview_status(
                    f"Preview unavailable: {failed} stopped. See Control log.",
                    error=True,
                )
                self._control_log(f"video pipeline failure ({failed}):\n{detail}")
                return
            if player_ok:
                self._set_video_state("LIVE", "#087A49", "#E4F7EE")
                self._set_preview_status(f"Preview active: {player_detail}")
                if hasattr(self, "lbl_video_placeholder"):
                    self.lbl_video_placeholder.hide()
                return
            if final:
                self._set_video_state("NO VIDEO", "#A92D2D", "#FFF0F0")
                self._set_preview_status(
                    f"Stream processes are alive but preview is not decoding: {player_detail}. "
                    "Check RX relay/TX logs in the Control tab.",
                    error=True,
                )
                self._control_log(
                    "video pipeline is alive but VLC did not reach Playing state\n"
                    f"player: {player_detail}\nRX:\n{result['rx']}\nTX:\n{result['tx']}"
                )
            else:
                self._set_preview_status(f"Preview buffering: {player_detail}")

        run_async(task, done)


    def _start_tx_video(
        self, on_done: Optional[Callable[[bool, object], None]] = None
    ):
        self._save_settings(silent=True)
        tx_dev, rx_dev, destination, route_label = self._current_video_route()
        mux_kbps = _rate_to_kbps(self.ed_muxrate.text().strip())
        if mux_kbps and self._last_goodput_kbps:
            margin = 100.0 * (self._last_goodput_kbps - mux_kbps) / mux_kbps
            if margin < 10.0:
                self._control_log(
                    f"warning: muxrate {mux_kbps:.0f} kbps leaves only "
                    f"{margin:.1f}% margin vs last iperf RX goodput "
                    f"{self._last_goodput_kbps:.0f} kbps"
                )
        elif not self._last_goodput_kbps:
            self._control_log("warning: run iperf first to measure video capacity margin")

        env = {
            "DEST": destination,
            "PORT": str(self.cfg.video_port),
            "SIZE": self.ed_capture_size.text().strip(),
            "OUT_SIZE": self.ed_outsize.text().strip(),
            "FPS": self.ed_fps.text().strip(),
            "OUT_FPS": self.ed_outfps.text().strip(),
            "BITRATE": self.ed_bitrate.text().strip(),
            "MUXRATE": self.ed_muxrate.text().strip(),
            "RATE_MODE": self.cb_mode.currentText(),
            "CRF": self.ed_crf.text().strip(),
            "PRESET": self.ed_preset.text().strip(),
            "H264_PROFILE": VIDEO_PRESETS.get(
                self.cb_video_preset.currentText(), {}
            ).get("profile", "main"),
            "BUFSIZE": self.ed_bufsize.text().strip(),
            "MUXDELAY": self.ed_muxdelay.text().strip(),
            "INPUT_FORMAT": self.ed_input_format.text().strip(),
            "GOP": VIDEO_PRESETS.get(
                self.cb_video_preset.currentText(), {}
            ).get("gop", self.ed_outfps.text().strip()),
            "SLICE_BYTES": VIDEO_PRESETS.get(
                self.cb_video_preset.currentText(), {}
            ).get("slice_bytes", "600"),
            "LOGLEVEL": "warning",
        }
        assigns = " ".join(_env(k, v) for k, v in env.items() if v)
        cmd = f"{_remote_cd(self.cfg.tx_gateway_dir)} && exec env {assigns} bash vlc_tx_video.sh"
        self._control_log(
            f"video path: {route_label}; camera={tx_dev.host}; "
            f"TX -> {destination}:{self.cfg.video_port}; "
            f"receiver relay={rx_dev.host}:{self.cfg.relay_port}"
        )

        def task():
            ssh.run(tx_dev, ssh.kill_pidfile_command(TX_VIDEO_PID), timeout=10.0)
            status, _out, error = ssh.run_detached_pidfile(
                tx_dev, cmd, TX_VIDEO_PID, TX_VIDEO_LOG
            )
            if status != 0:
                raise RuntimeError(f"TX launch failed: {error.strip()}")
            time.sleep(1.2)
            status, out, error = ssh.run(
                tx_dev,
                f"{ssh.pidfile_status_command(TX_VIDEO_PID)}; "
                f"tail -25 {ssh.shell_quote(TX_VIDEO_LOG)} 2>/dev/null || true",
                timeout=10.0,
            )
            if status != 0:
                detail = (out + error).strip()
                raise RuntimeError(f"TX encoder exited during startup: {detail}")
            return out.strip() or "encoder alive"

        self._control_log("TX video ...")
        def done(result, error):
            self._control_log(f"TX video: {error if error else result}")
            if on_done:
                on_done(error is None, error if error else result)

        run_async(task, done)


    def _stop_tx_video(self):
        tx_dev, _rx_dev, _destination, route_label = self._current_video_route()
        self._do(
            tx_dev,
            ssh.kill_pidfile_command(TX_VIDEO_PID),
            f"stop TX video ({route_label})",
        )


    def _launch_vlc(self, args: list, hw_none: bool) -> bool:
        self._stop_vlc()
        cmd = [self.cfg.vlc_path]
        if hw_none:
            cmd.append("--avcodec-hw=none")
        cmd += args
        try:
            self._vlc_proc = subprocess.Popen(cmd)
            return True
        except Exception as exc:  # noqa: BLE001
            self._control_log(f"VLC launch failed: {exc}")
            return False


    def _stop_vlc(self):
        if self._vlc_proc is not None:
            try:
                self._vlc_proc.terminate()
            except Exception:  # noqa: BLE001
                pass
            self._vlc_proc = None
