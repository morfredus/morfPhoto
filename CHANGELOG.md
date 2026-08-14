# Journal des versions - morfPhoto

Le format s'inspire de [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/)
et du [versionnage sémantique](https://semver.org/lang/fr/).

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
