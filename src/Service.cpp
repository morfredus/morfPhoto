/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/Service.h"
#include "morfphoto/ModuleRegistry.h"
#include "morfphoto/HttpServer.h"
#include "morfphoto/ModuleFactory.h"
#include "morfphoto/IModule.h"
#include "morfphoto/Version.h"

#include "morfbeacon/Heartbeat.h"
#include "morfbeacon/PresenceConfig.h"

#include <utility>

namespace morfphoto {

Service::Service(ServiceConfig config, QObject* parent)
    : QObject(parent),
      m_config(std::move(config)),
      m_registry(new ModuleRegistry(this)),
      m_http(nullptr) {

    // Construit les modules declares. Une erreur sur l'un (type inconnu,
    // parametre manquant) n'empeche pas les autres : on la note.
    for (const ModuleDef& def : m_config.modules) {
        QString error;
        IModule* m = ModuleFactory::create(def, &error);
        if (!m) {
            m_warnings << error;
            continue;
        }
        m_registry->add(m);
    }

    m_http = new HttpServer(m_config, m_registry, this);
}

Service::~Service() = default;

bool Service::start() {
    m_registry->startAll();
    const bool httpOk = (m_config.httpPort == 0) ? true : m_http->start();

    if (m_config.beaconEnabled) {
        morfbeacon::PresenceConfig pc;
        pc.appName             = m_config.appName;
        pc.version             = morfphoto::version();
        pc.instanceId          = m_config.instanceId;
        // Capacite STABLE annoncee dans le heartbeat : les consommateurs (PhotoHub,
        // morfAnalytics) cherchent 'photo_index', jamais le nom du service.
        pc.capabilities        = { QStringLiteral("photo_index") };
        pc.udpPort             = m_config.beaconUdpPort;
        pc.broadcastIntervalMs = m_config.beaconIntervalMs;
        pc.statusPort          = m_http ? m_http->port() : 0;
        pc.statusBindAddress   = m_config.bindAddress;

        m_heartbeat = new morfbeacon::Heartbeat(pc, m_registry, this);
        m_heartbeat->start();
    }

    return httpOk;
}

void Service::stop() {
    if (m_heartbeat)
        m_heartbeat->stop();
    if (m_http)
        m_http->stop();
    m_registry->stopAll();
}

int Service::moduleCount() const   { return m_registry->count(); }
quint16 Service::httpPort() const  { return m_http ? m_http->port() : 0; }
QStringList Service::warnings() const { return m_warnings; }
ModuleRegistry* Service::registry() const { return m_registry; }

} // namespace morfphoto
