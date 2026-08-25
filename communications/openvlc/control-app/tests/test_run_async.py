"""``run_async`` is the path every SSH action in the panel takes.

The split dropped its module-level ``_PENDING`` set, which is what keeps the
``_Signals`` object alive until its queued connection fires. Nothing caught it:
the name is only touched when an async job is actually started, and no test
started one. So one does now.
"""

from __future__ import annotations

import os
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import ui_snapshot  # noqa: F401,E402  (sets sys.path and the Qt platform)

from PySide6 import QtCore, QtWidgets  # noqa: E402

from openvlc_panel.ui import widgets  # noqa: E402


def _pump(predicate, timeout_ms: int = 4000) -> bool:
    """Run the event loop until ``predicate`` holds or the timeout expires."""

    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    deadline = QtCore.QElapsedTimer()
    deadline.start()
    while not predicate() and deadline.elapsed() < timeout_ms:
        app.processEvents(QtCore.QEventLoop.AllEvents, 20)
    return predicate()


class TestRunAsync(unittest.TestCase):
    def setUp(self):
        QtWidgets.QApplication.instance() or QtWidgets.QApplication([])

    def test_result_reaches_the_callback(self):
        got = []
        widgets.run_async(lambda: 21 * 2, lambda r, e: got.append((r, e)))
        self.assertTrue(_pump(lambda: bool(got)), "callback never ran")
        self.assertEqual(got, [(42, None)])

    def test_exception_reaches_the_callback(self):
        got = []

        def boom():
            raise RuntimeError("bench on fire")

        widgets.run_async(boom, lambda r, e: got.append((r, e)))
        self.assertTrue(_pump(lambda: bool(got)), "callback never ran")
        result, error = got[0]
        self.assertIsNone(result)
        self.assertIsInstance(error, RuntimeError)
        self.assertEqual(str(error), "bench on fire")

    def test_pending_set_drains(self):
        # A leak here would grow without bound over a long session.
        widgets._PENDING.clear()
        done = []
        widgets.run_async(lambda: None, lambda r, e: done.append(True))
        self.assertTrue(_pump(lambda: bool(done)))
        self.assertTrue(_pump(lambda: not widgets._PENDING),
                        msg=f"_PENDING still holds {len(widgets._PENDING)}")

    def test_many_concurrent_jobs_all_complete(self):
        results = []
        for value in range(10):
            widgets.run_async(lambda v=value: v,
                              lambda r, e: results.append(r))
        self.assertTrue(_pump(lambda: len(results) == 10),
                        msg=f"only {len(results)}/10 callbacks ran")
        self.assertEqual(sorted(results), list(range(10)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
