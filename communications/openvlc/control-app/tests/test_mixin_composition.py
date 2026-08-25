"""Invariants the mixin split must keep.

Two things can silently break when methods move between ui/ modules: a method
can be lost, or two mixins can end up defining the same name and one wins by
MRO accident. Neither shows up in the UI fingerprint if the affected code path
is not exercised at window build, so they are checked directly.
"""

from __future__ import annotations

import os
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import ui_snapshot  # noqa: F401,E402  (sets sys.path and the Qt platform)

from openvlc_panel.app import MainWindow  # noqa: E402
from openvlc_panel.ui import PANEL_MIXINS  # noqa: E402


# PySide6 injects these onto every QObject subclass; they are not ours.
_QT_INJECTED = {"staticMetaObject"}


def _own_names(cls) -> set[str]:
    return {k for k in vars(cls)
            if not k.startswith("__") and k not in _QT_INJECTED}


class TestMixinComposition(unittest.TestCase):
    def test_no_name_defined_by_two_mixins(self):
        seen: dict[str, str] = {}
        clashes: dict[str, list[str]] = {}
        for mixin in PANEL_MIXINS:
            for name in _own_names(mixin):
                if name in seen:
                    clashes.setdefault(name, [seen[name]]).append(mixin.__name__)
                seen[name] = mixin.__name__
        self.assertEqual(clashes, {},
                         msg="two mixins define the same name; MRO decides "
                             "which one runs, which is never what you want")

    def test_window_keeps_only_assembly(self):
        # MainWindow is meant to hold construction and teardown, nothing else.
        self.assertEqual(_own_names(MainWindow), {"closeEvent"})

    def test_every_mixin_name_reaches_the_window(self):
        for mixin in PANEL_MIXINS:
            for name in _own_names(mixin):
                self.assertTrue(hasattr(MainWindow, name),
                                msg=f"{mixin.__name__}.{name} is unreachable")

    def test_mixins_precede_qmainwindow(self):
        from PySide6 import QtWidgets
        mro = MainWindow.__mro__
        qt_index = mro.index(QtWidgets.QMainWindow)
        for mixin in PANEL_MIXINS:
            self.assertLess(mro.index(mixin), qt_index,
                            msg=f"{mixin.__name__} must override Qt, not the "
                                f"other way round")

    def test_every_tab_builder_is_owned_by_a_mixin(self):
        builders = [n for n in dir(MainWindow) if n.startswith("_build_")]
        self.assertTrue(builders)
        owned = {n for m in PANEL_MIXINS for n in _own_names(m)}
        for builder in builders:
            self.assertIn(builder, owned,
                          msg=f"{builder} drifted back onto MainWindow")


if __name__ == "__main__":
    unittest.main(verbosity=2)
