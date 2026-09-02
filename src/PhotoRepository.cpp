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
#include <QDateTime>
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

// Fragment SQL : photos ÉLIGIBLES AUX ANALYSES = présentes ET hors d'un dossier
// sorti des analyses (analytics_excluded). Une archive amovible reste « présente »
// support éjecté (jamais marquée disparue), donc analysée par défaut ; l'utilisateur
// peut la sortir explicitement des analyses sans effacer ses données. Chaîne fixe
// interne, jamais construite depuis une entrée client : aucune injection possible.
QString analyzablePredicate() {
    return QStringLiteral(
        "state = 'present' AND folder_id NOT IN "
        "(SELECT id FROM folders WHERE analytics_excluded = 1)");
}

// Empreinte STABLE inter-machines d'une photo, pour le dédoublonnage de la couche
// d'analyse quand plusieurs postes indexent le MÊME fichier (un CD gravé indexé sur
// deux machines, un dossier partagé). Deux copies partagent nom+taille+date de prise,
// donc la même empreinte -- le CHEMIN est exclu à dessein (il varie d'un poste à
// l'autre, montage ou lettre de lecteur différents). FNV-1a 64 bits, rendu en hexa :
// opaque (l'anonymat du dataset est préservé, aucun nom de fichier n'est exposé),
// compact, déterministe (aucune graine aléatoire, contrairement à qHash).
QString fingerprintOf(const QString& filename, qint64 size, const QString& takenAt) {
    const QByteArray key = (filename.toLower() + QLatin1Char('\x1f')
                            + QString::number(size) + QLatin1Char('\x1f') + takenAt).toUtf8();
    quint64 h = 1469598103934665603ULL;               // offset de base FNV-1a 64 bits
    for (const char c : key) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;                         // nombre premier FNV 64 bits
    }
    return QString::number(h, 16);
}

// Traduction filtre public -> colonne SQL. Liste blanche : aucune autre clé
// n'atteint le SQL, pas d'injection possible par un nom de champ.
QString filterColumn(const QString& key) {
    if (key == QLatin1String("year"))   return QStringLiteral("taken_year");
    if (key == QLatin1String("camera")) return QStringLiteral("camera_model");
    if (key == QLatin1String("lens"))   return QStringLiteral("lens");
    if (key == QLatin1String("type"))   return QStringLiteral("file_type");
    if (key == QLatin1String("folder")) return QStringLiteral("folder_id");
    if (key == QLatin1String("directory")) return QStringLiteral("directory");
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
                               const QVariant& label, bool recursive, bool removable,
                               const QVariant& volumeLabel, const QString& addedAt) {
    QSqlQuery q(db());
    if (!run(q, QStringLiteral(
                 "INSERT INTO folders (path, root_path, label, recursive, removable, "
                 "volume_label, added_at) VALUES (?, ?, ?, ?, ?, ?, ?)"),
             {path, rootPath, label, recursive ? 1 : 0, removable ? 1 : 0, volumeLabel, addedAt}))
        return -1;
    return q.lastInsertId().toInt();
}

QVector<FolderRow> PhotoRepository::activeFolders() {
    QVector<FolderRow> out;
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT id, path, root_path, recursive, enabled, removable "
                          "FROM folders WHERE enabled = 1 AND deleted_at IS NULL"));
    while (q.next())
        out.push_back({q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(),
                       q.value(3).toBool(), q.value(4).toBool(), q.value(5).toBool()});
    return out;
}

QVector<FolderRow> PhotoRepository::allFolders() {
    QVector<FolderRow> out;
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT id, path, root_path, recursive, enabled, removable FROM folders"));
    while (q.next())
        out.push_back({q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(),
                       q.value(3).toBool(), q.value(4).toBool(), q.value(5).toBool()});
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
    run(q, QStringLiteral("SELECT id, path, root_path, recursive, enabled, removable "
                          "FROM folders WHERE id = ?"), {folderId});
    if (!q.next())
        return false;
    if (out)
        *out = {q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(),
                q.value(3).toBool(), q.value(4).toBool(), q.value(5).toBool()};
    return true;
}

