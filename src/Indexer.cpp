/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/Indexer.h"
#include "morfphoto/PhotoRepository.h"
#include "morfphoto/ExifExtractor.h"
#include "morfphoto/PhotoScanner.h"

#include <QDateTime>
#include <QSet>
#include <QMutexLocker>

#include <utility>

namespace morfphoto {

namespace {
// Horodatage UTC lisible pour la comptabilité d'indexation.
QString nowIso() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}
} // namespace

Indexer::Indexer(PhotoRepository* repo, ExifExtractor* extractor, QStringList roots,
                 int probeTimeoutMs)
    : m_repo(repo), m_extractor(extractor), m_roots(std::move(roots)),
      m_probeTimeoutMs(probeTimeoutMs) {}

int Indexer::run(IndexMode mode, const QVector<int>& folderIds, const QString& trigger) {
    // Verrou non bloquant : refuser tout de suite plutôt que d'empiler des passes.
    if (!m_runMutex.tryLock())
        return -1;

    {
        QMutexLocker lock(&m_stateMutex);
        m_indexing = true;
        m_startedAt = nowIso();
    }

    const int runId = m_repo->startRun(indexModeName(mode), trigger, nowIso());
    RunCounts counts;

    QVector<FolderRow> folders = m_repo->activeFolders();
    if (!folderIds.isEmpty()) {
        const QSet<int> wanted(folderIds.begin(), folderIds.end());
        QVector<FolderRow> kept;
        for (const FolderRow& f : folders)
            if (wanted.contains(f.id))
                kept.push_back(f);
        folders = kept;
    }

    // Racines constatées indisponibles pendant CETTE passe : une fois une racine KO,
    // ses autres sélections sont sautées sans re-sonder (ne pas s'acharner sur une
    // source muette : un timeout par racine, pas un par dossier).
    // Progression : le nombre de dossiers est le dénominateur fiable, connu ici.
    m_pFoldersTotal = folders.size();
    m_pFoldersDone  = 0;
    m_pCurrentFolder.clear();
    reportProgress(0);

    QSet<QString> unavailableRoots;
    bool interrupted = false;
    for (const FolderRow& folder : folders) {
        m_pCurrentFolder = folder.path;
        reportProgress(counts.seen);            // dossier entamé
        if (reconcileFolder(folder, mode, runId, counts, unavailableRoots))
            interrupted = true;
        ++m_pFoldersDone;
        reportProgress(counts.seen);            // dossier terminé
    }

    // Une passe qui a perdu au moins une source n'est PAS une passe normale : le
    // dire pour que /status distingue « terminée » de « interrompue ».
    const QString finalState = interrupted ? QStringLiteral("interrupted")
                                           : QStringLiteral("done");
    m_repo->finishRun(runId, finalState, counts, nowIso());

    {
        QMutexLocker lock(&m_stateMutex);
        m_indexing = false;
        m_lastError.clear();
    }
    m_runMutex.unlock();
    return runId;
}

