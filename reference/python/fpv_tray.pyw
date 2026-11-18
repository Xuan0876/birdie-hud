"""FPV overlay as a resident tray app (no console). Double-click to start; right-click the tray icon for the menu.
Log: %APPDATA%\\fpv_overlay\\overlay.log. For a console, run overlay.bat instead."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
sys.path.insert(0, HERE)
sys.argv = [os.path.join(HERE, "overlay.py")] + sys.argv[1:]

import overlay  # noqa: E402

overlay.main()
