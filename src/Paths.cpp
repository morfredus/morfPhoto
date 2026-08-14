/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/Paths.h"

#include <QDir>
#include <QByteArray>

namespace morfphoto {

QString Paths::stateDir(const QString& serviceName) {
    // 1. Sous systemd, StateDirectory=morfsystem/<service> cree le dossier avec
    // le bon proprietaire (le User= de l'unite) et le bon mode, puis l'expose
    // via $STATE_DIRECTORY. On l'utilise tel quel : aucun probleme de droits a
    // rattraper. La variable peut lister plusieurs chemins (separes par ':'), le
    // premier est la racine principale.
    const QByteArray env = qgetenv("STATE_DIRECTORY");
    if (!env.isEmpty()) {
        const QString first = QString::fromLocal8Bit(env).split(QLatin1Char(':')).first();
        if (!first.isEmpty()) {
            QDir().mkpath(first);
            return first;
        }
    }

    // 2. Repli hors systemd (execution manuelle, Windows) : emplacement conforme
    // a l'OS. On cree le dossier ; a la difference de /etc il DOIT etre accessible
    // en ecriture au user courant.
#if defined(Q_OS_WIN)
    const QString base = qEnvironmentVariable("ProgramData", QStringLiteral("C:/ProgramData"));
    const QString dir  = QDir(base).filePath(QStringLiteral("morfsystem/") + serviceName + QStringLiteral("/state"));
#else
    const QString dir  = QStringLiteral("/var/lib/morfsystem/") + serviceName;
#endif
    QDir().mkpath(dir);
    return dir;
}

QString Paths::overrideConfigFile(const QString& serviceName) {
    return QDir(stateDir(serviceName)).filePath(serviceName + QStringLiteral(".overrides.json"));
}

} // namespace morfphoto
