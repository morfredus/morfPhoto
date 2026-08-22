/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QRegularExpression>
#include <QString>

namespace morfphoto {

// -----------------------------------------------------------------------------
// Identite canonique d'une source SMB poussee par PhotoHub.
//
// Une machine source = un hostname = un slug = un partage Windows = un point
// de montage = un fichier de credentials = une racine morfPhoto. Le slug est
// derive du hostname Windows (ASUS-DEV -> asus-dev), jamais de l'adresse IP :
// l'IP peut changer, le nom de machine est l'identite.
//
// Convention (identique cote helper privilegie et cote service) :
//   /mnt/photos_<slug>
//   /etc/morfsystem/smb-photos-<slug>.cred
// Les anciens montages /mnt/photos ou /mnt/photos-<hote> ne sont pas migrés
// automatiquement : cette convention s'applique aux nouvelles configs.
// -----------------------------------------------------------------------------

inline QString hostnameSlug(const QString& raw) {
    QString s = raw.trimmed().toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9._-]")), QStringLiteral("-"));
    while (s.startsWith(QLatin1Char('-')) || s.startsWith(QLatin1Char('.')))
        s.remove(0, 1);
    while (s.endsWith(QLatin1Char('-')) || s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return s;
}

inline bool isValidSourceSlug(const QString& slug) {
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9._-]*$"));
    return re.match(slug).hasMatch();
}

inline bool looksLikeIpv4(const QString& raw) {
    static const QRegularExpression re(
        QStringLiteral("^\\d{1,3}(?:\\.\\d{1,3}){3}$"));
    return re.match(raw.trimmed()).hasMatch();
}

inline QString mountpointForSlug(const QString& slug) {
    return QStringLiteral("/mnt/photos_%1").arg(slug);
}

inline QString credentialsPathForSlug(const QString& slug) {
    return QStringLiteral("/etc/morfsystem/smb-photos-%1.cred").arg(slug);
}

inline bool isManagedMountpoint(const QString& path) {
    // Nouveaux points de montage (underscore) ET anciens (tiret) : le helper
    // peut demonter un legacy, mais n'en cree plus.
    static const QRegularExpression re(
        QStringLiteral("^/mnt/photos[_-][a-z0-9][a-z0-9._-]*$"));
    return re.match(path).hasMatch();
}

inline bool isNewConventionMountpoint(const QString& path) {
    static const QRegularExpression re(
        QStringLiteral("^/mnt/photos_[a-z0-9][a-z0-9._-]*$"));
    return re.match(path).hasMatch();
}

} // namespace morfphoto
