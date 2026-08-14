/*
 * morfPhoto — exemple de demonstration
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Demarre le service avec un module 'example', puis expose l'API. A tester :
 *   curl http://localhost:8901/status
 *   curl http://localhost:8901/modules
 *   curl -X POST http://localhost:8901/example -d '{"hello":"world"}'
 */

#include <QCoreApplication>

#include <morfphoto/Service.h>
#include <morfphoto/ServiceConfig.h>
#include <morfphoto/Version.h>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    morfphoto::ServiceConfig cfg;
    cfg.httpPort         = 8901;
    cfg.beaconIntervalMs = 5000;

    morfphoto::ModuleDef ex;
    ex.type   = QStringLiteral("example");
    ex.id     = QStringLiteral("example-demo");
    ex.params = QJsonObject{ {"type", "example"}, {"period_ms", 3000} };
    cfg.modules.push_back(ex);

    morfphoto::Service service(cfg);
    if (!service.start()) {
        qWarning("API HTTP non demarree (port %u occupe ?)", cfg.httpPort);
        return 1;
    }

    qInfo("morfPhoto demo v%s : %d module(s) ; GET http://localhost:%u/status",
          qUtf8Printable(morfphoto::version()), service.moduleCount(), service.httpPort());

    return app.exec();
}
