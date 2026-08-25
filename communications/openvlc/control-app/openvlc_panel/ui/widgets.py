"""Small Qt helpers shared by the tabs.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

import threading
from typing import Callable

from PySide6 import QtCore, QtWidgets


class _Signals(QtCore.QObject):
    done = QtCore.Signal(object, object)  # (result, error)


# Keeps each _Signals alive until its queued connection has fired.
# Without it the object is collected mid-flight and the callback never
# runs -- which is every SSH action in the panel.
_PENDING: set = set()


def run_async(fn: Callable[[], object], on_done: Callable[[object, object], None]) -> None:
    sig = _Signals()
    _PENDING.add(sig)

    def _wrapped(result, error):
        _PENDING.discard(sig)
        on_done(result, error)

    sig.done.connect(_wrapped, QtCore.Qt.QueuedConnection)

    def worker():
        try:
            sig.done.emit(fn(), None)
        except Exception as exc:  # noqa: BLE001
            sig.done.emit(None, exc)

    threading.Thread(target=worker, daemon=True).start()


class LogBridge(QtCore.QObject):
    line = QtCore.Signal(str)
    error = QtCore.Signal(str)


class HistoryLineEdit(QtWidgets.QLineEdit):
    """Command input with bash-style Up/Down history recall."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._history: list[str] = []
        self._idx = 0

    def push(self, text: str) -> None:
        text = text.strip()
        if text and (not self._history or self._history[-1] != text):
            self._history.append(text)
        self._idx = len(self._history)

    def keyPressEvent(self, e):  # noqa: N802 (Qt override)
        if e.key() == QtCore.Qt.Key_Up and self._history:
            self._idx = max(0, self._idx - 1)
            self.setText(self._history[self._idx])
            return
        if e.key() == QtCore.Qt.Key_Down and self._history:
            self._idx = min(len(self._history), self._idx + 1)
            self.setText("" if self._idx >= len(self._history)
                         else self._history[self._idx])
            return
        super().keyPressEvent(e)
