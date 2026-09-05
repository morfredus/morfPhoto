/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/PhotoModule.h"
#include "morfphoto/PhotoRepository.h"
#include "morfphoto/ExifExtractor.h"
#include "morfphoto/PhotoScanner.h"
#include "morfphoto/ContextFile.h"
#include "morfphoto/Paths.h"

#include <QTimer>
#include <QtConcurrent>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QHostInfo>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QLoggingCategory>

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

    // Sources SMB poussées par PhotoHub : leurs points de montage sont des racines
    // autorisées au même titre que celles de la config. Les charger AVANT la
    // réconciliation, pour qu'une sélection sous un montage poussé reste couverte.
    m_sourceManager.load();
    for (const QString& mp : m_sourceManager.mountpoints())
        if (!m_roots.contains(mp))
            m_roots << mp;

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

    // Surveillance périodique : réconciliation incrémentale à cadence fixe. Une
    // cadence <= 0 désactive complètement l'automatique : aucune passe périodique,
    // aucune passe au démarrage. La base n'évolue plus alors que sur demande
    // explicite (API /index ou bouton PhotoHub) — utile pour ne mettre AUCUNE
    // pression de fond sur une machine modeste ou une source réseau.
    if (m_intervalMs > 0) {
        m_watchTimer = new QTimer(this);
        m_watchTimer->setInterval(m_intervalMs);
        connect(m_watchTimer, &QTimer::timeout, this, [this]() {
            triggerIndex(IndexMode::Incremental, {}, QStringLiteral("watch"));
        });
        m_watchTimer->start();

        // Première passe dès le démarrage, pour rendre la base cohérente tout de suite.
        triggerIndex(IndexMode::Incremental, {}, QStringLiteral("watch"));
    }

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
        m_startedAtEpoch = QDateTime::currentSecsSinceEpoch();
        m_lastError.clear();
        // Repartir d'une progression vierge : l'ancienne passe ne doit pas laisser
        // de compteurs résiduels visibles au tout début de la nouvelle.
        m_progFoldersTotal = 0;
        m_progFoldersDone  = 0;
        m_progFilesSeen    = 0;
        m_progFilesTotal   = -1;
        m_progFilesTotalFinal = false;
        m_progPhase        = QStringLiteral("indexing");
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
    QStringList roots;
    {
        QMutexLocker lock(&m_stateMutex);
        pass = ++m_passCounter;
        // Copie des racines sous verrou : addSource() (thread HTTP) peut en ajouter
        // une pendant qu'une passe démarre. La passe travaille sur son instantané.
        roots = m_roots;
    }
    QString error;
    {
        // Connexion et process ExifTool PROPRES à ce thread de passe.
        PhotoRepository repo(QStringLiteral("morfphoto:index:%1").arg(pass));
        if (repo.open(m_dbPath)) {
            ExifExtractor extractor(m_exiftoolBinary, m_stayOpen);
            extractor.open();
            Indexer indexer(&repo, &extractor, roots, m_probeTimeoutMs);
            // Relais de progression : l'Indexer rappelle depuis CE thread ; on recopie
            // sous verrou dans les membres lus par observableState() (thread HTTP).
            indexer.setProgressCallback([this](const IndexProgress& p) {
                QMutexLocker lock(&m_stateMutex);
                m_progFoldersDone   = p.foldersDone;
                m_progFoldersTotal  = p.foldersTotal;
                m_progFilesSeen     = p.filesSeen;
                m_progFilesTotal    = p.filesTotal;
                m_progFilesTotalFinal = p.filesTotalFinal;
                m_progPhase         = p.phase;
                m_progCurrentFolder = p.currentFolder;
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
    qint64 startEpoch = 0, filesSeen = 0;
    int    foldersTotal = 0;
    {
        QMutexLocker lock(&m_stateMutex);
        m_indexing = false;
        m_lastError = error;
        m_lastFoldersTotal = m_progFoldersTotal;
        startEpoch   = m_startedAtEpoch;
        filesSeen    = m_progFilesSeen;
        foldersTotal = m_progFoldersTotal;
    }

    // Passe terminee : la declarer a morfAnalytics comme activite HISTORIQUE
    // (contrat `activity/1` §6). Best-effort, hors verrou, jamais bloquant : une
    // telemetrie muette ne doit pas peser sur l'indexation. On est deja dans le
    // thread de passe, la synchronicite du POST (timeout court) est sans impact.
    reportIndexActivity(startEpoch, filesSeen, foldersTotal, error);
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
        prog["phase"]          = m_progPhase.isEmpty()
                                     ? QStringLiteral("indexing")
                                     : m_progPhase;
        prog["folders_total"]  = m_progFoldersTotal;
        prog["folders_done"]   = m_progFoldersDone;
        prog["files_seen"]     = static_cast<double>(m_progFilesSeen);
        prog["current_folder"] = m_progCurrentFolder.isEmpty()
                                     ? QJsonValue(QJsonValue::Null)
                                     : QJsonValue(m_progCurrentFolder);
        prog["files_total_final"] = m_progFilesTotalFinal;
        if (m_progFilesTotal >= 0) {
            prog["files_total"] = static_cast<double>(m_progFilesTotal);
            if (m_progFilesTotal == 0) {
                prog["percent"] = m_progFilesTotalFinal ? 100.0 : 0.0;
            } else {
                double pct = 100.0 * double(m_progFilesSeen) / double(m_progFilesTotal);
                // Tant que le total peut encore grandir, ne jamais afficher 100 % :
                // le dénominateur n'est pas clos, un 100 % mentirait.
                if (!m_progFilesTotalFinal && pct > 99.0)
                    pct = 99.0;
                prog["percent"] = qRound(pct * 10.0) / 10.0;
            }
        } else {
            prog["files_total"] = QJsonValue(QJsonValue::Null);
            prog["percent"]     = QJsonValue(QJsonValue::Null);
        }
        o["progress"] = prog;
    }

    // Cadence de l'indexation automatique : de quoi savoir, côté client, si une
    // passe de fond tourne et à quelle fréquence (ou si tout est à la demande).
    // m_intervalMs est fixé à la construction et ne change plus : lecture sûre.
    QJsonObject watch;
    watch["auto"]        = (m_intervalMs > 0);
    watch["interval_ms"] = m_intervalMs;
    o["watch"] = watch;

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

QJsonObject PhotoModule::activityJson() const {
    // Contrat generique `activity/1` : pendant une passe, on decrit l'indexation
    // en cours ; rien en cours => objet vide (le champ `activity` sera absent de
    // /status). Meme source verrouillee que observableState().
    QMutexLocker lock(&m_stateMutex);
    if (!m_indexing)
        return {};
    QJsonObject a;
    a["type"]  = QStringLiteral("indexation");
    a["state"] = QStringLiteral("running");
    if (m_startedAtEpoch > 0)
        a["started_at"] = static_cast<double>(m_startedAtEpoch);
    a["current"] = static_cast<double>(m_progFilesSeen);
    if (m_progFilesTotal >= 0) {
        a["total"] = static_cast<double>(m_progFilesTotal);
        double pct;
        if (m_progFilesTotal == 0) {
            pct = m_progFilesTotalFinal ? 100.0 : 0.0;
        } else {
            pct = 100.0 * double(m_progFilesSeen) / double(m_progFilesTotal);
            // Tant que le total peut grandir, ne jamais afficher 100 % : le
            // denominateur n'est pas clos, un 100 % mentirait.
            if (!m_progFilesTotalFinal && pct > 99.0)
                pct = 99.0;
        }
        a["progress_percent"] = qRound(pct * 10.0) / 10.0;
    }
    if (!m_progCurrentFolder.isEmpty())
        a["detail"] = m_progCurrentFolder;
    return a;
}

void PhotoModule::reportIndexActivity(qint64 startEpoch, qint64 filesSeen,
                                      int foldersTotal, const QString& error) const {
    // Ou envoyer : variable d'environnement d'abord, puis fichier admin partage
    // (meme convention que morfDeploy). Aucune source => rien n'est emis :
    // morfPhoto ne depend jamais de morfAnalytics.
    QString url = qEnvironmentVariable("MORFANALYTICS_ACTIVITY_URL").trimmed();
    if (url.isEmpty()) {
        QFile f(QStringLiteral("/etc/morfsystem/monitor-activity-url"));
        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text))
            url = QString::fromUtf8(f.readAll()).trimmed();
    }
    if (url.isEmpty())
        return;

    const qint64 end   = QDateTime::currentSecsSinceEpoch();
    const qint64 start = startEpoch > 0 ? startEpoch : end;

    QJsonObject meta;
    meta["files"]   = static_cast<double>(filesSeen);
    meta["folders"] = foldersTotal;
    if (!error.isEmpty())
        meta["error"] = error;

    QJsonObject payload;
    payload["type"]     = QStringLiteral("indexation");
    payload["project"]  = QStringLiteral("morfPhoto");
    payload["machine"]  = QHostInfo::localHostName();
    payload["start_ts"] = static_cast<double>(start);
    payload["end_ts"]   = static_cast<double>(end);
    payload["status"]   = error.isEmpty() ? QStringLiteral("success")
                                          : QStringLiteral("failed");
    payload["metadata"] = meta;

    // POST synchrone avec timeout court : on est dans le thread de passe (deja
    // termine cote indexation), donc bloquer un instant ici n'impacte pas le
    // service. QNAM + QEventLoop local pour ne suspendre que ce thread.
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    QNetworkReply* reply = nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(3000);
    loop.exec();
    if (!reply->isFinished())
        reply->abort();                 // timeout : on renonce, sans bruit ni blocage

    if (reply->error() != QNetworkReply::NoError)
        qWarning().noquote() << "morfPhoto: activite non signalee a morfAnalytics ("
                             << reply->errorString()
                             << ") - sans consequence pour l'indexation";
    reply->deleteLater();
}

QJsonObject PhotoModule::indexStatus() const {
    QJsonObject o = observableState();
    bool found = false;
    const QJsonObject runRaw = m_readRepo ? m_readRepo->latestRun(&found) : QJsonObject{};
    if (found) {
        QJsonObject run = runRaw;
        int lastFolders = 0;
        {
            QMutexLocker lock(&m_stateMutex);
            lastFolders = m_lastFoldersTotal;
        }
        if (lastFolders > 0 && !run.contains(QStringLiteral("folders_total")))
            run[QStringLiteral("folders_total")] = lastFolders;
        o["last_run"] = run;
    } else {
        o["last_run"] = QJsonValue(QJsonValue::Null);
    }
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

// --- Contexte photographique (morfphoto-context/2) --------------------------

QJsonArray PhotoModule::listContexts(const QString& status) const {
    return m_readRepo ? m_readRepo->listContexts(status) : QJsonArray{};
}

QJsonObject PhotoModule::getContext(const QString& directory) const {
    if (!m_readRepo) return {};
    return m_readRepo->getContext(directory);
}

bool PhotoModule::putContext(const QString& directory, const QString& context,
                             const QString& subject, const QVariant& motif,
                             const QVariant& description, QJsonObject* out, QString* error) {
    if (!m_readRepo) { if (error) *error = QStringLiteral("base non ouverte"); return false; }

    // Garde-fou de périmètre : n'écrire un `.morfphoto.json` que sous une racine autorisée.
    if (matchingRoot(directory).isEmpty()) {
        if (error) *error = QStringLiteral("le dossier %1 n'est sous aucune racine autorisee").arg(directory);
        return false;
    }
    // Contrat V2 : context ET subject obligatoires et dans le vocabulaire gelé. La
    // tolérance aux valeurs hors vocabulaire vaut à la LECTURE d'un fichier existant,
    // jamais à l'ÉCRITURE que morfPhoto produit lui-même.
    const QString ctx  = context.trimmed().toUpper();
    const QString subj = subject.trimmed().toUpper();
    if (!ContextFile::isKnownContext(ctx)) {
        if (error) *error = QStringLiteral("context invalide: %1").arg(context);
        return false;
    }
    if (!ContextFile::isKnownSubject(subj)) {
        if (error) *error = QStringLiteral("subject invalide: %1").arg(subject);
        return false;
    }
    // Ne qualifier qu'un répertoire réellement indexé (contient des photos présentes).
    if (!m_readRepo->directoryHasPhotos(directory)) {
        if (error) *error = QStringLiteral("dossier inconnu ou sans photo indexee: %1").arg(directory);
        return false;
    }
    // La source peut dormir (PC éteint la nuit) : son montage réseau figé ferait bloquer
    // l'écriture ~180 s (timeout CIFS) et gèlerait ce worker HTTP. Sonde bornée d'abord :
    // une source injoignable renvoie une erreur claire tout de suite au lieu de bloquer.
    if (!probeAccessible(directory, m_probeTimeoutMs)) {
        if (error) *error = QStringLiteral("source injoignable (endormie ou deconnectee ?) : "
                                           "impossible d'ecrire le contexte dans %1").arg(directory);
        return false;
    }

    // Préserver `created` d'un contexte antérieur ; toujours rafraîchir `updated`.
    const QString nowLocal = QDateTime::currentDateTime().toString(Qt::ISODate);
    const QJsonObject existing = m_readRepo->getContext(directory);
    const QJsonValue createdV = existing.value(QStringLiteral("created"));
    const QString created = (createdV.isString() && !createdV.toString().isEmpty())
        ? createdV.toString() : nowLocal;

    const QByteArray bytes = ContextFile::serialize(ctx, subj, motif, description,
                                                    created, nowLocal);
    QString werr;
    if (!ContextFile::write(directory, bytes, &werr)) {
        if (error) *error = QStringLiteral("ecriture du contexte impossible: %1").arg(werr);
        return false;
    }

    // Mettre à jour la projection tout de suite (sans attendre une passe), à partir du
    // fichier réellement écrit (mtime constaté).
    const QString filePath = QDir(directory).filePath(ContextFile::fileName());
    FolderContext fc;
    fc.directory   = directory;
    fc.schema      = 2;
    fc.context     = ctx;
    fc.subject     = subj;
    fc.motif       = motif;
    fc.description = description;
    fc.created     = created;
    fc.updated     = nowLocal;
    fc.sourceMtime = QFileInfo(filePath).lastModified().toSecsSinceEpoch();
    fc.status      = QStringLiteral("ok");
    m_readRepo->upsertContext(fc);

    if (out) *out = m_readRepo->getContext(directory);
    return true;
}

QByteArray PhotoModule::thumbnail(const QString& path, bool* ok) const {
    if (ok) *ok = false;
    // Sonde bornée AVANT tout accès disque : isWithinRoots (canonicalFilePath) puis
    // exists() bloqueraient ~180 s sur une source endormie et gèleraient ce worker.
    // absolutePath() est purement lexical (aucun stat), donc sûr même mont figé.
    if (!probeAccessible(QFileInfo(path).absolutePath(), m_probeTimeoutMs))
        return {};
    // Garde-fou : ne servir que des fichiers sous une racine autorisee, existants.
    if (!isWithinRoots(path, m_roots))
        return {};
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return {};
    if (!m_exiftoolReady)
        return {};   // sans exiftool, pas d'extraction possible

    // Apercu JPEG embarque : ThumbnailImage (petit, rapide, present sur JPEG comme RAW),
    // repli PreviewImage (plus grand, surtout sur les RAW). Process one-off borne.
    auto extract = [this](const QString& tag, const QString& p) -> QByteArray {
        QProcess proc;
        proc.start(m_exiftoolBinary, {QStringLiteral("-b"), tag, p});
        if (!proc.waitForStarted(3000))
            return {};
        if (!proc.waitForFinished(8000)) {
            proc.kill();
            proc.waitForFinished(1000);
            return {};
        }
        return proc.readAllStandardOutput();
    };

    QByteArray jpeg = extract(QStringLiteral("-ThumbnailImage"), path);
    if (jpeg.isEmpty())
        jpeg = extract(QStringLiteral("-PreviewImage"), path);
    if (jpeg.isEmpty())
        return {};
    if (ok) *ok = true;
    return jpeg;
}

bool PhotoModule::deleteContext(const QString& directory, QJsonObject* out, QString* error) {
    if (!m_readRepo) { if (error) *error = QStringLiteral("base non ouverte"); return false; }
    if (matchingRoot(directory).isEmpty()) {
        if (error) *error = QStringLiteral("le dossier %1 n'est sous aucune racine autorisee").arg(directory);
        return false;
    }
    // Source endormie : sonde bornée avant de toucher le montage (voir putContext).
    if (!probeAccessible(directory, m_probeTimeoutMs)) {
        if (error) *error = QStringLiteral("source injoignable (endormie ou deconnectee ?) : "
                                           "impossible de retirer le contexte de %1").arg(directory);
        return false;
    }
    QString rerr;
    if (!ContextFile::remove(directory, &rerr)) {
        if (error) *error = QStringLiteral("suppression du contexte impossible: %1").arg(rerr);
        return false;
    }
    m_readRepo->deleteContextRow(directory);
    if (out) *out = m_readRepo->getContext(directory);   // status:"unqualified"
    return true;
}

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

// --- Sources SMB poussées ---------------------------------------------------

bool PhotoModule::addSource(const QString& host, const QString& share, const QString& username,
                            const QString& password, const QString& hostname, bool writable,
                            QJsonObject* out, QString* error) {
    QJsonObject src;
    QString err;
    if (!m_sourceManager.addSource(host, share, username, password, hostname, writable, &src, &err)) {
        if (error) *error = err;
        if (out) *out = src;
        return false;
    }
    const QString mp = src.value(QStringLiteral("mountpoint")).toString();
    {
        QMutexLocker lock(&m_stateMutex);
        if (!m_roots.contains(mp))
            m_roots << mp;
    }
    // Reactiver une selection eventuellement desactivee avant que la racine existe.
    // Pas d'indexation automatique : PhotoHub la declenche apres choix du dossier.
    reconcileRoots();
    // Redemarrage APRES la reponse HTTP, uniquement si morfphoto.json a change :
    // le client doit voir le service recharger vraiment la racine. Si la config
    // etait deja correcte, on evite un redemarrage inutile.
    if (src.value(QStringLiteral("restart_needed")).toBool()) {
        QTimer::singleShot(1500, this, [this]() {
            m_sourceManager.scheduleServiceRestart();
        });
    }
    if (out) *out = src;
    return true;
}

QJsonArray PhotoModule::listSources() const {
    return m_sourceManager.listSources();
}

bool PhotoModule::helperReady(QJsonObject* out, QString* error) const {
    return m_sourceManager.helperReady(out, error);
}

bool PhotoModule::addFolder(const QString& path, const QVariant& label, bool recursive,
                            bool removable, const QVariant& volumeLabel,
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
        // Ré-ajout d'un chemin retiré : le restaurer, puis réappliquer les réglages
        // de support demandés (l'utilisateur peut le re-déclarer amovible et nommer
        // son volume au passage).
        if (!restoreFolder(existing, out))
            return false;
        m_readRepo->setFolderMedia(existing, removable, volumeLabel);
        if (out) *out = findById(m_readRepo->listFoldersDetail(), existing);
        return true;
    }

    const int id = m_readRepo->addFolder(path, root, label, recursive, removable, volumeLabel, nowIso());
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

bool PhotoModule::setFolderMedia(int folderId, bool removable, const QVariant& volumeLabel,
                                 QJsonObject* out) {
    if (!m_readRepo || !m_readRepo->getFolder(folderId))
        return false;
    m_readRepo->setFolderMedia(folderId, removable, volumeLabel);
    if (out) *out = findById(m_readRepo->listFoldersDetail(), folderId);
    return true;
}

bool PhotoModule::setFolderAnalyticsExcluded(int folderId, bool excluded, QJsonObject* out) {
    if (!m_readRepo || !m_readRepo->getFolder(folderId))
        return false;
    m_readRepo->setFolderAnalyticsExcluded(folderId, excluded);
    if (out) *out = findById(m_readRepo->listFoldersDetail(), folderId);
    return true;
}

QJsonObject PhotoModule::purge(const QString& scope, const QVariant& value) {
    QJsonObject o;
    o["scope"] = scope;
    if (!m_readRepo) {
        o["error"] = QStringLiteral("base non ouverte");
        return o;
    }
    // Une passe pourrait écrire pendant qu'on efface : refuser la purge tant qu'une
    // indexation tourne (garde simple, cohérente avec « une seule passe à la fois »).
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_indexing) {
            o["error"] = QStringLiteral("indexation en cours, reessayer apres la passe");
            return o;
        }
    }
    int deleted = 0;
    if (scope == QLatin1String("folder")) {
        bool ok = false;
        const int id = value.toInt(&ok);
        if (!ok) { o["error"] = QStringLiteral("id de dossier invalide"); return o; }
        deleted = m_readRepo->purgeFolder(id);
    } else if (scope == QLatin1String("year")) {
        bool ok = false;
        const int year = value.toInt(&ok);
        if (!ok) { o["error"] = QStringLiteral("annee invalide"); return o; }
        deleted = m_readRepo->purgeByYear(year);
    } else if (scope == QLatin1String("camera")) {
        const QString cam = value.toString();
        if (cam.isEmpty()) { o["error"] = QStringLiteral("boitier vide"); return o; }
        deleted = m_readRepo->purgeByCamera(cam);
    } else if (scope == QLatin1String("all")) {
        deleted = m_readRepo->purgeAll();
    } else {
        o["error"] = QStringLiteral("portee inconnue: %1").arg(scope);
        return o;
    }
    o["deleted"] = deleted;
    return o;
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