QJsonArray PhotoRepository::listFoldersDetail() {
    QJsonArray arr;
    QSqlQuery q(db());
    q.exec(QStringLiteral(
        "SELECT id, path, root_path, label, enabled, auto_disabled, recursive, "
        "removable, volume_label, analytics_excluded, added_at, last_scan_at, deleted_at "
        "FROM folders ORDER BY path"));
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

void PhotoRepository::setFolderMedia(int folderId, bool removable, const QVariant& volumeLabel) {
    // Régler le caractère amovible et le libellé de volume. Ne touche jamais aux
    // fichiers : c'est une propriété de la sélection, appliquée dès la prochaine passe.
    QSqlQuery q(db());
    run(q, QStringLiteral("UPDATE folders SET removable = ?, volume_label = ? WHERE id = ?"),
        {removable ? 1 : 0, volumeLabel, folderId});
}

void PhotoRepository::setFolderAnalyticsExcluded(int folderId, bool excluded) {
    // Bascule NON destructive : les données restent, seules les analyses les ignorent.
    QSqlQuery q(db());
    run(q, QStringLiteral("UPDATE folders SET analytics_excluded = ? WHERE id = ?"),
        {excluded ? 1 : 0, folderId});
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
             "files_updated = ?, files_missing = ?, files_unavailable = ?, errors_count = ? "
             "WHERE id = ?"),
        {finishedAt, state, counts.seen, counts.created, counts.updated, counts.missing,
         counts.unavailable, counts.errors, runId});
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
    // Boîtiers / objectifs / années : dimensions ANALYTIQUES, elles respectent
    // l'exclusion d'analyse (un dossier sorti des analyses n'y compte plus). Les
    // totaux bruts de photothèque ci-dessus (total/présentes/disparues, dossiers)
    // décrivent au contraire TOUTE la base indexée : ils l'ignorent volontairement.
    o["cameras"]        = scalar(QStringLiteral("SELECT COUNT(DISTINCT camera_model) FROM files "
                                                "WHERE ") + analyzablePredicate() +
                                                QStringLiteral(" AND camera_model IS NOT NULL"));
    o["lenses"]         = scalar(QStringLiteral("SELECT COUNT(DISTINCT lens) FROM files "
                                                "WHERE ") + analyzablePredicate() +
                                                QStringLiteral(" AND lens IS NOT NULL"));
    o["years"]          = yearsArr;

    // Couverture du CONTEXTE (morfphoto-context/2), par RÉPERTOIRE de photos présentes.
    // Trois états distincts : qualified (ligne ok), invalid (ligne invalide), unqualified
    // (aucun `.morfphoto.json` valide). unqualified n'est jamais fusionné avec INCONNU,
    // qui est une valeur de `context` (donc comptée parmi les qualifiés).
    const int dirsTotal = scalar(QStringLiteral(
        "SELECT COUNT(*) FROM (SELECT DISTINCT directory FROM files WHERE state='present')"));
    const int qualified = scalar(QStringLiteral(
        "SELECT COUNT(*) FROM folder_contexts c WHERE c.status='ok' AND EXISTS "
        "(SELECT 1 FROM files f WHERE f.directory=c.directory AND f.state='present')"));
    const int invalid = scalar(QStringLiteral(
        "SELECT COUNT(*) FROM folder_contexts c WHERE c.status='invalid' AND EXISTS "
        "(SELECT 1 FROM files f WHERE f.directory=c.directory AND f.state='present')"));
    auto coverageBy = [this](const QString& column) -> QJsonObject {
        QJsonObject by;
        QSqlQuery q(db());
        q.exec(QStringLiteral(
            "SELECT c.%1 AS v, COUNT(*) AS n FROM folder_contexts c "
            "WHERE c.status='ok' AND c.%1 IS NOT NULL AND EXISTS "
            "(SELECT 1 FROM files f WHERE f.directory=c.directory AND f.state='present') "
            "GROUP BY c.%1 ORDER BY n DESC, v").arg(column));
        while (q.next())
            by.insert(q.value(0).toString(), q.value(1).toInt());
        return by;
    };
    QJsonObject contexts;
    contexts["qualified"]   = qualified;
    contexts["invalid"]     = invalid;
    contexts["unqualified"] = dirsTotal - qualified - invalid;
    contexts["by_context"]  = coverageBy(QStringLiteral("context"));
    contexts["by_subject"]  = coverageBy(QStringLiteral("subject"));
    o["contexts"] = contexts;
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
                          "WHERE %3 AND %1 IS NOT NULL "
                          "GROUP BY %1 ORDER BY count DESC, %2")
               .arg(column, label, analyzablePredicate()));
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
                          "WHERE ") + analyzablePredicate() +
                          QStringLiteral(" AND taken_year IS NOT NULL "
                          "GROUP BY taken_year ORDER BY taken_year"));
    while (q.next())
        arr.append(rowToJson(q));
    return arr;
}

