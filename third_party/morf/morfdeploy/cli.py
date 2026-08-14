"""One entry point, four actions, every platform.

Replaces install-service.sh, install-service.ps1, update-service.sh,
update-service.ps1, uninstall-service.sh and uninstall-service.ps1 in each
project. A project keeps a two-line wrapper so `./service.sh install` still
reads the way it always did.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .backends import select
from .core import DeployError, Deployer
from .manifest import Manifest, ManifestError


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="morfdeploy",
        description="Install, update or remove a morfSystem service on this machine.",
    )
    parser.add_argument(
        "action",
        choices=("install", "update", "uninstall", "status", "is-installed", "config"),
        help="What to do",
    )
    parser.add_argument(
        "mode",
        nargs="?",
        choices=("merge", "push"),
        default=None,
        help="config: 'merge' (default, add this version's new keys, keep yours) "
             "or 'push' (replace the deployed config from the repo; needs --force)",
    )
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path.cwd(),
        help="Project root holding service.json (default: current directory)",
    )
    parser.add_argument(
        "--rebuild",
        action="store_true",
        help="Rebuild even when a binary is already present (install only)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="update: redeploy and restart even when nothing changed. "
             "config push: confirm replacing the deployed config",
    )
    parser.add_argument(
        "--purge",
        action="store_true",
        help="uninstall: also remove the configuration and the binary directory",
    )
    parser.add_argument(
        "--backup",
        type=Path,
        metavar="DIR",
        default=None,
        help="uninstall --purge: copy the configuration into DIR before removing it",
    )
    return parser


def main(argv: list | None = None) -> int:
    args = build_parser().parse_args(argv)

    try:
        manifest = Manifest.load(args.repo.resolve())
    except ManifestError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    # `status` must work on an unsupported platform: refusing to even report
    # what is installed would be unhelpful where being honest is the point.
    # `is-installed` answers the same question, and an unsupported platform
    # hosts nothing: reporting "no" there is both true and useful.
    if args.action in ("status", "is-installed"):
        backend = select()
        if not backend.supported:
            if args.action == "is-installed":
                return 1
            print(backend.supported_note, file=sys.stderr)
            return 3

    try:
        deployer = Deployer(manifest)
        if args.action == "install":
            deployer.install(rebuild=args.rebuild)
        elif args.action == "update":
            deployer.update(force=args.force)
        elif args.action == "config":
            deployer.config(mode=args.mode or "merge", force=args.force)
        elif args.action == "uninstall":
            if args.backup is not None and not args.purge:
                print("--backup only applies with --purge "
                      "(without --purge the configuration is kept anyway).",
                      file=sys.stderr)
                return 2
            deployer.uninstall(purge=args.purge, backup_dir=args.backup)
        elif args.action == "is-installed":
            # Deliberately silent, and the only action whose exit status IS the
            # answer. A caller sweeping the parc needs a decision, not prose --
            # which is exactly why `status` output must never be parsed for it
            # (see backends/base.py).
            #
            #   0  registered with the system
            #   1  not registered
            #   2  cannot tell -- the caller lacks the rights to even ask
            #
            # The third answer matters: without it, a permission error reads as
            # "not installed", and a sweep skips a running service while
            # reporting success.
            if not deployer.backend.can_query_installation(manifest):
                return 2
            return 0 if deployer.backend.is_installed(manifest) else 1
        else:
            deployer.status()
    except DeployError as exc:
        print(f"\n{exc}", file=sys.stderr)
        return 1
    except NotImplementedError as exc:
        print(f"\n{exc}", file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 130

    return 0


if __name__ == "__main__":
    sys.exit(main())
