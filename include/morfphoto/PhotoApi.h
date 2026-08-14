/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QByteArray>
#include <QString>

// -----------------------------------------------------------------------------
// PhotoApi : routage du contrat public /api/v1, branché sur le PhotoModule.
//
// Cette couche ne contient aucun métier ni SQL : elle valide les paramètres,
// appelle le module (la façade du domaine), sérialise le résultat. Le serveur
// HTTP générique lui délègue toute requête sous /api/. Corps d'erreur uniforme
// { "error": "...", "detail": "..." }, codes HTTP standards.
// -----------------------------------------------------------------------------
namespace morfphoto {

class PhotoModule;

class PhotoApi {
public:
    struct Result {
        int        code;
        QByteArray body;   // JSON UTF-8
    };

    explicit PhotoApi(PhotoModule* module);

    // Route une requête déjà réduite au chemin (sans query) + la query brute.
    Result handle(const QByteArray& method, const QString& path,
                  const QString& queryString, const QByteArray& body);

private:
    PhotoModule* m_module;
};

} // namespace morfphoto
