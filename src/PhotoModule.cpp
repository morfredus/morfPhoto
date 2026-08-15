/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/PhotoModule.h"
#include "morfphoto/PhotoRepository.h"
#include "morfphoto/ExifExtractor.h"
#include "morfphoto/PhotoScanner.h"
#include "morfphoto/Paths.h"

#include <QTimer>
#include <QtConcurrent>
#include <QDateTime>
#include <QJsonArray>
#include <QMutexLocker>

namespace morfphoto {

namespace {
QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }

// Retrouve le détail d'une sélection par son id dans une liste déjà chargée.
QJsonObject findById(const QJsonArray& folders, int id) {
    for (const QJsonValue& v : folders) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("id")).toInt() == id)
            return o;
    }
    return {};
}
} // namespace

PhotoModule::PhotoModule(const QString& id, const QJsonObject& params, QObject* parent)
    : IModule(id, QStringLiteral("photo"), parent) {
    // Emplacement de la base : configurable, sinon l'état du service (doctrine
    // FILESYSTEM.md : /var/lib/morfsystem/morfphoto, seule zone où le service écrit).
    m_dbPath = params.value(QStringLiteral("db_path")).toString();
    if (m_dbPath.isEmpty())
        m_dbPath = Paths::stateDir(QStringLiteral("morfphoto")) + QStringLiteral("/photos.db");

    // Racines AUTORISÉES : le périmètre que morfPhoto a le droit d'explorer.
    for (const QJsonValue& v : params.value(QStringLiteral("roots")).toArray())
        m_roots << v.toString();

    const QJsonObject watch = params.value(QStringLiteral("watch")).toObject();
    m_intervalMs = watch.value(QStringLiteral("interval_ms")).toInt(m_intervalMs);
    // Délai borné des sondes d'accessibilité de racine (tolérance aux montages
    // distants). Configurable : pas de seuil en dur (règle 11 morfSystem).
    m_probeTimeoutMs = watch.value(QStringLiteral("availability_timeout_ms")).toInt(m_probeTimeoutMs);

    const QJsonObject exif = params.value(QStringLiteral("exiftool")).toObject();
    m_exiftoolBinary = exif.value(QStringLiteral("binary")).toString(QStringLiteral("exiftool"));
    m_stayOpen       = exif.value(QStringLiteral("stay_open")).toBool(true);
}

PhotoModule::~PhotoModule() {
    if (m_started)
        stop();
}

bool PhotoModule::start() {
    m_readRepo = new PhotoRepository(QStringLiteral("morfphoto:read"));
    if (!m_readRepo->open(m_dbPath)) {
        const QString e = m_readRepo->openError();
        delete m_readRepo;
        m_readRepo = nullptr;
        QMutexLocker lock(&m_stateMutex);
        m_lastError = QStringLiteral("db: ") + e;   // visible via /modules et /api/v1/index/status
        return false;
    }

    // Aligner les sélections sur les racines autorisées actuelles (une racine
    // retirée désactive ses sélections sans effacer l'historique).
    reconcileRoots();

    // Sonder le prérequis ExifTool UNE fois, dès le démarrage. Le service tourne
    // même sans lui (les fichiers restent indexés), mais l'extraction EXIF échoue
    // alors en silence : on capture le verdict pour l'exposer dans l'état observable
    // plutôt que de le découvrir en constatant des métadonnées toutes nulles.
    {
        QString detail;
        const bool ready = ExifExtractor::probe(m_exiftoolBinary, &detail);
        QMutexLocker lock(&m_stateMutex);
        m_exiftoolReady  = ready;
        m_exiftoolDetail = detail;
    }

    // Surveillance périodique : réconciliation incrémentale à cadence fixe.
    m_watchTimer = new QTimer(this);
    m_watchTimer->setInterval(m_intervalMs);
    connect(m_watchTimer, &QTimer::timeout, this, [this]() {
        triggerIndex(IndexMode::Incremental, {}, QStringLiteral("watch"));
    });
    m_watchTimer->start();

    // Première passe dès le démarrage, pour rendre la base cohérente tout de suite.
    triggerIndex(IndexMode::Incremental, {}, QStringLiteral("watch"));

    m_started = true;
    return true;
}

