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

Indexer::Indexer(PhotoRepository* repo, ExifExtractor* extractor, QStringList roots)
    : m_repo(repo), m_extractor(extractor), m_roots(std::move(roots)) {}

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

    for (const FolderRow& folder : folders)
        reconcileFolder(folder, mode, runId, counts);

    m_repo->finishRun(runId, QStringLiteral("done"), counts, nowIso());

    {
        QMutexLocker lock(&m_stateMutex);
        m_indexing = false;
        m_lastError.clear();
    }
    m_runMutex.unlock();
    return runId;
}

void Indexer::reconcileFolder(const FolderRow& folder, IndexMode mode, int runId,
                              RunCounts& counts) {
    // Garde-fou : la sélection doit rester sous sa propre racine autorisée.
    if (!isWithinRoots(folder.path, {folder.rootPath})) {
        m_repo->logError(runId, folder.path, QStringLiteral("scan"),
                         QStringLiteral("selection hors de sa racine autorisee, ignoree"),
                         nowIso());
        ++counts.errors;
        return;
    }

    const KnownFiles known = m_repo->existingFilesInFolder(folder.id);
    QSet<QString> seen;

    for (const FileInfo& info : scanFolder(folder.path, folder.recursive)) {
        // Défense en profondeur : ne jamais rien indexer hors des racines globales.
        if (!isWithinRoots(info.path, m_roots))
            continue;
        ++counts.seen;
        seen.insert(info.path);

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

    // Disparus : connus, non revus, encore marqués présents.
    QVector<int> missingIds;
    for (auto it = known.constBegin(); it != known.constEnd(); ++it)
        if (!seen.contains(it.key()) && it->state != QLatin1String("missing"))
            missingIds.push_back(it->id);
    if (!missingIds.isEmpty()) {
        m_repo->markMissing(missingIds, nowIso());
        counts.missing += missingIds.size();
    }

    m_repo->setFolderScanned(folder.id, nowIso());
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
