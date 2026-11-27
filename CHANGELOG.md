# Journal des versions - morfPhoto

Le format s'inspire de [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/)
et du [versionnage sémantique](https://semver.org/lang/fr/).

## [0.24.0] - 2026-11-27

### Added

- `lc:flag` = `reject` is read: `rejected: true` and `picked: false`, attributed to `lightcraft`. The three states `pick`, `none` and `reject` are now confirmed on real files (a rejected photo showed `reject`). A flag is a single state, so `pick` and `none` also say "not rejected" (`rejected: false`).

### Changed

- **`rejected` is resolved by "one provider is enough"**, like `edited`: true as soon as one provider says the version is rejected, false if at least one says it is not and none says the opposite, unknown otherwise. Before, the first provider with a value won, so a photo rejected in Lightcraft stayed "not rejected" behind darktable (which says "not rejected" for any rating from 0 to 5). The only photos that change are those where providers disagree.
- The source recorded for `edited` and `rejected` is now the first provider that gave the winning value (it could name a provider that said the opposite).

### Tested on a real file

- The real flagged `.xmp` was read on a copy (then removed), with the flag set to `reject` in the copy: `rejected: true`, `picked: false`, attributed to `lightcraft`, while the rating and the edited state stayed with darktable; the default listing hides it and `?rejected=only` returns it. The file on disk read `pick` at the time of the run (the flag had been changed again since), so the confirmation of `reject` comes from the content shown by the user, and the run used a copy edited to `reject`.

## [0.23.0] - 2026-11-27

### Added

- `lightcraft` provider and the `picked` field. Lightcraft writes its own `lc:` properties in the same `.xmp` as darktable; its flag `lc:flag` is read as a picked photo: `pick` gives `picked: true`, `none` gives `false`. Both values are confirmed on a real file (the flag read `none`, then `pick` after it was set). Any other value, such as `reject`, stays unknown because its meaning is not confirmed, and `lc:settings` (the editing settings in JSON) is not read. The value is attributed to `lightcraft` even though the file is classified `darktable`, so `sources.picked` tells the truth.
- A version carries `picked` (`true`, `false` or `null`), `/api/v1/library` accepts `?picked=true|false` (a photo passes if one of its versions does, an unknown value never passes), and the dataset gets a `picked` column (principal version) with `with=library`. `/library/sidecars` and the `library` block of the index status count `with_lightcraft` / `by_source.lightcraft`.
- Schema v8: a `picked` column on `library_versions`. The sidecar fingerprint format changes, so every photo is read again on the next pass, from its sidecars and its stored EXIF tags, without ExifTool.

### Tested on a real file

- The real `.xmp` where the flag is `pick` was read on a copy (then removed): `picked: true` attributed to `lightcraft`, the rating and the edited state still coming from darktable. The committed tests use a reduced, anonymised version.

## [0.22.0] - 2026-11-27

### Added

- `dxo` provider: DxO PhotoLab sidecars (`photo.ext.dop`, version 0). A `.dop` is not XML but a text file of nested tables (`Name = { ... },`), read by a small bounded parser (depth, size and node limits, so a corrupt file cannot overflow the stack or the memory). It reads what a real PhotoLab 8.23 file confirms: the rating (0 to 5), the keywords (each inner table is one hierarchical keyword, and the internal darktable tags that PhotoLab imports are dropped like everywhere else), the decimal GPS position and the copyright notice (IPTC). `Edited` stays unknown on purpose: PhotoLab writes a `.dop` as soon as an image is opened, with automatic corrections already applied, so its presence proves nothing; a rule needs a second file, edited by hand. Only the first element of `Items` is read, the format of virtual copies not being confirmed. `/library/sidecars`, the `source` filter and `gps_source` know the new `dxo` name.
- Scanner: `.dop` files are collected next to `.xmp` files and attached to their image by name (`IMG.DNG.dop` is the version 0 of `IMG.DNG`, together with its `IMG.DNG.xmp` if any).

### Tested on real files

- A real PhotoLab `.dop` and a real mixed XMP (darktable plus a second editor writing its properties as elements in a second `rdf:Description`, with a hierarchical keyword that is only a darktable internal tag) were read through the whole chain on copies, then removed: both sidecars are detected, the rating, the position and the "edited" state come out right and the internal keywords are dropped. The committed tests use reduced and anonymised versions (position, copyright and most settings replaced; structure and syntax unchanged), never the original files.

### Not read yet

- The `lc:` properties of the second editor in that XMP (`lc:settings`, `lc:flag`): their meaning is not confirmed, so nothing is guessed.

## [0.21.0] - 2026-11-27

### Added

- `GET /api/v1/photos/dataset?with=gps` adds `latitude`, `longitude` and `gps_source` (dictionary: `file` or the program whose sidecar gave the position) so a client can draw a map. The EXIF of the file is the authority and a sidecar position is used only when the file has none; a photo with no position stays `null`, never (0, 0). `with` now takes a comma separated list (`library`, `gps`, both), independent of each other, and an unknown value answers `400`. Without the option the response is unchanged.

## [0.20.2] - 2026-11-27

### Fixed

- A pass over unchanged photos spent almost all its time in the perimeter guard. Profiling (temporary timers per phase, 3 000 photos, 3 passes) showed `isWithinRoots` at about 0.8 ms per call, called for each photo three times (counting, indexing, library): 2.4 s + 2.4 s of a 5.0 s pass, and the whole 2.6 s library surcharge. The cause is the real-path resolution (links, case, `..`) of the file and of every root at each call. The guard now resolves the directory once (cached per pass) for an ordinary file, whose place is the one of its directory, and still resolves each link individually since a link may point outside the root (`FileInfo::isSymlink`). The guard keeps its meaning; only its cost changes.
- Measured on the same 3 000 photos, fake ExifTool, Windows disk: a pass without change went from 5.0 s to 0.30 s with the library off, and from 7.6 s to 0.31 to 0.64 s with it on (16 to 25 times faster). The suspicion left open by 0.20.1 is settled: it was this guard, not the library.

### Not exercised

- The link branch (`isSymlink`) is not covered by a test: creating a symbolic link on Windows needs a privilege, so it remains to be checked on Linux.

## [0.20.1] - 2026-11-27

### Changed

- Documentation only: measured effect of the grouped indexing writes of 0.20.0, same synthetic corpus of 3 000 photos, ExifTool failing on every file, run on a Windows disk. A pass over unchanged photos: library off 12.9 s before, 5.2 s after (2.5 times faster); library on 9.4 s before, 7.8 s after (1.2 times faster). A first full pass: library off 141.5 s before, 130.0 s after; library on 167.3 s before, 154.2 s after (about 8 % faster, dominated by the failing ExifTool). Reads did not change (every `/library` request stays under 50 ms). What remains of an unchanged pass (about 1.7 ms per photo with the library off, 2.6 ms with it on) is not explained yet: it has not been profiled, so no cause is claimed.

## [0.20.0] - 2026-11-27

### Added

- Cursor pagination on `GET /api/v1/library`: the response carries `next` (opaque, `null` on the last page) and `?cursor=` continues from it, ordered by capture date (undated photos last) then identifier. Unlike `page`, it skips or repeats no photo when the catalogue changes between two reads (a pass running during a long read). An unreadable cursor answers `400`.
- `ETag` and `304 Not Modified` on `/library...` and `photos/dataset`: the validator is the catalogue revision (`PhotoModule::catalogRevision`), new at each start, after each indexing pass and after each write through the API. A request whose `If-None-Match` matches is answered `304` before anything is computed, so the server work is saved, not only the bandwidth. `Cache-Control: no-cache` makes clients revalidate.

### Changed

- Indexing writes are grouped: a pass now commits by batches of 200 files or one second instead of one SQLite commit (one `fsync`) per file. On a synthetic corpus of 3 000 unchanged photos a pass took 9 to 13 s, almost all of it spent synchronising the disk (about 4 ms per file); the ExifTool extraction stays outside any transaction, so a slow file never holds a write lock open.

### Measured

- On 5 000 photos (library on) every read is under 60 ms: `/library` page of 500 in 35 ms (deep page 49 ms), facets 50 ms, the `with=library` dataset 48 ms for 550 KB. Reading was not the bottleneck, so no extra index was added; the library adds about 18 % to a first full pass (167 s against 141 s for 3 000 photos, ExifTool failing on every file) and nothing noticeable to an unchanged pass.

## [0.19.1] - 2026-11-27

### Fixed

- `/library/sidecars` and the `library` block of the index status counted an unreadable sidecar as a sidecar of an "unknown" software. Unreadable sidecars are now counted only in `unreadable`; `by_source` and `with_sidecar` (and so `without_sidecar`) only count sidecars that could be read. Found while building the PhotoHub panel.

## [0.19.0] - 2026-11-27

### Added

- Photo library, read-only (contract `morfphoto-library/1`, morfSystem 1.9.0): the layer that programs leave beside the images (rating, reject flag, colour labels, keywords, title, "edited", versions). It is off by default and turned on by naming providers in `sidecars.providers` (`darktable`, `xmp`); the XMP embedded in the image (`file`) is always read last. Each field is resolved on its own by provider priority, an unknown value stays `null`, the EXIF of the image remains the authority on the shot and no sidecar is ever written.
- `GET /api/v1/library` (photos with their versions, filters `min_rating`, `rejected`, `edited`, `color_label`, `unlabeled`, `source`, `keyword` with hierarchical descendants, the EXIF filters, dates, folder and file name), `/library/{id}` and `?ids=`, `/library/facets` (each list counted with every filter but its own, plus the unknown values), `/library/subfolders`, `/library/sidecars` (providers, photos per provider, unreadable sidecars). `404 library_disabled` while the library is off.
- `GET /api/v1/photos/dataset?with=library` adds the library columns (`rating`, `rejected`, `edited`, `edited_any`, `rating_max`, `versions`, `color_labels`, `source`, `keywords` and their dictionaries) with one row per photo; without the option the response is unchanged.
- `/api/v1/index/status` carries a `library` block, the beacon announces the `photo_library` capability when the library is on, and the schema moves to v7 (`library_files`, `library_versions`, `library_keywords`, `library_sidecars`, all rebuildable).
- Incremental by fingerprint: turning the library on re-reads each photo once, then only photos whose sidecar list changed are read again; turning it off removes its values and leaves the EXIF alone.

### Not yet

- Cursor pagination and `ETag`/`304` on `/library` (planned with the indexing performance work), and the sidecars of Capture One, RawTherapee, DxO and ON1 (formats to be checked on real files first).

Inspired by the EXIF + XMP library of the Rhettus/morfPhoto fork (Immage); the sidecar layer is rewritten as a provider interface instead of being tied to darktable.

## [0.18.2] - 2026-11-27

### Changed

- Documentation only: the roadmap points to the proposed `morfphoto-library/1` contract (photo library layer read from the sidecars of several programs), kept in the morfSystem repository.

## [0.18.1] - 2026-11-27

### Changed

- Documentation only: the README no longer says that morfAnalytics cannot send the API key (it can since 0.80.0).

## [0.18.0] - 2026-11-27

### Added

- Optional API key: `api_key` in the configuration, or the `MORFPHOTO_API_KEY` environment variable (it wins), makes every `/api/` route require `Authorization: Bearer <key>` and answer `401 unauthorized` otherwise. The key is compared through SHA-256 digests over their full length, so the response time reveals nothing about it. With no key configured nothing changes, so no existing client of the fleet breaks.
- `/status` reports `api_key_required` (a boolean, never the key). `/healthz`, `/status` and `/modules` stay open so supervision keeps working.

Note: morfAnalytics sends the key from 0.80.0 on. Inspired by the API key of the Rhettus/morfPhoto fork (Immage), rewritten for this code base.

## [0.17.0] - 2026-11-27

### Added

- Grid thumbnails: `GET /api/v1/thumbnail` now returns an image of at most 400 px on its longest side. A full-size JPEG is decoded directly at reduced size (no ExifTool process, orientation applied); a RAW uses its embedded `PreviewImage`, with `ThumbnailImage` as fallback, scaled down to the grid. A preview that is already small (400 px, 1 MB) is served untouched.
- Disk cache for those thumbnails, under `<state>/cache/thumbnails` (rebuildable, safe to delete). The key follows path, size and modification date, so an edited or replaced photo is never served from a stale thumbnail. Writes are atomic, the size is capped (`thumbnails.max_mb`, default 512) with the least recently used removed first, and writes stop when the disk is nearly full (`thumbnails.min_free_mb`, default 512). `thumbnails.cache` set to `false` turns it off.
- `/status` reports the cache (`thumbnails`: `enabled`, `dir`, `hits`, `misses`, `writes`, `full`).

### Changed

- morfPhoto now links Qt Gui, used only for `QImage` and `QImageReader` (no window, no display). On Debian the package pulls `libqt6gui6`.

Inspired by the thumbnail work in the Rhettus/morfPhoto fork (Immage), rewritten for this code base.

## [0.16.1] - 2026-11-26

### Fixed

- Windows package workflow: `git` is installed in the MSYS2 environment and the checkout is declared safe, so the packaging can prove the state of the sources ("build provenance is dirty=None ... Refusing to package").

## [0.16.0] - 2026-11-26

### Added

- GitHub Actions CI (`.github/workflows/ci.yml`): build and `ctest` on Ubuntu 24.04 (Qt 6.4), Linux arm64 and Windows (MSYS2 UCRT64) for every pull request and push to `main`/`dev`. `packages.yml` builds the `.deb` amd64, the `.deb` arm64 and the Windows packages as workflow artifacts on every push to `main`; `package-all.py` (morfTools) takes them for the released commit and builds locally only what is missing. Each target declares its artifact in `morfproject.json` (`package.ci`). Workflows are generated by morfTools `scripts/gen-ci-workflows.py`.

## [0.15.6] - 2026-11-18

### Changed

- README (en, fr): the Qt SQLite driver (`libqt6sql6-sqlite`) is listed with the runtime prerequisites.

## [0.15.5] - 2026-11-18

### Fixed

- Debian package: it now depends on `libqt6sql6-sqlite`. The SQLite driver of Qt is a plugin loaded at run time, so the automatic dependency detection (`dpkg-shlibdeps`) missed it and the package only pulled `libqt6sql6`. On a machine without the driver, the SQLite storage failed with "Driver not loaded". Declared as a runtime system dependency in `service.json` (not blocking: on Windows the driver is shipped by `windeployqt`).

## [0.15.4] - 2026-11-17

### Changed

- Vendored morfDeploy resynchronised to 0.22.0: on Windows, the build libraries are recognised under MSYS2 (UCRT64) and verified with pacman instead of reported as "no package for this platform".

## [0.15.3] - 2026-11-07

### Removed

- Dead service-template leftovers: `ExampleModule`, the `example` module type, the
  `examples/minimal` demo (which used the template port 8901) and the `POST /example` route.

### Changed

- The no-config fallback now starts an empty `photo` module (no root, database in the
  state directory) instead of the demo module, so the service stays startable.

## [0.15.2] - 2026-11-07

### Changed

- Removed leftover references to the service skeleton (docs, comments, scripts).

## [0.15.1] - 2026-11-07

### Security

- **Privileged helper**: `mount`, `umount`, `dmesg` and `systemctl` are now run through absolute
  paths with a minimal imposed environment (`PATH=/usr/sbin:/usr/bin:/sbin:/bin`, `LC_ALL=C`),
  never the caller's `PATH`. Host, share, slug and mount point checks use anchored patterns, so a
  trailing newline is no longer accepted.
- **Credential file** (`.cred`) and `/etc/fstab` / `morfphoto.json` are written atomically
  (temporary file in the same directory created with its final mode, `fsync`, `rename`). The
  password is never written to a file with a permissive mode, and `morfphoto.json` is explicitly
  `0644` whatever the caller's umask. `umount` failures are now checked (mount and unmount) and
  stop the operation before `/etc/fstab` is touched.
- **ExifTool**: a file name containing a newline or carriage return is refused (it could inject
  ExifTool arguments through the stay-open argfile), `--` is inserted before the path, and the
  `{readyN}` marker is only recognised alone on a line so a crafted file name cannot desynchronise
  the stream.
- **CSRF**: write requests (`POST`, `PUT`, `PATCH`, `DELETE`) whose `Origin` differs from `Host` are
  refused with `403 cross_origin_forbidden`; `Access-Control-Allow-Origin: *` is now sent on `GET`
  responses only.

### Fixed

- `GET /api/v1/photos`: `(page - 1) * page_size` is computed in 64 bits and `page` is capped, so a
  huge `page` can no longer overflow.

## [0.15.0] - 2026-11-02

### Added

- **`GET /api/v1/sources` reports `reachable`** for each pushed source: a bounded probe of the
  mount point (same `availability_timeout_ms` as the indexer, never a bare `statvfs`). Unlike
  `mounted`, it turns false when the source machine is off or its address changed, so PhotoHub
  can offer to reconfigure the access instead of failing silently.

### Fixed

- The privileged helper now classes mount.cifs "could not resolve address" as
  `host_unreachable` (it fell back to the generic `cifs_mount_failed`), so a client can retry
  a source by IP address after a hostname that the server cannot resolve.

## [0.14.9] - 2026-11-01

### Fixed

- ISO read from Ricoh GR III (and other Pentax-family maker notes) was wrong (9 instead of 200):
  a bare `-ISO#` let ExifTool pick the maker-note raw index code. The extractor now requests
  `-ExifIFD:ISO#` explicitly. Existing photos need a full reindex to correct stored values.

## [0.14.8] - 2026-10-15

### Fixed

- Schema v6: composite index `files(directory, state)`. `/api/v1/photos/summary` (polled by
  PhotoHub) joined every folder context to the files of its directory with a full table scan;
  during an indexing pass it took minutes, blocked the single HTTP thread and made `/status`
  and PhotoHub time out ("morfPhoto injoignable"). The migration runs once at startup.
- The summary is cached (10 s when idle, 60 s during an indexing pass), so repeated polling no
  longer recomputes it on the single HTTP thread.

## [0.14.7] - 2026-10-15

### Fixed

- `/status` no longer runs the full `summary()` (DISTINCT and correlated EXISTS queries):
  during an indexing pass it took over 10 s on the Pi, so morfMonitor (3 s timeout) never
  saw the running activity. It now uses three light counters (`statusCounts`).

### Changed

- Resync the vendored morfBeacon (0.8.0).

## [0.14.6] - 2026-10-10

### Changed

- Resync the vendored morfDeploy (0.21.2): `config push` skips identical files.

## [0.14.5] - 2026-10-10

### Changed

- Resync the vendored morfDeploy (0.21.1): `install`/`update` now copy `VERSION` next to the binary.

## [0.14.4] - 2026-09-29

### Changed

- **Vendored morfDeploy 0.21.0.** An update now brings the installed config fully up
  to the example: new keys also reach modules already present (matched by `id`),
  documentation comments follow the example, and keys a version declares in
  `removed_keys` (service.json) are deleted after a backup.

## [0.14.3] - 2026-09-27

### Changed

- Re-vendored morfBeacon to 0.7.2 (documentation-only release; `include/` and
  `src/` unchanged).

## [0.14.2] - 2026-09-27

### Fixed

- **`ROADMAP.md` was the template's.** It still said "this repository is a template"
  and listed template chores. It now states morfPhoto's own scope and non-goals
  (no business analysis, no write to photos except a folder's `.morfphoto.json`, no
  implicit deletion).

### Changed

- Re-vendored morfDeploy to 0.20.7 (dead-code removal and punctuation only; no
  behaviour change).
- Em dashes replaced by `-` in the project's own files.

## [0.14.1] - 2026-09-07

### Changed

- **Resync vendored morfBeacon to 0.7.1.** Picks up the async `/status` reply fix:
  the beacon status server no longer blocks the event loop draining a reply to a
  slow client, so the heartbeat can no longer be starved (which produced false
  "service down" alerts across the parc). No API change.

## [0.14.0] - 2026-09-06

### Added

- **Representative folder sampling for the preview** (`GET /api/v1/photos?directory=…&sample=N`).
  Returns N image paths **spread across the whole folder** and **deduplicated by stem**
  (filename without extension), so a shot's RAW and JPEG count once and the preview never
  shows two near-identical frames. Distinct from pagination: taking the first N photos
  tended to return the same scene (a RAW+JPEG pair, or a burst), giving a poor overview.
  Reads the database only (no mount access, so a sleeping source cannot block it).

## [0.13.1] - 2026-09-06

### Fixed

- **A sleeping source no longer freezes morfPhoto's HTTP workers.** When a network
  source (SMB/CIFS) goes dead - the source PC asleep - any access to its mount blocks
  until the CIFS timeout (~180 s). Three endpoints touched the mount directly and could
  hang: `PUT`/`DELETE /api/v1/context` (writing/removing the `.morfphoto.json` sidecar)
  and `GET /api/v1/thumbnail` (preview). Each now runs a bounded accessibility probe
  (`probeAccessible`, `watch.availability_timeout_ms`, default 5 s) before touching the
  path - `thumbnail` probes before `isWithinRoots`, whose `canonicalFilePath()` would
  itself block. An unreachable source now returns a clear error fast instead of hanging.
  (`/status` and `/healthz` were already safe: they only read the database and
  `/proc/mounts`.)

### Changed

- **Mounts now fail fast on a dead share.** The privileged helper mounts CIFS with
  `soft,echo_interval=10`, so I/O on a source whose machine fell asleep errors in
  ~20 s instead of blocking ~180 s (the default CIFS timeout). Applies to both the test
  mount and the persisted `fstab` line; re-push a source to migrate its options.

## [0.13.0] - 2026-09-05

### Added

- **Per-source read/write mounts, so context qualification can actually write.**
  Pushed SMB sources were always mounted read-only, which made writing the
  `.morfphoto.json` sidecar beside the photos impossible - the qualification screen
  failed with "Système de fichiers accessible en lecture seulement" even though
  morfPhoto only ever writes that one sidecar, never the photos. A source can now be
  declared **qualifiable** (`writable`), mounted read/write; archives stay read-only.
  - `POST /api/v1/sources` accepts a `writable` flag (default false, backward
    compatible), threaded through PhotoModule and SourceManager to the privileged
    helper. The stored source metadata records `writable`.
  - The privileged helper's `mount` verb takes an optional 6th argument `rw|ro`
    (default `ro`), writes the matching fstab option, and **remounts when the current
    mode no longer matches** - so re-pushing a source to make it qualifiable actually
    flips an existing read-only mount to read/write instead of silently keeping it ro.
  - When `rw` is requested the helper performs a real **write probe** (create then
    delete a temp file) after mounting: if the Windows share denies writes for the
    account, it fails immediately with a clear message instead of letting the failure
    surface later when saving a context.

## [0.12.1] - 2026-09-03

### Changed

- Re-vendored morfDeploy 0.20.5 (opt-in arm64 cross-packaging and the sysroot
  `.shlibs` Depends resolution). The VERSION is bumped so the source tag matches
  the rebuilt artifact after this vendored-tooling update; without it the release
  provenance check rejects a package built past the previous tag.

## [0.12.0] - 2026-09-02

### Added

- **Thumbnail endpoint** `GET /api/v1/thumbnail?path=<file>`: returns an `image/jpeg`
  preview (embedded thumbnail extracted by exiftool, JPEG as well as RAW), so pure
  clients (PhotoHub) can show a folder preview without ever reading the photo files.
  The path is validated as within an allowed root and pointing at an existing file;
  anything else returns 404.
- `GET /api/v1/photos` gains a `directory` filter (fully decoded), to fetch a sample
  of a folder's files for that preview.
- The API layer can now return non-JSON responses: `PhotoApi::Result` carries a
  `contentType` and the HTTP server honours it (JSON stays the default).

## [0.11.1] - 2026-09-02

### Fixed

- `GET /api/v1/context` and `DELETE /api/v1/context` now decode the `directory`
  query parameter fully: a directory path carries `/` characters that `QUrlQuery`
  keeps percent-encoded (`%2F`) by default, so the lookup missed and always returned
  `unqualified`. Reading the parameter as `QUrl::FullyDecoded` fixes both endpoints.

## [0.11.0] - 2026-09-02

### Added

- **Photographic context per folder** (contract `morfphoto-context/2`). Each photo
  directory may carry a `.morfphoto.json` file describing two independent dimensions:
  `context` (conditions/intent of the session: `LIBRE`, `DECOUVERTE`, `EVENEMENT`,
  `SPECTACLE`, `MISSION`, `SPECIALISEE`, `INCONNU`) and `subject` (dominant subject:
  `GENERAL`, `PERSONNES`, `ANIMAUX`, `PAYSAGE`, `ARCHITECTURE`, `DETAIL`). The file on
  disk is the source of truth; morfPhoto is its sole writer.
- Schema migration **v5**: new `folder_contexts` table, a reconstructible projection of
  the on-disk files keyed by directory (`files.directory`). No column added to `files`;
  the context is joined at read time.
- Context discovery during an indexing pass: after a complete scan on an available root,
  each seen directory's `.morfphoto.json` is read (idempotent by mtime) and projected.
  A missing or invalid file never blocks indexing or EXIF extraction; issues are logged
  under stage `context`.
- API (additive): `GET /api/v1/contexts[?status=qualified|unqualified|invalid]`,
  `GET /api/v1/context?directory=…`, `PUT /api/v1/context` (writes `.morfphoto.json`
  atomically; `context` and `subject` mandatory and validated), `DELETE /api/v1/context`.
  `GET /api/v1/photos/dataset` gains `context`/`subject` columns and dictionaries;
  `GET /api/v1/photos/summary` gains a `contexts` coverage block.
- The three states stay distinct everywhere: absent file → `unqualified`, `context=INCONNU`
  → qualified but deliberately undecided, broken file → `invalid`.

## [0.10.0] - 2026-08-24

### Ajouté

- Support du contrat générique **activité en cours** (`activity/1`,
  `morfSystem/docs/CONTRAT-ACTIVITE.md`). Pendant une indexation, `/status`
  expose un objet optionnel `activity` (type, état, début, avancement, dossier
  courant), lu en temps réel par morfMonitor. Absent quand rien ne tourne ;
  jamais dans le heartbeat beacon.
- À la fin d'une passe d'indexation, l'activité **terminée** est déclarée à
  morfAnalytics (`POST /api/monitor/activity`) pour historisation : best-effort,
  jamais bloquant, URL d'ingestion lue dans `MORFANALYTICS_ACTIVITY_URL` ou
  `/etc/morfsystem/monitor-activity-url` (même convention que morfDeploy). Sans
  cible configurée, rien n'est émis : morfPhoto ne dépend jamais de morfAnalytics.

## [0.9.10] - 2026-08-23

### Corrigé

- Un mot de passe SMB erroné remontait « Authentification ✓ / Partage monté ✗
  mount error(13): Permission denied » (`cifs_mount_failed`), trompeur : le
  montage échouait en réalité sur `STATUS_LOGON_FAILURE`. `mount.cifs` n'imprime
  pas le motif NT (seulement « Permission denied ») ; le helper privilégié lit
  désormais le journal noyau (`dmesg`) après un échec pour classer sur le motif
  réel. Un mauvais mot de passe est donc catégorisé `auth_failed` (message
  explicite : vérifier le mot de passe, jamais le PIN), et l'étape
  d'authentification n'est plus marquée réussie à tort.

## [0.9.9] - 2026-08-23

### Ajouté

- `GET /api/v1/sources/ready` et verbe helper `probe` : PhotoHub vérifie que le
  helper setuid est présent, traversable et démarrable avant d'envoyer un mot
  de passe. Un POST `/sources` refait le même contrôle.

## [0.9.8] - 2026-08-22

### Modifié

- L'indexation commence tout de suite, sans parcours de comptage. Le total
  (`files_total`) grandit dossier après dossier, ou reprend le `files_seen` de la
  dernière passe comme estimation. `files_total_final` passe à vrai quand tous les
  dossiers ont été listés. Le pourcentage s'adapte ; il reste sous 99 % tant que
  le total n'est pas clos. Un seul walk disque/SMB : pas de charge CPU supplémentaire.

## [0.9.7] - 2026-08-22

### Modifié

- Pendant le précomptage, `folders_done` avance dossier par dossier (puis revient à 0 à l'indexation) et `files_seen` accumule les fichiers trouvés, pour que PhotoHub puisse afficher un pourcentage de **comptage** distinct du pourcentage d'indexation.

## [0.9.6] - 2026-08-22

### Corrigé

- Classification SMB : `STATUS_ACCOUNT_LOCKED_OUT` (souvent suivi de `return code = -13`) n'est plus un `permission_denied` avec authentification marquée OK. Code `account_locked`, étape `smb_auth` en échec. Les messages d'auth rappellent : identifiant = nom Windows, mot de passe = session locale ou compte Microsoft, jamais l'e-mail ni le PIN.
- Progression d'indexation : précomptage léger des fichiers image (sans EXIF), puis `files_seen / files_total`. `GET /api/v1/index/status` expose `progress.phase` (`discovering` / `indexing`), `files_total` / `percent` à `null` tant que le dénominateur n'est pas fiable. Les dossiers restent une information secondaire.

## [0.9.5] - 2026-08-22

### Corrigé

- Helper setuid : `mount.cifs` teste l'UID *réel* (`getuid`), pas l'UID effectif. Sans `setuid(0)` après avoir mémorisé le compte du service, le premier montage échouait (`no match for /mnt/photos_<slug> found in /etc/fstab`) alors que l'authentification SMB avait réussi. Les options `uid=`/`gid=` du CIFS restent celles de l'appelant pour que morfPhoto puisse lire le partage.
- Ce message n'est plus classé `permission_denied` (ACL Windows) : code `cifs_needs_root` si une ancienne version du helper est encore en place.

## [0.9.4] - 2026-08-22

### Corrigé

- Helper privilégié : `QStringLiteral` n'accepte qu'un littéral C, pas une variable (`kFstab`, `kConfigJson`). Remplacé par `QString::fromLatin1` pour compiler sous GCC Linux/ARM.

## [0.9.3] - 2026-08-22

### Corrigé

- Autoconfiguration SMB multi-postes : une machine = un hostname normalisé = un montage `/mnt/photos_<slug>` = un fichier `/etc/morfsystem/smb-photos-<slug>.cred` = une racine. Plus de ressource générique (`/mnt/photos`, `smb-photos.cred`) ni d'identité dérivée de l'IP.
- Le helper crée le point de montage, vérifie un vrai CIFS lisible (sans `nofail` au test), n'écrit `fstab` qu'ensuite, ajoute la racine à `morfphoto.json` sans toucher aux autres, revalide le JSON, et ne redémarre morfPhoto que si la config a changé.
- Les erreurs d'authentification (`STATUS_LOGON_FAILURE`) et les autres causes (partage introuvable, hôte injoignable, JSON invalide) remontent classées jusqu'à PhotoHub, avec le détail de l'étape en échec.

## [0.9.2] - 2026-08-21

### Corrigé

- Helper privilégié : autoriser explicitement l'exécution setuid de Qt (QCoreApplication::setSetuidAllowed) ; sans cela le helper avortait « running setuid, this is a security hole » et le bouton « Envoyer la config » échouait.
- Copie vendorée de morfdeploy alignée sur 0.17.4 (dossier du helper traversable par le compte de service).

## [0.9.1] - 2026-08-21

### Ajouté

- Enregistrement des compilations au niveau CMake (record_compile) : la durée de compile est signalée à morfAnalytics quel que soit le déclencheur (cmake --build direct, morf upgrade, déploiement morfDeploy), plus seulement via morfDeploy.

## [0.9.0] - 2026-08-21

### Ajouté

- Sources SMB poussées par PhotoHub : POST /api/v1/sources monte un partage en lecture seule via un helper privilégié setuid (morfphoto-helper), l'ajoute aux racines et persiste la métadonnée (jamais le mot de passe). Multi-machine, un point de montage /mnt/photos-<hôte> par poste, sans écrasement.

## [0.8.1] - 2026-08-21

### Modifié

- Resynchroniser la copie vendorée de morfdeploy vers 0.17.3.

## [0.8.0] - 2026-08-20

### Ajouté

- Mise à jour de la copie vendorée de morfDeploy 0.14.0 pour le packaging avec
  provenance vérifiée.

## [0.7.2] - 2026-08-18

### Ajouté

- **Déclaration de dépendance système** dans `service.json` : `exiftool` (paquet
  Debian `libimage-exiftool-perl`), **optionnelle**, `required_for` la capacité
  `exif`. morfDeploy 0.7.0 la détecte et propose de l'installer lors de
  `deploy`/`install` ; sans elle, morfPhoto indexe mais les métadonnées EXIF
  restent vides jusqu'à installation + réindexation. Formalise le prérequis
  connu. Aucun changement de code du service.

## [0.7.1] - 2026-08-17

### Ajouté

- **Déclaration de purge** dans `service.json` : la catégorie `database` (la base
  d'indexation `photos.db`, sous `state_dir`) est désormais annonçée à morfDeploy,
  donc effaçable par `service.py purge database` et `morf purge morfPhoto database`
  (avec `--dry-run`). Emplacement fixe sous `$STATE_DIRECTORY`, purge de type
  `path` sûre : les photos d'origine ne sont jamais touchées, seule la base d'index
  (reconstructible par réindexation) est effacée. Aucun changement de code du
  service : capacité déclarée, exécutée par le socle.

## [0.7.0] - 2026-08-17

### Ajouté

- **Empreinte de dédoublonnage dans le dataset.** `/api/v1/photos/dataset` porte une
  nouvelle colonne `fingerprint` (une par photo) : empreinte FNV-1a **stable
  inter-machines** de `nom + taille + date de prise` (le chemin est exclu, car il
  varie d'un poste à l'autre). Elle permet à morfAnalytics de fusionner plusieurs
  photothèques du parc en **ne comptant qu'une fois** une photo indexée sur deux postes
  (même CD, dossier partagé). `filename`/`size` ne sont PAS exposés : le dataset reste
  anonyme, seule l'empreinte opaque en sort. Colonne **additive** : les consommateurs
  existants l'ignorent sans changement.

## [0.6.0] - 2026-08-17

### Ajouté

- **Supports amovibles (CD/DVD, disques d'archive).** Une sélection peut être déclarée
  `removable` : morfPhoto ne marque **jamais** ses fichiers disparus quand le support
  est absent. Le cas piège d'un point de montage resté présent mais vide (un CD partagé
  en SMB puis éjecté renvoie « 0 fichier vu ») ne fait donc plus sortir toute l'archive
  des données. Les photos gravées restent indexées, donc dans morfAnalytics, support
  éjecté **et** après un redémarrage du service (la base SQLite est persistante).
  Un `volume_label` optionnel nomme le disque (« PHOTOS-2015 ») pour le reconnaître.
- **Exclusion des analyses sans suppression.** `analytics_excluded` sort une sélection
  des agrégats analytiques (dataset, années, boîtiers, objectifs, focales) **sans effacer
  ses données** : les fichiers restent indexés et consultables, réversible à tout moment.
  Les totaux bruts de photothèque (`files_total/present/missing`, dossiers) décrivent
  toujours toute la base indexée.
- **Purge définitive, sélective ou totale** (`POST /api/v1/purge`, portées
  `folder|year|camera|all`). Contrairement au retrait doux (réversible), elle **efface
  réellement** les lignes de la base ; les fichiers d'origine sur le disque ne sont
  jamais touchés. Refusée pendant une passe d'indexation.
- **API dossiers enrichie.** `POST /api/v1/folders` accepte `removable` et `volume_label` ;
  `PATCH /api/v1/folders/{id}` accepte désormais n'importe quel sous-ensemble de
  `enabled`, `removable`, `volume_label`, `analytics_excluded` (le détail de la
  sélection expose ces trois nouveaux champs).

### Modifié

- **Schéma SQLite v4** : colonnes additives `removable`, `volume_label`,
  `analytics_excluded` sur `folders`. Migration transparente pour une base existante
  (valeurs par défaut neutres, aucun historique perdu).

## [0.5.7] - 2026-08-16

### Retiré

- **`scripts/linux/deploy-config.sh` supprimé, redondant.** Le déploiement de config
  est centralisé dans morfdeploy (`service.py config push --force`, ou `morf config
  deploy morfPhoto`) : même effet (remplace la config déployée depuis le dépôt,
  sauvegarde horodatée, redémarrage), mais **multi-plateforme** (Linux et Windows) et
  sans script bash par projet. README (FR + EN) repointés.

## [0.5.6] - 2026-08-15

### Ajouté

- **Script `scripts/linux/deploy-config.sh`** (il manquait, alors que morfMonitor
  l'avait). Il copie `config/morfphoto.json` (ou l'exemple à défaut) vers
  `/etc/morfsystem/morfphoto/morfphoto.json`, en **sauvegardant** l'ancien fichier
  (`.bak-<date>`) et en **affichant le diff** appliqué, puis redémarre le service.
  Contrairement à `service.py update` (qui n'ajoute que les clés manquantes), il
  **remplace** la valeur déployée - c'est la bonne commande après avoir changé une
  racine ou `watch.interval_ms`. Options `--no-restart`, `--if-absent`, `--help` ;
  testable sans root via `MORF_SUDO`/`MORF_CONFIG_DIR`.

## [0.5.5] - 2026-08-15

### Modifié

- **Indexation à la demande par défaut.** Le défaut de `watch.interval_ms` passe de
  86400000 (une fois par jour) à **`0`** : plus aucune passe automatique, ni périodique
  ni au démarrage. L'indexation ne se fait plus que sur demande explicite (bouton
  PhotoHub ou `POST /api/v1/index`) - aucune pression de fond sur la machine. Une cadence
  périodique reste possible en mettant une valeur positive. Exemple de config et README
  (FR + EN) alignés.

## [0.5.4] - 2026-08-15

### Modifié

- **Cadence de l'indexation automatique bien moins agressive.** Le défaut passe de
  **5 minutes à une fois par jour** (`watch.interval_ms` : 300000 -> 86400000). Une
  passe re-parcourt et `stat` tout l'arbre surveillé plus une sonde par racine :
  toutes les 5 minutes, cela mettait une pression de fond permanente sur la machine,
  surtout sur des montages réseau. L'indexation à la demande (bouton PhotoHub /
  `POST /api/v1/index`) reste évidemment disponible pour prendre les ajouts tout de suite.

### Ajouté

- **Désactivation complète de l'automatique** : `watch.interval_ms` à `0` (ou négatif)
  supprime toute passe de fond - plus de passe périodique ni de passe au démarrage. La
  base n'évolue alors que sur demande explicite. Corrige aussi un piège : `0` faisait
  auparavant tourner le timer en continu.
- **Cadence exposée dans `/api/v1/index/status`** : objet `watch` `{auto, interval_ms}`,
  pour savoir côté client si une passe de fond tourne et à quelle fréquence.

## [0.5.3] - 2026-08-15

### Ajouté

- **Progression d'indexation exposée dans `/api/v1/index/status`.** Pendant une passe,
  l'état observable porte un objet `progress` : `folders_total` (dénominateur fiable,
  connu d'avance), `folders_done`, `files_seen` (compteur cumulé) et `current_folder`.
  L'`Indexer` publie son avancée via un callback (au démarrage, à chaque dossier, et
  toutes les 200 entrées d'un gros dossier) ; `PhotoModule` la recopie sous verrou pour
  le thread HTTP. De quoi matérialiser une vraie barre de progression côté PhotoHub.

## [0.5.2] - 2026-08-14

### Corrigé

- **Troncature des grandes réponses HTTP** dans `HttpServer::reply()`. Resynchronisation
  du correctif issu du patron `morfTemplateService` : la méthode fermait la connexion
  sans drainer le tampon d'écriture, ce qui coupait toute réponse dépassant la taille
  du tampon socket (~20 Ko). On attend désormais que `bytesToWrite()` retombe à zéro
  avant `disconnectFromHost()`.
- Resynchronisation de la copie vendorée de **morfBeacon** (`third_party/morf/beacon`)
  en 0.6.1 : même classe de bug corrigée dans son `StatusServer` (grande réponse
  `/status` coupée faute de drainage du tampon d'écriture).

## [0.5.1] - 2026-08-14

### Ajouté

- **Table des dossiers dans l'export `/api/v1/photos/dataset`.** La colonne
  `folder_id` portait l'identifiant brut ; l'export inclut désormais un objet
  `folders` (id → libellé, le label s'il existe sinon le chemin) pour que la couche
  d'analyse puisse **filtrer et étiqueter par dossier** sans deviner un nom.
  Strictement additif.

## [0.5.0] - 2026-08-14

### Ajouté

- **`GET /api/v1/photos/dataset` : export compact pour la couche d'analyse.** morfPhoto
  reste souverain sur ses données ; morfAnalytics doit pouvoir les rapatrier pour les
  agréger, croiser et filtrer sans jamais lire les fichiers directement. Ce nouvel
  endpoint renvoie les colonnes analytiques de **toutes les photos présentes**
  (`taken_at`, boîtier, objectif, type, focale, focale 35 mm, ouverture, ISO, vitesse
  en secondes, dossier) en **format colonnaire** avec **dictionnaires** pour les
  chaînes répétées (boîtier/objectif/type) : une seule réponse légère même sur une
  photothèque de 20 000+ images. Les valeurs restent **brutes** (aucun regroupement,
  l'interprétation vit chez morfAnalytics) et les **NULL sont préservés** (une donnée
  EXIF absente reste `null`, jamais un 0 trompeur : la couche d'analyse peut ainsi
  montrer la qualité réelle des métadonnées). Changement strictement additif : le
  protocole reste `morfphoto/1`.

## [0.4.0] - 2026-08-14

### Ajouté

- **Tolérance à la disparition d'une source pendant l'indexation.** Une racine peut
  être un montage réseau (SMB/CIFS, NFS...) valide au démarrage d'une passe mais qui
  disparaît en cours de route (l'hôte du partage s'endort ; le noyau attend le timeout
  CIFS, jusqu'à ~180 s, avant de retenter). Jusqu'ici ce cas corrompait la base : le
  scan devenu partiel servait de référence et des fichiers **pourtant toujours présents**
  étaient marqués disparus. Désormais morfPhoto **ne confond plus absence de source et
  suppression de fichiers**.
- **Sonde d'accessibilité bornée dans le temps** (`probeAccessible`) : le stat
  potentiellement bloquant est déporté sur un thread ; au-delà du délai imparti, la
  racine est jugée indisponible au lieu de laisser l'indexation rester figée le temps
  du timeout du montage. Générique à tout montage distant, sans code spécifique à SMB.
- **Verdict de disparition fiabilisé** : un fichier n'est marqué disparu **que si sa
  racine a été parcourue entièrement ET répond toujours à la fin**. Un scan incomplet
  (source perdue en cours) n'applique aucun retrait ; les fichiers déjà acquis restent
  en base. La racine indisponible est journalisée (stage `availability`) et sa
  sélection sautée sans s'acharner sur la source (une sonde par racine et par passe).
- **État de passe `interrupted`** : `index_runs.state` vaut `interrupted` (au lieu de
  `done`) dès qu'une racine a été indisponible. Exposé via `GET /api/v1/index/status`
  (`last_run.state`, `last_run.files_unavailable`) et, en compact, dans `GET /status`
  et `GET /modules` (`last_index_state`) : on distingue une indexation terminée
  normalement d'une passe interrompue par une source injoignable.
- **Schéma SQLite v3** : migration incrémentale (ajout de `index_runs.files_unavailable`),
  appliquée automatiquement à l'ouverture d'une base v2. Le protocole API reste
  `morfphoto/1` (changements strictement additifs).

### Configuration

- **`watch.availability_timeout_ms`** (défaut 5000) : délai borné des sondes
  d'accessibilité d'une racine. Documenté dans `config/morfphoto.example.json`.

## [0.3.2] - 2026-08-14

### Corrigé

- Nettoyage des métadonnées de service héritées du patron morfTemplateService au
  moment du clonage : suppression du drapeau `template: true` (qui faisait sauter
  morfPhoto à l'installation via `morf.py install --services`), correction de
  `status_url` vers le port réservé **8793** au lieu de 8901 (celui du patron,
  qui provoquait un faux « installed but not running » dans `morf doctor`), et
  description propre au service. Description de l'unité systemd corrigée aussi.

## [0.3.1] - 2026-08-14

### Modifié

- Resynchronisation de la copie vendorée de **morfBeacon**
  (`third_party/morf/beacon`) en 0.6.0, alignée sur le dépôt source
  (`IMetricsProvider.h`, `StatusServer.cpp`). Aucun changement de comportement ;
  la copie statique reste en phase avec le parc.

## [0.3.0] - 2026-08-13

### Ajouté

- **Prérequis ExifTool rendu explicite.** ExifTool est une dépendance d'exécution
  obligatoire que `service.py` n'installe pas (paquet système). Sur une machine où
  il manquait, les fichiers étaient bien indexés mais toutes les métadonnées
  restaient nulles (boîtiers, objectifs, années à zéro), sans le moindre signal.
- **Sonde au démarrage** (`ExifExtractor::probe`) : le module photo lance
  `exiftool -ver` une fois au démarrage et retient le verdict.
- **Bloc `exiftool` dans l'état observable** (`GET /api/v1/index/status` et
  `GET /modules`) : `{ available, binary, detail }`. Quand ExifTool est introuvable,
  `last_error` porte désormais un message explicite (« installer le paquet
  libimage-exiftool-perl ») plutôt que de laisser l'extraction échouer en silence.
  Changement strictement additif : le protocole reste `morfphoto/1`.

### Documentation

- **README (FR/EN)** : ExifTool documenté comme prérequis d'exécution, avec la
  commande d'installation (`libimage-exiftool-perl`) et la conduite à tenir sous
  Windows (PATH ou `binary` en chemin complet).
- **`config/morfphoto.example.json`** : commentaire sur le bloc `exiftool` rappelant
  le prérequis système et son effet en cas d'absence.

## [0.2.0] - 2026-08-13

### Ajouté

- **`POST /api/v1/folders/{id}/restore`** : restaure un dossier retiré (annule le
  retrait doux). Le dossier redevient surveillé et une passe d'indexation ravive
  aussitôt ses fichiers.
- **Colonne `deleted_at`** exposée dans `GET /api/v1/folders` : distingue un dossier
  RETIRÉ (retrait doux, horodaté) d'un dossier simplement désactivé. Jusque-là les
  deux valaient `enabled = 0` ; PhotoHub peut désormais présenter les dossiers
  retirés dans une fenêtre séparée.

### Modifié

- **Retrait doux** (`DELETE /api/v1/folders/{id}`) : horodate désormais `deleted_at`.
- **Ré-ajout d'un chemin retiré** (`POST /api/v1/folders`) : restaure le dossier
  existant au lieu d'échouer sur la contrainte d'unicité ; un chemin déjà surveillé
  renvoie un message clair.
- **Schéma SQLite v2** : migration incrémentale (ajout de `folders.deleted_at`),
  appliquée automatiquement à l'ouverture d'une base v1. Le protocole API reste
  `morfphoto/1` (changements strictement additifs).

## [0.1.1] - 2026-08-11

### Ajouté

- **`GET /api/v1/roots`** : expose les racines autorisées (lecture seule ; la
  configuration reste souveraine). Permet à PhotoHub d'afficher le périmètre
  permis et de guider l'utilisateur, qui ne peut déclarer une sélection qu'à
  l'intérieur de ces racines.

## [0.1.0] - 2026-08-11

Première version. morfPhoto naît comme service C++/Qt à partir du gabarit
morfTemplateService ; sa première implémentation avait été prototypée en Python
pour figer les contrats (schéma, API, modèle d'état) avant de passer au C++.

### Ajouté

- **Service permanent d'indexation d'une photothèque locale.** morfPhoto maintient
  en continu une base SQLite qui représente l'état réel des dossiers surveillés :
  il détecte les fichiers nouveaux, modifiés et disparus, en extrait les
  métadonnées et met la base à jour, sans intervention de l'utilisateur. C'est le
  propriétaire unique des métadonnées photo de l'écosystème.
- **Extraction EXIF via ExifTool** (`QProcess` en mode persistant `-stay_open`).
  Les valeurs sont stockées **brutes** : 49, 50 et 51 mm restent 49, 50 et 51 ; un
  RAW et son JPEG sont deux lignes distinctes. Aucune interprétation ici.
- **Indexation incrémentale** fondée sur le triplet `(chemin, taille, mtime)` :
  seuls les fichiers neufs ou modifiés sont ré-extraits. Un fichier disparu est
  marqué absent, jamais supprimé (« observer et conserver, jamais détruire »).
- **Surveillance continue** par réconciliation périodique (cadence configurable).
  Une seule passe à la fois : une demande concurrente est refusée, jamais empilée.
- **Base SQLite** propriété exclusive de morfPhoto, avec migrations de schéma
  (`PRAGMA user_version`) et mode WAL pour lire pendant l'indexation.
- **API HTTP `/api/v1`** : `photos` (liste paginée + filtrée), `photos/{id}`,
  `photos/summary`, `photos/{cameras,lenses,focals,years}`, `index` (déclenchement
  asynchrone) et `index/status`, `folders` (déclaration de sélections sous racines
  autorisées, avec retrait doux préservant l'historique).
- **Racines autorisées + sélections** : la configuration définit le périmètre que
  morfPhoto a le droit d'explorer ; les sélections effectives, gérées via l'API,
  restent toujours à l'intérieur. Une sélection hors racine est refusée (403),
  même par requête forgée.
- **Endpoints socle morfSystem** `/status` et `/healthz`, annonce **morfBeacon**
  avec la capacité `photo_index`, port de service réservé **8793**.
- **Multi-plateforme** : Windows, Linux x86_64 et Raspberry Pi (ARM64), sans
  dépendance tierce hors Qt 6 et ExifTool (morfBeacon est vendoré).
- **Test de fumée headless** (`test/smoke_main.cpp`, option `MORFPHOTO_BUILD_SMOKE`,
  lançable par `ctest`) : couvre la classification des types, le garde-fou de
  périmètre, la souveraineté du mapping EXIF et le cycle d'indexation (verdict
  incrémental, disparu/ravivé, hors-racine refusé, retrait doux), sans ExifTool.