void PhotoModule::stop() {
    m_started = false;
    if (m_watchTimer) {
        m_watchTimer->stop();
        m_watchTimer->deleteLater();
        m_watchTimer = nullptr;
    }
    // Attendre la fin d'une passe éventuellement en cours avant de fermer la base.
    if (m_pass.isRunning())
        m_pass.waitForFinished();
    if (m_readRepo) {
        m_readRepo->close();
        delete m_readRepo;
        m_readRepo = nullptr;
    }
}

// --- Indexation -------------------------------------------------------------

QJsonObject PhotoModule::triggerIndex(IndexMode mode, const QVector<int>& folderIds,
                                      const QString& trigger) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_indexing) {
            // Une seule passe à la fois : refuser sans empiler.
            QJsonObject o;
            o["accepted"]   = false;
            o["state"]      = QStringLiteral("already_running");
            o["started_at"] = m_startedAt;
            return o;
        }
        m_indexing  = true;
        m_startedAt = nowIso();
        m_lastError.clear();
        // Repartir d'une progression vierge : l'ancienne passe ne doit pas laisser
        // de compteurs résiduels visibles au tout début de la nouvelle.
        m_progFoldersTotal = 0;
        m_progFoldersDone  = 0;
        m_progFilesSeen    = 0;
        m_progCurrentFolder.clear();
    }

    m_pass = QtConcurrent::run([this, mode, folderIds, trigger]() {
        doPass(mode, folderIds, trigger);
    });

    QJsonObject o;
    o["accepted"] = true;
    o["state"]    = QStringLiteral("indexing");
    return o;
}

void PhotoModule::doPass(IndexMode mode, QVector<int> folderIds, QString trigger) {
    int pass;
    {
        QMutexLocker lock(&m_stateMutex);
        pass = ++m_passCounter;
    }
    QString error;
    {
        // Connexion et process ExifTool PROPRES à ce thread de passe.
        PhotoRepository repo(QStringLiteral("morfphoto:index:%1").arg(pass));
        if (repo.open(m_dbPath)) {
            ExifExtractor extractor(m_exiftoolBinary, m_stayOpen);
            extractor.open();
            Indexer indexer(&repo, &extractor, m_roots, m_probeTimeoutMs);
            // Relais de progression : l'Indexer rappelle depuis CE thread ; on recopie
            // sous verrou dans les membres lus par observableState() (thread HTTP).
            indexer.setProgressCallback([this](int done, int total, qint64 seen,
                                               const QString& folder) {
                QMutexLocker lock(&m_stateMutex);
                m_progFoldersDone  = done;
                m_progFoldersTotal = total;
                m_progFilesSeen    = seen;
                m_progCurrentFolder = folder;
            });
            indexer.run(mode, folderIds, trigger);
            extractor.close();
            const QJsonObject st = indexer.state();
            if (!st.value(QStringLiteral("last_error")).isNull())
                error = st.value(QStringLiteral("last_error")).toString();
        } else {
            error = QStringLiteral("ouverture de la base impossible pour la passe");
        }
    }
    QMutexLocker lock(&m_stateMutex);
    m_indexing = false;
    m_lastError = error;
}

