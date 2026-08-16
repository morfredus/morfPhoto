# morfPhoto

*Read in another language: **English** (this document) · [Français](README.fr.md).*

[![Version](https://img.shields.io/badge/version-0.5.7-blue)](CHANGELOG.md)
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
  folder is a soft retire that preserves its history.
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
GET  /api/v1/photos              paginated + filtered list (year, camera, lens, type, folder, state)
GET  /api/v1/photos/{id}         one file
GET  /api/v1/photos/summary      global counters
GET  /api/v1/photos/cameras|lenses|focals|years
POST /api/v1/index               trigger a pass (async): {"mode":"incremental|full"}
GET  /api/v1/index/status        indexing state + last run
GET  /api/v1/folders             watched selections
POST /api/v1/folders             declare a selection (403 outside an allowed root)
PATCH  /api/v1/folders/{id}      enable / disable
DELETE /api/v1/folders/{id}      soft retire (history preserved)
```

Allowed roots are declared in the configuration; selections managed through the API
always stay inside them.

## Build

Needs **Qt 6** (Core, Network, Sql, Concurrent) and, at runtime, **ExifTool**.
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

## License

GPL-3.0-only - © 2026 morfredus (Frédéric Biron).
