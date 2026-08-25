"""UI layer of the control panel, one module per concern.

``app.py`` used to be a single 3488-line ``MainWindow`` holding every tab, its
handlers, the log monitor, the iperf runner and the video pipeline. Nothing
could be read, reviewed or tested in isolation.

The decomposition here is by **mixin**: each module owns one concern's methods
on a mixin class, and ``MainWindow`` inherits them all. At runtime it is still
one object, so every ``self.`` reference works exactly as before -- which is
what made the split verifiable by ``tests/test_ui_structure.py`` (it builds the
real window headless and compares the widget fingerprint) rather than by
reading three thousand lines and hoping.

That choice is deliberate and it is a first step, not the destination. Mixins
make the concerns *visible* while leaving the shared state they reach through
(``self._series``, ``self._latest``, ``self._streamer``, the widget attributes)
implicit. Turning that state into an explicit object passed to each tab is the
natural next move, and it is far easier now that the readers and writers are no
longer spread through one file. Doing both at once would have produced a change
no test could check.

Layering rule: modules here may import from the package root (``ssh``,
``config``, ``stats``, ``diagnostics``) but never from each other, except for
``theme``, ``deps``, ``text`` and ``widgets``, which hold no state.

    theme.py          constants, video presets, stylesheet
    deps.py           optional third-party imports (pyqtgraph, python-vlc)
    text.py           pure string/number helpers, no Qt
    widgets.py        small shared Qt helpers

    chrome.py         header, page intros, buttons, status line
    tab_*.py          one module per tab
    monitor.py        log streaming and per-line dispatch
    services.py       remote systemd control and the SSH command helpers
    iperf.py          performance tests
    video_session.py  relay, embedded player, TX camera

The tab and subsystem modules are generated from app.py by
``tools/split_app.py``; this file and the leaf modules above are hand-written.
"""

from __future__ import annotations

from .chrome import ChromeMixin
from .iperf import IperfMixin
from .monitor import MonitorMixin
from .services import ServicesMixin
from .tab_agc import AgcTabMixin
from .tab_console import ConsoleTabMixin
from .tab_control import ControlTabMixin
from .tab_dashboard import DashboardTabMixin
from .tab_diagnostics import DiagnosticsTabMixin
from .tab_settings import SettingsTabMixin
from .tab_video import VideoTabMixin
from .video_session import VideoSessionMixin

#: Every mixin MainWindow is built from, in MRO order.
PANEL_MIXINS = (
    ChromeMixin,
    ControlTabMixin,
    DashboardTabMixin,
    AgcTabMixin,
    VideoTabMixin,
    DiagnosticsTabMixin,
    ConsoleTabMixin,
    SettingsTabMixin,
    MonitorMixin,
    ServicesMixin,
    IperfMixin,
    VideoSessionMixin,
)

__all__ = ["PANEL_MIXINS"] + [m.__name__ for m in PANEL_MIXINS]
