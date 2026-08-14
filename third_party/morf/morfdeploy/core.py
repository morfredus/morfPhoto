"""The four steps every install-service script performed.

Find or build the binary, stop whatever runs, copy the binary and the
configurations into a fixed directory, hand the result to the service manager.
Six projects each carried their own copy of this, differing by a service name
and a directory.

There is no `platform.system()` in this file, and that is the design rule
rather than a coincidence: the moment orchestration starts asking which system
it runs on, the platform boundary has leaked and the single core becomes a
label on top of the old duplication.
"""

from __future__ import annotations

import getpass
import hashlib
import os
import platform
import shutil
import subprocess
from pathlib import Path

from .backends import ServiceBackend, select
from .manifest import Manifest


class DeployError(RuntimeError):
    """A deployment step failed for a reason worth reporting as-is."""


def invoking_user() -> str:
    """Who asked for this, not who is running it.

    Under sudo the process is root, but the build must not be: a build tree
    owned by root inside the clone breaks the next ordinary build, in a place
    that says nothing about the install that caused it.
    """
    return os.environ.get("SUDO_USER") or getpass.getuser()


def detect_preset() -> tuple:
    """CMake preset and build directory for this machine.

    Architecture, not operating system: an ARM64 Raspberry Pi and an x64 Linux
    box run the same backend and need different presets, so this belongs to the
    build step rather than to any platform module.
    """
    if platform.system() == "Windows":
        return "mingw", "build-mingw"
    machine = platform.machine().lower()
    if machine in ("aarch64", "arm64"):
        return "linux-arm64", "build-arm64"
    return "linux", "build"


def locate_binary(manifest: Manifest, build_dir: str) -> Path | None:
    """The freshly built binary, if it is there.

    Several layouts are tried because the parc has more than one: a service
    subdirectory for the C++ services, the build root for the simpler ones.
    """
    name = manifest.binary_name()
    for candidate in (
        manifest.repo_root / build_dir / "service" / name,
        manifest.repo_root / build_dir / name,
    ):
        if candidate.is_file():
            return candidate
    return None


