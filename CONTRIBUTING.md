# Contribuer à morfPhoto

Ce dépôt est le **squelette commun** des services morfSystem. Modifier ce template
impacte tous les futurs services : rester générique et prudent.

## 1. Philosophie

- **Aucun métier ici.** Le template ne contient que la mécanique commune (config,
  registre, serveur HTTP, annonce, service). Le métier vit dans les modules des
  projets qui clonent ce template.
- **Fonctionnel du premier coup.** Après clonage (`new-service.*`), le projet doit
  compiler et tourner sans modification.
- **Qt Core + Network uniquement.** morfBeacon reste vendoré. Portable Windows /
  Linux x64 / Raspberry Pi.
- **Renommable.** Tout nom propre au template doit rester l'un des trois jetons
  gérés par les scripts de clonage : `morfPhoto`, `morfphoto`,
  `MORFPHOTO`. Ne pas introduire d'autre variante de nom.

## 2. Faire évoluer le template

Toute pièce ajoutée doit être **utile à tous les services** (voir ROADMAP). Après
modification, vérifier que le clonage produit toujours un projet compilable :

```sh
scripts/new-service.sh morftest morfTest /tmp/morfTest
cd /tmp/morfTest && cmake --preset mingw && cmake --build --preset mingw
```

## 3. Style

- C++17, conventions des projets frères : en-tête de licence SPDX, namespace
  `morfphoto`, commentaires en français expliquant le **pourquoi**, marqueurs
  `>>> A ADAPTER <<<` aux endroits que le développeur doit personnaliser.
- Fins de ligne : voir `.gitattributes` (LF dans le dépôt ; `.ps1` en CRLF).
