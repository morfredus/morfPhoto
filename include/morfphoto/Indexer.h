/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QSet>
#include <QJsonObject>
#include <QMutex>

#include <functional>

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
//
// Tolérance aux sources distantes : une racine réseau (SMB/CIFS, NFS...) peut
// disparaître pendant une passe. Une racine indisponible ne doit JAMAIS être prise
// pour une suppression de fichiers. L'indexeur sonde donc l'accessibilité de la
// racine avant et après le scan (délai borné), n'applique un verdict de disparition
// que sur un scan mené jusqu'au bout de façon fiable, et clôt alors la passe en
// 'interrupted' plutôt qu'en 'done'. Générique à tout montage distant, sans code
// spécifique à SMB.
// -----------------------------------------------------------------------------
namespace morfphoto {

class PhotoRepository;
class ExifExtractor;

enum class IndexMode { Incremental, Full };

inline QString indexModeName(IndexMode m) {
    return m == IndexMode::Full ? QStringLiteral("full") : QStringLiteral("incremental");
}

// Progression d'une passe, destinée à GET /api/v1/index/status.
// L'indexation commence tout de suite : pas de parcours dédié au comptage
// (un second walk doublerait l'I/O, surtout sur un montage réseau).
// filesTotal < 0 : aucun dénominateur encore (tout début, pas de passe précédente).
// filesTotalFinal = false : le total peut encore grandir (dossiers restants, ou
// estimation reprise de la dernière passe) ; le pourcentage s'adapte.
struct IndexProgress {
    QString phase;                 // indexing (conservé pour compatibilité clients)
    int     foldersDone  = 0;
    int     foldersTotal = 0;
    qint64  filesSeen    = 0;
    qint64  filesTotal   = -1;
    bool    filesTotalFinal = false;
    QString currentFolder;
};

class Indexer {
public:
    // `probeTimeoutMs` : délai maximal d'attente d'une sonde d'accessibilité de
    // racine avant de la déclarer indisponible (borne le « ça a l'air figé » face à
    // un montage réseau muet). Défaut 5 s ; <= 0 = attente illimitée (chemins locaux).
    Indexer(PhotoRepository* repo, ExifExtractor* extractor, QStringList roots,
            int probeTimeoutMs = 5000);

    // Lance une passe. Retourne l'identifiant de run, ou -1 si une passe tourne
    // déjà. `folderIds` vide = tous les dossiers actifs. `trigger` : watch|api|cli.
    int run(IndexMode mode, const QVector<int>& folderIds, const QString& trigger);

    // État observable : { state: idle|indexing, started_at, last_error }.
    QJsonObject state() const;

    // Suivi de progression d'une passe. Rappelée pendant `run()` : l'indexation
    // démarre sans attendre un total définitif. Le callback est invoqué DANS le
    // thread de la passe.
    using ProgressFn = std::function<void(const IndexProgress&)>;
    void setProgressCallback(ProgressFn cb) { m_progress = std::move(cb); }

private:
    // Réconcilie une sélection. `unavailableRoots` : cache des racines déjà sondées
    // indisponibles pendant CETTE passe (ne pas re-sonder, ne pas s'acharner sur la
    // source). Retourne true si la racine de cette sélection était indisponible
    // (avant ou pendant le scan) : la passe sera alors close en 'interrupted'.
    bool reconcileFolder(const FolderRow& folder, IndexMode mode, int runId,
                         RunCounts& counts, QSet<QString>& unavailableRoots);
    bool extract(int runId, const QString& path, RunCounts& counts, ExifData& out);

    // Émet la progression courante vers le callback (si défini). Le contexte de
    // dossier (m_p*) n'est écrit et lu que dans le thread de la passe : pas de verrou.
    void reportProgress(qint64 filesSeen) const;

    PhotoRepository* m_repo;
    ExifExtractor*   m_extractor;
    QStringList      m_roots;
    int              m_probeTimeoutMs;

    QMutex          m_runMutex;    // garde « une seule passe » (tryLock)
    mutable QMutex  m_stateMutex;  // protège l'état observable
    bool            m_indexing = false;
    QString         m_startedAt;
    QString         m_lastError;

    ProgressFn m_progress;
    QString    m_pPhase;
    int        m_pFoldersTotal = 0;
    int        m_pFoldersDone  = 0;
    qint64     m_pFilesTotal   = -1;
    qint64     m_pFilesDiscovered = 0;
    bool       m_pFilesTotalFinal = false;
    QString    m_pCurrentFolder;
};

} // namespace morfphoto
