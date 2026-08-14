# Créer votre service à partir du template

Retour à l'[index de la documentation](README.md).

---

Guide pas à pas pour transformer ce squelette en un vrai service.

## 1. Cloner le template

```sh
scripts/new-service.sh morfwatch morfWatch        # Linux/macOS
# scripts\new-service.ps1 morfwatch morfWatch     # Windows
```

Un nouveau dossier `../morfWatch` est créé, tous les noms remplacés. Il
**compile déjà** :

```sh
cd ../morfWatch
cmake --preset mingw && cmake --build --preset mingw
```

## 2. Coder un module (le métier)

Renommez `src/ExampleModule.*` (ex. `TemperatureModule`) et implémentez `IModule` :

```cpp
// include/morfwatch/TemperatureModule.h
#pragma once
#include "morfwatch/IModule.h"

namespace morfwatch {
class TemperatureModule : public IModule {
    Q_OBJECT
public:
    explicit TemperatureModule(const QString& id, QObject* parent = nullptr)
        : IModule(id, QStringLiteral("temperature"), parent) {}
    bool start() override;                 // ouvrir le capteur, lancer un timer...
    void stop() override;
    QJsonObject statusJson() const override; // { "celsius": 21.4, ... }
};
} // namespace morfwatch
```

Poussez vos mesures de façon **asynchrone** ; `statusJson()` renvoie un instantané.

## 3. Enregistrer le type

Dans `src/ModuleFactory.cpp` :

```cpp
#include "morfwatch/TemperatureModule.h"
// ...
if (type == QLatin1String("temperature"))
    return new TemperatureModule(def.id, parent);
```

et ajouter `"temperature"` à `knownTypes()`.

## 4. Déclarer les sources dans CMake

Dans `CMakeLists.txt`, remplacer `ExampleModule.*` par vos fichiers (repères
`>>> A ADAPTER <<<`).

## 5. Adapter l'API HTTP

Dans `src/HttpServer.cpp`, remplacer la route `POST /example` par vos endpoints
métier (ex. `POST /command`), ou en ajouter. Les routes GET (`/status`, `/healthz`,
`/modules`) sont génériques et peuvent rester telles quelles.

## 6. Configurer

Éditer `config/morfwatch.example.json` : réglages globaux + liste `modules` avec
vos types et paramètres.

## 7. Installer en service

```sh
# Linux
sudo ./service.py install
journalctl -u morfwatch -f
# Windows (PowerShell Administrateur)
```

## 8. Finitions

- Adapter les commentaires `>>> A ADAPTER <<<` restants.
- Mettre à jour `README`, `CHANGELOG`, `docs/`.
- Rester dans l'esprit morfSystem : `/status` compatible morfBeacon, annonce LAN
  active, un binaire autonome.
