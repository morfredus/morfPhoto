/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QObject>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include "morfbeacon/IMetricsProvider.h"

namespace morfphoto {

class IModule;

// -----------------------------------------------------------------------------
// ModuleRegistry : collection des modules actifs et point d'agregation.
//
// - detient les IModule (en devient le parent Qt) ;
// - demarre / arrete tout le monde ;
// - fabrique la vue JSON servie par /modules ;
// - implemente morfbeacon::IMetricsProvider : /status et le heartbeat exposent
//   ainsi un resume sans effort (>>> a enrichir selon votre metier <<<).
// -----------------------------------------------------------------------------
class ModuleRegistry : public QObject, public morfbeacon::IMetricsProvider {
    Q_OBJECT
public:
    explicit ModuleRegistry(QObject* parent = nullptr);
    ~ModuleRegistry() override;

    void add(IModule* module);                       // le registre en prend possession
    void startAll();
    void stopAll();
    int  count() const;

    QJsonArray  modulesJson() const;                 // [ {id,type,status}, ... ]
    QJsonObject moduleJson(const QString& id, bool* found) const;

    // Premier module d'un type donné, ou nullptr. Sert au serveur HTTP pour
    // atteindre le module métier (ex. « photo ») derrière ses routes /api.
    IModule* firstOfType(const QString& type) const;

    // --- morfbeacon::IMetricsProvider ------------------------------------
    QJsonObject metrics() const override;            // resume pour /status
    QString     state() const override;              // ok | warning | starting

signals:
    void updated(const QString& id);

private:
    QVector<IModule*> m_modules;
};

} // namespace morfphoto
