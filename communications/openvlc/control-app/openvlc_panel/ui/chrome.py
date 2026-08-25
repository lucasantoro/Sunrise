"""Window chrome and the small widget helpers every tab uses.

Extracted from app.py by tools/split_app.py. See ui/__init__.py for why the
decomposition is by mixin.
"""

from __future__ import annotations

from PySide6 import QtCore, QtGui, QtWidgets

from .theme import APP_MARK


class ChromeMixin:
    """Window chrome and the small widget helpers every tab uses."""

    def _build_app_header(self) -> QtWidgets.QWidget:
        header = QtWidgets.QWidget()
        header.setObjectName("appHeader")
        header.setFixedHeight(82)
        lay = QtWidgets.QHBoxLayout(header)
        lay.setContentsMargins(22, 10, 22, 10)

        mark = QtWidgets.QLabel()
        mark.setFixedSize(58, 58)
        mark.setPixmap(QtGui.QIcon(str(APP_MARK)).pixmap(54, 54))
        lay.addWidget(mark)

        titles = QtWidgets.QVBoxLayout()
        titles.setSpacing(1)
        title = QtWidgets.QLabel("OpenVLC Control Panel")
        title.setObjectName("appTitle")
        subtitle = QtWidgets.QLabel("Visible-light link control, validation and live video")
        subtitle.setObjectName("appSubtitle")
        titles.addWidget(title)
        titles.addWidget(subtitle)
        lay.addLayout(titles)
        lay.addStretch(1)

        self.lbl_header_link = QtWidgets.QLabel("LINK  UNKNOWN")
        self.lbl_header_link.setAlignment(QtCore.Qt.AlignCenter)
        self.lbl_header_link.setMinimumWidth(132)
        self.lbl_header_link.setStyleSheet(
            "color:#C7D3E7; background:#263B5F; border-radius:14px;"
            "padding:7px 12px; font-size:9pt; font-weight:700;"
        )
        lay.addWidget(self.lbl_header_link)
        return header


    @staticmethod
    def _page_intro(title: str, subtitle: str) -> QtWidgets.QWidget:
        box = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(box)
        lay.setContentsMargins(0, 0, 0, 3)
        lay.setSpacing(2)
        heading = QtWidgets.QLabel(title)
        heading.setObjectName("sectionTitle")
        detail = QtWidgets.QLabel(subtitle)
        detail.setObjectName("sectionSubtitle")
        detail.setWordWrap(True)
        lay.addWidget(heading)
        lay.addWidget(detail)
        return box


    @staticmethod
    def _set_button_role(button: QtWidgets.QPushButton, role: str) -> None:
        button.setProperty("role", role)


    def _log_header(self, title: str,
                    edit: QtWidgets.QPlainTextEdit) -> QtWidgets.QHBoxLayout:
        """A label + Copy/Clear controls row shared by every log console."""
        row = QtWidgets.QHBoxLayout()
        row.addWidget(QtWidgets.QLabel(title))
        row.addStretch(1)
        btn_copy = QtWidgets.QPushButton("Copy")
        self._set_button_role(btn_copy, "quiet")
        btn_copy.setToolTip("Copy the whole log to the clipboard")
        btn_copy.clicked.connect(
            lambda: QtWidgets.QApplication.clipboard().setText(
                edit.toPlainText()))
        btn_clear = QtWidgets.QPushButton("Clear")
        self._set_button_role(btn_clear, "quiet")
        btn_clear.setToolTip("Clear this console")
        btn_clear.clicked.connect(edit.clear)
        row.addWidget(btn_copy)
        row.addWidget(btn_clear)
        return row


    def _spin(self, lo: int, hi: int, value: int) -> QtWidgets.QSpinBox:
        box = QtWidgets.QSpinBox()
        box.setRange(lo, hi)
        box.setValue(int(value))
        return box


    def _status(self, text: str, err: bool = False):
        self.statusBar().showMessage(text)


    def _control_log(self, text: str):
        if hasattr(self, "txt_control"):
            self.txt_control.appendPlainText(text)
        if hasattr(self, "txt_diagnostics"):
            self.txt_diagnostics.appendPlainText(text)
        self._status(text.splitlines()[0][:100] if text else "")
