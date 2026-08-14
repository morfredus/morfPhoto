/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/ModuleRegistry.h"
#include "morfphoto/IModule.h"

namespace morfphoto {

ModuleRegistry::ModuleRegistry(QObject* parent) : QObject(parent) {}
ModuleRegistry::~ModuleRegistry() = default;

void ModuleRegistry::add(IModule* module) {
    if (!module)
        return;
    module->setParent(this);
    m_modules.push_back(module);
    connect(module, &IModule::updated, this, &ModuleRegistry::updated);
}

void ModuleRegistry::startAll() { for (IModule* m : m_modules) m->start(); }
void ModuleRegistry::stopAll()  { for (IModule* m : m_modules) m->stop(); }
int  ModuleRegistry::count() const { return m_modules.size(); }

QJsonArray ModuleRegistry::modulesJson() const {
    QJsonArray arr;
    for (const IModule* m : m_modules) {
        QJsonObject o;
        o["id"]     = m->id();
        o["type"]   = m->type();
        o["status"] = m->statusJson();
        arr.append(o);
    }
    return arr;
}

QJsonObject ModuleRegistry::moduleJson(const QString& id, bool* found) const {
    for (const IModule* m : m_modules) {
        if (m->id() == id) {
            if (found) *found = true;
            QJsonObject o;
            o["id"]     = m->id();
            o["type"]   = m->type();
            o["status"] = m->statusJson();
            return o;
        }
    }
    if (found) *found = false;
    return QJsonObject{};
}

IModule* ModuleRegistry::firstOfType(const QString& type) const {
    for (IModule* m : m_modules)
        if (m->type() == type)
            return m;
    return nullptr;
}

QJsonObject ModuleRegistry::metrics() const {
    // >>> A ENRICHIR : exposez ici les compteurs pertinents pour votre service.
    QJsonObject m;
    m["modules"] = m_modules.size();
    return m;
}

QString ModuleRegistry::state() const {
    if (m_modules.isEmpty())
        return QStringLiteral("starting");
    return QStringLiteral("ok");
}

} // namespace morfphoto
