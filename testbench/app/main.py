"""Entrypoint for the Sonitude Audio Algorithm Test Bench GUI."""

from __future__ import annotations

import logging
import sys

from PySide6.QtCore import QCoreApplication
from PySide6.QtWidgets import QApplication

from app.ui.layout_persist import APP, ORG
from app.ui.main_window import MainWindow


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    QCoreApplication.setOrganizationName(ORG)
    QCoreApplication.setApplicationName(APP)
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
