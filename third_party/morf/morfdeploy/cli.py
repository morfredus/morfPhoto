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
        choices=("install", "update", "uninstall", "status", "is-installed",
                 "config", "purge"),
        help="What to do",
    )
    parser.add_argument(
        "params",
        nargs="*",
        metavar="ARG",
        help="config: 'merge' (default, add this version's new keys, keep yours) "
             "or 'push' (replace the deployed config from the repo; needs --force). "
             "purge: one or more category ids declared in service.json",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        dest="all_categories",
        help="purge: every declared category (mutually exclusive with naming ids)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="purge/uninstall: show what would be removed without removing anything",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        dest="list_categories",
        help="purge: print the declared categories as JSON and exit "
             "(discovery for an orchestrator; removes nothing)",
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
             "config push: confirm replacing the deployed config. "
             "purge: erase even while the service is running (guard override)",
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

    # Options that belong to a single action are refused elsewhere rather than
    # silently ignored: an option the person typed and that did nothing is how
    # they end up trusting an effect that never happened.
    if (args.all_categories or args.list_categories) and args.action != "purge":
        print("--all and --list only apply to 'purge'.", file=sys.stderr)
        return 2
    if args.dry_run and args.action not in ("purge", "uninstall"):
        print("--dry-run only applies to 'purge' and 'uninstall'.", file=sys.stderr)
        return 2

    # `config` takes at most one positional, and only merge/push. Validated here
    # now that the positional is the generic `params` shared with purge.
    if args.action == "config":
        if len(args.params) > 1 or (args.params and args.params[0] not in ("merge", "push")):
            print("config takes one optional mode: 'merge' (default) or 'push'.",
                  file=sys.stderr)
            return 2
    elif args.action != "purge" and args.params:
        print(f"'{args.action}' takes no positional arguments.", file=sys.stderr)
        return 2

    # purge's ambiguities are refused before any manifest is even read: --all
    # with a list of ids contradicts itself, and neither one leaves nothing to
    # do. The unknown-category check needs the manifest, so it lives in the core.
    # `--list` is a query, exempt from all of it: it names nothing and removes
    # nothing, so combining it with ids, --all or --dry-run is contradictory.
    if args.action == "purge":
        if args.list_categories and (args.params or args.all_categories or args.dry_run):
            print("purge --list is a query: it takes no ids, --all or --dry-run.",
                  file=sys.stderr)
            return 2
        if not args.list_categories:
            if args.all_categories and args.params:
                print("purge takes either category ids or --all, not both.",
                      file=sys.stderr)
                return 2
            if not args.all_categories and not args.params:
                print("purge needs at least one category id, or --all.",
                      file=sys.stderr)
                return 2

    try:
        manifest = Manifest.load(args.repo.resolve())
    except ManifestError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    # Discovery is answered from the manifest alone: no backend, no privileges,
    # no platform support needed. An orchestrator asks this of every clone to
    # learn what each one can erase, so it must succeed even where the service
    # could never run -- returning an empty list, never an error.
    if args.action == "purge" and args.list_categories:
        import json
        catalog = {
            "service": manifest.service_name,
            "display_name": manifest.display_name,
            "categories": [
                {"id": c.id, "label": c.label,
                 "destructive": c.destructive, "type": c.kind}
                for c in manifest.purge_categories
            ],
        }
        # ASCII-safe on purpose: this is a machine interface consumed by an
        # orchestrator over a pipe. A child process on Windows encodes its stdout
        # in the locale code page (cp1252), not UTF-8, so a label with an accent
        # would reach the reader as mojibake or a decode error. Escaped \uXXXX is
        # pure ASCII, survives any code page, and json.loads restores the accents.
        print(json.dumps(catalog, ensure_ascii=True))
        return 0

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
            mode = args.params[0] if args.params else "merge"
            deployer.config(mode=mode, force=args.force)
        elif args.action == "purge":
            deployer.purge(ids=args.params, purge_all=args.all_categories,
                           dry_run=args.dry_run, force=args.force)
        elif args.action == "uninstall":
            if args.backup is not None and not args.purge:
                print("--backup only applies with --purge "
                      "(without --purge the configuration is kept anyway).",
                      file=sys.stderr)
                return 2
            deployer.uninstall(purge=args.purge, backup_dir=args.backup,
                               dry_run=args.dry_run)
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
