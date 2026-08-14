/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QStringList>
#include "morfphoto/ServiceConfig.h"

class QObject;

namespace morfphoto {

class IModule;

// -----------------------------------------------------------------------------
// ModuleFactory : fabrique un IModule a partir d'une declaration (ModuleDef).
//
// Point d'extension COMPILE-TIME. >>> POUR AJOUTER UN MODULE METIER : ecrivez sa
// classe (heritant d'IModule), ajoutez une branche dans create() et son nom dans
// knownTypes(). Rien d'autre a modifier. <<<
// -----------------------------------------------------------------------------
namespace ModuleFactory {

// Cree le module correspondant a `def`. nullptr si type inconnu / mal configure ;
// `error` (optionnel) est alors renseigne.
IModule* create(const ModuleDef& def, QString* error = nullptr, QObject* parent = nullptr);

// Types de modules connus (ex. {"example"}).
QStringList knownTypes();

} // namespace ModuleFactory

} // namespace morfphoto
