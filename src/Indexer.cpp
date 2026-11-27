/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/Indexer.h"
#include "morfphoto/PhotoRepository.h"
#include "morfphoto/ExifExtractor.h"
#include "morfphoto/PhotoScanner.h"
#include "morfphoto/ContextFile.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QSet>
#include <QDir>
#include <QFileInfo>
#include <QFile>
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

    // Dénominateur initial : dernière passe, si elle a vu des fichiers. Ça donne
    // un pourcentage dès la première photo, sans re-parcourir l'arbre. Le total
    // s'ajuste ensuite (il grandit, ou il se recale quand tous les dossiers sont
    // listés). startRun() viendrait après : latestRun() serait alors CETTE passe vide.
    bool hadPrev = false;
    const QJsonObject prevRun = m_repo->latestRun(&hadPrev);
    const qint64 estimate = (hadPrev)
        ? qint64(prevRun.value(QStringLiteral("files_seen")).toDouble())
        : 0;

    const int runId = m_repo->startRun(indexModeName(mode), trigger, nowIso());
    RunCounts counts;

    // mtimes de contexte connus, chargés une fois : la découverte ne relit un
    // `.morfphoto.json` que s'il a changé, et sait quelles lignes retirer.
    m_ctxMtimes = m_repo->knownContextMtimes();

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
    m_pFoldersTotal     = folders.size();
    m_pFoldersDone      = 0;
    m_pCurrentFolder.clear();
    m_pFilesDiscovered  = 0;
    m_pFilesTotalFinal  = false;
    m_pPhase            = QStringLiteral("indexing");
    // Pas de parcours de comptage : l'EXIF (ou le touch) commence avec le premier
    // dossier. Un walk à vide doublerait l'I/O disque/SMB pour un pourcentage.
    m_pFilesTotal = (estimate > 0) ? estimate : -1;
    reportProgress(0);

    QSet<QString> unavailableRoots;

    bool interrupted = false;
    if (m_pFoldersTotal == 0) {
        m_pFilesTotal = 0;
        m_pFilesTotalFinal = true;
        reportProgress(0);
    }
    for (const FolderRow& folder : folders) {
        m_pCurrentFolder = folder.path;
        reportProgress(counts.seen);            // dossier entamé
        if (reconcileFolder(folder, mode, runId, counts, unavailableRoots))
            interrupted = true;
        ++m_pFoldersDone;
        if (m_pFoldersDone >= m_pFoldersTotal) {
            // Tous les dossiers ont été listés (ou sautés) : le total ne bougera plus.
            m_pFilesTotal = m_pFilesDiscovered;
            m_pFilesTotalFinal = true;
        }
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

bool Indexer::fileWithinRoots(const FileInfo& info, QHash<QString, bool>& cache) const {
    if (info.isSymlink)
        return isWithinRoots(info.path, m_roots);
    const auto it = cache.constFind(info.directory);
    if (it != cache.constEnd())
        return it.value();
    const bool ok = isWithinRoots(info.directory, m_roots);
    cache.insert(info.directory, ok);
    return ok;
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
    const bool library = !m_libraryProviders.isEmpty();
    // Etat de lecture de la bibliotheque avant la passe : un fichier jamais lu par elle
    // (premiere activation) doit repasser par ExifTool pour en rapporter l'XMP embarque.
    const auto libraryBefore = library ? m_repo->libraryState(folder.id)
                                       : QHash<QString, PhotoRepository::LibraryFileState>();
    QSet<QString> touched;   // fichiers neufs ou re-extraits pendant cette passe
    QSet<QString> seen;
    QSet<QString> dirs;   // répertoires distincts vus, pour la découverte de contexte

    // Scan interruptible : la closure re-sonde la racine périodiquement. Si la
    // source meurt en plein parcours, le scan s'arrête et signale l'incomplétude.
    const ScanResult scan = scanFolder(
        folder.path, folder.recursive,
        [this, &folder]() { return probeAccessible(folder.rootPath, m_probeTimeoutMs); },
        library);

    // Total « connu pour l'instant » = fichiers déjà listés. Ça grandit dossier
    // après dossier ; le pourcentage se recalcule sans second parcours.
    qint64 nThis = 0;
    QHash<QString, bool> withinCache;   // dossier -> dans les racines ? (resolu une fois)
    for (const FileInfo& info : scan.files) {
        if (fileWithinRoots(info, withinCache))
            ++nThis;
    }
    m_pFilesDiscovered += nThis;
    if (m_pFilesTotal < 0 || m_pFilesDiscovered > m_pFilesTotal)
        m_pFilesTotal = m_pFilesDiscovered;
    reportProgress(counts.seen);

    // Ecritures groupees : sans transaction, chaque insertFile / updateFile / touchSeen est
    // un commit SQLite, donc un fsync. Sur des milliers de photos inchangees, une passe
    // passait l'essentiel de son temps a synchroniser le disque. On valide par paquets de
    // 200 fichiers OU toutes les secondes ; un arret brutal ne perd que le lot en cours, que
    // la passe suivante refait. L'extraction ExifTool (lente, jusqu'a 15 s au pire) se fait
    // HORS transaction : un lot ouvert pendant ce temps bloquerait les autres ecritures du
    // service (contexte photographique, sources).
    constexpr int kBatchFiles = 200;
    constexpr qint64 kBatchMs = 1000;
    QElapsedTimer batchClock;
    batchClock.start();
    int inBatch = 0;
    bool batchOpen = m_repo->beginBatch();
    auto flushBatch = [&](bool force) {
        if (!batchOpen)
            return;
        if (!force && ++inBatch < kBatchFiles && batchClock.elapsed() < kBatchMs)
            return;
        m_repo->commitBatch();
        batchOpen = force ? false : m_repo->beginBatch();
        inBatch = 0;
        batchClock.restart();
    };
    auto suspendBatch = [&]() {
        if (batchOpen) {
            m_repo->commitBatch();
            batchOpen = false;
            inBatch = 0;
        }
    };
    auto resumeBatch = [&]() {
        if (!batchOpen) {
            batchOpen = m_repo->beginBatch();
            batchClock.restart();
        }
    };

    for (const FileInfo& info : scan.files) {
        // Défense en profondeur : ne jamais rien indexer hors des racines globales.
        if (!fileWithinRoots(info, withinCache))
            continue;
        ++counts.seen;
        seen.insert(info.path);
        dirs.insert(info.directory);

        // Un gros dossier ne doit pas figer la barre : rafraîchir assez souvent
        // pour qu'un corpus déséquilibré (10 / 5000 / 20) avance au fichier.
        if (counts.seen % 25 == 0)
            reportProgress(counts.seen);

        auto it = known.find(info.path);
        if (it == known.end()) {
            ExifData exif;
            suspendBatch();
            const bool ok = extract(runId, info.path, counts, exif);
            resumeBatch();
            m_repo->insertFile(folder.id, info, exif, ok, nowIso());
            ++counts.created;
            touched.insert(info.path);
        } else if (mode == IndexMode::Full || it->size != info.size || it->mtime != info.mtime) {
            ExifData exif;
            suspendBatch();
            const bool ok = extract(runId, info.path, counts, exif);
            resumeBatch();
            m_repo->updateFile(it->id, info, exif, ok, nowIso());
            ++counts.updated;
            touched.insert(info.path);
        } else if (library && libraryBefore.value(info.path).fingerprint.isEmpty()) {
            // Fichier inchange mais jamais lu par la bibliotheque : une relecture unique.
            ExifData exif;
            suspendBatch();
            const bool ok = extract(runId, info.path, counts, exif);
            resumeBatch();
            m_repo->updateFile(it->id, info, exif, ok, nowIso());
            touched.insert(info.path);
        } else {
            m_repo->touchSeen(it->id, nowIso());
        }
        flushBatch(false);
    }
    flushBatch(true);   // le lot en cours est valide avant tout verdict de disparition

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

    // Ici seulement : disque présent ET scan complet. Rafraîchir le CONTEXTE des
    // répertoires vus est alors sûr (on ne supprimera pas un contexte parce qu'une
    // source était momentanément muette). Vaut aussi pour un support amovible présent.
    refreshContexts(dirs, runId, counts);
    if (library)
        refreshLibrary(folder, scan, touched, runId);

    // Support amovible (CD/DVD, disque d'archive) : l'absence est NORMALE. Même un
    // scan mené jusqu'au bout ne vaut jamais suppression ici - un disque éjecté (ou
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

void Indexer::refreshLibrary(const FolderRow& folder, const ScanResult& scan,
                             const QSet<QString>& touched, int runId) {
    QHash<QString, QVector<SidecarFile>> sidecarsByDir;
    for (const SidecarFile& s : scan.sidecars)
        sidecarsByDir[s.directory].push_back(s);
    QHash<QString, QSet<QString>> imagesByDir;
    for (const FileInfo& info : scan.files)
        imagesByDir[info.directory].insert(info.filename.toLower());

    // Identifiants et empreintes APRES l'insertion des fichiers neufs.
    const auto state = m_repo->libraryState(folder.id);

    // Un dossier = une transaction (par paquets de 200) : une ecriture isolee par photo
    // coute un fsync chacune.
    m_repo->beginBatch();
    int written = 0;
    QHash<QString, bool> withinCache;
    for (const FileInfo& info : scan.files) {
        if (!fileWithinRoots(info, withinCache))
            continue;
        const auto st = state.constFind(info.path);
        if (st == state.constEnd())
            continue;
        const QVector<SidecarRef> refs = matchSidecars(info.filename, info.directory,
                                                       sidecarsByDir.value(info.directory),
                                                       imagesByDir.value(info.directory));
        const QString fingerprint = libraryFingerprint(refs, m_libraryProviders);
        if (!touched.contains(info.path) && st->fingerprint == fingerprint)
            continue;   // rien n'a bouge : on ne rouvre rien
        readLibraryFile(st->id, fingerprint, refs, runId);
        if (++written % 200 == 0) {
            m_repo->commitBatch();
            m_repo->beginBatch();
        }
    }
    m_repo->commitBatch();
}

void Indexer::readLibraryFile(int fileId, const QString& fingerprint,
                              const QVector<SidecarRef>& refs, int runId) {
    constexpr qint64 kMaxSidecarBytes = 16 * 1024 * 1024;
    QVector<ProviderRead> reads;
    QVector<SidecarEntry> entries;
    for (const SidecarRef& ref : refs) {
        QFile f(ref.path);
        QByteArray bytes;
        const bool opened = f.open(QIODevice::ReadOnly);
        if (opened)
            bytes = f.read(kMaxSidecarBytes + 1);
        // Le format se reconnait a l'extension : `.dop` = DxO PhotoLab, le reste du XMP.
        const bool isDop = ref.name.endsWith(QLatin1String(".dop"), Qt::CaseInsensitive);
        const bool sizeOk = opened && bytes.size() <= kMaxSidecarBytes;
        XmpDocument xdoc;
        if (!isDop && sizeOk)
            xdoc = parseXmp(bytes);
        ProviderRead read = isDop
            ? (sizeOk ? readSidecarDop(bytes, ref.version) : ProviderRead())
            : readSidecarXmp(xdoc, ref.version);
        if (read.source.isEmpty()) {
            // Illisible ou pas du XMP : compte dans le rapport, ne bloque rien.
            entries.push_back({ref.name, QString(), false});
            m_repo->logError(runId, ref.path, QStringLiteral("library"),
                             opened ? QStringLiteral("sidecar illisible") : f.errorString(), nowIso());
            continue;
        }
        // Lightcraft ecrit dans le meme .xmp que darktable : le drapeau `lc:flag` est lu a part, attribue
        // a `lightcraft`, meme si le fichier est classe darktable (ou n'est pas un fournisseur actif).
        if (!isDop && m_libraryProviders.contains(QStringLiteral("lightcraft"))) {
            const ProviderRead lc = readSidecarLightcraft(xdoc, ref.version);
            if (!lc.source.isEmpty())
                reads.push_back(lc);
        }
        // Un sidecar darktable lu avec le seul fournisseur « xmp » reste exploitable.
        if (!m_libraryProviders.contains(read.source)) {
            if (read.source == QLatin1String("darktable") && m_libraryProviders.contains(QStringLiteral("xmp")))
                read.source = QStringLiteral("xmp");
            else
                continue;   // fournisseur non active : le sidecar n'existe pas pour nous
        }
        entries.push_back({ref.name, read.source, true});
        reads.push_back(read);
    }
    // XMP embarque, deja lu par ExifTool : aucune relecture du fichier image.
    const ProviderRead embedded = readEmbedded(m_repo->rawExif(fileId));
    if (!embedded.source.isEmpty())
        reads.push_back(embedded);

    LibraryResult result = mergeReads(reads, m_libraryProviders);
    result.sidecars = entries;
    m_repo->writeLibrary(fileId, fingerprint, result);
}

void Indexer::refreshContexts(const QSet<QString>& directories, int runId, RunCounts& counts) {
    for (const QString& dir : directories) {
        const QString path = QDir(dir).filePath(ContextFile::fileName());
        const QFileInfo fi(path);

        if (!fi.exists()) {
            // `.morfphoto.json` retiré depuis la dernière fois : effacer la projection.
            // (S'il n'a jamais existé, il n'y a pas de ligne : rien à faire.)
            if (m_ctxMtimes.contains(dir)) {
                m_repo->deleteContextRow(dir);
                m_ctxMtimes.remove(dir);
            }
            continue;
        }

        const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
        if (m_ctxMtimes.value(dir, -1) == mtime)
            continue;   // inchangé depuis la dernière lecture : idempotent

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            m_repo->logError(runId, path, QStringLiteral("context"),
                             QStringLiteral("lecture impossible: %1").arg(f.errorString()),
                             nowIso());
            ++counts.errors;
            continue;
        }
        const QByteArray bytes = f.readAll();
        f.close();

        FolderContext fc = ContextFile::parse(bytes);
        fc.directory   = dir;
        fc.sourceMtime = mtime;
        m_repo->upsertContext(fc);
        m_ctxMtimes[dir] = mtime;

        // Diagnostics : un JSON invalide ou un avertissement est journalisé sans jamais
        // interrompre la passe (un défaut sémantique ne casse pas l'analyse technique).
        if (fc.status == QLatin1String("invalid")) {
            m_repo->logError(runId, path, QStringLiteral("context"),
                             fc.error.toString(), nowIso());
            ++counts.errors;
        } else if (!fc.warnings.isEmpty()) {
            m_repo->logError(runId, path, QStringLiteral("context"),
                             QStringLiteral("avertissements: %1").arg(fc.warnings.join(QLatin1Char(','))),
                             nowIso());
        }
    }
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
    if (!m_progress)
        return;
    IndexProgress p;
    p.phase         = m_pPhase;
    p.foldersDone   = m_pFoldersDone;
    p.foldersTotal  = m_pFoldersTotal;
    p.filesSeen     = filesSeen;
    p.filesTotal    = m_pFilesTotal;
    p.filesTotalFinal = m_pFilesTotalFinal;
    p.currentFolder = m_pCurrentFolder;
    m_progress(p);
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
