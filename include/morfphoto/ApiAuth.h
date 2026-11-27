/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QByteArray>
#include <QCryptographicHash>

namespace morfphoto {

// -----------------------------------------------------------------------------
// Cle d'API partagee, OPTIONNELLE (`Authorization: Bearer <cle>`).
//
// Pourquoi : morfPhoto ecoute sur le LAN (0.0.0.0) et son API lit le disque (vignettes)
// et ecrit des fichiers (contexte, sources SMB). Sur un reseau partage ou expose, une
// cle empeche le premier venu de s'en servir. SANS cle configuree, tout passe : aucun
// client existant du parc n'est casse, le choix de la proteger appartient a l'admin.
//
// Seules les routes metier `/api/` sont protegees. `/healthz`, `/status` et `/modules`
// restent ouverts : la supervision du parc (morfMonitor, beacon) doit continuer de
// fonctionner sans connaitre la cle.
// -----------------------------------------------------------------------------

inline bool isApiPath(const QByteArray& path) {
    return path.startsWith("/api/");
}

// Vrai si l'en-tete `Authorization` porte la cle attendue (ou si aucune cle n'est exigee).
// On compare des empreintes SHA-256 sur toute leur longueur : le temps de reponse ne
// renseigne ni sur le contenu de la cle ni sur sa taille.
inline bool isAuthorized(const QByteArray& authorization, const QByteArray& apiKey) {
    if (apiKey.isEmpty())
        return true;
    const QByteArray value = authorization.trimmed();
    if (value.left(7).toLower() != "bearer ")
        return false;
    const QByteArray given    = QCryptographicHash::hash(value.mid(7).trimmed(), QCryptographicHash::Sha256);
    const QByteArray expected = QCryptographicHash::hash(apiKey, QCryptographicHash::Sha256);
    char diff = 0;
    for (int i = 0; i < expected.size(); ++i)
        diff |= given.at(i) ^ expected.at(i);
    return diff == 0;
}

} // namespace morfphoto