class Deployer:
    """Runs the four steps against a backend."""

    def __init__(self, manifest: Manifest, backend: ServiceBackend | None = None):
        self.manifest = manifest
        self.backend = backend or select()

        if not self.backend.supported:
            raise DeployError(self.backend.supported_note)

    # -- Preconditions ----------------------------------------------------

    def check_privileges(self) -> None:
        if self.backend.requires_privileges() and not self.backend.has_privileges():
            raise DeployError(
                f"Installing a {self.backend.name} service requires administrator rights.\n"
                + self.backend.privilege_hint()
            )

    # -- Step 1 -----------------------------------------------------------

    def ensure_binary(self, rebuild: bool = False) -> Path:
        preset, build_dir = detect_preset()
        binary = locate_binary(self.manifest, build_dir)

        if binary is not None and not rebuild:
            print(f"  binary found: {binary}")
            return binary

        print(f"  building (preset {preset})...")
        self.backend.build_as_user(self.manifest.repo_root, preset, invoking_user())

        binary = locate_binary(self.manifest, build_dir)
        if binary is None:
            raise DeployError(
                f"No binary named '{self.manifest.binary_name()}' under "
                f"{self.manifest.repo_root / build_dir} after building."
            )
        print(f"  built: {binary}")
        return binary

    # -- Step 2 -----------------------------------------------------------

    def stop_existing(self) -> None:
        self.backend.stop(self.manifest)

    # -- Step 3 -----------------------------------------------------------

    def install_files(self, binary: Path, app_dir: Path, config_dir: Path) -> list:
        """Place the binary and the configurations; return what was written."""
        app_dir.mkdir(parents=True, exist_ok=True)
        target = app_dir / self.manifest.binary_name()
        shutil.copy2(binary, target)
        target.chmod(0o755)
        print(f"  binary installed: {target}")
        written = [target]

        # The binary alone is not enough where its shared libraries do not come
        # from a system location: on Windows the Qt and compiler DLLs must sit
        # beside it. The backend decides -- a no-op wherever the system already
        # provides them, so this line costs nothing on Linux. Done here, right
        # after the copy, so an install and an update both get it. The source
        # binary is passed too: the deployment tool is found from the build's
        # CMake cache, which lives beside it, not beside the installed copy.
        self.backend.install_runtime(target, binary)

        for config in self.manifest.configs:
            dest = config.resolved_dest(config_dir)
            dest.parent.mkdir(parents=True, exist_ok=True)

            # The real file wins over the example -- resolved HERE rather than
            # baked into the manifest. A manifest naming config/<name>.json is
            # right on the machine that wrote it and wrong in every clone,
            # because that file is local and untracked. morfNotify shipped
            # exactly that way and could not find its own configuration.
            source = self.manifest.repo_root / config.source
            if config.source.endswith(".example.json"):
                real = Path(str(source).replace(".example.json", ".json"))
                if real.is_file():
                    source = real

            # Never overwrite by default: these hold settings edited by hand on
            # this machine. Delivering a default over them destroys local state
            # nobody asked to lose.
            if dest.exists() and not config.overwrite:
                print(f"  config kept:      {dest}")
                continue

            # Nothing at the new location: adopt what an earlier convention
            # left behind, rather than installing a pristine example over a
            # machine that was already configured. The settings survive the
            # directory layout that happened to hold them.
            # The migration is attempted BEFORE the source is required, because
            # it does not need one: it copies from the previous location. Making
            # a missing source skip the whole entry meant morfNotify never
            # migrated a configuration that was sitting right there in /opt --
            # and then the service was registered anyway and crash-looped 85
            # times against a file nobody had put in place.
            previous = config.find_predecessor()
            if previous is not None:
                shutil.copy2(previous, dest)
                dest.chmod(0o644)
                written.append(dest)
                print(f"  config migrated:  {previous}")
                print(f"                 -> {dest}")
                print(f"  the old file is left in place; remove it once satisfied")
                continue

            # Nothing at the destination, nothing to migrate, and no source to
            # copy: this service will not start. Refusing here is the whole
            # point -- registering it anyway produces a unit that restarts
            # forever against a file that does not exist, and the journal fills
            # with an error the install never mentioned.
            if not source.is_file():
                homes = ", ".join(config.migrate_from) or "none declared"
                raise DeployError(
                    "\n".join([
                        f"No configuration to install for {self.manifest.display_name}.",
                        f"  declared source : {config.source} (absent from this clone)",
                        f"  destination     : {dest} (absent)",
                        f"  earlier homes   : {homes} (none found)",
                        "",
                        "The service would start and immediately exit. Nothing has "
                        "been registered.",
                    ])
                )

            shutil.copy2(source, dest)
            dest.chmod(0o644)
            written.append(dest)
            print(f"  config installed: {dest}")

        return written

    def enrich_configs(self, config_dir: Path, deep_lists: bool = False) -> bool:
        """Bring every installed configuration up to the new version's keys.

        Runs after the configs are in place, in install and update alike: a
        config that was kept, or migrated from an older layout, may predate keys
        this version introduced. A fresh one copied from the example already has
        them, so the merge is a no-op there. Enriching only -- never touches a
        value the user set, never removes a key.

        Returns True when a key was actually added, because that decides whether
        the service must be restarted: a new option written into a file the
        running process read at startup changes nothing until it reads it again.
        """
        from .configmerge import merge_config

        changed = False
        for config in self.manifest.configs:
            dest = config.resolved_dest(config_dir)
            if not dest.is_file():
                continue

            # The reference is the example this version ships -- the canonical
            # set of keys -- not whatever real file the repo may also carry.
            source = config.source
            if not source.endswith(".example.json"):
                source = source.replace(".json", ".example.json")
            reference = self.manifest.repo_root / source
            if not reference.is_file():
                continue

            try:
                added, obsolete = merge_config(reference, dest, deep_lists=deep_lists)
            except (OSError, ValueError) as exc:
                print(f"  could not enrich {dest}: {exc}")
                continue

            settings = [k for k in added if not k.rsplit(".", 1)[-1].startswith("_comment")]
            comments = len(added) - len(settings)
            if settings:
                print(f"  config enriched: {dest}")
                for key in settings:
                    print(f"    + {key}  (new option, default applied -- review it)")
                if comments:
                    print(f"    + {comments} documentation comment(s)")
            if added:
                changed = True
            if obsolete:
                # Reported, never removed -- and not a change: nothing was
                # written, so it cannot justify restarting anything.
                print(f"  {dest}: keys no longer in the reference, kept as-is:")
                for key in obsolete:
                    print(f"    ? {key}")

        return changed

    # -- Is there anything to do at all? ----------------------------------

    @staticmethod
    def digest(path: Path) -> str | None:
        """Content fingerprint, or None when the file is not there.

        Content, not size and date: `git checkout` and a rebuild both rewrite
        timestamps on files whose bytes are identical, so a date comparison
        would report a change on every single run -- exactly the noise this is
        meant to remove. At a few megabytes the read costs nothing measurable.
        """
        if not path.is_file():
            return None
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        return digest.hexdigest()

    def already_deployed(self, binary: Path, app_dir: Path, config_dir: Path) -> bool:
        """True when deploying would put back exactly what is already there.

        Stopping a service, copying a byte-identical file over itself and
        restarting is not a no-op: it is a gap in supervision, a reset uptime
        and, for a service mid-task, an interruption -- paid at the very moment
        one believed nothing was being touched. A parc-wide `upgrade` bounced
        every service on the machine for it.
        """
        installed = app_dir / self.manifest.binary_name()
        if self.digest(installed) != self.digest(binary):
            return False
        # A configuration that has never been placed still has to be, even when
        # the binary matches -- and the service must then read it.
        return all(config.resolved_dest(config_dir).is_file()
                   for config in self.manifest.configs)

    def chown_to_user(self, installed: list) -> None:
        """Give back only the files this install actually wrote.

        The shell scripts ran `chown -R` on the application directory, which is
        harmless for a dedicated /opt/<service> and dangerous anywhere else:
        morfSync puts its binary in /usr/local/bin, and recursing there would
        hand the whole system directory to one user. Narrowing to the files we
        placed removes the hazard entirely rather than guarding against it, and
        it is what was meant in the first place.

        A no-op on Windows, whose ownership model is different and whose
        ProgramData ACLs already grant what is needed.
        """
        if platform.system() == "Windows":
            return
        user = invoking_user()
        if user == "root":
            return

        # The dedicated application directory itself must belong to the user
        # too: the unit runs with User=<user> and WorkingDirectory=app_dir, and
        # a module creating its runtime data there (a cache/, a sqlite file)
        # fails on a root-owned directory -- silently, deep inside the module,
        # with an error message pointing at the configuration. That is exactly
        # what happened to morfAnalytics after a from-scratch install: mkdir
        # under sudo made /opt/morfanalytics root's, and narrowing the chown to
        # written files had dropped the directory entry.
        #
        # The directory ENTRY only, never recursive, and only when its name is
        # the service's -- a dedicated /opt/<service>. A shared location (the
        # old /usr/local/bin) can never match and is never handed over, which
        # is what the narrowing was protecting against.
        paths = list(installed)
        app_dir = self.manifest.app_dir()
        if app_dir.exists() and app_dir.name == self.manifest.service_name:
            paths.insert(0, app_dir)

        for path in paths:
            try:
                shutil.chown(path, user=user, group=user)
            except (OSError, LookupError) as exc:
                print(f"  could not chown {path}: {exc}")

    def verify_writable(self, app_dir: Path) -> None:
        """Warn, loudly, if the run user cannot write in the app directory.

        The unit runs with User=<user> and WorkingDirectory=app_dir; a module
        creating runtime data there (a cache, a sqlite file) fails on a
        directory the user does not own -- and may fail silently, deep inside
        the service, with symptoms that point elsewhere. That exact chain cost
        an investigation once: /opt/morfanalytics owned by root, the analytics
        cache uncreatable, and a UI message blaming the configuration.

        Checked HERE, at install time, where the person who can fix it is
        looking. A warning rather than a failure: some services never write to
        their app_dir, and a deliberately shared app_dir is valid.
        """
        if platform.system() == "Windows":
            return
        user = invoking_user()
        if user == "root" or not shutil.which("sudo"):
            return
        probe = subprocess.run(["sudo", "-u", user, "test", "-w", str(app_dir)],
                               check=False)
        if probe.returncode != 0:
            print()
            print(f"  WARNING: {app_dir} is not writable by '{user}', who the")
            print("  service runs as. A module creating runtime data there (a cache,")
            print("  a database) will fail -- possibly silently. Fix:")
            print(f"      sudo chown {user}:{user} {app_dir}")

    def report_legacy_binaries(self) -> None:
        """Name the copies an earlier layout left behind, without touching them."""
        stale = [Path(p) for p in self.manifest.legacy_binaries]
        stale = [p for p in stale if p.exists()]
        if not stale:
            return
        print()
        print("An earlier layout installed this binary elsewhere. Still present:")
        for path in stale:
            print(f"    {path}")
        print("Nothing runs them now -- the service unit points at the new location.")
        print("Left in place deliberately: removing an executable you did not ask")
        print("to remove is not tidying up. Delete them once you are satisfied.")

    # -- Step 4 -----------------------------------------------------------

    def register(self, app_dir: Path) -> None:
        self.backend.install(self.manifest, app_dir, invoking_user())

    # -- Whole operations -------------------------------------------------

    def install(self, rebuild: bool = False) -> None:
        app_dir = self.manifest.app_dir()
        print(f"Installing {self.manifest.display_name} ({self.backend.name})")
        print(f"  user:   {invoking_user()}")
        print(f"  source: {self.manifest.repo_root}")
        print(f"  binaire : {app_dir}")
        print(f"  config  : {self.manifest.config_dir()}")
        print(f"  etat    : {self.manifest.state_dir()}")
        print()

        self.check_privileges()
        binary = self.ensure_binary(rebuild=rebuild)
        self.stop_existing()
        written = self.install_files(binary, app_dir, self.manifest.config_dir())
        # An install always registers, even over an identical binary: the whole
        # point may be that the service is NOT registered yet. Only `update`
        # can conclude there is nothing to do.
        self.enrich_configs(self.manifest.config_dir())
        self.chown_to_user(written)
        self.verify_writable(app_dir)
        self.register(app_dir)

        self.report_legacy_binaries()

        print()
        print(f"{self.manifest.display_name} installed and started.")
        if self.manifest.status_url:
            print(f"Check with:  curl {self.manifest.status_url}")

    def update(self, force: bool = False) -> None:
        """Rebuild and replace, keeping the service registered.

        Distinct from install because it always rebuilds -- an update whose
        whole purpose is to ship new code must not silently reuse the binary
        that happens to be lying in the build directory.

        And it stops when there is nothing to ship. `force` restarts anyway,
        for the times when restarting IS the intention.
        """
        # « Not installed » must never be concluded from « I was not allowed to
        # ask ». Unelevated on Windows, schtasks answers access-denied for a
        # task registered as SYSTEM, with the same exit status as « no such
        # task » -- so this said a running service was not installed, and sent
        # the person to `install`, the wrong action entirely.
        if not self.backend.can_query_installation(self.manifest):
            raise DeployError(
                f"Cannot tell whether {self.manifest.display_name} is installed: "
                "this process is not allowed to ask the service manager.\n"
                + self.backend.privilege_hint()
            )
        if not self.backend.is_installed(self.manifest):
            raise DeployError(
                f"{self.manifest.display_name} is not installed on this machine.\n"
                "Run the install action first."
            )
        print(f"Updating {self.manifest.display_name} ({self.backend.name})")
        print()
        self.check_privileges()
        binary = self.ensure_binary(rebuild=True)
        app_dir = self.manifest.app_dir()
        config_dir = self.manifest.config_dir()

        # Enrichment first, because its outcome is part of the decision: a key
        # added to a file the running process read at startup means nothing
        # until it reads it again. It writes only when the reference gained an
        # option, so on an unchanged version it is silent and costs nothing.
        config_changed = self.enrich_configs(config_dir)

        if not force and not config_changed and self.already_deployed(
                binary, app_dir, config_dir):
            print(f"  binary unchanged: {app_dir / self.manifest.binary_name()}")
            print("  configurations in place, nothing to enrich")
            print()
            print(f"{self.manifest.display_name} is already up to date: "
                  "nothing deployed, the service was NOT restarted.")
            print("Use --force to redeploy and restart anyway.")
            return

        self.stop_existing()
        written = self.install_files(binary, app_dir, config_dir)
        self.chown_to_user(written)
        self.verify_writable(app_dir)
        self.register(app_dir)
        print()
        print(f"{self.manifest.display_name} updated.")

    def config(self, mode: str = "merge", force: bool = False) -> None:
        """Refresh an installed service's configuration from the repository.

        Fills the gap between `update` (which ships code and adds only
        top-level keys) and editing the deployed file by hand: a scripted,
        repeatable way to take a new version's settings into account WITHOUT
        reinstalling. Two modes, non-destructive by default -- a timestamped
        `.bak` is written before any change:

          merge (default): deep-enrich the deployed config with the keys this
            version's example introduced, INCLUDING new keys inside a module the
            user already has (matched by id). Existing values are always kept,
            and no list entry is ever added. This is how a new option such as a
            module's `morfsync_url` reaches an installation on its own.

          push (requires --force): replace the deployed config with the
            repository's. The occasional, deliberate "start from the shipped
            config again".

        Reads only what is on disk; the service is restarted only if something
        actually changed, because a new key means nothing until the process
        reads it again.
        """
        if not self.backend.can_query_installation(self.manifest):
            raise DeployError(
                f"Cannot tell whether {self.manifest.display_name} is installed: "
                "this process is not allowed to ask the service manager.\n"
                + self.backend.privilege_hint()
            )
        if not self.backend.is_installed(self.manifest):
            raise DeployError(
                f"{self.manifest.display_name} is not installed on this machine.\n"
                "There is no deployed configuration to update. Run install first."
            )
        self.check_privileges()
        config_dir = self.manifest.config_dir()
        print(f"Configuring {self.manifest.display_name} ({self.backend.name})")
        print(f"  config  : {config_dir}")
        print()

        if mode == "push":
            if not force:
                raise DeployError(
                    "config push REPLACES the deployed configuration with the "
                    "repository's copy.\n"
                    "Pass --force to confirm (a timestamped backup is written "
                    "first).\n"
                    "To only add this version's new keys while keeping your "
                    "settings, use 'config' with no mode (merge)."
                )
            changed = self._push_configs(config_dir)
        else:  # merge -- the safe default
            changed = self.enrich_configs(config_dir, deep_lists=True)
            if not changed:
                print("  every key of this version is already present.")

        print()
        if changed:
            print("  restarting to apply the configuration...")
            self.backend.stop(self.manifest)
            self.backend.start(self.manifest)
            print()
            print(f"{self.manifest.display_name} reconfigured.")
        else:
            print(f"{self.manifest.display_name}: nothing changed, "
                  "the service was NOT restarted.")

    def _push_configs(self, config_dir: Path) -> bool:
        """Overwrite each deployed configuration with the repository's copy.

        A timestamped backup of the deployed file is written first. The source
        is the repository's real config when it ships one, otherwise the example
        -- the same resolution as a fresh install, so `push` and `install`
        deliver the same bytes.
        """
        from datetime import datetime

        changed = False
        for config in self.manifest.configs:
            dest = config.resolved_dest(config_dir)

            source = self.manifest.repo_root / config.source
            if config.source.endswith(".example.json"):
                real = Path(str(source).replace(".example.json", ".json"))
                if real.exists():
                    source = real
            if not source.exists():
                print(f"  no repository source for {dest.name}, kept as-is")
                continue

            if dest.exists():
                stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
                backup = dest.with_name(f"{dest.name}.bak-{stamp}")
                shutil.copy2(dest, backup)
                print(f"  backup:          {backup}")

            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, dest)
            dest.chmod(0o644)
            print(f"  config replaced: {dest}")
            print(f"                <- {source}")
            changed = True

        return changed

    def config_footprint(self) -> list:
        """Every configuration location this service has ever used.

        Read from the manifest, not hard-coded: the current config, plus each
        earlier home declared in migrate_from. A service moved twice knows all
        three of its addresses, so a teardown can find them without a second
        list drifting from this one.
        """
        paths = []
        config_dir = self.manifest.config_dir()
        for config in self.manifest.configs:
            paths.append(config.resolved_dest(config_dir))
            for previous in config.migrate_from:
                paths.append(Path(os.path.expandvars(previous)))
        return paths

    def uninstall(self, purge: bool = False, backup_dir: Path | None = None) -> None:
        print(f"Uninstalling {self.manifest.display_name} ({self.backend.name})")
        self.check_privileges()
        self.backend.uninstall(self.manifest)

        app_dir = self.manifest.app_dir()
        configs = [p for p in self.config_footprint() if p.exists()]
        legacy = [Path(p) for p in self.manifest.legacy_binaries if Path(p).exists()]

        if not purge:
            print()
            print("Service removed. Your configuration was kept:")
            for path in configs or [app_dir]:
                print(f"    {path}")
            print("Re-run with --purge to remove it too "
                  "(add --backup to copy it first).")
            self.report_legacy_binaries()
            return

        # --purge: copy first if asked, then remove. The backup happens before
        # any deletion, so an interrupted run never leaves a half-removed config
        # with no copy of what was there.
        if backup_dir is not None and configs:
            backup_dir.mkdir(parents=True, exist_ok=True)
            print(f"\nBacking up configuration to {backup_dir}:")
            for path in configs:
                # The backup name encodes the FULL source path, flattened. Two
                # configs can share a basename in different directories -- the
                # current one in /etc, an old one migrate_from left in /opt --
                # and naming both by basename alone let the second silently
                # overwrite the first, losing exactly what the backup exists to
                # keep.
                flat = str(path).lstrip("/\\").replace(":", "").replace("/", "_").replace("\\", "_")
                dest = backup_dir / f"{self.manifest.service_name}__{flat}"
                shutil.copy2(path, dest)
                print(f"    {path}  ->  {dest.name}")

        print("\nRemoving:")
        for path in configs:
            self._remove(path)
        for path in legacy:
            self._remove(path)
        # The application directory holds the binary and, before the /etc move,
        # old configs. Removed last, and only on purge.
        if app_dir.exists():
            self._remove(app_dir)
        # The configuration directory is this service's own namespace under
        # /etc/morfsystem/<service>. It can hold files we never installed but the
        # service wrote at runtime -- a secrets vault, a state file -- which the
        # per-file removal above leaves behind. On purge we own that directory, so
        # remove it wholesale, as we do the application directory. The name guard
        # keeps a shared parent (/etc/morfsystem itself) untouched: it is never
        # named after a single service.
        config_dir = self.manifest.config_dir()
        if config_dir.exists() and config_dir.name == self.manifest.service_name:
            self._remove(config_dir)
        print("\nService and configuration removed.")

    @staticmethod
    def _remove(path: Path) -> None:
        try:
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
                # A dedicated legacy directory left empty (e.g. /etc/homeserverhub
                # after its one file goes) is tidied, but only if empty -- never
                # a recursive removal of a parent we did not create.
                parent = path.parent
                if parent.name and not any(parent.iterdir()):
                    parent.rmdir()
            print(f"    {path}")
        except OSError as exc:
            print(f"    could not remove {path}: {exc}")

    def status(self) -> None:
        # Same honesty as update(): silence about rights would turn a running
        # service into a missing one, in the one report meant to be trusted.
        if not self.backend.can_query_installation(self.manifest):
            print(f"{self.manifest.display_name}: cannot tell ({self.backend.name}) -- "
                  "not allowed to ask the service manager")
            print(self.backend.privilege_hint())
            return
        if not self.backend.is_installed(self.manifest):
            print(f"{self.manifest.display_name}: not installed ({self.backend.name})")
            return
        print(self.backend.status(self.manifest))