bool Indexer::reconcileFolder(const FolderRow& folder, IndexMode mode, int runId,
                              RunCounts& counts, QSet<QString>& unavailableRoots) {
    // Garde-fou : la sélection doit rester sous sa propre racine autorisée.
    if (!isWithinRoots(folder.path, {folder.rootPath})) {
        m_repo->logError(runId, folder.path, QStringLiteral("scan"),
                         QStringLiteral("selection hors de sa racine autorisee, ignoree"),
                         nowIso());
        ++counts.errors;
        return false;
    }

    // Racine déjà déclarée indisponible plus tôt dans la passe : sauter tout de
    // suite, sans nouvelle sonde (ne pas re-solliciter une source muette).
    if (unavailableRoots.contains(folder.rootPath)) {
        ++counts.unavailable;
        return true;
    }

    // Sonde AVANT le scan : une racine réseau valide au dernier passage a pu
    // disparaître entre-temps. Indisponible => on ne touche à RIEN (surtout pas de
    // markMissing : une source absente n'est pas une suppression de fichiers).
    if (!probeAccessible(folder.rootPath, m_probeTimeoutMs)) {
        unavailableRoots.insert(folder.rootPath);
        m_repo->logError(runId, folder.rootPath, QStringLiteral("availability"),
                         QStringLiteral("racine indisponible au demarrage du scan "
                                        "(source injoignable), selection ignoree"),
                         nowIso());
        ++counts.unavailable;
        return true;
    }

    const KnownFiles known = m_repo->existingFilesInFolder(folder.id);
    QSet<QString> seen;

    // Scan interruptible : la closure re-sonde la racine périodiquement. Si la
    // source meurt en plein parcours, le scan s'arrête et signale l'incomplétude.
    const ScanResult scan = scanFolder(
        folder.path, folder.recursive,
        [this, &folder]() { return probeAccessible(folder.rootPath, m_probeTimeoutMs); });

    for (const FileInfo& info : scan.files) {
        // Défense en profondeur : ne jamais rien indexer hors des racines globales.
        if (!isWithinRoots(info.path, m_roots))
            continue;
        ++counts.seen;
        seen.insert(info.path);

        // Un gros dossier ne doit pas figer la barre : on rafraîchit le compteur de
        // fichiers toutes les 200 entrées (le total de dossiers, lui, ne bouge pas).
        if (counts.seen % 200 == 0)
            reportProgress(counts.seen);

        auto it = known.find(info.path);
        if (it == known.end()) {
            ExifData exif;
            const bool ok = extract(runId, info.path, counts, exif);
            m_repo->insertFile(folder.id, info, exif, ok, nowIso());
            ++counts.created;
        } else if (mode == IndexMode::Full || it->size != info.size || it->mtime != info.mtime) {
            ExifData exif;
            const bool ok = extract(runId, info.path, counts, exif);
            m_repo->updateFile(it->id, info, exif, ok, nowIso());
            ++counts.updated;
        } else {
            m_repo->touchSeen(it->id, nowIso());
        }
    }

    // Point crucial : le verdict de disparition n'est FIABLE que si la racine a été
    // parcourue entièrement ET qu'elle répond toujours à la fin. Un scan incomplet
    // (source perdue en cours) ne sert jamais de référence : les fichiers déjà vus
    // restent acquis, aucun n'est marqué disparu, la passe est interrompue.
    if (!scan.completed || !probeAccessible(folder.rootPath, m_probeTimeoutMs)) {
        unavailableRoots.insert(folder.rootPath);
        m_repo->logError(runId, folder.rootPath, QStringLiteral("availability"),
                         QStringLiteral("racine devenue indisponible pendant le scan : "
                                        "reconciliation partielle, aucun fichier marque "
                                        "disparu"),
                         nowIso());
        ++counts.unavailable;
        return true;
    }

    // Support amovible (CD/DVD, disque d'archive) : l'absence est NORMALE. Même un
    // scan mené jusqu'au bout ne vaut jamais suppression ici — un disque éjecté (ou
    // remplacé par un autre) monté sur un point resté présent mais vide renverrait
    // sinon « 0 fichier vu » et ferait marquer disparue toute l'archive, qui
    // sortirait alors de morfAnalytics. On ne marque donc JAMAIS disparu un fichier
    // d'un dossier amovible : ses photos restent acquises, support présent ou non
    // (seul un retrait volontaire les sort de l'analyse). Les neufs/modifiés ont déjà
    // été pris en compte plus haut quand le support est là.
    if (!folder.removable) {
        // Disparus : connus, non revus, encore marqués présents. Sûr ici : scan complet.
        QVector<int> missingIds;
        for (auto it = known.constBegin(); it != known.constEnd(); ++it)
            if (!seen.contains(it.key()) && it->state != QLatin1String("missing"))
                missingIds.push_back(it->id);
        if (!missingIds.isEmpty()) {
            m_repo->markMissing(missingIds, nowIso());
            counts.missing += missingIds.size();
        }
    }

    m_repo->setFolderScanned(folder.id, nowIso());
    return false;
}

bool Indexer::extract(int runId, const QString& path, RunCounts& counts, ExifData& out) {
    if (!m_extractor)
        return false;
    QString error;
    out = m_extractor->extract(path, &error);
    if (!out.ok) {
        // Une erreur d'extraction est journalisée et la passe continue.
        m_repo->logError(runId, path, QStringLiteral("exif"), error, nowIso());
        ++counts.errors;
        return false;
    }
    return true;
}

void Indexer::reportProgress(qint64 filesSeen) const {
    if (m_progress)
        m_progress(m_pFoldersDone, m_pFoldersTotal, filesSeen, m_pCurrentFolder);
}

QJsonObject Indexer::state() const {
    QMutexLocker lock(&m_stateMutex);
    QJsonObject o;
    o["state"]      = m_indexing ? QStringLiteral("indexing") : QStringLiteral("idle");
    o["started_at"] = m_indexing ? QJsonValue(m_startedAt) : QJsonValue(QJsonValue::Null);
    o["last_error"] = m_lastError.isEmpty() ? QJsonValue(QJsonValue::Null)
                                            : QJsonValue(m_lastError);
    return o;
}

} // namespace morfphoto