QJsonObject PhotoRepository::photoDataset() {
    // Dictionnaires : chaque chaine repetee (boitier, objectif, type) n'est stockee
    // qu'une fois ; les colonnes ne portent que son index. Un QHash retient l'index
    // deja attribue, un QJsonArray garde l'ordre d'apparition (l'index EST la position).
    QJsonArray camDict, lensDict, typeDict, ctxDict, subjDict;
    QHash<QString, int> camIdx, lensIdx, typeIdx, ctxIdx, subjIdx;
    auto intern = [](const QVariant& v, QJsonArray& dict, QHash<QString, int>& idx) -> QJsonValue {
        if (v.isNull())
            return QJsonValue::Null;   // valeur absente preservee (jamais un index bidon)
        const QString s = v.toString();
        auto it = idx.constFind(s);
        if (it != idx.constEnd())
            return it.value();
        const int n = dict.size();
        dict.append(s);
        idx.insert(s, n);
        return n;
    };
    // Un reel brut, ou null si la colonne est NULL (pas de 0 trompeur).
    auto num = [](const QVariant& v) -> QJsonValue {
        return v.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue::fromVariant(v);
    };

    QJsonArray takenAt, camera, lens, fileType, focal, focal35, aperture, iso, shutterS, folder;
    QJsonArray context, subject;   // contexte photographique (morfphoto-context/2)
    QJsonArray fingerprint;   // empreinte de dedup (une par photo), pour l'analyse multi-sources
    int count = 0;

    QSqlQuery q(db());
    // Un seul parcours des photos presentes ; ordre chronologique stable et utile.
    // filename et size ne sont PAS exposes : ils servent uniquement a calculer
    // l'empreinte (le dataset reste anonyme, seule l'empreinte opaque en sort).
    // Jointure GAUCHE sur folder_contexts (cle = repertoire) : context/subject sont deux
    // dimensions INDEPENDANTES, null si le dossier n'est pas qualifie. La distinction
    // null (aucun .morfphoto.json) vs "INCONNU" (valeur choisie) est ainsi preservee.
    q.exec(QStringLiteral(
        "SELECT f.taken_at, f.camera_model, f.lens, f.file_type, f.focal_length, "
        "f.focal_length_35mm, f.aperture, f.iso, f.shutter_speed_s, f.folder_id, "
        "f.filename, f.size, c.context, c.subject "
        "FROM files f LEFT JOIN folder_contexts c ON c.directory = f.directory WHERE ")
        + analyzablePredicate() +
        QStringLiteral(" ORDER BY f.taken_at IS NULL, f.taken_at, f.id"));
    while (q.next()) {
        takenAt.append(num(q.value(0)));
        camera.append(intern(q.value(1), camDict, camIdx));
        lens.append(intern(q.value(2), lensDict, lensIdx));
        fileType.append(intern(q.value(3), typeDict, typeIdx));
        focal.append(num(q.value(4)));
        focal35.append(num(q.value(5)));
        aperture.append(num(q.value(6)));
        iso.append(num(q.value(7)));
        shutterS.append(num(q.value(8)));
        folder.append(num(q.value(9)));
        fingerprint.append(fingerprintOf(q.value(10).toString(), q.value(11).toLongLong(),
                                         q.value(0).toString()));
        context.append(intern(q.value(12), ctxDict, ctxIdx));
        subject.append(intern(q.value(13), subjDict, subjIdx));
        ++count;
    }

    QJsonObject columns{
        {"taken_at", takenAt}, {"camera", camera}, {"lens", lens}, {"file_type", fileType},
        {"focal_length", focal}, {"focal_length_35mm", focal35}, {"aperture", aperture},
        {"iso", iso}, {"shutter_speed_s", shutterS}, {"folder_id", folder},
        {"context", context}, {"subject", subject},
        {"fingerprint", fingerprint},
    };
    QJsonObject dictionaries{
        {"camera", camDict}, {"lens", lensDict}, {"file_type", typeDict},
        {"context", ctxDict}, {"subject", subjDict},
    };

    // Libellés des dossiers : la colonne folder_id porte l'identifiant brut ; cette
    // table de correspondance id -> libellé permet à la couche d'analyse de filtrer
    // et d'étiqueter par dossier sans deviner un nom. Libellé = label si présent,
    // sinon le chemin. Clé en texte (JSON n'a pas de clé entière).
    QJsonObject folders;
    {
        QSqlQuery fq(db());
        fq.exec(QStringLiteral("SELECT id, COALESCE(label, path) FROM folders"));
        while (fq.next())
            folders.insert(QString::number(fq.value(0).toInt()), fq.value(1).toString());
    }

    return QJsonObject{
        {"count", count}, {"dictionaries", dictionaries}, {"columns", columns},
        {"folders", folders},
    };
}

