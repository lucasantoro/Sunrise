"""One-shot refactor: split openvlc_panel/app.py into ui/ mixin modules.

MainWindow held every tab in one 3488-line class. Splitting it by moving code
between modules is mechanical and easy to get subtly wrong by hand, so it is
done by AST extraction instead: exact line ranges, no reflowing, no retyping.

The decomposition is by mixin. Each module holds one concern's methods on a
mixin class and MainWindow inherits them, so every ``self.`` reference keeps
working unchanged and the change is verifiable by the UI fingerprint test
rather than by reading. Turning the shared state those mixins reach through
into an explicit object is the next step, and is deliberately NOT done here:
you cannot sensibly extract dependencies you cannot see, and until this split
they were buried in one file.

Run from the control-app directory:

    .venv/Scripts/python.exe tools/split_app.py
"""

from __future__ import annotations

import ast
import io
import os
import sys

APP = os.path.join("openvlc_panel", "app.py")
UI_DIR = os.path.join("openvlc_panel", "ui")

# --------------------------------------------------------------- the plan
#
# Each entry is (module, mixin class name, docstring, [method names]).
# Anything not listed stays on MainWindow in app.py.

PLAN = [
    ("chrome", "ChromeMixin",
     "Window chrome and the small widget helpers every tab uses.",
     ["_build_app_header", "_page_intro", "_set_button_role", "_log_header",
      "_spin", "_status", "_control_log"]),

    ("tab_control", "ControlTabMixin",
     "Control tab: verify, prepare the link, run a performance test.",
     ["_build_control"]),

    ("tab_dashboard", "DashboardTabMixin",
     "Link Quality tab: live channel health, summary cards and charts.",
     ["_build_dashboard", "_build_link_summary", "_push", "_readout_text",
      "_set_summary_card", "_metric_color", "_metric", "_fresh_metric_text",
      "_active_rate_text", "_refresh_summary_panels", "_refresh_charts",
      "_refresh_link_health", "_set_link", "_counter_delta",
      "_record_quality_sample", "_quality_stat", "_quality_summary",
      "_read", "_clear_read", "_mark_stale_metrics"]),

    ("tab_agc", "AgcTabMixin",
     "AGC tuning tab: threshold sweep capture and export.",
     ["_build_agc", "_agc_toggle", "_agc_ingest", "_agc_render", "_agc_clear",
      "_agc_save_csv"]),

    ("tab_video", "VideoTabMixin",
     "Video tab layout, camera routing and presets.",
     ["_build_video", "_video_route", "_current_video_route",
      "_video_camera_route_changed", "_set_advanced_video_visible",
      "_apply_video_preset", "_apply_video_capacity_hint"]),

    ("tab_diagnostics", "DiagnosticsTabMixin",
     "Diagnostics tab: preflight checks and report collection.",
     ["_build_diagnostics", "_run_preflight", "_collect_diagnostics"]),

    ("tab_console", "ConsoleTabMixin",
     "Console tab: pick a node, send one command.",
     ["_CONSOLE_PRESETS", "_build_console", "_console_fill", "_nodes",
      "_refresh_console_targets", "_console_device", "_console_send"]),

    ("tab_settings", "SettingsTabMixin",
     "Settings tab: endpoints, paths, ports and validated profiles.",
     ["_build_settings", "_save_settings", "_test_connections",
      "_probe_device", "_local_summary", "_device_summary",
      "_update_budget_button"]),

    ("monitor", "MonitorMixin",
     "Log streaming: start/stop a source and dispatch each parsed line.",
     ["_MONITOR_UNITS", "_toggle_monitor", "_stop_monitor", "_monitor_sources",
      "_refresh_monitor_sources", "_start_monitor_if_needed", "_monitor_trx",
      "_sync_monitor_buttons", "_on_log_error", "_on_log_line"]),

    ("services", "ServicesMixin",
     "Remote service control and the two SSH command helpers.",
     ["_bridge_ctl", "_trx_ctl", "_trx_ctl_all", "_setup_tx_pi",
      "_start_bbb_tx", "_do", "_sudo_do"]),

    ("iperf", "IperfMixin",
     "Performance tests: plan the flows, run them, stop them.",
     ["_iperf_plan", "_iperf_cleanup_command", "_unique_devices",
      "_set_iperf_running", "_stop_iperf", "_run_iperf",
      "_update_iperf_button"]),

    ("video_session", "VideoSessionMixin",
     "Video pipeline: relay, embedded player and the TX camera.",
     ["_record_fetch", "_start_live", "_start_video_session",
      "_after_live_started", "_after_tx_video_started", "_toggle_video_session",
      "_stop_video_session", "_set_video_state", "_set_preview_status",
      "_play_embedded", "_stop_embedded", "_stop_live", "_local_player_state",
      "_check_video_pipeline", "_start_tx_video", "_stop_tx_video",
      "_launch_vlc", "_stop_vlc"]),
]

