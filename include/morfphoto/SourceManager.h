/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>

namespace morfphoto {

// -----------------------------------------------------------------------------
// SourceManager : sources SMB POUSSEES par PhotoHub.
//
// PhotoHub envoie {host, share, username, password, hostname}. morfPhoto indexe
// des chemins LOCAUX : le helper privilegie monte le partage en cifs lecture
// seule sous /mnt/photos_<slug>, ou slug derive du hostname Windows.
//
// Une machine = un slug = un montage = un .cred = une racine. Jamais de ressource
// generique partagee (/mnt/photos, smb-photos.cred). L'IP sert uniquement a
// joindre le partage, jamais a nommer la source.
//
// Le mot de passe ne transite qu'une fois jusqu'au helper (stdin). morfPhoto
// ne le stocke pas. Metadonnees dans /var/lib/morfsystem/morfphoto/sources.json.
// -----------------------------------------------------------------------------
class SourceManager {
public:
    explicit SourceManager(QString serviceName,
                           QString helperPath = QStringLiteral(
                               "/usr/lib/morfsystem/morfphoto/morfphoto-helper"));

    void load();

    // Monte (ou revalide) une source. hostname = identite canonique (obligatoire,
    // pas une IP). host = cible CIFS (IP ou nom resolvable). `writable` : source
    // qualifiable, montee en lecture/ecriture pour que morfPhoto y ecrive le sidecar
    // `.morfphoto.json` (jamais les photos) ; false = archive en lecture seule.
    // *out recoit le rapport du helper (mountpoint, steps, restart_needed, ...).
    bool addSource(const QString& host, const QString& share, const QString& username,
                   const QString& password, const QString& hostname, bool writable,
                   QJsonObject* out, QString* error);

    QJsonArray  listSources() const;

    // Present, droits, et verbe `probe` (setuid reel). Sans ca PhotoHub n'envoie
    // pas le mot de passe.
    bool helperReady(QJsonObject* out, QString* error) const;
    QStringList mountpoints() const;

    // Redemarrage systemd via le helper, APRES reponse HTTP : le JSON a deja
    // ete valide, le service doit le recharger. No-op hors Linux.
    void scheduleServiceRestart() const;

private:
    QString  storePath() const;
    void     save() const;
    static bool isMounted(const QString& mountpoint);

    QString     m_serviceName;
    QString     m_helperPath;
    QJsonArray  m_sources;
};

} // namespace morfphoto
