/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/PhotoRepository.h"
#include "morfphoto/PhotoSchema.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QJsonDocument>
#include <QJsonValue>
#include <QVariant>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

#include <utility>

namespace morfphoto {

namespace {

// Exécute une requête préparée avec ses valeurs liées positionnelles. Journalise
// et renvoie false en cas d'échec : une erreur SQL ne s'avale jamais en silence.
bool run(QSqlQuery& q, const QString& sql, const QVariantList& binds = {}) {
    if (!q.prepare(sql)) {
        qWarning() << "morfPhoto SQL prepare:" << q.lastError().text() << sql;
        return false;
    }
    for (const QVariant& b : binds)
        q.addBindValue(b);
    if (!q.exec()) {
        qWarning() << "morfPhoto SQL exec:" << q.lastError().text() << sql;
        return false;
    }
    return true;
}

// Convertit la ligne courante d'une requête en objet JSON. Les NULL deviennent
// des null JSON ; raw_exif (stocké en texte JSON) est rendu comme objet.
QJsonObject rowToJson(const QSqlQuery& q) {
    QJsonObject o;
    const QSqlRecord rec = q.record();
    for (int i = 0; i < rec.count(); ++i) {
        const QString name = rec.fieldName(i);
        const QVariant v = q.value(i);
        if (v.isNull()) {
            // Une colonne NULL doit ressortir en null JSON, pas en "" ni 0.
            o[name] = QJsonValue::Null;
            continue;
        }
        if (name == QLatin1String("raw_exif")) {
            const QJsonDocument d = QJsonDocument::fromJson(v.toString().toUtf8());
            o[name] = d.isObject() ? QJsonValue(d.object()) : QJsonValue::Null;
            continue;
        }
        o[name] = QJsonValue::fromVariant(v);
    }
    return o;
}

// Sérialise les tags bruts pour la colonne raw_exif (NULL si vide).
QVariant rawExifVariant(const ExifData& exif) {
    if (exif.raw.isEmpty())
        return {};
    return QString::fromUtf8(QJsonDocument(exif.raw).toJson(QJsonDocument::Compact));
}

// Traduction filtre public -> colonne SQL. Liste blanche : aucune autre clé
// n'atteint le SQL, pas d'injection possible par un nom de champ.
QString filterColumn(const QString& key) {
    if (key == QLatin1String("year"))   return QStringLiteral("taken_year");
    if (key == QLatin1String("camera")) return QStringLiteral("camera_model");
    if (key == QLatin1String("lens"))   return QStringLiteral("lens");
    if (key == QLatin1String("type"))   return QStringLiteral("file_type");
    if (key == QLatin1String("folder")) return QStringLiteral("folder_id");
    if (key == QLatin1String("state"))  return QStringLiteral("state");
    return {};
}

} // namespace

PhotoRepository::PhotoRepository(QString connectionName)
    : m_connectionName(std::move(connectionName)) {}

PhotoRepository::~PhotoRepository() {
    close();
}

QSqlDatabase PhotoRepository::db() const {
    return QSqlDatabase::database(m_connectionName);
}

bool PhotoRepository::open(const QString& dbPath) {
    // Créer le dossier de la base au besoin (première installation).
    const QFileInfo fi(dbPath);
    QDir().mkpath(fi.absolutePath());

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        m_openError = QStringLiteral("driver QSQLITE indisponible");
        qWarning() << "morfPhoto:" << m_openError;
        return false;
    }
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(dbPath);
    if (!database.open()) {
        m_openError = database.lastError().text();
        qWarning() << "morfPhoto: ouverture base impossible" << dbPath << m_openError;
        return false;
    }
    // Scoper les PRAGMA : une requête encore active bloquerait le commit des
    // migrations (« SQL statements in progress »). La détruire la finalise.
    {
        QSqlQuery q(database);
        q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
        // WAL : lectures concurrentes (API) pendant l'écriture (indexation).
        q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
        // Attendre plutôt qu'échouer si une autre connexion écrit au même moment
        // (l'API et une passe d'indexation ont chacune leur connexion).
        q.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
    }
    return applyMigrations();
}

