/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include "morfbeacon/PresenceConfig.h"

namespace morfphoto {

// -----------------------------------------------------------------------------
// fillAnnouncedDetail : renseigne le DETAIL annonce du service dans un
// PresenceConfig, pour que morfbeacon::describeService le serialise dans /status.
//
// >>> MODELE. Ce squelette montre a un nouveau service COMMENT se decrire pour
// >>> le parc. Un service morfSystem se rend « bavard » d'une seule facon :
// >>>   1. lister ici son API (methode, chemin, resume) ;
// >>>   2. si -- et seulement si -- il expose une interface web, renseigner
// >>>      pc.webUiPath / pc.webUiLabel / pc.webUiDescription (voir morfAnalytics
// >>>      et morfMonitor). Sans interface, ne rien mettre : describeService
// >>>      n'emettra alors que le bloc `api`, sans cle `web_ui` fantome.
// >>>   3. appeler describeService dans HttpServer::buildStatusJson (fait plus bas).
//
// Point UNIQUE : le detail annonce est defini une seule fois, ici. Le heartbeat
// morfBeacon, lui, reste maigre -- il annonce des CAPACITES, pas l'API -- donc
// Service.cpp n'appelle pas cette fonction : le detail ne vit que dans /status.
//
// En-tete (inline) : aucun fichier source ni entree CMake supplementaires.
inline void fillAnnouncedDetail(morfbeacon::PresenceConfig& pc) {
    // API publique de morfPhoto. Les routes de cadre (/status, /healthz, /modules)
    // ne sont pas listees : un observateur les connait deja par le protocole.
    pc.apiBasePath = QStringLiteral("/api/v1");
    pc.api = {
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/photos"),          QStringLiteral("liste paginee + filtree")},
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/photos/{id}"),     QStringLiteral("fiche d'un fichier")},
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/photos/summary"),  QStringLiteral("compteurs globaux")},
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/photos/cameras"),  QStringLiteral("boitiers distincts")},
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/photos/lenses"),   QStringLiteral("objectifs distincts")},
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/photos/focals"),   QStringLiteral("focales brutes distinctes")},
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/photos/years"),    QStringLiteral("annees + comptes")},
        {QStringLiteral("POST"),   QStringLiteral("/api/v1/index"),           QStringLiteral("declenche une passe (async)")},
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/index/status"),    QStringLiteral("etat d'indexation")},
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/roots"),           QStringLiteral("racines autorisees (lecture seule)")},
        {QStringLiteral("GET"),    QStringLiteral("/api/v1/folders"),         QStringLiteral("selections surveillees")},
        {QStringLiteral("POST"),   QStringLiteral("/api/v1/folders"),         QStringLiteral("declare une selection (sous racine)")},
        {QStringLiteral("PATCH"),  QStringLiteral("/api/v1/folders/{id}"),    QStringLiteral("active / desactive")},
        {QStringLiteral("DELETE"), QStringLiteral("/api/v1/folders/{id}"),    QStringLiteral("retrait doux (historique conserve)")},
    };
}

} // namespace morfphoto
