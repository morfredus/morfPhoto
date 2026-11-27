# morfPhoto

*Read in another language: **English** (this document) · [Français](README.fr.md).*

[![Version](https://img.shields.io/badge/version-0.24.0-blue)](CHANGELOG.md)
![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)
![Qt](https://img.shields.io/badge/Qt-6-41CD52?logo=qt)
![Build](https://img.shields.io/badge/CMake-3.21+-064F8C?logo=cmake)
![License](https://img.shields.io/badge/License-GPL--3.0--only-blue)

**morfPhoto keeps a local database that continuously mirrors the real state of the
watched photo folders.** It is the single source of truth for photo metadata in the
morfSystem ecosystem.

It is a **permanent service**: as long as it runs it owns the photo metadata and
serves it over HTTP. It reconciles the configured folders - detecting new, modified
and missing files and extracting their metadata with ExifTool - **on demand by
default**, or on a periodic schedule when `watch.interval_ms` sets one. The database
is not an export of the folders: it is their representation. morfPhoto produces **no
business analysis** - grouping, deduplication and interpretation belong to a separate
layer (morfAnalytics).

## What it does

- **Indexes a local photo library.** Configured roots are walked recursively; the
  `(path, size, mtime)` triple decides what needs re-processing (no full hashing).
- **Extracts EXIF via ExifTool** (`QProcess`, persistent `-stay_open` mode) and
  stores values **raw**: 49, 50 and 51 mm stay 49, 50 and 51; a RAW and its JPEG
  are two distinct rows. Shutter speed is kept both as `1/250` and in seconds.
- **Indexes on demand by default**: `watch.interval_ms` defaults to `0`, meaning no
  background pass runs - the database updates only when asked (PhotoHub button or
  `POST /api/v1/index`), for zero background pressure on the machine. Set a positive
  cadence (e.g. `86400000`, once a day) to also run a periodic reconciliation. One
  pass at a time - a concurrent request is refused, never queued.
- **Never destroys implicitly**: a vanished file is marked missing; removing a
  folder is a soft retire that preserves its history. Permanent deletion exists
  (`POST /api/v1/purge`, by folder, year, camera or all), but it stays an **explicit**,
  confirmed action, never automatic.
- **Handles removable media** (CD/DVD, archive disks). A selection flagged removable
  never has its photos marked missing while the medium is absent - even if the mount
  point stayed present but empty (the ejected-CD trap). Burned photos therefore remain
  in the database and in morfAnalytics, medium removed and after a restart. An optional
  volume label helps recognise the disk.
- **Separates analysis from preservation**: a selection can be taken out of analytics
  (`analytics_excluded`) without erasing its data - reversible at any time.
- **Tolerates a disappearing remote source**: a root may be a network mount
  (SMB/CIFS, NFS...) that becomes unreachable mid-pass. morfPhoto does not mistake an
  absent source for deletion: a file is only marked missing when its root was scanned
  to completion reliably. Otherwise the pass is cleanly interrupted
  (`last_run.state = interrupted`), already-indexed files stay, and a later pass
  resumes when the source returns. The accessibility-probe timeout is configurable
  (`watch.availability_timeout_ms`).
- **Serves a stable HTTP API** and announces itself on the LAN via morfBeacon with
  the `photo_index` capability.

## HTTP contract (`/api/v1`)

```
GET  /status                     rich diagnostic (morfBeacon-compatible)
GET  /healthz                    liveness
GET  /api/v1/photos              paginated + filtered list (year, camera, lens, type, folder, directory, state)
GET  /api/v1/photos/{id}         one file
GET  /api/v1/photos/summary      global counters
GET  /api/v1/photos/cameras|lenses|focals|years
POST /api/v1/index               trigger a pass (async): {"mode":"incremental|full"}
GET  /api/v1/index/status        indexing state + last run
                                 (progress.phase, files_seen, files_total, files_total_final, percent;
                                  percent adapts until files_total_final is true)
GET  /api/v1/folders             watched selections
POST /api/v1/folders             declare a selection (403 outside an allowed root); optional removable, volume_label
PATCH  /api/v1/folders/{id}      update enabled, removable, volume_label, analytics_excluded (any subset)
DELETE /api/v1/folders/{id}      soft retire (history preserved)
GET  /api/v1/contexts            directories + photographic context (filter ?status=qualified|unqualified|invalid)
GET  /api/v1/context             one directory's context (?directory=...); unqualified if no valid .morfphoto.json
PUT  /api/v1/context             write .morfphoto.json: {directory, context, subject, motif?, description?} (context+subject required)
DELETE /api/v1/context           remove .morfphoto.json (?directory=...): the folder becomes unqualified again
GET  /api/v1/thumbnail           image/jpeg grid thumbnail of a file (?path=...), 400 px max; JPEG read directly, RAW via its embedded preview (exiftool); cached on disk
GET  /api/v1/library             photo library: photos + versions (rating, rejected, labels, keywords, edited), filterable, `page`/`page_size` or `cursor`; ETag/304; off unless `sidecars.providers` is set
GET  /api/v1/library/{id}        one photo of the library (also `?ids=1,2,3`)
GET  /api/v1/library/facets      counts per filter value, each list computed with every filter but its own
GET  /api/v1/library/subfolders  direct subfolders (?folder_path=...) with their photo counts
GET  /api/v1/library/sidecars    report: active providers, photos per provider, unreadable sidecars
GET  /api/v1/photos/dataset?with=library,gps   analysis export plus the library columns (one row per photo) and/or the position (`gps`)
GET  /api/v1/roots               allowed roots (config + validated SMB mounts)
GET  /api/v1/sources             pushed SMB sources (+ mounted, reachable)
GET  /api/v1/sources/ready       privileged helper startable
POST /api/v1/sources             mount a Windows share under /mnt/photos_<hostname>
POST /api/v1/purge               PERMANENT deletion: {"scope":"folder|year|camera|all","value":...}
```

Allowed roots are declared in the configuration; selections managed through the API
always stay inside them.

### API key (optional)

By default the API is open, as it has always been. To protect it, set `api_key` in
`morfphoto.json` (or the `MORFPHOTO_API_KEY` environment variable, which wins): every
`/api/` request must then carry `Authorization: Bearer <key>`, otherwise it gets `401`.
`/healthz`, `/status` and `/modules` stay open so fleet supervision keeps working;
`/status` only reports `api_key_required` (never the key). Generate a key with
`openssl rand -hex 32`. Clients that read the API must be given the key: PhotoHub asks
for it (File > API key) and stores it encrypted on the Windows account; morfAnalytics (0.80.0 and later) takes it in its `photo` module
(`api_key` or `api_keys`).

### Photo library (optional, read-only)

Next to the EXIF, programs leave their own layer beside the images: star rating, reject flag,
colour labels, keywords, title, "edited or not", versions. morfPhoto can read it, following the
`morfphoto-library/1` contract (morfSystem repository, `docs/CONTRAT-BIBLIOTHEQUE-PHOTO.md`).
It is **off by default**; naming at least one provider turns it on:

```json
"sidecars": { "providers": ["darktable", "xmp", "dxo", "lightcraft"] }
```

- `darktable`: `photo.ext.xmp` and `photo_NN.ext.xmp` (virtual copies become versions).
- `xmp`: generic XMP (Lightroom and Camera Raw, Bridge, digiKam, XnView...), `photo.xmp` or
  `photo.ext.xmp`, shared by a RAW and its JPEG.
- `dxo`: DxO PhotoLab (`photo.ext.dop`, a text file of nested tables): rating, keywords, position and copyright notice.
  "Edited" stays unknown (PhotoLab writes a `.dop` as soon as an image is opened, with automatic corrections already
  applied) and the virtual copies are not read yet.
- `lightcraft`: the `lc:flag` that Lightcraft writes in the same `.xmp` as darktable: `pick` is a picked photo (`picked: true`), `none` no flag (`false`) and `reject` a rejected photo
  (`rejected: true`). Any other value stays unknown, and `lc:settings` is not read. Enable it next to `darktable`: the flag is attributed to `lightcraft`.
- `file`: the XMP embedded in the image itself (read through ExifTool), always last.

The list order is the priority order and each field is resolved on its own: the first provider
with a value wins. Two fields are the exception and follow "one provider is enough": **edited** and **rejected**
are true as soon as one provider says so (a rejection made in one program is a gesture another program cannot undo). The EXIF of the image stays the authority on the shot; sidecars never replace
it. An unknown value is `null`, never a default (a missing rating is not 0 stars). morfPhoto never
writes a sidecar. Turning the library on re-reads each photo once; a later pass only re-reads the
photos whose sidecars changed. Capture One, RawTherapee and ON1 are not read yet. A version carries `picked` (true, false or `null`) and `/library` filters on `?picked=true|false`.

`/library` pages with `cursor` (the `next` field of the previous page) instead of `page` when the catalogue may change between two reads: no photo is skipped or repeated. `/library` and `photos/dataset?with=library` carry an `ETag` (the catalogue revision, new after each indexing pass and each write); a client that sends it back in `If-None-Match` gets `304` without any work done on the server.

## Build

Needs **Qt 6** (Core, Network, Sql, Concurrent) and, at runtime, **ExifTool** and the **Qt SQLite
driver** (`libqt6sql6-sqlite` on Debian: the `.deb` pulls it in, a source install needs it by hand;
without it the index fails with "Driver not loaded").
morfBeacon is vendored under `third_party/morf/beacon`.

**ExifTool is a required runtime dependency**, and `service.py` does not install it
(it is a system package, not something the build produces). Without it the service
still runs and still indexes files, but every metadata extraction fails and the EXIF
columns stay empty (no cameras, lenses or years). Install it from your distribution:

```sh
sudo apt install libimage-exiftool-perl   # Debian, Ubuntu, Raspberry Pi OS
```

On Windows, install ExifTool and either put `exiftool.exe` on the `PATH` or point
`modules[].exiftool.binary` in the configuration at its full path. Once installed,
`GET /api/v1/index/status` reports `exiftool.available: true`; if it is missing, the
same endpoint says so in `last_error`.

```sh
cmake --preset mingw        # or linux / linux-arm64
cmake --build --preset mingw
```

## Run

```sh
./build-mingw/service/morfphoto.exe --config config/morfphoto.json
curl http://127.0.0.1:8793/api/v1/photos/summary
```

## Install as a service

```sh
# Any platform: Linux, Windows, Raspberry Pi
sudo ./service.py install      # build if needed, install, start
sudo ./service.py update       # rebuild, replace the binary, restart
sudo ./service.py uninstall    # deregister, keeping your configuration
./service.py status            # what the system says about it
```

Deployment is vendored under `third_party/morf/morfdeploy`; resynchronise the
vendored copies with `scripts/sync-morf.sh`.

To **redeploy the configuration** to `/etc/morfsystem/morfphoto/` after editing it
(a root, `watch.interval_ms`, ExifTool path...), without a full rebuild, use the
parc's unified command (backs up a timestamped copy, then restarts; Linux and
Windows alike):

```sh
sudo ./service.py config push --force     # or, from the parc root: morf config deploy morfPhoto
```

Unlike `service.py update` (which only adds missing keys, never overwrites), `config
push` replaces the deployed file with the one from the repo - keep a real
`config/morfphoto.json` in your clone, with your roots, as the deployed reference.
(Use `service.py config` with no mode to only add a new version's keys while keeping
your settings.)

## Continuous integration

GitHub Actions workflows live in `.github/workflows/` (generated by the morfTools script
`gen-ci-workflows.py`):

- **`ci.yml`**: on every pull request (and push to `main`/`dev`), builds and runs the tests on
  Ubuntu 24.04 (Qt 6.4), Linux arm64 and Windows (MSYS2 UCRT64). Documentation-only changes skip it and an outdated run is cancelled by a
  newer commit.
- **`packages.yml`**: on every push to `main` or a manual run (*Actions > Packages > Run
  workflow*), builds the `.deb` amd64, the `.deb` arm64 and the Windows packages. Packages are workflow artifacts kept 14 days; releases are
  published by the morfSystem tooling, which takes the packages of the run of the released
  commit and builds locally only what is missing.

## License

GPL-3.0-only - © 2026 morfredus (Frédéric Biron).