void PhotoRepository::close() {
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase database = QSqlDatabase::database(m_connectionName);
            if (database.isOpen())
                database.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool PhotoRepository::isOpen() const {
    return QSqlDatabase::contains(m_connectionName) && db().isOpen();
}

bool PhotoRepository::applyMigrations() {
    QSqlDatabase database = db();
    int current = 0;
    {
        // Scoper la lecture : la requête doit être finalisée avant d'ouvrir la
        // transaction, sinon le commit échoue (« SQL statements in progress »).
        QSqlQuery q(database);
        if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
            m_openError = QStringLiteral("user_version: ") + q.lastError().text();
            qWarning() << "morfPhoto: lecture user_version impossible" << q.lastError().text();
            return false;
        }
        current = q.value(0).toInt();
    }
    if (current >= kSchemaVersion)
        return true;

    // Appliquer, dans l'ordre, chaque version de schéma manquante. Chaque version
    // est sa propre transaction scellée par `PRAGMA user_version` : une base en v1
    // reçoit uniquement la v2, sans rejouer la création initiale.
    for (int version = current + 1; version <= kSchemaVersion; ++version) {
        const bool inTx = database.transaction();
        for (const QString& stmt : migrationStatements(version)) {
            QSqlQuery m(database);
            if (!m.exec(stmt)) {
                m_openError = QStringLiteral("migration v%1: %2").arg(version).arg(m.lastError().text());
                qWarning() << "morfPhoto: migration echouee" << version << m.lastError().text() << stmt;
                if (inTx) database.rollback();
                return false;
            }
        }
        // user_version est un entier de PRAGMA (pas de valeur externe) : interpolation sûre.
        { QSqlQuery v(database); v.exec(QStringLiteral("PRAGMA user_version = %1").arg(version)); }
        if (inTx && !database.commit()) {
            m_openError = QStringLiteral("commit v%1: %2").arg(version).arg(database.lastError().text());
            qWarning() << "morfPhoto: commit migration echoue" << version << database.lastError().text();
            return false;
        }
    }
    return true;
}

// --- Dossiers ---------------------------------------------------------------

int PhotoRepository::addFolder(const QString& path, const QString& rootPath,
                               const QVariant& label, bool recursive,
                               const QString& addedAt) {
    QSqlQuery q(db());
    if (!run(q, QStringLiteral(
                 "INSERT INTO folders (path, root_path, label, recursive, added_at) "
                 "VALUES (?, ?, ?, ?, ?)"),
             {path, rootPath, label, recursive ? 1 : 0, addedAt}))
        return -1;
    return q.lastInsertId().toInt();
}

QVector<FolderRow> PhotoRepository::activeFolders() {
    QVector<FolderRow> out;
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT id, path, root_path, recursive, enabled "
                          "FROM folders WHERE enabled = 1 AND deleted_at IS NULL"));
    while (q.next())
        out.push_back({q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(),
                       q.value(3).toBool(), q.value(4).toBool()});
    return out;
}

QVector<FolderRow> PhotoRepository::allFolders() {
    QVector<FolderRow> out;
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT id, path, root_path, recursive, enabled FROM folders"));
    while (q.next())
        out.push_back({q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(),
                       q.value(3).toBool(), q.value(4).toBool()});
    return out;
}

void PhotoRepository::setFolderAutoDisabled(int folderId, bool autoDisabled) {
    // Couper (ou rétablir) la surveillance sans jamais toucher aux données
    // historiques. enabled suit auto_disabled pour arrêter les passes.
    QSqlQuery q(db());
    run(q, QStringLiteral("UPDATE folders SET auto_disabled = ?, enabled = ? WHERE id = ?"),
        {autoDisabled ? 1 : 0, autoDisabled ? 0 : 1, folderId});
}

void PhotoRepository::setFolderScanned(int folderId, const QString& when) {
    QSqlQuery q(db());
    run(q, QStringLiteral("UPDATE folders SET last_scan_at = ? WHERE id = ?"), {when, folderId});
}

bool PhotoRepository::getFolder(int folderId, FolderRow* out) {
    QSqlQuery q(db());
    run(q, QStringLiteral("SELECT id, path, root_path, recursive, enabled "
                          "FROM folders WHERE id = ?"), {folderId});
    if (!q.next())
        return false;
    if (out)
        *out = {q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(),
                q.value(3).toBool(), q.value(4).toBool()};
    return true;
}

QJsonArray PhotoRepository::listFoldersDetail() {
    QJsonArray arr;
    QSqlQuery q(db());
    q.exec(QStringLiteral(
        "SELECT id, path, root_path, label, enabled, auto_disabled, recursive, "
        "added_at, last_scan_at, deleted_at FROM folders ORDER BY path"));
    while (q.next())
        arr.append(rowToJson(q));
    return arr;
}

