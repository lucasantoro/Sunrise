"""Optional third-party dependencies, probed once.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

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