QJsonObject PhotoModule::observableState() const {
    QMutexLocker lock(&m_stateMutex);
    QJsonObject o;
    o["state"]      = m_indexing ? QStringLiteral("indexing") : QStringLiteral("idle");
    o["started_at"] = m_indexing ? QJsonValue(m_startedAt) : QJsonValue(QJsonValue::Null);

    // Progression de la passe en cours : le total de dossiers est un dénominateur
    // fiable (une barre déterminée), le compteur de fichiers montre l'avancée dans un
    // gros dossier. current_folder null si la passe vient juste de démarrer.
    if (m_indexing) {
        QJsonObject prog;
        prog["folders_total"]  = m_progFoldersTotal;
        prog["folders_done"]   = m_progFoldersDone;
        prog["files_seen"]     = static_cast<double>(m_progFilesSeen);
        prog["current_folder"] = m_progCurrentFolder.isEmpty()
                                     ? QJsonValue(QJsonValue::Null)
                                     : QJsonValue(m_progCurrentFolder);
        o["progress"] = prog;
    }

    // État du prérequis ExifTool. Un binaire absent ne fait pas tomber le service
    // (les fichiers sont indexés), mais laisse les métadonnées vides : le dire ici
    // évite une enquête sur des colonnes EXIF mystérieusement nulles.
    QJsonObject exif;
    exif["available"] = m_exiftoolReady;
    exif["binary"]    = m_exiftoolBinary;
    exif["detail"]    = m_exiftoolDetail.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                   : QJsonValue(m_exiftoolDetail);
    o["exiftool"] = exif;

    // last_error : priorité à l'erreur de la dernière passe ; à défaut, un ExifTool
    // introuvable est une cause d'échec permanente qui mérite ce champ (sinon
    // l'extraction rate chaque fichier sans que rien ne remonte au niveau module).
    QString err = m_lastError;
    if (err.isEmpty() && !m_exiftoolReady)
        err = QStringLiteral("ExifTool introuvable (%1) : les photos sont indexees mais "
                             "les metadonnees restent vides. Installer le paquet "
                             "libimage-exiftool-perl.").arg(m_exiftoolBinary);
    o["last_error"] = err.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(err);
    return o;
}

QJsonObject PhotoModule::indexStatus() const {
    QJsonObject o = observableState();
    bool found = false;
    const QJsonObject run = m_readRepo ? m_readRepo->latestRun(&found) : QJsonObject{};
    o["last_run"] = found ? QJsonValue(run) : QJsonValue(QJsonValue::Null);
    return o;
}

// --- Lectures ---------------------------------------------------------------

QJsonObject PhotoModule::summary() const {
    return m_readRepo ? m_readRepo->summary() : QJsonObject{};
}
QJsonObject PhotoModule::listPhotos(const QVariantMap& filters, int page, int pageSize) const {
    return m_readRepo ? m_readRepo->listPhotos(filters, page, pageSize) : QJsonObject{};
}
QJsonObject PhotoModule::getPhoto(int fileId, bool* found) const {
    if (!m_readRepo) { if (found) *found = false; return {}; }
    return m_readRepo->getPhoto(fileId, found);
}
QJsonArray PhotoModule::cameras() const { return m_readRepo ? m_readRepo->distinctCameras() : QJsonArray{}; }
QJsonArray PhotoModule::lenses()  const { return m_readRepo ? m_readRepo->distinctLenses()  : QJsonArray{}; }
QJsonArray PhotoModule::focals()  const { return m_readRepo ? m_readRepo->distinctFocals()  : QJsonArray{}; }
QJsonArray PhotoModule::years()   const { return m_readRepo ? m_readRepo->years()           : QJsonArray{}; }
QJsonObject PhotoModule::photoDataset() const { return m_readRepo ? m_readRepo->photoDataset() : QJsonObject{}; }

// --- Dossiers ---------------------------------------------------------------

QJsonArray PhotoModule::listFolders() const {
    return m_readRepo ? m_readRepo->listFoldersDetail() : QJsonArray{};
}

QJsonArray PhotoModule::allowedRoots() const {
    QJsonArray arr;
    for (const QString& root : m_roots)
        arr.append(root);
    return arr;
}

QString PhotoModule::matchingRoot(const QString& path) const {
    for (const QString& root : m_roots)
        if (isWithinRoots(path, {root}))
            return root;
    return {};
}

