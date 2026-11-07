# Contribuer à morfPhoto

morfPhoto a une seule responsabilité : être la source de vérité des métadonnées
d'une photothèque locale (voir `ROADMAP.md`).

## 1. Philosophie

- **Qt Core + Network (+ SQL) uniquement.** morfBeacon reste vendoré. Portable
  Windows / Linux x64 / Raspberry Pi.
- **Aucune écriture dans les photos.** Le seul fichier écrit dans une source est
  le `.morfphoto.json` d'un dossier.

## 2. Vérifier une modification

```sh
cmake --preset mingw && cmake --build --preset mingw
ctest --test-dir build-mingw
```

## 3. Style

- C++17, conventions des projets frères : en-tête de licence SPDX, namespace
  `morfphoto`, commentaires en français expliquant le **pourquoi**.
- Fins de ligne : voir `.gitattributes` (LF dans le dépôt ; `.ps1` en CRLF).
