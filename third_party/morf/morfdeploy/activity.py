"""Emission (optionnelle) d'une activite de build vers morfAnalytics Monitor.

morfDeploy sait ce qu'il compile (projet, preset, debut/fin, resultat) : c'est la
source naturelle et exacte des evenements de compilation, sans rien faire deviner a
personne (cf. domaine Monitor de morfAnalytics). Il les signale ici.

Principe : best-effort et sans dependance. Si l'URL d'ingestion n'est pas
configuree, rien n'est emis ; si morfAnalytics est injoignable, on n'en fait pas une
erreur -- une telemetrie muette ne doit jamais faire echouer un build.
"""
from __future__ import annotations

import json
import os
import socket
import urllib.request
from pathlib import Path

# Ou trouver l'URL d'ingestion des activites du domaine Monitor de morfAnalytics.
# Deux sources, dans l'ordre :
#   1. la variable d'environnement MORFANALYTICS_ACTIVITY_URL (ex.
#      "http://pi4fred:8799/api/monitor/activity") -- pratique pour `morf build`,
#      qui tourne sous le compte utilisateur ;
#   2. a defaut, un fichier /etc/morfsystem/monitor-activity-url (une ligne).
#      Necessaire car `sudo service.py update` efface l'environnement : un fichier
#      admin, lu quel que soit l'appelant, est le moyen robuste et pose une fois.
# Aucune des deux => aucune emission. morfDeploy ne depend jamais de morfAnalytics.
_ENV_URL = "MORFANALYTICS_ACTIVITY_URL"
_FILE_URL = Path("/etc/morfsystem/monitor-activity-url")


def _target_url() -> str | None:
    env = os.environ.get(_ENV_URL)
    if env and env.strip():
        return env.strip()
    try:
        if _FILE_URL.is_file():
            text = _FILE_URL.read_text(encoding="utf-8").strip()
            return text or None
    except OSError:
        pass
    return None


def _project_name(repo_root: Path) -> str:
    name = Path(repo_root).name
    # Le nom de projet ne porte jamais le suffixe du bac a sable de developpement.
    if name.endswith("_travail"):
        name = name[: -len("_travail")]
    return name


def emit_build_activity(repo_root: Path, preset: str, start: float, end: float,
                        ok: bool) -> None:
    """Signale une compilation a morfAnalytics, si configure. Ne leve jamais."""
    url = _target_url()
    if not url:
        return
    payload = {
        "type": "compile",
        "project": _project_name(repo_root),
        "machine": socket.gethostname(),
        "start": int(start),
        "end": int(end),
        "status": "success" if ok else "failed",
        "metadata": {"preset": preset},
    }
    try:
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            url, data=data, headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=3).read()
    except Exception:  # noqa: BLE001 - injoignable, timeout, DNS... jamais bloquant
        pass
