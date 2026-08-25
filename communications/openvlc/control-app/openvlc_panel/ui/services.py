"""Remote service control and the two SSH command helpers.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

from .. import ssh
from ..config import Device
from .text import _remote_cd
from .theme import VALID_BUDGETS
from .widgets import run_async


class ServicesMixin:
    """Remote service control and the two SSH command helpers."""

    def _bridge_ctl(self, action: str):
        cmd = f"systemctl {action} openvlc-rx"
        self._sudo_do(self.cfg.rx_pi, cmd, f"bridge {action}")


    def _trx_ctl(self, attr: str, action: str):
        dev = getattr(self.cfg, attr, None)
        if dev is None or not dev.host:
            self._status(f"{attr}: host not configured (Settings)", err=True)
            return
        label = "Transceiver A" if attr == "trx_a" else "Transceiver B"
        self._sudo_do(dev, f"systemctl {action} openvlc-transceiver",
                      f"{label} {action}")


    def _trx_ctl_all(self, action: str):
        self._save_settings(silent=True)
        targets = [
            ("Transceiver A", self.cfg.trx_a),
            ("Transceiver B", self.cfg.trx_b),
        ]
        configured = [(label, dev) for label, dev in targets if dev.host]
        if not configured:
            self._status("Transceiver A/B hosts not configured (Settings)", err=True)
            return
        self._control_log(f"Transceivers {action} ...")

        def task():
            lines = []
            for label, dev in configured:
                st, out, err = ssh.run_sudo(
                    dev, f"systemctl {action} openvlc-transceiver", timeout=60.0
                )
                lines.append(f"## {label}\n[exit {st}]\n{(out + err).strip()}")
            return "\n\n".join(lines)

        run_async(task, lambda r, e: self._control_log(f"Transceivers {action}: {e if e else r}"))


    def _setup_tx_pi(self):
        cmd = f"{_remote_cd(self.cfg.tx_gateway_dir)} && bash setup_tx_pi.sh"
        self._sudo_do(self.cfg.tx_pi, cmd, "TX Pi route")


    def _start_bbb_tx(self):
        budget = self.cfg.tx_budget
        info = VALID_BUDGETS.get(budget)
        if info is None:
            self._control_log(
                f"Invalid TX budget {budget}. Select one of: "
                f"{', '.join(str(v) for v in sorted(VALID_BUDGETS))}."
            )
            return
        cmd = (
            f"{_remote_cd(self.cfg.bbb_tx_dir)} && "
            f"OPENVLC_TX_ENABLE_MODE=0 bash {info['script']} "
            "rx=0 self_id=7 dst_id=8 pool_size=50 mtu=900"
        )
        self._do(self.cfg.bbb, cmd, f"BBB TX budget {budget}", detached=True)


    def _do(self, dev: Device, cmd: str, label: str, detached: bool = False):
        self._control_log(f"{label} ...")

        def task():
            if detached:
                st, out, err = ssh.run_detached(dev, cmd)
                return f"[exit {st}] {out}{err}"
            st, out, err = ssh.run(dev, cmd, timeout=60.0)
            return f"[exit {st}] {out}{err}"

        run_async(task, lambda r, e: self._control_log(f"{label}: {e if e else r}"))


    def _sudo_do(self, dev: Device, cmd: str, label: str):
        self._control_log(f"{label} ...")

        def task():
            st, out, err = ssh.run_sudo(dev, cmd, timeout=60.0)
            return f"[exit {st}] {out}{err}"

        run_async(task, lambda r, e: self._control_log(f"{label}: {e if e else r}"))
