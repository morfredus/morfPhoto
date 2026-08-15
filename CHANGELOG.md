# Journal des versions - morfPhoto

Le format s'inspire de [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/)
et du [versionnage sémantique](https://semver.org/lang/fr/).

## [0.5.6] - 2026-08-15

### Ajouté

- **Script `scripts/linux/deploy-config.sh`** (il manquait, alors que morfMonitor
  l'avait). Il copie `config/morfphoto.json` (ou l'exemple à défaut) vers
  `/etc/morfsystem/morfphoto/morfphoto.json`, en **sauvegardant** l'ancien fichier
  (`.bak-<date>`) et en **affichant le diff** appliqué, puis redémarre le service.
  Contrairement à `service.py update` (qui n'ajoute que les clés manquantes), il
  **remplace** la valeur déployée — c'est la bonne commande après avoir changé une
  racine ou `watch.interval_ms`. Options `--no-restart`, `--if-absent`, `--help` ;
  testable sans root via `MORF_SUDO`/`MORF_CONFIG_DIR`.

## [0.5.5] - 2026-08-15

### Modifié

- **Indexation à la demande par défaut.** Le défaut de `watch.interval_ms` passe de
  86400000 (une fois par jour) à **`0`** : plus aucune passe automatique, ni périodique
  ni au démarrage. L'indexation ne se fait plus que sur demande explicite (bouton
  PhotoHub ou `POST /api/v1/index`) — aucune pression de fond sur la machine. Une cadence
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
  supprime toute passe de fond — plus de passe périodique ni de passe au démarrage. La
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
