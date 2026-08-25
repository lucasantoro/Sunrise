"""String and number helpers used across the tabs. No Qt.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

import re
from typing import Optional

from .. import ssh


def _remote_cd(path: str) -> str:
    path = path.strip() or "."
    if path.startswith("~"):
        return f"cd {path}"
    return f"cd {ssh.shell_quote(path)}"


def _env(name: str, value: str) -> str:
    return f"{name}={ssh.shell_quote(value)}"


def _rate_to_kbps(value: str) -> float:
    value = value.strip()
    if not value:
        return 0.0
    m = re.match(r"^([0-9]+(?:\.[0-9]+)?)([kKmM]?)$", value)
    if not m:
        return 0.0
    number = float(m.group(1))
    suffix = m.group(2).lower()
    if suffix == "m":
        return number * 1000.0
    if suffix == "k":
        return number
    return number / 1000.0


def _extract_iperf_kbps(text: str) -> float:
    matches = re.findall(r"([0-9]+(?:\.[0-9]+)?)\s+([KMG]?bits)/sec", text)
    if not matches:
        return 0.0
    value, unit = matches[-1]
    kbps = float(value)
    if unit == "Mbits":
        kbps *= 1000.0
    elif unit == "bits":
        kbps /= 1000.0
    elif unit == "Gbits":
        kbps *= 1000000.0
    return kbps


def _extract_iperf_loss(text: str) -> Optional[tuple[int, int, float]]:
    matches = re.findall(r"([0-9]+)/([0-9]+)\s+\(([0-9]+(?:\.[0-9]+)?)%\)", text)
    if not matches:
        return None
    lost, total, pct = matches[-1]
    return int(lost), int(total), float(pct)