// --- Contexte photographique par répertoire (morfphoto-context/2) -----------
//
// La table folder_contexts est une PROJECTION reconstructible des `.morfphoto.json`.
// Le fichier disque reste souverain ; la base n'est qu'un cache d'index.

void PhotoRepository::upsertContext(const FolderContext& ctx) {
    // UPSERT par répertoire (clé unique) : une passe met à jour sans dupliquer.
    const QString warnings = ctx.warnings.isEmpty()
        ? QString() : ctx.warnings.join(QLatin1Char(','));
    QSqlQuery q(db());
    run(q, QStringLiteral(
             "INSERT INTO folder_contexts "
             "(directory, schema, context, subject, motif, description, created, updated, "
             " source_mtime, status, warnings, error, indexed_at) "
             "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?) "
             "ON CONFLICT(directory) DO UPDATE SET "
             " schema=excluded.schema, context=excluded.context, subject=excluded.subject, "
             " motif=excluded.motif, description=excluded.description, created=excluded.created, "
             " updated=excluded.updated, source_mtime=excluded.source_mtime, "
             " status=excluded.status, warnings=excluded.warnings, error=excluded.error, "
             " indexed_at=excluded.indexed_at"),
        {ctx.directory, ctx.schema, ctx.context, ctx.subject, ctx.motif, ctx.description,
         ctx.created, ctx.updated, ctx.sourceMtime, ctx.status,
         warnings.isEmpty() ? QVariant() : QVariant(warnings), ctx.error,
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)});
}

void PhotoRepository::deleteContextRow(const QString& directory) {
    QSqlQuery q(db());
    run(q, QStringLiteral("DELETE FROM folder_contexts WHERE directory = ?"), {directory});
}

QHash<QString, qint64> PhotoRepository::knownContextMtimes() {
    QHash<QString, qint64> out;
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT directory, source_mtime FROM folder_contexts"));
    while (q.next())
        out.insert(q.value(0).toString(), q.value(1).toLongLong());
    return out;
}

bool PhotoRepository::directoryHasPhotos(const QString& directory) {
    QSqlQuery q(db());
    run(q, QStringLiteral("SELECT 1 FROM files WHERE directory = ? AND state = 'present' LIMIT 1"),
        {directory});
    return q.next();
}

QJsonObject PhotoRepository::getContext(const QString& directory) {
    QJsonObject o;
    o["directory"]   = directory;
    o["photo_count"] = 0;
    {
        QSqlQuery c(db());
        run(c, QStringLiteral("SELECT COUNT(*) FROM files WHERE directory = ? AND state='present'"),
            {directory});
        if (c.next())
            o["photo_count"] = c.value(0).toInt();
    }
    QSqlQuery q(db());
    run(q, QStringLiteral(
             "SELECT schema, context, subject, motif, description, created, updated, "
             "status, warnings, error FROM folder_contexts WHERE directory = ?"),
        {directory});
    if (!q.next()) {
        // Aucune ligne = aucun `.morfphoto.json` valide : NON QUALIFIÉ (distinct d'INCONNU).
        o["status"] = QStringLiteral("unqualified");
        return o;
    }
    // "invalid" en base reste "invalid" côté API ; sinon "qualified".
    const QString st = q.value(7).toString();
    o["status"]      = (st == QLatin1String("invalid")) ? QStringLiteral("invalid")
                                                        : QStringLiteral("qualified");
    auto sv = [](const QVariant& v) -> QJsonValue {
        return v.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(v.toString());
    };
    o["schema"]      = q.value(0).isNull() ? QJsonValue(QJsonValue::Null)
                                           : QJsonValue(q.value(0).toInt());
    o["context"]     = sv(q.value(1));
    o["subject"]     = sv(q.value(2));
    o["motif"]       = sv(q.value(3));
    o["description"] = sv(q.value(4));
    o["created"]     = sv(q.value(5));
    o["updated"]     = sv(q.value(6));
    const QString warnings = q.value(8).toString();
    QJsonArray warr;
    if (!warnings.isEmpty())
        for (const QString& w : warnings.split(QLatin1Char(','), Qt::SkipEmptyParts))
            warr.append(w);
    o["warnings"]    = warr;
    o["error"]       = sv(q.value(9));
    return o;
}