void PhotoRepository::setFolderEnabled(int folderId, bool enabled) {
    // Action volontaire de l'utilisateur : elle lève le drapeau auto_disabled.
    QSqlQuery q(db());
    run(q, QStringLiteral("UPDATE folders SET enabled = ?, auto_disabled = 0 WHERE id = ?"),
        {enabled ? 1 : 0, folderId});
}

void PhotoRepository::softDeleteFolder(int folderId, const QString& now) {
    // Retrait DOUX : les fichiers passent 'deleted' (conservés), la sélection est
    // désactivée et horodatée. La donnée n'est jamais supprimée physiquement ;
    // `deleted_at` la classe comme retirée (fenêtre séparée dans PhotoHub).
    QSqlQuery q1(db());
    run(q1, QStringLiteral("UPDATE files SET state = 'deleted', disappeared_at = ? "
                           "WHERE folder_id = ? AND state != 'deleted'"), {now, folderId});
    QSqlQuery q2(db());
    run(q2, QStringLiteral("UPDATE folders SET enabled = 0, deleted_at = ? WHERE id = ?"),
        {now, folderId});
}

void PhotoRepository::restoreFolder(int folderId) {
    // Annule un retrait doux : la sélection redevient surveillée et active. Les
    // fichiers restent 'deleted' jusqu'à la prochaine passe, qui les ravive
    // (touchSeen/updateFile repassent 'present'). auto_disabled est remis à plat :
    // c'est un choix volontaire de l'utilisateur, comme setFolderEnabled.
    QSqlQuery q(db());
    run(q, QStringLiteral("UPDATE folders SET enabled = 1, auto_disabled = 0, "
                          "deleted_at = NULL WHERE id = ?"), {folderId});
}

int PhotoRepository::folderIdByPath(const QString& path, bool* deleted) {
    QSqlQuery q(db());
    run(q, QStringLiteral("SELECT id, deleted_at FROM folders WHERE path = ?"), {path});
    if (!q.next())
        return -1;
    if (deleted)
        *deleted = !q.value(1).isNull();
    return q.value(0).toInt();
}

// --- Fichiers ---------------------------------------------------------------

KnownFiles PhotoRepository::existingFilesInFolder(int folderId) {
    KnownFiles map;
    QSqlQuery q(db());
    run(q, QStringLiteral("SELECT id, path, size, mtime, state FROM files WHERE folder_id = ?"),
        {folderId});
    while (q.next())
        map.insert(q.value(1).toString(),
                   {q.value(0).toInt(), q.value(2).toLongLong(),
                    q.value(3).toLongLong(), q.value(4).toString()});
    return map;
}

void PhotoRepository::insertFile(int folderId, const FileInfo& info, const ExifData& exif,
                                 bool exifOk, const QString& now) {
    QSqlQuery q(db());
    run(q, QStringLiteral(
             "INSERT INTO files (folder_id, path, directory, filename, extension, file_type, "
             "size, mtime, state, exif_ok, first_seen_at, last_seen_at, last_indexed_at, "
             "taken_at, make, camera_model, lens, focal_length, focal_length_35mm, aperture, "
             "shutter_speed, shutter_speed_s, iso, exposure_compensation, rating, latitude, "
             "longitude, raw_exif) VALUES "
             "(?,?,?,?,?,?,?,?,'present',?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"),
        {folderId, info.path, info.directory, info.filename, info.extension, info.fileType,
         info.size, info.mtime, exifOk ? 1 : 0, now, now, now,
         exif.takenAt, exif.make, exif.cameraModel, exif.lens, exif.focalLength,
         exif.focalLength35mm, exif.aperture, exif.shutterSpeed, exif.shutterSpeedS,
         exif.iso, exif.exposureCompensation, exif.rating, exif.latitude, exif.longitude,
         rawExifVariant(exif)});
}

