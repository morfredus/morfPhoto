/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QMutex>
#include <QString>

#include <atomic>

// -----------------------------------------------------------------------------
// Vignettes de grille : reduction a 400 px et cache disque.
//
// Pourquoi : l'apercu embarque d'un RAW est tres variable (160 px sur certains
// boitiers, plusieurs Mo sur d'autres) et son extraction relance exiftool a chaque
// demande, surtout couteux sur un montage reseau. Une vignette bornee a 400 px sur
// son plus grand cote, gardee sur disque, rend l'apercu quasi instantane pour un
// client (PhotoHub) qui affiche une grille.
//
// Le cache est RECONSTRUCTIBLE : on peut le supprimer sans perte. Il vit donc dans
// la zone `cache/` de l'etat du service, jamais dans /etc. Les fonctions de reduction
// sont pures (octets ou fichier en entree, JPEG en sortie) pour rester testables.
// -----------------------------------------------------------------------------
namespace morfphoto {

// Plus grand cote d'une vignette de grille, en pixels.
inline constexpr int kThumbnailEdge = 400;

// Reduit un JPEG encode (apercu embarque) a la grille. Un JPEG deja assez petit
// (cote <= 400 px, <= 1 Mo) est rendu tel quel, sans recompression. Vide si les octets
// ne sont pas une image lisible.
QByteArray gridThumbnailFromBytes(const QByteArray& jpeg);

// Idem a partir d'un fichier JPEG pleine taille : decodage reduit directement par la
// bibliotheque JPEG (bien plus rapide et leger qu'un decodage complet), orientation EXIF
// appliquee. Vide si le fichier n'est pas lisible.
QByteArray gridThumbnailFromFile(const QString& path);

// Cache disque : un fichier JPEG par vignette sous `<dir>/<2 hex>/<cle>.jpg`, ecritures
// atomiques, plafond de taille (le plus ancien d'abord) et garde-fou d'espace libre.
class ThumbnailCache {
public:
    // maxBytes <= 0 : aucun plafond. minFreeBytes : plus aucune ecriture en dessous.
    ThumbnailCache(const QString& dir, qint64 maxBytes, qint64 minFreeBytes);

    // Cle stable d'une vignette : change des que le fichier change (taille, date) ou
    // que le format de vignette change (version), donc jamais de vignette perimee.
    static QString key(const QString& path, qint64 size, qint64 mtime);

    // Vignette en cache, ou vide. Compte un succes ou un echec.
    QByteArray load(const QString& key);

    // Range une vignette. false si le cache est plein, desactive ou si l'ecriture echoue
    // (le service continue sans cache : ce n'est jamais une erreur pour le client).
    bool store(const QString& key, const QByteArray& jpeg);

    QString dir() const { return m_dir; }

    // Compteurs pour /status : de quoi savoir si le cache sert reellement.
    QJsonObject stats() const;

    // Applique le plafond de taille maintenant (appele aussi periodiquement par store).
    void trim();

private:
    QString filePath(const QString& key) const;

    QString m_dir;
    qint64  m_maxBytes;
    qint64  m_minFreeBytes;
    QMutex  m_trimMutex;
    std::atomic<qint64> m_hits{0};
    std::atomic<qint64> m_misses{0};
    std::atomic<qint64> m_writes{0};
    std::atomic<bool>   m_full{false};
};

} // namespace morfphoto
