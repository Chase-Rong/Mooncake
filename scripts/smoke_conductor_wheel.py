#!/usr/bin/env python3
"""Check a release wheel's Conductor contents and isolated installed import."""

import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: smoke_conductor_wheel.py WHEEL")
    wheel = Path(sys.argv[1]).resolve()
    with zipfile.ZipFile(wheel) as archive:
        names = archive.namelist()
        if "mooncake/conductor.py" not in names or not any(
            name.startswith("mooncake/_conductor.") and name.endswith(".so")
            for name in names
        ):
            raise SystemExit(f"{wheel.name}: missing Conductor wrapper or extension")
    env = os.environ.copy()
    for key in ("PYTHONPATH", "PYTHONHOME", "LD_LIBRARY_PATH", "LD_PRELOAD"):
        env.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="conductor-wheel-smoke-") as temporary:
        subprocess.run(
            [sys.executable, "-I", "-m", "venv", temporary], env=env, check=True
        )
        python = str(Path(temporary) / "bin" / "python")
        subprocess.run(
            [
                python,
                "-I",
                "-m",
                "pip",
                "install",
                "--no-index",
                "--no-deps",
                str(wheel),
            ],
            env=env,
            check=True,
        )
        subprocess.run(
            [
                python,
                "-I",
                "-c",
                "import mooncake.conductor, mooncake._conductor; "
                "client = mooncake.conductor.ConductorClient(); "
                "assert client.health_check() == mooncake.conductor.HEALTH_NOT_INITIALIZED; "
                "assert client.close() == mooncake.conductor.OK; "
                "print(mooncake.conductor.__file__); print(mooncake._conductor.__file__)",
            ],
            cwd=temporary,
            env=env,
            check=True,
        )


if __name__ == "__main__":
    main()
