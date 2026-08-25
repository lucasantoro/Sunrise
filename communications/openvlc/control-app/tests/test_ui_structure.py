"""The built UI must not change shape while it is being refactored.

Splitting ``app.py`` moves thousands of lines between modules. This constructs
the real window headless and compares its widget fingerprint against
``ui_baseline.txt``, which was captured before the split. Any widget that
appears, disappears or changes its visible text fails here.

Regenerate the baseline only when a change to the UI is intended:

    QT_QPA_PLATFORM=offscreen python tests/ui_snapshot.py > tests/ui_baseline.txt
"""

from __future__ import annotations

import os
import unittest

import ui_snapshot

BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "ui_baseline.txt")


class TestUiStructure(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app, cls.window = ui_snapshot.build_window()
        cls.rows = ui_snapshot.describe(cls.window)

    def _baseline(self) -> list[str]:
        with open(BASELINE, encoding="utf-8") as fh:
            return [line.rstrip("\n") for line in fh if line.strip()]

    def test_window_builds(self):
        self.assertIsNotNone(self.window)

    def test_tabs_unchanged(self):
        current = [r for r in self.rows if r.startswith("TAB\t")]
        expected = [r for r in self._baseline() if r.startswith("TAB\t")]
        self.assertEqual(current, expected)

    def test_widget_fingerprint_unchanged(self):
        current = self.rows
        expected = self._baseline()
        missing = [r for r in expected if r not in current]
        added = [r for r in current if r not in expected]
        self.assertEqual(
            (missing, added), ([], []),
            msg=(f"\n{len(missing)} widget(s) missing, {len(added)} added.\n"
                 f"missing: {missing[:10]}\nadded:   {added[:10]}"),
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