# Module-level code that moves out of app.py wholesale.
THEME_NAMES = [
    "POLL_SECONDS", "HISTORY", "VALID_BUDGETS", "VIDEO_PRESETS", "RELAY_PID",
    "RELAY_LOG", "TX_VIDEO_PID", "TX_VIDEO_LOG", "IPERF_SERVER_PID",
    "IPERF_SERVER_LOG", "IPERF_CLIENT_PID", "IPERF_CLIENT_LOG",
    "IPERF_DIRECTIONS", "VIDEO_CAMERA_ROUTES", "AGC_FEATURE_READY",
    "ASSET_DIR", "APP_MARK", "APP_STYLE",
]
WIDGET_NAMES = ["_Signals", "_PENDING", "run_async", "LogBridge",
                "HistoryLineEdit"]
TEXT_NAMES = ["_remote_cd", "_env", "_rate_to_kbps", "_extract_iperf_kbps",
              "_extract_iperf_loss"]

HEADER = '''"""{doc}

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations
'''


def node_span(node, src_lines):
    """Line range including decorators, 0-based half-open."""

    start = node.lineno
    if getattr(node, "decorator_list", None):
        start = min(d.lineno for d in node.decorator_list)
    return start - 1, node.end_lineno


def collect_names(text):
    """Every bare name referenced in a chunk of source."""

    try:
        tree = ast.parse("class _T:\n" + "\n".join(
            "    " + line if line.strip() else line for line in text.splitlines()))
    except SyntaxError:
        tree = ast.parse(text)
    return {n.id for n in ast.walk(tree) if isinstance(n, ast.Name)} | {
        n.attr for n in ast.walk(tree) if isinstance(n, ast.Attribute)}


def _anchor_asset_dir():
    """Re-anchor ASSET_DIR after the move into ui/.

    The original resolved it against its own file while living in the package
    root. Emitted verbatim into ui/theme.py it would point at
    openvlc_panel/ui/assets, which does not exist -- a silent regression that
    shows up only as a missing window icon.
    """

    path = os.path.join(UI_DIR, "theme.py")
    text = io.open(path, encoding="utf-8").read()
    old = 'ASSET_DIR = Path(__file__).resolve().parent / "assets"'
    new = ("# Anchored to the PACKAGE root, not this file: theme.py lives in ui/\n"
           "# while assets/ stayed beside __init__.py.\n"
           'ASSET_DIR = Path(__file__).resolve().parent.parent / "assets"')
    if old not in text:
        print("  ! ASSET_DIR not found in theme.py", file=sys.stderr)
        return
    io.open(path, "w", encoding="utf-8", newline="\n").write(
        text.replace(old, new, 1))
    print("  anchored ASSET_DIR to the package root")


