"""Thin paramiko helpers: connect (optionally via a jump host), run one-shot
commands, launch detached background jobs, and stream a remote log.

The OpenVLC control panel issues short ``sudo`` commands and starts/stops a few
long-running helpers (the bridge, a socat relay, iperf, the TX video). Long jobs
are launched detached with ``nohup ... &`` so the SSH channel can close. Helpers
started by the panel can also write PID files so they can be stopped without
broad ``pkill`` calls.
"""

from __future__ import annotations

import threading
from typing import Callable, Optional

import paramiko

from .config import Device


def _connect(dev: Device, timeout: float = 8.0) -> paramiko.SSHClient:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    sock = None
    if dev.jump_host:
        jump = paramiko.SSHClient()
        jump.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        jump.connect(
            dev.jump_host, port=dev.jump_port, username=dev.jump_user,
            password=dev.jump_password or None, timeout=timeout,
            allow_agent=False, look_for_keys=False,
        )
        transport = jump.get_transport()
        sock = transport.open_channel(
            "direct-tcpip", (dev.host, dev.port), ("127.0.0.1", 0)
        )
        # Keep the jump client alive on the target so the tunnel stays open.
        client._jump = jump  # type: ignore[attr-defined]

    client.connect(
        dev.host, port=dev.port, username=dev.user,
        password=dev.password or None, timeout=timeout,
        allow_agent=False, look_for_keys=False, sock=sock,
    )
    return client


def run(dev: Device, command: str, timeout: float = 20.0) -> tuple[int, str, str]:
    """Run one command, return (exit_status, stdout, stderr)."""
    client = _connect(dev, timeout=timeout)
    try:
        _stdin, stdout, stderr = client.exec_command(command, timeout=timeout)
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        status = stdout.channel.recv_exit_status()
        return status, out, err
    finally:
        _close(client)


def run_sudo(dev: Device, command: str, timeout: float = 60.0) -> tuple[int, str, str]:
    """Run one command through sudo, feeding the saved device password if set.

    The GUI is a lab tool and already stores the SSH password in its config.
    Reusing it here avoids interactive ``sudo`` prompts, which cannot work from
    a non-interactive SSH command channel.
    """
    client = _connect(dev, timeout=timeout)
    try:
        sudo = "sudo -S -p ''" if dev.password else "sudo -n"
        _stdin, stdout, stderr = client.exec_command(
            f"{sudo} bash -lc {_q(command)}", timeout=timeout
        )
        if dev.password:
            _stdin.write(dev.password + "\n")
            _stdin.flush()
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        status = stdout.channel.recv_exit_status()
        if status != 0 and not dev.password:
            err += (
                "\nHint: sudo needs a password. Save the device password in "
                "Settings or configure passwordless sudo for this lab user.\n"
            )
        return status, out, err
    finally:
        _close(client)


def run_detached(dev: Device, command: str,
                 log_path: str = "/tmp/openvlc_panel.log") -> tuple[int, str, str]:
    """Launch a long-running command detached from the SSH session."""
    wrapped = f"nohup bash -lc {_q(command)} >{_q(log_path)} 2>&1 &"
    return run(dev, wrapped, timeout=15.0)


def run_detached_pidfile(dev: Device, command: str, pidfile: str,
                         log_path: str) -> tuple[int, str, str]:
    """Launch a detached command and store the remote shell PID in ``pidfile``."""
    wrapped = (
        f"nohup bash -lc {_q(command)} >{_q(log_path)} 2>&1 "
        f"& echo $! >{_q(pidfile)}"
    )
    return run(dev, wrapped, timeout=15.0)


def kill_pidfile_command(pidfile: str) -> str:
    """Return a shell snippet that stops and removes one PID-file process."""
    q = _q(pidfile)
    return (
        f"if [ -s {q} ]; then "
        f"pid=$(cat {q}); "
        f"kill \"$pid\" 2>/dev/null || true; "
        f"sleep 0.2; "
        f"kill -9 \"$pid\" 2>/dev/null || true; "
        f"rm -f {q}; "
        f"fi"
    )


def pidfile_status_command(pidfile: str) -> str:
    """Return a shell snippet that reports whether a PID-file process is alive."""
    q = _q(pidfile)
    return (
        f"if [ ! -s {q} ]; then echo missing-pidfile; exit 2; fi; "
        f"pid=$(cat {q}); "
        f"if kill -0 \"$pid\" 2>/dev/null; then "
        f"echo alive:$pid; "
        f"else echo dead:$pid; exit 3; fi"
    )


def shell_quote(value: object) -> str:
    """Single-quote a value for embedding in a remote bash command."""
    return _q(str(value))


def sftp_get(dev: Device, remote_path: str, local_path: str) -> None:
    """Download a file from the device (used for 'record & fetch')."""
    client = _connect(dev, timeout=15.0)
    try:
        sftp = client.open_sftp()
        try:
            # Expand a leading ~ since SFTP does not do shell expansion.
            if remote_path.startswith("~"):
                home = client.exec_command("echo $HOME")[1].read().decode().strip()
                remote_path = home + remote_path[1:]
            sftp.get(remote_path, local_path)
        finally:
            sftp.close()
    finally:
        _close(client)


def ping(dev: Device) -> tuple[bool, str]:
    """Quick reachability/auth check."""
    try:
        status, out, _err = run(dev, "echo openvlc-ok", timeout=8.0)
        return (status == 0 and "openvlc-ok" in out), "connected"
    except Exception as exc:  # noqa: BLE001 - surface any SSH error to the UI
        return False, str(exc)


def _close(client: paramiko.SSHClient) -> None:
    try:
        client.close()
    finally:
        jump = getattr(client, "_jump", None)
        if jump is not None:
            jump.close()


def _q(value: str) -> str:
    """Single-quote a string for safe embedding inside bash -lc '...'."""
    return "'" + value.replace("'", "'\\''") + "'"


class LogStreamer:
    """Stream a remote command's stdout line by line on a background thread.

    Used for ``journalctl -u openvlc-rx -f``. Call :meth:`stop` to end it.
    """

    def __init__(self, dev: Device, command: str,
                 on_line: Callable[[str], None],
                 on_error: Callable[[str], None]):
        self._dev = dev
        self._command = command
        self._on_line = on_line
        self._on_error = on_error
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._client: Optional[paramiko.SSHClient] = None

    def start(self) -> None:
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._client is not None:
            try:
                self._client.close()
            except Exception:  # noqa: BLE001
                pass

    def _run(self) -> None:
        try:
            self._client = _connect(self._dev, timeout=8.0)
            _stdin, stdout, _stderr = self._client.exec_command(
                self._command, get_pty=True
            )
            for line in iter(stdout.readline, ""):
                if self._stop.is_set():
                    break
                line = line.rstrip("\r\n")
                if line:
                    self._on_line(line)
        except Exception as exc:  # noqa: BLE001
            if not self._stop.is_set():
                self._on_error(str(exc))
        finally:
            if self._client is not None:
                _close(self._client)