void PhotoRepository::updateFile(int fileId, const FileInfo& info, const ExifData& exif,
                                 bool exifOk, const QString& now) {
    // Ravive un fichier qui était 'missing'. Si l'extraction a échoué, mettre à
    // jour size/mtime mais NE PAS toucher aux colonnes EXIF : préférer garder
    // l'ancienne valeur plutôt que de l'effacer sur une erreur transitoire.
    QString sql = QStringLiteral(
        "UPDATE files SET directory = ?, filename = ?, extension = ?, file_type = ?, "
        "size = ?, mtime = ?, exif_ok = ?, last_seen_at = ?, last_indexed_at = ?, "
        "state = 'present', disappeared_at = NULL");
    QVariantList binds = {info.directory, info.filename, info.extension, info.fileType,
                          info.size, info.mtime, exifOk ? 1 : 0, now, now};
    if (exifOk) {
        sql += QStringLiteral(
            ", taken_at = ?, make = ?, camera_model = ?, lens = ?, focal_length = ?, "
            "focal_length_35mm = ?, aperture = ?, shutter_speed = ?, shutter_speed_s = ?, "
            "iso = ?, exposure_compensation = ?, rating = ?, latitude = ?, longitude = ?, "
            "raw_exif = ?");
        binds += QVariantList{exif.takenAt, exif.make, exif.cameraModel, exif.lens,
                              exif.focalLength, exif.focalLength35mm, exif.aperture,
                              exif.shutterSpeed, exif.shutterSpeedS, exif.iso,
                              exif.exposureCompensation, exif.rating, exif.latitude,
                              exif.longitude, rawExifVariant(exif)};
    }
    sql += QStringLiteral(" WHERE id = ?");
    binds += fileId;
    QSqlQuery q(db());
    run(q, sql, binds);
}

void PhotoRepository::touchSeen(int fileId, const QString& now) {
    // Fichier inchangé : noter qu'il est toujours là. Ravive un 'missing' réapparu.
    QSqlQuery q(db());
    run(q, QStringLiteral("UPDATE files SET last_seen_at = ?, state = 'present', "
                          "disappeared_at = NULL WHERE id = ?"), {now, fileId});
}

void PhotoRepository::markMissing(const QVector<int>& fileIds, const QString& now) {
    if (fileIds.isEmpty())
        return;
    QStringList holders;
    QVariantList binds = {now};
    for (int id : fileIds) { holders << QStringLiteral("?"); binds << id; }
    QSqlQuery q(db());
    run(q, QStringLiteral("UPDATE files SET state = 'missing', disappeared_at = ? "
                          "WHERE id IN (%1) AND state != 'missing'").arg(holders.join(',')),
        binds);
}

// --- Passes -----------------------------------------------------------------

int PhotoRepository::startRun(const QString& mode, const QString& trigger,
                              const QString& startedAt) {
    QSqlQuery q(db());
    if (!run(q, QStringLiteral(
                 "INSERT INTO index_runs (started_at, mode, trigger, state) "
                 "VALUES (?, ?, ?, 'running')"), {startedAt, mode, trigger}))
        return -1;
    return q.lastInsertId().toInt();
}

void PhotoRepository::finishRun(int runId, const QString& state, const RunCounts& counts,
                                const QString& finishedAt) {
    QSqlQuery q(db());
    run(q, QStringLiteral(
             "UPDATE index_runs SET finished_at = ?, state = ?, files_seen = ?, files_new = ?, "
             "files_updated = ?, files_missing = ?, errors_count = ? WHERE id = ?"),
        {finishedAt, state, counts.seen, counts.created, counts.updated, counts.missing,
         counts.errors, runId});
}

void PhotoRepository::logError(int runId, const QVariant& path, const QString& stage,
                               const QString& message, const QString& when) {
    QSqlQuery q(db());
    run(q, QStringLiteral(
             "INSERT INTO index_errors (run_id, path, stage, message, occurred_at) "
             "VALUES (?, ?, ?, ?, ?)"), {runId, path, stage, message, when});
}

// --- Lectures ---------------------------------------------------------------