QJsonArray PhotoRepository::listContexts(const QString& statusFilter) {
    QJsonArray arr;
    QSqlQuery q(db());
    // Un répertoire par groupe (files.directory) contenant des photos PRÉSENTES, joint
    // à son contexte éventuel. date = plus ancienne prise de vue du dossier (repli sur
    // le nom du dossier plus bas). L'exclusion analytique n'entre PAS en jeu ici : la
    // qualification concerne toute la photothèque indexée.
    q.exec(QStringLiteral(
        "SELECT f.directory AS directory, COUNT(*) AS photo_count, MIN(f.taken_at) AS date, "
        "c.status, c.context, c.subject, c.motif, c.description, c.updated, c.warnings "
        "FROM files f LEFT JOIN folder_contexts c ON c.directory = f.directory "
        "WHERE f.state = 'present' "
        "GROUP BY f.directory ORDER BY date IS NULL, date, f.directory"));
    while (q.next()) {
        const QString directory = q.value(0).toString();
        const QVariant rawStatus = q.value(3);
        // Statut EXPOSÉ : aucune ligne -> unqualified ; ligne invalide -> invalid ; sinon qualified.
        QString status;
        if (rawStatus.isNull())
            status = QStringLiteral("unqualified");
        else if (rawStatus.toString() == QLatin1String("invalid"))
            status = QStringLiteral("invalid");
        else
            status = QStringLiteral("qualified");
        if (!statusFilter.isEmpty() && statusFilter != status)
            continue;

        auto sv = [](const QVariant& v) -> QJsonValue {
            return v.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(v.toString());
        };
        QJsonObject o;
        o["directory"]   = directory;
        o["label"]       = QFileInfo(directory).fileName();
        o["photo_count"] = q.value(1).toInt();
        const QVariant date = q.value(2);
        // date lisible AAAA-MM-JJ si connue, sinon nom du dossier (souvent daté).
        o["date"] = date.isNull() ? QJsonValue(QFileInfo(directory).fileName())
                                   : QJsonValue(date.toString().left(10));
        o["status"]      = status;
        o["context"]     = sv(q.value(4));
        o["subject"]     = sv(q.value(5));
        o["motif"]       = sv(q.value(6));
        o["description"] = sv(q.value(7));
        o["updated"]     = sv(q.value(8));
        const QString warnings = q.value(9).toString();
        QJsonArray warr;
        if (!warnings.isEmpty())
            for (const QString& w : warnings.split(QLatin1Char(','), Qt::SkipEmptyParts))
                warr.append(w);
        o["warnings"]    = warr;
        arr.append(o);
    }
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

// --- Purge (suppression DÉFINITIVE) -----------------------------------------
//
// Contrairement au retrait doux (state='deleted', réversible), la purge efface
// vraiment les lignes : aucune restauration. Réservée à un choix explicite de
// l'utilisateur (nettoyer un boîtier de test, une année en double, un CD ré-gravé).

int PhotoRepository::purgeFolder(int folderId) {
    // Ordre imposé par la clé étrangère files.folder_id -> folders.id : effacer les
    // fichiers AVANT la sélection, sinon la suppression du dossier échoue.
    QSqlQuery q1(db());
    run(q1, QStringLiteral("DELETE FROM files WHERE folder_id = ?"), {folderId});
    const int n = q1.numRowsAffected();
    QSqlQuery q2(db());
    run(q2, QStringLiteral("DELETE FROM folders WHERE id = ?"), {folderId});
    return n < 0 ? 0 : n;
}

int PhotoRepository::purgeByYear(int year) {
    // taken_year est une colonne calculée (année de prise de vue). Toutes les
    // sélections confondues : c'est une suppression par critère, pas par dossier.
    QSqlQuery q(db());
    run(q, QStringLiteral("DELETE FROM files WHERE taken_year = ?"), {year});
    const int n = q.numRowsAffected();
    return n < 0 ? 0 : n;
}

int PhotoRepository::purgeByCamera(const QString& camera) {
    QSqlQuery q(db());
    run(q, QStringLiteral("DELETE FROM files WHERE camera_model = ?"), {camera});
    const int n = q.numRowsAffected();
    return n < 0 ? 0 : n;
}

int PhotoRepository::purgeAll() {
    // Remise à zéro complète du domaine : fichiers, sélections et historique des
    // passes. L'ordre respecte les clés étrangères (enfants d'abord). La base et son
    // schéma restent en place (aucune migration à rejouer), seulement vidés.
    QSqlQuery qc(db());
    run(qc, QStringLiteral("SELECT COUNT(*) FROM files"));
    const int n = qc.next() ? qc.value(0).toInt() : 0;
    QSqlQuery q1(db()); run(q1, QStringLiteral("DELETE FROM index_errors"));
    QSqlQuery q2(db()); run(q2, QStringLiteral("DELETE FROM files"));
    QSqlQuery q3(db()); run(q3, QStringLiteral("DELETE FROM index_runs"));
    QSqlQuery q4(db()); run(q4, QStringLiteral("DELETE FROM folders"));
    return n;
}

} // namespace morfphoto
