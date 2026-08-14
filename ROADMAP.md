# Roadmap - morfPhoto

Ce dépôt est un **template** : il doit rester minimal, générique et fonctionnel du
premier coup. On n'y ajoute que ce qui sert à **tous** les services, jamais du
métier.

## Pistes

- **Tests d'exemple** (un petit test du serveur HTTP et du registre) que les
  services clonés héritent.
- **CI d'exemple** (`.github/workflows`) : build multi-plateforme.
- **`scripts/windows/update-service.ps1`** (pendant Windows de `service.py update`).
- **Option d'authentification** du serveur HTTP (jeton) réutilisable.
- **Squelette de `docs/fr/PROTOCOL.md`** à remplir par le service cloné.
- **Rechargement de config** (SIGHUP) générique.

## Non-objectifs

- Aucune logique métier (capteurs, notifications, collecte…) : elle vit dans les
  services qui clonent ce template.
- Pas de dépendance externe au-delà de Qt (morfBeacon reste vendoré).
