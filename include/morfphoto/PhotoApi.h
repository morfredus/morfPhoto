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
        QByteArray body;   // JSON UTF-8 par defaut, ou binaire (voir contentType)
        // Type MIME de la reponse. JSON par defaut ; certaines routes renvoient du
        // binaire (ex. une vignette image/jpeg). Le serveur HTTP l'utilise tel quel.
        QByteArray contentType = QByteArrayLiteral("application/json; charset=utf-8");
        // Validateur de cache (ETag), vide s'il n'y en a pas. Voir PhotoModule::catalogRevision.
        QByteArray etag;
    };

    explicit PhotoApi(PhotoModule* module);

    // Route une requête déjà réduite au chemin (sans query) + la query brute.
    // `ifNoneMatch` : en-tete If-None-Match de la requete (ETag deja connu du client).
    Result handle(const QByteArray& method, const QString& path,
                  const QString& queryString, const QByteArray& body,
                  const QByteArray& ifNoneMatch = {});

private:
    Result handleRoute(const QByteArray& method, const QString& path,
                       const QString& queryString, const QByteArray& body);
    PhotoModule* m_module;
};

} // namespace morfphoto
