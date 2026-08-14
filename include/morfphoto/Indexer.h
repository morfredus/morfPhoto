/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <QMutex>

#include "morfphoto/PhotoTypes.h"

// -----------------------------------------------------------------------------
// Indexer : exécution d'une passe de réconciliation.
//
// Une passe rend la base cohérente avec le disque pour un ensemble de dossiers :
// parcourir, décider du verdict de chaque fichier (neuf, inchangé, modifié,
// disparu), extraire l'EXIF des neufs et modifiés, écrire, marquer les disparus,
// tenir à jour index_runs, journaliser les erreurs sans les avaler.
//
// Règle forte : UNE SEULE passe à la fois. L'indexeur porte le garde (verrou non
// bloquant) et l'état observable (idle/indexing, début, dernière erreur). Si une
// passe tourne déjà, run() renvoie -1 : jamais de file d'attente de rescans.
// -----------------------------------------------------------------------------
namespace morfphoto {

class PhotoRepository;
class ExifExtractor;

enum class IndexMode { Incremental, Full };

inline QString indexModeName(IndexMode m) {
    return m == IndexMode::Full ? QStringLiteral("full") : QStringLiteral("incremental");
}

class Indexer {
public:
    Indexer(PhotoRepository* repo, ExifExtractor* extractor, QStringList roots);

    // Lance une passe. Retourne l'identifiant de run, ou -1 si une passe tourne
    // déjà. `folderIds` vide = tous les dossiers actifs. `trigger` : watch|api|cli.
    int run(IndexMode mode, const QVector<int>& folderIds, const QString& trigger);

    // État observable : { state: idle|indexing, started_at, last_error }.
    QJsonObject state() const;

private:
    void reconcileFolder(const FolderRow& folder, IndexMode mode, int runId,
                         RunCounts& counts);
    bool extract(int runId, const QString& path, RunCounts& counts, ExifData& out);

    PhotoRepository* m_repo;
    ExifExtractor*   m_extractor;
    QStringList      m_roots;

    QMutex          m_runMutex;    // garde « une seule passe » (tryLock)
    mutable QMutex  m_stateMutex;  // protège l'état observable
    bool            m_indexing = false;
    QString         m_startedAt;
    QString         m_lastError;
};

} // namespace morfphoto
