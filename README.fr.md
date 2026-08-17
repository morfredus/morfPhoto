# morfPhoto

*Lire dans une autre langue : [English](README.md) · **Français** (ce document).*

[![Version](https://img.shields.io/badge/version-0.7.0-blue)](CHANGELOG.md)
![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)
![Qt](https://img.shields.io/badge/Qt-6-41CD52?logo=qt)
![Build](https://img.shields.io/badge/CMake-3.21+-064F8C?logo=cmake)
![License](https://img.shields.io/badge/License-GPL--3.0--only-blue)

**morfPhoto maintient une base locale qui reflète en permanence l'état réel des
dossiers photo surveillés.** C'est le propriétaire unique des métadonnées photo
dans l'écosystème morfSystem.

C'est un **service permanent** : tant qu'il tourne, il est le propriétaire des
métadonnées photo et les sert en HTTP. Il réconcilie les dossiers configurés - repère
les fichiers nouveaux, modifiés ou disparus, extrait leurs métadonnées avec ExifTool -
**à la demande par défaut**, ou selon une cadence périodique quand `watch.interval_ms`
en définit une. La base n'est pas un export des dossiers : elle en est la
représentation. morfPhoto ne produit **aucune analyse métier** - regroupement,
déduplication et interprétation vivent dans une couche distincte (morfAnalytics).

## Ce qu'il fait

- **Indexe une photothèque locale.** Les racines configurées sont parcourues
  récursivement ; le triplet `(chemin, taille, mtime)` décide seul de ce qui doit
  être retraité (pas de hash complet).
- **Extrait l'EXIF via ExifTool** (`QProcess`, mode persistant `-stay_open`) et
  stocke les valeurs **brutes** : 49, 50 et 51 mm restent 49, 50 et 51 ; un RAW et
  son JPEG sont deux lignes distinctes. La vitesse est gardée en `1/250` et en
  secondes.
- **Indexe à la demande par défaut** : `watch.interval_ms` vaut `0` par défaut, donc
  aucune passe de fond - la base n'est mise à jour que sur demande (bouton PhotoHub ou
  `POST /api/v1/index`), sans aucune pression de fond sur la machine. Mettre une cadence
  positive (ex. `86400000`, une fois par jour) pour réactiver une réconciliation
  périodique. Une seule passe à la fois - une demande concurrente est refusée, jamais
  empilée.
- **Ne détruit jamais implicitement** : un fichier disparu est marqué absent ;
  retirer un dossier est un retrait doux qui conserve son historique. La suppression
  définitive existe (`POST /api/v1/purge`, par dossier, année, boîtier ou totale),
  mais elle reste un geste **explicite** et confirmé, jamais automatique.
- **Gère les supports amovibles** (CD/DVD, disques d'archive). Une sélection déclarée
  amovible n'a **jamais** ses photos marquées disparues quand le support est absent -
  même si le point de montage est resté présent mais vide (le piège d'un CD éjecté).
  Les photos gravées restent donc dans la base et dans morfAnalytics, support retiré et
  après un redémarrage. Un nom de volume optionnel aide à reconnaître le disque.
- **Sépare l'analyse de la conservation** : une sélection peut être sortie des analyses
  (`analytics_excluded`) sans que ses données soient effacées - réversible à tout moment.
- **Tolère la disparition d'une source distante** : une racine peut être un montage
  réseau (SMB/CIFS, NFS...) qui devient injoignable en pleine passe. morfPhoto ne
  confond pas absence de source et suppression : il ne marque un fichier disparu que
  si sa racine a été parcourue jusqu'au bout de façon fiable. Sinon la passe est
  interrompue proprement (`last_run.state = interrupted`), les fichiers déjà indexés
  restent acquis, et une passe ultérieure reprend au retour de la source. Le délai
  des sondes d'accessibilité est réglable (`watch.availability_timeout_ms`).
- **Expose une API HTTP stable** et s'annonce sur le réseau local avec morfBeacon
  et la capacité `photo_index`.

## Contrat HTTP (`/api/v1`)

```
GET  /status                     diagnostic riche (compatible morfBeacon)
GET  /healthz                    vivant ?
GET  /api/v1/photos              liste paginée + filtrée (year, camera, lens, type, folder, state)
GET  /api/v1/photos/{id}         une fiche
GET  /api/v1/photos/summary      compteurs globaux
GET  /api/v1/photos/cameras|lenses|focals|years
POST /api/v1/index               déclenche une passe (async) : {"mode":"incremental|full"}
GET  /api/v1/index/status        état d'indexation + dernière passe
GET  /api/v1/folders             sélections surveillées
POST /api/v1/folders             déclare une sélection (403 hors d'une racine autorisée) ; champs optionnels removable, volume_label
PATCH  /api/v1/folders/{id}      modifie enabled, removable, volume_label, analytics_excluded (sous-ensemble libre)
DELETE /api/v1/folders/{id}      retrait doux (historique conservé)
POST /api/v1/purge               suppression DÉFINITIVE : {"scope":"folder|year|camera|all","value":...}
```

Les racines autorisées sont déclarées dans la configuration ; les sélections
gérées par l'API restent toujours à l'intérieur.

## Compiler

Nécessite **Qt 6** (Core, Network, Sql, Concurrent) et, à l'exécution, **ExifTool**.
morfBeacon est vendoré dans `third_party/morf/beacon`.

**ExifTool est une dépendance d'exécution obligatoire**, et `service.py` ne
l'installe pas (c'est un paquet système, pas un produit de la compilation). Sans
lui, le service tourne quand même et indexe les fichiers, mais chaque extraction de
métadonnées échoue et les colonnes EXIF restent vides (aucun boîtier, objectif ni
année). L'installer depuis la distribution :

```sh
sudo apt install libimage-exiftool-perl   # Debian, Ubuntu, Raspberry Pi OS
```

Sous Windows, installer ExifTool puis placer `exiftool.exe` dans le `PATH`, ou
pointer `modules[].exiftool.binary` de la configuration sur son chemin complet. Une
fois installé, `GET /api/v1/index/status` renvoie `exiftool.available: true` ; s'il
manque, le même endpoint le signale dans `last_error`.

```sh
cmake --preset mingw        # ou linux / linux-arm64
cmake --build --preset mingw
```

## Lancer

```sh
./build-mingw/service/morfphoto.exe --config config/morfphoto.json
curl http://127.0.0.1:8793/api/v1/photos/summary
```

## Installer en service

```sh
# Toutes plateformes : Linux, Windows, Raspberry Pi
sudo ./service.py install      # compile si besoin, installe, démarre
sudo ./service.py update       # recompile, remplace le binaire, redémarre
sudo ./service.py uninstall    # désinscrit, en conservant votre configuration
./service.py status            # ce que le système en dit
```

Le déploiement est vendoré dans `third_party/morf/morfdeploy` ; resynchroniser les
copies vendorées avec `scripts/sync-morf.sh`.

Pour **redéployer la configuration** vers `/etc/morfsystem/morfphoto/` après l'avoir
modifiée (une racine, `watch.interval_ms`, le chemin d'ExifTool...), sans tout
recompiler, utiliser la commande unifiée du parc (sauvegarde horodatée, puis
redémarre ; Linux comme Windows) :

```sh
sudo ./service.py config push --force     # ou, depuis la racine du parc : morf config deploy morfPhoto
```

Contrairement à `service.py update` (qui n'ajoute que les clés manquantes, sans jamais
écraser), `config push` remplace le fichier déployé par celui du dépôt - garder un vrai
`config/morfphoto.json` dans le clone, avec ses racines, comme référence déployée.
(Sans mode, `service.py config` n'ajoute que les clés d'une nouvelle version en gardant
vos réglages.)

## Licence

GPL-3.0-only - © 2026 morfredus (Frédéric Biron).
