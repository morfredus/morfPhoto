"""What a project must declare to be installable.

Every install-service.sh in the parc carried the same four steps and differed
only by a service name and a directory. Those two values, plus the handful of
paths around them, are what this manifest holds -- so the algorithm can live in
one place and the project keeps stating its own facts.

The manifest is JSON for the same reason the shared parc configuration is:
neither C++ nor Python is privileged, and a project written in either declares
itself the same way.
"""

from __future__ import annotations

import json
import os
import platform
import sys
from dataclasses import dataclass, field
from pathlib import Path

MANIFEST_NAME = "service.json"

#: Single environment override for the install directory, across the whole
#: parc. The per-project prefixes it replaces (MT_, MN_, MS_) had already
#: drifted -- morfAnalytics carried morfMonitor's MT_APP_DIR verbatim, and
#: morfSync had none at all, so its documented override silently did nothing.
APP_DIR_ENV = "MORF_APP_DIR"

#: Still honoured, in the order written. Each was one project's own spelling of
#: the same idea, which is how morfAnalytics ended up reading morfMonitor's
#: MT_APP_DIR and morfSync documented an override nothing read.
LEGACY_APP_DIR_ENV = ("MORF_MONITOR_APP_DIR", "MT_APP_DIR", "MN_APP_DIR", "MS_APP_DIR")


class ManifestError(RuntimeError):
    """The manifest is missing, malformed, or incomplete."""


@dataclass(frozen=True)
class ConfigFile:
    """A configuration to place at install time.

    `overwrite` is false by default, and that default matters: these files hold
    settings edited by hand on the machine. An install that overwrites them
    destroys local state to deliver a default nobody asked for.
    """

    source: str
    dest: object          # str, or {"linux": ..., "windows": ...}
    overwrite: bool = False

    #: Places this configuration used to live. On install, the first one found
    #: is adopted rather than replaced by the example -- a settings file
    #: someone edited by hand outlives the directory convention it was written
    #: under. morfSync has already been moved once, from /etc/homeserverhub to
    #: /etc/morfsync, and that migration was hard-coded into its install script
    #: where nothing else could see it or learn from it.
    migrate_from: tuple = ()

    def find_predecessor(self) -> Path | None:
        """An earlier home of this configuration that still holds a file."""
        for candidate in self.migrate_from:
            path = Path(os.path.expandvars(candidate))
            if path.is_file():
                return path
        return None

    def resolved_dest(self, config_dir: Path) -> Path:
        """Absolute destinations are honoured as-is; relative ones sit in config_dir.

        The shared parc configuration lives outside any single application's
        directory -- it is read by morfMonitor and by morfDashboard -- so
        it must be expressible as an absolute path.

        And that path is not the same everywhere. `dest` may therefore be an
        object keyed by platform, exactly as `app_dir` is. A single string
        "/etc/morfsystem/morfsystem.json" would resolve on Windows to \\etc\\...
        on whatever drive happens to be current: a real directory, silently
        created, that nothing ever reads.
        """
        raw = self.dest
        if isinstance(raw, dict):
            key = {"Windows": "windows", "Darwin": "darwin"}.get(
                platform.system(), "linux")
            raw = raw.get(key) or raw.get("linux") or ""
            if not raw:
                raise ManifestError(
                    f"Configuration destination not declared for this platform: {self.source}")

        dest = Path(os.path.expandvars(raw))
        return dest if dest.is_absolute() else config_dir / dest


