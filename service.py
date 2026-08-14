#!/usr/bin/env python3
"""Install, update or remove this service — on Linux, Windows or a Raspberry Pi.

    ./service.py install       build if needed, install, start
    ./service.py update        rebuild, replace the binary, restart
    ./service.py uninstall     deregister, keeping the application directory
    ./service.py status        what the system says about it

Replaces install-service.sh, install-service.ps1, update-service.sh,
update-service.ps1, uninstall-service.sh and uninstall-service.ps1. The four
steps are the same everywhere and live in one place; only the service manager
differs, and that is the one thing each platform backend describes.

What this service is -- its name, its directory, its configurations -- is
declared in service.json, next to this file.

The orchestration is vendored under third_party/morf/morfdeploy, so a clone of
this project alone remains installable without fetching anything else.
Resynchronise it with scripts/sync-morf.sh; `morf doctor` reports drift.
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "third_party" / "morf"))

try:
    from morfdeploy.cli import main
except ImportError as exc:  # pragma: no cover - only when the copy is missing
    print(
        f"Cannot load the vendored deployment core: {exc}\n"
        f"Expected under {HERE / 'third_party' / 'morf' / 'morfdeploy'}.\n"
        "Restore it with:  ./scripts/sync-morf.sh",
        file=sys.stderr,
    )
    raise SystemExit(2) from exc

if __name__ == "__main__":
    # The repository root is this file's directory, so the command works from
    # anywhere -- including from sudo, whose working directory is not
    # necessarily the one the person was standing in.
    sys.exit(main([*sys.argv[1:], "--repo", str(HERE)]))
