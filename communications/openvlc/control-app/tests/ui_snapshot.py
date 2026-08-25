"""Canonical fingerprint of the built UI, for refactor regression checks.

PySide6 runs headless under ``QT_QPA_PLATFORM=offscreen``, so the whole window
can be constructed in a test. This walks the widget tree and emits a stable
text form: class name, object name, and the user-visible text of the widgets
that carry any. Layout geometry is deliberately excluded — it depends on the
platform theme and would make the fingerprint noisy.

Used by ``test_ui_structure.py`` and runnable directly to regenerate the
baseline:

    QT_QPA_PLATFORM=offscreen python tests/ui_snapshot.py > tests/ui_baseline.txt
"""

from __future__ import annotations

import os
import sys
import tempfile

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PySide6 import QtWidgets  # noqa: E402

# Widgets whose visible text is part of the contract we want to preserve.
_TEXT_ROLES = (
    QtWidgets.QPushButton,
    QtWidgets.QLabel,
    QtWidgets.QCheckBox,
    QtWidgets.QRadioButton,
    QtWidgets.QGroupBox,
    QtWidgets.QLineEdit,
)


def _text_of(widget: QtWidgets.QWidget) -> str:
    if isinstance(widget, QtWidgets.QLineEdit):
        # Placeholders are part of the UI; live values are not.
        return widget.placeholderText()
    if isinstance(widget, _TEXT_ROLES):
        return widget.text() if hasattr(widget, "text") else widget.title()
    if isinstance(widget, QtWidgets.QComboBox):
        return "|".join(widget.itemText(i) for i in range(widget.count()))
    return ""


def describe(window: QtWidgets.QWidget) -> list[str]:
    """One sorted line per widget: ``Class\tobjectName\ttext``.

    Sorted rather than tree-ordered so that moving a builder into another module
    — which can change construction order — does not register as a change. What
    must not change is the *set* of widgets and their text.
    """

    rows = []
    for widget in window.findChildren(QtWidgets.QWidget):
        rows.append("\t".join((
            type(widget).__name__,
            widget.objectName(),
            _text_of(widget).replace("\n", " ").replace("\t", " ").strip(),
        )))
    tabs = window.findChildren(QtWidgets.QTabWidget)
    for tab in tabs:
        for index in range(tab.count()):
            rows.append(f"TAB\t{index}\t{tab.tabText(index)}")
    return sorted(rows)


def build_window():
    """Build MainWindow against DEFAULT settings, never the user's.

    The window text depends on the configuration -- a button reads "Run 20 s"
    or "Run 10 s" depending on the saved iperf duration. Loading
    ~/.openvlc_panel.json made this fingerprint break whenever anyone changed a
    setting at the bench, which is not a regression and is not something a test
    should notice. Pointing CONFIG_PATH at a path that does not exist makes
    Config.load() return defaults, so the baseline is reproducible on any
    machine -- and guarantees the suite can never write over real settings.
    """

    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])

    from openvlc_panel import config as config_mod
    original = config_mod.CONFIG_PATH
    config_mod.CONFIG_PATH = os.path.join(
        tempfile.gettempdir(), "openvlc_panel_test_config_does_not_exist.json")
    try:
        from openvlc_panel.app import MainWindow
        window = MainWindow()
    finally:
        config_mod.CONFIG_PATH = original
    return app, window


def main() -> int:
    # The UI contains non-ASCII (arrows, symbols) and the Windows console
    # default codepage cannot encode it; force UTF-8 on the stream itself so
    # the baseline is byte-identical wherever it is regenerated.
    sys.stdout.reconfigure(encoding="utf-8")
    _, window = build_window()
    for row in describe(window):
        print(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
