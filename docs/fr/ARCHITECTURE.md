# Architecture - morfPhoto

Retour à l'[index de la documentation](README.md).

---

Service Qt (Core + Network), sans interface. Le squelette ne contient **aucun
métier** : tout ce qui est propre à un service donné vit dans des `IModule`.

## Les pièces

```
Service (façade : câble tout à partir d'une ServiceConfig)
├── ModuleRegistry     -> collectionne les IModule, agrège leur état
│     └── IModule (interface, QObject)   ◀── POINT D'EXTENSION (VOTRE MÉTIER)
│            └── ExampleModule  (démo : compteur ; à remplacer)
├── HttpServer         -> API HTTP (GET /status /healthz /modules ; POST /example)
└── morfbeacon::Heartbeat -> annonce UDP (découverte LAN)
        ▲ IMetricsProvider
        └── ModuleRegistry expose un résumé (nombre de modules, ...)
```

### `ServiceConfig` / `ModuleDef`

Chargées depuis un fichier JSON. `ServiceConfig` porte les réglages globaux
(`httpPort`, `bindAddress`, `beacon`) et la liste des modules. Un `ModuleDef`
(`type`, `id`, `params`) décrit un module à instancier. **À enrichir** avec vos
réglages propres.

### `IModule` (interface, QObject) - le point d'extension

C'est **ici** que vit le métier. Une sous-classe implémente `start()`, `stop()`
et `statusJson()` (état exposé dans `/modules`), et peut émettre `updated()`.
`id()`/`type()` l'identifient. Voir `ExampleModule` pour un squelette minimal.

### `ModuleFactory`

Fabrique un `IModule` à partir d'un `ModuleDef`. Point d'extension **compile-time** :
une branche par type ; `knownTypes()` les liste.

### `ModuleRegistry` (QObject + `morfbeacon::IMetricsProvider`)

Détient les modules, les démarre/arrête, agrège leur `statusJson()` pour `/modules`
et fournit un résumé à `/status` (via `IMetricsProvider`).

### `HttpServer` (QObject)

Serveur HTTP/1.1 minimal gérant **GET et POST** (lecture du corps via
`Content-Length`). Routes fournies à titre d'exemple : à adapter à votre API.

### `Service` (façade)

L'unique objet manipulé par le démon : construit les modules (via la fabrique),
démarre le serveur HTTP puis le heartbeat morfBeacon.

## Fil d'exécution

Tout tourne sur **le thread principal Qt**. Les modules travaillent de façon
événementielle (timers, sockets) et exposent un instantané via `statusJson()` ;
le serveur HTTP répond sans bloquer. Un module lent doit rester asynchrone et ne
publier qu'un instantané.

Dans morfPhoto, chaque **passe d'indexation** s'exécute donc sur un thread du pool
(`QtConcurrent`), avec sa propre connexion SQLite : même si une racine réseau se
bloque (montage SMB/CIFS ou NFS muet), `/status` et `/healthz` continuent de
répondre sur le thread principal. La passe elle-même ne reste pas figée
indéfiniment : elle sonde l'accessibilité de chaque racine dans un délai borné
(`watch.availability_timeout_ms`) et, si une source disparaît, interrompt proprement
sa réconciliation **sans marquer les fichiers comme disparus** (un scan partiel
n'est jamais une référence fiable pour un verdict de disparition). L'état de la
dernière passe distingue `done` (terminée normalement) de `interrupted` (source
indisponible), visible dans `/status`, `/modules` et `GET /api/v1/index/status`.

## Dépendance morfBeacon (embarquée)

morfBeacon est vendoré dans `third_party/morf/beacon` (lié statiquement) : build
autonome, sans dépôt externe. Resynchroniser avec `scripts/sync-morf.(sh|ps1)`.

## Portabilité

Aucun code spécifique à une plateforme dans le squelette. Comportement identique
Windows / Linux x64 / Raspberry Pi (ARM64). Installation en service fournie pour
systemd (Linux) et Planificateur de tâches (Windows).
