/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QObject>
#include <QStringList>
#include "morfphoto/ServiceConfig.h"

namespace morfbeacon { class Heartbeat; }

namespace morfphoto {

class ModuleRegistry;
class HttpServer;

// -----------------------------------------------------------------------------
// Service : facade qui cable tout a partir d'une ServiceConfig.
//
//   config JSON
//     -> modules (ModuleFactory) -> ModuleRegistry
//     -> HttpServer (API)
//     -> morfbeacon::Heartbeat (annonce de presence sur le LAN)
//
// Seul objet manipule par le demon (service/main.cpp).
// -----------------------------------------------------------------------------
class Service : public QObject {
    Q_OBJECT
public:
    explicit Service(ServiceConfig config, QObject* parent = nullptr);
    ~Service() override;

    bool start();
    void stop();

    int         moduleCount() const;
    quint16     httpPort() const;
    QStringList warnings() const;

    ModuleRegistry* registry() const;

private:
    ServiceConfig          m_config;
    ModuleRegistry*        m_registry;
    HttpServer*            m_http;
    morfbeacon::Heartbeat* m_heartbeat = nullptr;
    QStringList            m_warnings;
};

} // namespace morfphoto