@dataclass(frozen=True)
class Manifest:
    """A project's deployment facts."""

    repo_root: Path
    service_name: str
    display_name: str
    binary: str
    app_dirs: dict = field(default_factory=dict)
    config_dirs: dict = field(default_factory=dict)
    state_dirs: dict = field(default_factory=dict)
    configs: tuple = ()
    description: str = ""
    status_url: str = ""

    #: Places an earlier convention installed this binary. Reported at install,
    #: never deleted: removing an executable nobody asked us to remove is not a
    #: tidy-up, and a stale copy left unmentioned is how someone ends up
    #: debugging the version they are not running.
    legacy_binaries: tuple = ()

    # -- Derived paths ----------------------------------------------------

    def app_dir(self) -> Path:
        """Where the binary and configurations are installed.

        The environment override wins over the manifest, which wins over a
        conventional default. Resolution order is fixed here rather than in
        each backend so that the answer cannot depend on the platform for any
        reason other than the path itself.
        """
        override = os.environ.get(APP_DIR_ENV)
        if override:
            return Path(override)

        # The prefixes MORF_APP_DIR replaces are still honoured, and say so.
        # Someone who wrote MT_APP_DIR in a note a year ago should get the
        # directory they asked for, not silently get the default while
        # believing otherwise -- which is what a straight rename would deliver.
        for legacy in LEGACY_APP_DIR_ENV:
            value = os.environ.get(legacy)
            if value:
                print(
                    f"[note] {legacy} is honoured but superseded by {APP_DIR_ENV}, "
                    f"which is the same variable for every service.",
                    file=sys.stderr,
                )
                return Path(value)

        key = {"Windows": "windows", "Darwin": "darwin"}.get(platform.system(), "linux")
        declared = self.app_dirs.get(key)
        if declared:
            return Path(os.path.expandvars(declared))

        if key == "windows":
            base = os.environ.get("ProgramData", r"C:\ProgramData")
            return Path(base) / self.service_name
        return Path("/opt") / self.service_name

    def config_dir(self) -> Path:
        """Where THIS service's configuration lives -- separate from its binary.

        /etc is where a Linux administrator looks first, and the FHS is explicit
        that host configuration belongs there rather than beside the program.
        Keeping it out of app_dir also means wiping /opt/<service> to reinstall
        cleanly no longer takes the settings with it.

        The parc previously put both in /opt, self-contained-bundle style. Only
        morfSync was doing it the conventional way, and it got normalised onto
        the twelve that were not -- the right goal, the wrong reference.
        """
        key = {"Windows": "windows", "Darwin": "darwin"}.get(platform.system(), "linux")
        declared = self.config_dirs.get(key)
        if declared:
            return Path(os.path.expandvars(declared))
        if key == "windows":
            base = os.environ.get("ProgramData", r"C:\ProgramData")
            return Path(base) / self.service_name
        return Path("/etc") / self.service_name

    def state_dir(self) -> Path:
        """Where THIS service's PERSISTENT STATE lives -- separate from both its
        binary and its configuration.

        The FHS is explicit: variable data a service generates itself (generated
        keys, encrypted vaults, sync sequences, collection cursors, dedup
        indexes, last-run timestamps) belongs under /var/lib, not in /etc (which
        is the administrator's read-only reference) nor in /opt (which is the
        program). Putting state in /etc is exactly what left morfCollector's
        vault unwritable under a root-owned /etc/morfsystem: a service running as
        its own user could never create its key.

        On Linux this pairs with the unit's `StateDirectory=morfsystem/<service>`,
        which systemd creates owned by the service User with the right mode and
        exposes as $STATE_DIRECTORY. This method reports the same path so the
        install summary and any explicit __STATE_DIR__ substitution agree with it.
        """
        key = {"Windows": "windows", "Darwin": "darwin"}.get(platform.system(), "linux")
        declared = self.state_dirs.get(key)
        if declared:
            return Path(os.path.expandvars(declared))
        if key == "windows":
            base = os.environ.get("ProgramData", r"C:\ProgramData")
            return Path(base) / "morfsystem" / self.service_name / "state"
        return Path("/var/lib/morfsystem") / self.service_name

    def binary_name(self) -> str:
        """The executable's file name, with the platform's extension."""
        if platform.system() == "Windows" and not self.binary.endswith(".exe"):
            return self.binary + ".exe"
        return self.binary

    def installed_binary(self) -> Path:
        return self.app_dir() / self.binary_name()

    # -- Loading ----------------------------------------------------------

    @classmethod
    def load(cls, repo_root: Path) -> "Manifest":
        path = repo_root / MANIFEST_NAME
        if not path.is_file():
            raise ManifestError(
                f"No {MANIFEST_NAME} in {repo_root}.\n"
                f"A project states its own deployment facts there; see "
                f"morfTemplateService/{MANIFEST_NAME} for the reference."
            )
        try:
            raw = json.loads(path.read_text(encoding="utf-8-sig"))
        except json.JSONDecodeError as exc:
            raise ManifestError(f"{path} is not valid JSON: {exc}") from exc

        missing = [k for k in ("service_name", "binary") if not raw.get(k)]
        if missing:
            raise ManifestError(f"{path} declares no {' and no '.join(missing)}.")

        configs = tuple(
            ConfigFile(
                source=entry["source"],
                dest=entry["dest"],
                overwrite=bool(entry.get("overwrite", False)),
                migrate_from=tuple(entry.get("migrate_from", ())),
            )
            for entry in raw.get("configs", [])
            if entry.get("source") and entry.get("dest")
        )

        return cls(
            repo_root=repo_root,
            service_name=raw["service_name"],
            display_name=raw.get("display_name") or raw["service_name"],
            binary=raw["binary"],
            app_dirs=raw.get("app_dir") or {},
            config_dirs=raw.get("config_dir") or {},
            state_dirs=raw.get("state_dir") or {},
            configs=configs,
            description=raw.get("description", ""),
            status_url=raw.get("status_url", ""),
            legacy_binaries=tuple(raw.get("legacy_binaries", ())),
        )
