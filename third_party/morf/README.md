# Modules communs « morf » (copie vendorée)

Copie autonome de la bibliothèque partagée de morfredus utilisée par morfPhoto :

- `beacon/` — **morfBeacon** : supervision LAN (heartbeat UDP « je suis actif » +
  endpoint HTTP `/status`). morfPhoto s'en sert pour s'annoncer sur le réseau.

> morfPhoto n'embarque **que** morfBeacon (contrairement à ComponentHub, qui
> embarque aussi `update/`) : c'est un service sans interface, il n'a pas besoin
> du dialogue de mise à jour morfUpdate.

## Pourquoi vendoré ?

Le projet se compile **sans dépendre d'aucun dépôt externe** : le code est ici et
lié **statiquement** dans l'exécutable. Compilation parfaite et fonctionnelle du
premier coup, sur Windows, Linux x64 et Raspberry Pi (ARM64), avec le même
toolchain et le même Qt que le service.

## Mise à jour

Ne pas éditer le code ici : la **source de vérité** est le dépôt voisin
`morfBeacon`. Pour resynchroniser :

```sh
# Windows
powershell -ExecutionPolicy Bypass -File scripts\sync-morf.ps1
# Linux / Raspberry Pi
scripts/sync-morf.sh
```

Le script recopie `include/`, `src/` et `VERSION` ; il ne touche pas au
`CMakeLists.txt` vendoré (volontairement allégé : ni presets, ni exemples). Si le
dépôt source est ailleurs, définir la variable d'environnement `MORF_SRC_BASE`.

Version vendorée : voir `beacon/VERSION`.
