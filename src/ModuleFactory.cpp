/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/ModuleFactory.h"
#include "morfphoto/IModule.h"
#include "morfphoto/ExampleModule.h"   // module de demonstration (conserve pour reference)
#include "morfphoto/PhotoModule.h"     // module metier de morfPhoto

namespace morfphoto {
namespace ModuleFactory {

// -----------------------------------------------------------------------------
// POUR AJOUTER UN MODULE METIER :
//   1. ecrire la classe (heriter d'IModule) ;
//   2. ajouter une branche dans create() qui lit ses parametres (def.params) ;
//   3. ajouter son nom dans knownTypes().
// Aucune autre partie du code (registre, serveur HTTP, service) ne change.
// -----------------------------------------------------------------------------

IModule* create(const ModuleDef& def, QString* error, QObject* parent) {
    const QString type = def.type.toLower();

    if (type == QLatin1String("example")) {
        const int periodMs = def.params.value("period_ms").toInt(5000);
        return new ExampleModule(def.id, periodMs, parent);
    }

    // Module metier : indexation permanente de la phototheque.
    if (type == QLatin1String("photo")) {
        return new PhotoModule(def.id, def.params, parent);
    }

    if (error)
        *error = QStringLiteral("type de module inconnu : '%1'").arg(def.type);
    return nullptr;
}

QStringList knownTypes() {
    return { QStringLiteral("example"), QStringLiteral("photo") };
}

} // namespace ModuleFactory
} // namespace morfphoto
