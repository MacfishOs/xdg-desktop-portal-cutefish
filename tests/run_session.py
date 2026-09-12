#!/usr/bin/env python3
"""Isolate paths inherited by D-Bus-activated helpers as well as test processes."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="cutefish-portal-session-") as directory:
    environment = os.environ.copy()
    for variable, name in (("XDG_CONFIG_HOME", "config"),
                           ("XDG_DATA_HOME", "data"),
                           ("XDG_CACHE_HOME", "cache"),
                           ("XDG_STATE_HOME", "state"),
                           ("XDG_RUNTIME_DIR", "runtime")):
        path = Path(directory) / name
        path.mkdir(mode=0o700)
        environment[variable] = str(path)
    try:
        result = subprocess.run([sys.argv[1], "--", *sys.argv[2:]], env=environment)
    finally:
        # The frontend's document helper may leave a FUSE mount after the bus exits.
        unmount = shutil.which("fusermount3") or shutil.which("fusermount")
        if unmount:
            subprocess.run([unmount, "-u", str(Path(directory) / "runtime" / "doc")],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

sys.exit(result.returncode)
