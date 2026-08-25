"""The entry point must actually start.

The window fingerprint test builds ``MainWindow`` directly, so it never touched
``main()`` -- and ``main()`` was where the split left a missing ``QtGui``
import. Anything reached only on the way in (splash screen, icons, stylesheet)
needs its own check.

Two layers here, because they fail differently:

* ``test_main_runs`` executes the real ``main()`` with the event loop stubbed,
  which catches undefined names and bad Qt calls on the startup path.
* ``test_no_undefined_names`` runs pyflakes over the package. That is the check
  that would have caught all three post-split defects at once, including the
  ones on code paths no test exercises.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import ui_snapshot  # noqa: F401,E402  (sets sys.path and the Qt platform)

PKG_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class TestStartup(unittest.TestCase):
    def test_main_runs(self):
        """Start the real entry point in a fresh process.

        Run in-process it collides with the QApplication another test already
        created, and stubbing that away would stop testing the thing that
        broke. A subprocess starts it exactly the way run.py does.
        """

        driver = (
            "import os; os.environ['QT_QPA_PLATFORM']='offscreen';"
            "from PySide6 import QtWidgets;"
            "QtWidgets.QApplication.exec = lambda self: 0;"
            "from openvlc_panel.app import main; main();"
            "print('STARTED')"
        )
        result = subprocess.run(
            [sys.executable, "-c", driver],
            cwd=PKG_ROOT, capture_output=True, text=True, timeout=120,
        )
        self.assertIn("STARTED", result.stdout,
                      msg=f"exit={result.returncode}{chr(10)}{result.stderr}")

    def test_no_undefined_names(self):
        result = subprocess.run(
            [sys.executable, "-m", "pyflakes", "openvlc_panel", "run.py"],
            cwd=PKG_ROOT, capture_output=True, text=True,
        )
        undefined = [line for line in result.stdout.splitlines()
                     if "undefined name" in line]
        self.assertEqual(undefined, [], msg="\n" + "\n".join(undefined))


if __name__ == "__main__":
    unittest.main(verbosity=2)