def main() -> int:
    src = io.open(APP, encoding="utf-8").read()
    lines = src.splitlines(keepends=True)
    tree = ast.parse(src)

    main_cls = next(n for n in tree.body
                    if isinstance(n, ast.ClassDef) and n.name == "MainWindow")
    methods = {}
    for node in main_cls.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            methods[node.name] = node_span(node, lines)
        # Class-level attributes travel with the mixin that reads them.
        # Missing these on the first run cost a NameError at window build.
        elif isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name):
            methods[node.targets[0].id] = node_span(node, lines)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            methods[node.target.id] = node_span(node, lines)

    module_level = {}
    for node in tree.body:
        name = getattr(node, "name", None)
        if name is None and isinstance(node, ast.Assign):
            target = node.targets[0]
            name = target.id if isinstance(target, ast.Name) else None
        # Annotated assignments too: _PENDING is one, and missing it silently
        # broke every async action in the panel until pyflakes found it.
        if name is None and isinstance(node, ast.AnnAssign):
            name = node.target.id if isinstance(node.target, ast.Name) else None
        if name:
            module_level[name] = node_span(node, lines)

    planned = {m for _, _, _, names in PLAN for m in names}
    missing = sorted(planned - set(methods))
    if missing:
        print("methods in the plan that do not exist:", missing, file=sys.stderr)
        return 1

    os.makedirs(UI_DIR, exist_ok=True)
    taken: set[str] = set()

    def emit(fname, doc, names, source_map, extra_imports, cls=None):
        chunks = []
        for name in names:
            if name not in source_map:
                print("  ! missing", name, file=sys.stderr)
                continue
            a, b = source_map[name]
            chunks.append("".join(lines[a:b]).rstrip() + "\n")
            taken.add(name)
        body = "\n\n".join(chunks)
        out = [HEADER.format(doc=doc)]
        out.extend(extra_imports)
        out.append("\n\n")
        if cls:
            out.append(f"class {cls}:\n")
            out.append(f'    """{doc}"""\n\n')
            out.append(body)
        else:
            out.append(body)
        path = os.path.join(UI_DIR, fname)
        io.open(path, "w", encoding="utf-8", newline="\n").write("".join(out))
        print("  wrote", path, "(%d names)" % len(chunks))

    print("emitting ui/ modules:")

    emit("theme.py", "Constants, presets and the application stylesheet.",
         THEME_NAMES, module_level,
         ["\nimport os\nfrom pathlib import Path\n"])
    _anchor_asset_dir()
    emit("deps.py", "Optional third-party dependencies, probed once.", [],
         {}, ["""
try:
    import pyqtgraph as pg
    HAVE_PG = True
except Exception:  # noqa: BLE001
    pg = None
    HAVE_PG = False

try:
    import vlc  # python-vlc: embeds libvlc into a Qt widget
    HAVE_VLC = True
except Exception:  # noqa: BLE001
    vlc = None
    HAVE_VLC = False
"""])
    emit("widgets.py", "Small Qt helpers shared by the tabs.",
         WIDGET_NAMES, module_level,
         ["\nimport threading\n",
          "from typing import Callable\n\n",
          "from PySide6 import QtCore, QtWidgets\n"])
    emit("text.py", "String and number helpers used across the tabs. No Qt.",
         TEXT_NAMES, module_level,
         ["\nimport re\n", "from typing import Optional\n\n",
          "from .. import ssh\n"])

    common = [
        "\nimport os\n", "import re\n", "import subprocess\n",
        "import threading\n", "import time\n",
        "from collections import deque\n",
        "from datetime import datetime\n",
        "from pathlib import Path\n",
        "from typing import Callable, Optional\n\n",
        "from PySide6 import QtCore, QtGui, QtWidgets\n\n",
        "from .. import ssh\n",
        "from ..config import Config, Device\n",
        "from ..stats import parse_line\n",
        "from .deps import HAVE_PG, HAVE_VLC, pg, vlc\n",
        "from .text import (_remote_cd, _env, _rate_to_kbps,\n"
        "                   _extract_iperf_kbps, _extract_iperf_loss)\n",
        "from .theme import *  # noqa: F401,F403  (constants and presets)\n",
        "from .widgets import LogBridge, HistoryLineEdit, run_async\n",
    ]
    for fname, cls, doc, names in PLAN:
        emit(f"{fname}.py", doc, names, methods, common, cls=cls)

    leftover = sorted(set(methods) - taken)
    print("\nstaying on MainWindow:", leftover)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