QJsonObject PhotoRepository::summary() {
    auto scalar = [this](const QString& sql) -> int {
        QSqlQuery q(db());
        q.exec(sql);
        return q.next() ? q.value(0).toInt() : 0;
    };
    QJsonArray yearsArr = years();
    QJsonObject o;
    o["files_total"]    = scalar(QStringLiteral("SELECT COUNT(*) FROM files"));
    o["files_present"]  = scalar(QStringLiteral("SELECT COUNT(*) FROM files WHERE state='present'"));
    o["files_missing"]  = scalar(QStringLiteral("SELECT COUNT(*) FROM files WHERE state='missing'"));
    o["folders_total"]  = scalar(QStringLiteral("SELECT COUNT(*) FROM folders"));
    o["folders_active"] = scalar(QStringLiteral("SELECT COUNT(*) FROM folders WHERE enabled=1"));
    o["cameras"]        = scalar(QStringLiteral("SELECT COUNT(DISTINCT camera_model) FROM files "
                                                "WHERE state='present' AND camera_model IS NOT NULL"));
    o["lenses"]         = scalar(QStringLiteral("SELECT COUNT(DISTINCT lens) FROM files "
                                                "WHERE state='present' AND lens IS NOT NULL"));
    o["years"]          = yearsArr;
    return o;
}

QJsonObject PhotoRepository::listPhotos(const QVariantMap& filters, int page, int pageSize) {
    QStringList clauses;
    QVariantList binds;
    for (auto it = filters.constBegin(); it != filters.constEnd(); ++it) {
        const QString col = filterColumn(it.key());
        if (col.isEmpty() || it.value().isNull())
            continue;
        clauses << (col + QStringLiteral(" = ?"));
        binds << it.value();
    }
    const QString where = clauses.isEmpty() ? QString()
                                            : QStringLiteral(" WHERE ") + clauses.join(QStringLiteral(" AND "));

    int total = 0;
    {
        QSqlQuery q(db());
        run(q, QStringLiteral("SELECT COUNT(*) FROM files") + where, binds);
        if (q.next())
            total = q.value(0).toInt();
    }

    const int offset = (page - 1) * pageSize;
    QVariantList pageBinds = binds;
    pageBinds << pageSize << offset;
    QSqlQuery q(db());
    // Exclut raw_exif (lourd) ; la fiche complète le donne via getPhoto.
    run(q, QStringLiteral(
             "SELECT id, folder_id, path, directory, filename, extension, file_type, size, mtime, "
             "taken_at, taken_year, make, camera_model, lens, focal_length, focal_length_35mm, "
             "aperture, shutter_speed, shutter_speed_s, iso, exposure_compensation, rating, "
             "latitude, longitude, state FROM files") + where +
             QStringLiteral(" ORDER BY taken_at IS NULL, taken_at, id LIMIT ? OFFSET ?"),
        pageBinds);

    QJsonArray items;
    while (q.next())
        items.append(rowToJson(q));

    QJsonObject o;
    o["items"]     = items;
    o["total"]     = total;
    o["page"]      = page;
    o["page_size"] = pageSize;
    return o;
}

QJsonObject PhotoRepository::getPhoto(int fileId, bool* found) {
    QSqlQuery q(db());
    run(q, QStringLiteral("SELECT * FROM files WHERE id = ?"), {fileId});
    if (!q.next()) {
        if (found) *found = false;
        return {};
    }
    if (found) *found = true;
    return rowToJson(q);
}

QJsonArray PhotoRepository::distinct(const QString& column, const QString& label) {
    // `column` vient d'un appelant interne fixe, jamais d'une entrée client.
    QJsonArray arr;
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT %1 AS %2, COUNT(*) AS count FROM files "
                          "WHERE state='present' AND %1 IS NOT NULL "
                          "GROUP BY %1 ORDER BY count DESC, %2").arg(column, label));
    while (q.next())
        arr.append(rowToJson(q));
    return arr;
}

QJsonArray PhotoRepository::distinctCameras() { return distinct(QStringLiteral("camera_model"), QStringLiteral("camera")); }
QJsonArray PhotoRepository::distinctLenses()  { return distinct(QStringLiteral("lens"), QStringLiteral("lens")); }
QJsonArray PhotoRepository::distinctFocals()  { return distinct(QStringLiteral("focal_length"), QStringLiteral("focal_length")); }

QJsonArray PhotoRepository::years() {
    QJsonArray arr;
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT taken_year AS year, COUNT(*) AS count FROM files "
                          "WHERE state='present' AND taken_year IS NOT NULL "
                          "GROUP BY taken_year ORDER BY taken_year"));
    while (q.next())
        arr.append(rowToJson(q));
    return arr;
}

QJsonObject PhotoRepository::latestRun(bool* found) {
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT * FROM index_runs ORDER BY id DESC LIMIT 1"));
    if (!q.next()) {
        if (found) *found = false;
        return {};
    }
    if (found) *found = true;
    return rowToJson(q);
}

} // namespace morfphoto
