"""Test package for the gateway bridges.

The runtime modules sit flat at the repo root because that is how they are
deployed to the Pi -- they import each other by bare name and the installer
copies them into one directory. Tests live in here for tidiness, so the root
has to be on sys.path for those bare imports to resolve.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