bool PhotoModule::addFolder(const QString& path, const QVariant& label, bool recursive,
                            QJsonObject* out, QString* error) {
    // Validation « sous racine » : règle de sécurité, portée par le métier, donc
    // valable quelle que soit la porte (y compris une requête HTTP forgée).
    const QString root = matchingRoot(path);
    if (root.isEmpty()) {
        if (error) *error = QStringLiteral("le dossier %1 n'est sous aucune racine autorisee").arg(path);
        return false;
    }
    if (!m_readRepo) { if (error) *error = QStringLiteral("base non ouverte"); return false; }

    // Le chemin est UNIQUE. S'il est déjà connu, ne pas échouer bêtement sur la
    // contrainte : un chemin retiré se restaure, un chemin déjà surveillé est
    // signalé tel quel (message clair plutôt qu'« insertion impossible »).
    bool deleted = false;
    const int existing = m_readRepo->folderIdByPath(path, &deleted);
    if (existing >= 0) {
        if (!deleted) {
            if (error) *error = QStringLiteral("le dossier %1 est deja surveille").arg(path);
            return false;
        }
        return restoreFolder(existing, out);
    }

    const int id = m_readRepo->addFolder(path, root, label, recursive, nowIso());
    if (id < 0) { if (error) *error = QStringLiteral("insertion impossible"); return false; }
    if (out) *out = findById(m_readRepo->listFoldersDetail(), id);
    return true;
}

bool PhotoModule::setFolderEnabled(int folderId, bool enabled, QJsonObject* out) {
    if (!m_readRepo || !m_readRepo->getFolder(folderId))
        return false;
    m_readRepo->setFolderEnabled(folderId, enabled);
    if (out) *out = findById(m_readRepo->listFoldersDetail(), folderId);
    return true;
}

bool PhotoModule::removeFolder(int folderId) {
    if (!m_readRepo || !m_readRepo->getFolder(folderId))
        return false;
    // Retrait DOUX : historique conservé, sélection désactivée (voir contrat).
    m_readRepo->softDeleteFolder(folderId, nowIso());
    return true;
}

bool PhotoModule::restoreFolder(int folderId, QJsonObject* out) {
    if (!m_readRepo || !m_readRepo->getFolder(folderId))
        return false;
    m_readRepo->restoreFolder(folderId);
    if (out) *out = findById(m_readRepo->listFoldersDetail(), folderId);
    // Raviver les fichiers tout de suite plutôt que d'attendre la passe périodique.
    triggerIndex(IndexMode::Incremental, {folderId}, QStringLiteral("restore"));
    return true;
}

void PhotoModule::reconcileRoots() {
    if (!m_readRepo)
        return;
    const QJsonArray folders = m_readRepo->listFoldersDetail();
    for (const QJsonValue& v : folders) {
        const QJsonObject f = v.toObject();
        const int id = f.value(QStringLiteral("id")).toInt();
        const QString path = f.value(QStringLiteral("path")).toString();
        const bool enabled = f.value(QStringLiteral("enabled")).toInt() == 1;
        const bool autoDisabled = f.value(QStringLiteral("auto_disabled")).toInt() == 1;
        const bool covered = isWithinRoots(path, m_roots);
        if (!covered && enabled)
            m_readRepo->setFolderAutoDisabled(id, true);
        else if (covered && autoDisabled)
            m_readRepo->setFolderAutoDisabled(id, false);
    }
}

// --- /modules ---------------------------------------------------------------

QJsonObject PhotoModule::statusJson() const {
    QJsonObject o = observableState();
    if (m_readRepo && m_readRepo->isOpen()) {
        const QJsonObject s = m_readRepo->summary();
        o["files_present"]  = s.value(QStringLiteral("files_present"));
        o["files_missing"]  = s.value(QStringLiteral("files_missing"));
        o["folders_active"] = s.value(QStringLiteral("folders_active"));

        // Verdict de la dernière passe, en compact : done|interrupted|null. Permet à
        // morfMonitor de distinguer une indexation terminée normalement d'une passe
        // interrompue par une source indisponible, sans parser tout `last_run`.
        bool found = false;
        const QJsonObject run = m_readRepo->latestRun(&found);
        o["last_index_state"] = found ? run.value(QStringLiteral("state"))
                                      : QJsonValue(QJsonValue::Null);
    }
    return o;
}

} // namespace morfphoto
