"""Replace the uniform import block in generated ui/ modules with real ones.

split_app.py gives every emitted module the same import header, which is safe
but leaves each one importing a dozen names it never touches, plus a wildcard
``from .theme import *``. This rewrites each header to exactly what the module
body references, so reading a module tells you what it actually depends on.

Idempotent: run it again after regenerating and it produces the same files.

    .venv/Scripts/python.exe tools/tidy_imports.py
"""

from __future__ import annotations

import ast
import io
import os
import sys

UI_DIR = os.path.join("openvlc_panel", "ui")
GENERATED = ("chrome", "tab_control", "tab_dashboard", "tab_agc", "tab_video",
             "tab_diagnostics", "tab_console", "tab_settings", "monitor",
             "services", "iperf", "video_session")

# name -> import line providing it. Order here is the order emitted.
PROVIDERS = [
    ("os", "import os"),
    ("re", "import re"),
    ("subprocess", "import subprocess"),
    ("threading", "import threading"),
    ("time", "import time"),
    ("deque", "from collections import deque"),
    ("datetime", "from datetime import datetime"),
    ("Path", "from pathlib import Path"),
    ("Callable", "from typing import Callable"),
    ("Optional", "from typing import Optional"),
    ("QtCore", "from PySide6 import QtCore"),
    ("QtGui", "from PySide6 import QtGui"),
    ("QtWidgets", "from PySide6 import QtWidgets"),
    ("ssh", "from .. import ssh"),
    ("Config", "from ..config import Config"),
    ("Device", "from ..config import Device"),
    ("parse_line", "from ..stats import parse_line"),
    ("HAVE_PG", "from .deps import HAVE_PG"),
    ("HAVE_VLC", "from .deps import HAVE_VLC"),
    ("pg", "from .deps import pg"),
    ("vlc", "from .deps import vlc"),
    ("_remote_cd", "from .text import _remote_cd"),
    ("_env", "from .text import _env"),
    ("_rate_to_kbps", "from .text import _rate_to_kbps"),
    ("_extract_iperf_kbps", "from .text import _extract_iperf_kbps"),
    ("_extract_iperf_loss", "from .text import _extract_iperf_loss"),
    ("LogBridge", "from .widgets import LogBridge"),
    ("HistoryLineEdit", "from .widgets import HistoryLineEdit"),
    ("run_async", "from .widgets import run_async"),
]


def theme_names() -> list[str]:
    path = os.path.join(UI_DIR, "theme.py")
    tree = ast.parse(io.open(path, encoding="utf-8").read())
    names = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name):
            name = node.targets[0].id
            if not name.startswith("_"):
                names.append(name)
    return names


def used_names(body: str) -> set[str]:
    tree = ast.parse(body)
    return {n.id for n in ast.walk(tree) if isinstance(n, ast.Name)}


def group(lines: list[str]) -> list[str]:
    """Merge ``from X import a`` / ``from X import b`` into one line."""

    merged: dict[str, list[str]] = {}
    plain: list[str] = []
    for line in lines:
        if line.startswith("from "):
            module, _, name = line.partition(" import ")
            merged.setdefault(module, []).append(name)
        else:
            plain.append(line)
    out = list(plain)
    for module, names in merged.items():
        out.append(f"{module} import {', '.join(names)}")
    return out


def tidy(stem: str, theme: list[str]) -> bool:
    path = os.path.join(UI_DIR, f"{stem}.py")
    text = io.open(path, encoding="utf-8").read()
    marker = "\n\nclass "
    head, sep, tail = text.partition(marker)
    if not sep:
        print("  ! no class in", path, file=sys.stderr)
        return False

    doc_end = head.index('"""', head.index('"""') + 3) + 3
    docstring = head[:doc_end]
    body = sep + tail

    used = used_names(body)
    keep = [line for name, line in PROVIDERS if name in used]
    theme_used = [n for n in theme if n in used]

    blocks = []
    std = [l for l in keep if l.startswith("import ") or
           l.startswith("from collections") or l.startswith("from datetime") or
           l.startswith("from pathlib") or l.startswith("from typing")]
    qt = [l for l in keep if l.startswith("from PySide6")]
    local = [l for l in keep if l.startswith("from ..") or l.startswith("from .")]
    if theme_used:
        local.append("from .theme import " + ", ".join(theme_used))
    for block in (std, qt, local):
        if block:
            blocks.append("\n".join(sorted(set(group(block)))))

    new = (docstring + "\n\nfrom __future__ import annotations\n\n"
           + "\n\n".join(blocks) + "\n" + body)
    if new == text:
        return False
    io.open(path, "w", encoding="utf-8", newline="\n").write(new)
    return True


def main() -> int:
    theme = theme_names()
    changed = 0
    for stem in GENERATED:
        if tidy(stem, theme):
            print("  tidied", stem)
            changed += 1
    print(f"{changed} module(s) rewritten")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
