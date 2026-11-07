/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QByteArray>
#include <QString>

namespace morfphoto {

// -----------------------------------------------------------------------------
// Garde anti-CSRF des ecritures (specification commune du parc).
//
// Un navigateur envoie TOUJOURS `Origin` sur une ecriture cross-origin ; les clients
// non-navigateur (PhotoHub, morfMonitor, curl) n'en envoient pas et passent. Une
// ecriture dont l'Origin n'a pas le meme host:port que `Host` vient donc d'une page
// tierce : a refuser (403 cross_origin_forbidden).
// -----------------------------------------------------------------------------

// Vrai pour POST, PUT, PATCH, DELETE.
inline bool isWriteMethod(const QByteArray& method) {
    return method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE";
}

// Vrai si l'ecriture doit etre refusee. `origin` vide = en-tete absent = client
// non-navigateur. L'origine opaque « null » (iframe sandbox, file://) differe de Host
// et est donc refusee.
inline bool isCrossOriginWrite(const QByteArray& method, const QByteArray& origin,
                               const QByteArray& host) {
    if (!isWriteMethod(method) || origin.trimmed().isEmpty())
        return false;
    QByteArray o = origin.trimmed().toLower();
    const int scheme = o.indexOf("://");
    if (scheme >= 0)
        o = o.mid(scheme + 3);
    return o != host.trimmed().toLower();
}

// Valeur d'un en-tete (insensible a la casse) dans le bloc d'en-tetes, ou vide.
inline QByteArray headerValue(const QByteArray& headerBlock, const QByteArray& name) {
    const QByteArray prefix = name.toLower() + ":";
    for (const QByteArray& line : headerBlock.split('\n')) {
        const QByteArray l = line.trimmed();
        if (l.toLower().startsWith(prefix))
            return l.mid(prefix.size()).trimmed();
    }
    return {};
}

} // namespace morfphoto
