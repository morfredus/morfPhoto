/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Bibliotheque photo (contrat morfphoto-library/1) : ecriture et lectures SQL.
//
// Meme regle que le reste du depot : tout le SQL vit ici. Les filtres sont CONSTRUITS a
// partir d'une liste blanche de fragments fixes ; les valeurs venues du client ne passent
// jamais que par des parametres lies, jamais par concatenation.

#include "morfphoto/PhotoRepository.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>

namespace morfphoto {

namespace {

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

QJsonValue nullable(const QVariant& v) {
    return v.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue::fromVariant(v);
}
QJsonValue nullableBool(const QVariant& v) {
    return v.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(v.toInt() != 0);
}

// Echappe % et _ d'un motif LIKE (ESCAPE '\').
QString likeEscape(const QString& s) {
    QString out;
    for (const QChar c : s) {
        if (c == QLatin1Char('%') || c == QLatin1Char('_') || c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

QString placeholders(int n) {
    QStringList p;
    for (int i = 0; i < n; ++i)
        p << QStringLiteral("?");
    return p.join(QLatin1Char(','));
}

QVariantList numbers(const QStringList& values) {
    QVariantList out;
    for (const QString& v : values) {
        bool ok = false;
        const double d = v.toDouble(&ok);
        if (ok)
            out << d;
    }
    return out;
}

QVariantList strings(const QStringList& values) {
    QVariantList out;
    for (const QString& v : values)
        out << v;
    return out;
}

// Une dimension de filtre : un fragment SQL sur `files f` et ses valeurs liees.
struct Clause {
    QString sql;
    QVariantList binds;
};

// Dimensions d'un filtre. Le nom sert aux facettes : « tous les filtres SAUF le sien ».
enum class Dim { Kind, Camera, Lens, Iso, Aperture, Focal, Shutter, Taken, Folder, Filename,
                 Rating, Rejected, Picked, Edited, ColorLabel, Source, Keyword };

// Fragment « un version satisfait ceci » : EXISTS sur library_versions (alias v).
QString versionExists(const QString& cond) {
    return QStringLiteral("EXISTS (SELECT 1 FROM library_versions v WHERE v.file_id = f.id AND ")
         + cond + QLatin1Char(')');
}

// Construit les clauses du filtre, sans celle de `except` (facettes).
QVector<Clause> buildClauses(const LibraryFilter& f, const Dim* except) {
    QVector<Clause> out;
    auto skip = [except](Dim d) { return except && *except == d; };

    out.append({QStringLiteral("f.state = 'present'"), {}});

    if (!skip(Dim::Kind) && !f.kinds.isEmpty())
        out.append({QStringLiteral("f.file_type IN (%1)").arg(placeholders(f.kinds.size())), strings(f.kinds)});
    if (!skip(Dim::Camera) && !f.cameras.isEmpty())
        out.append({QStringLiteral("f.camera_model IN (%1)").arg(placeholders(f.cameras.size())), strings(f.cameras)});
    if (!skip(Dim::Lens) && !f.lenses.isEmpty())
        out.append({QStringLiteral("f.lens IN (%1)").arg(placeholders(f.lenses.size())), strings(f.lenses)});
    auto numeric = [&](Dim d, const QStringList& values, const char* column) {
        if (skip(d) || values.isEmpty())
            return;
        const QVariantList n = numbers(values);
        // Une valeur non numerique ne correspond a rien : le filtre ne laisse rien passer.
        out.append({n.isEmpty() ? QStringLiteral("0")
                                : QStringLiteral("f.%1 IN (%2)").arg(QLatin1String(column), placeholders(n.size())), n});
    };
    numeric(Dim::Iso, f.iso, "iso");
    numeric(Dim::Aperture, f.aperture, "aperture");
    numeric(Dim::Focal, f.focal, "focal_length");
    if (!skip(Dim::Shutter) && !f.shutter.isEmpty())
        out.append({QStringLiteral("f.shutter_speed IN (%1)").arg(placeholders(f.shutter.size())), strings(f.shutter)});

    if (!skip(Dim::Taken)) {
        if (!f.takenFrom.isEmpty())
            out.append({QStringLiteral("substr(f.taken_at, 1, 10) >= ?"), {f.takenFrom}});
        if (!f.takenTo.isEmpty())
            out.append({QStringLiteral("substr(f.taken_at, 1, 10) <= ?"), {f.takenTo}});
    }
    if (!skip(Dim::Folder) && !f.folderPath.isEmpty()) {
        if (f.recursive)
            out.append({QStringLiteral("(f.directory = ? OR f.directory LIKE ? ESCAPE '\\')"),
                        {f.folderPath, likeEscape(f.folderPath) + QStringLiteral("/%")}});
        else
            out.append({QStringLiteral("f.directory = ?"), {f.folderPath}});
    }
    if (!skip(Dim::Filename) && !f.filename.isEmpty())
        out.append({QStringLiteral("f.filename LIKE ? ESCAPE '\\'"),
                    {QStringLiteral("%") + likeEscape(f.filename) + QStringLiteral("%")}});

    // Filtres de version : une photo passe si UNE de ses versions satisfait la condition.
    // Une valeur inconnue (null) ne passe pas un filtre sur cette valeur.
    if (!skip(Dim::Rating) && f.minRating >= 0)
        out.append({versionExists(QStringLiteral("v.rating >= ?")), {f.minRating}});
    if (!skip(Dim::Rejected)) {
        if (f.rejected == QLatin1String("only"))
            out.append({versionExists(QStringLiteral("v.rejected = 1")), {}});
        else if (f.rejected != QLatin1String("all"))   // hide (defaut) : seule une photo dont
            // TOUTES les versions sont rejetees disparait ; une photo sans sidecar reste.
            out.append({QStringLiteral("NOT (EXISTS (SELECT 1 FROM library_versions v WHERE v.file_id = f.id AND v.rejected = 1) "
                                       "AND NOT EXISTS (SELECT 1 FROM library_versions v WHERE v.file_id = f.id "
                                       "AND (v.rejected IS NULL OR v.rejected = 0)))"), {}});
    }
    if (!skip(Dim::Picked) && (f.picked == QLatin1String("true") || f.picked == QLatin1String("false")))
        out.append({versionExists(QStringLiteral("v.picked = ?")), {f.picked == QLatin1String("true") ? 1 : 0}});
    if (!skip(Dim::Edited) && (f.edited == QLatin1String("edited") || f.edited == QLatin1String("unedited")))
        out.append({versionExists(QStringLiteral("v.edited = ?")), {f.edited == QLatin1String("edited") ? 1 : 0}});
    if (!skip(Dim::ColorLabel)) {
        if (!f.colorLabels.isEmpty()) {
            int mask = 0;
            for (const QString& n : f.colorLabels)
                mask |= labelMaskFromName(n);
            out.append({versionExists(QStringLiteral("(v.labels & ?) != 0")), {mask}});
        }
        if (f.unlabeled)
            out.append({QStringLiteral("NOT EXISTS (SELECT 1 FROM library_versions v WHERE v.file_id = f.id AND v.labels != 0)"), {}});
    }
    if (!skip(Dim::Source) && !f.sources.isEmpty()) {
        // Un sidecar de ce logiciel, ou (pour `file`) une valeur venue de l'XMP embarque.
        QStringList parts;
        QVariantList binds;
        QStringList sidecarSources;
        for (const QString& s : f.sources) {
            if (s == embeddedProviderId())
                parts << QStringLiteral("EXISTS (SELECT 1 FROM library_versions v WHERE v.file_id = f.id AND v.sources LIKE '%\"file\"%')");
            else
                sidecarSources << s;
        }
        if (!sidecarSources.isEmpty()) {
            parts << QStringLiteral("EXISTS (SELECT 1 FROM library_sidecars s WHERE s.file_id = f.id AND s.source IN (%1))")
                         .arg(placeholders(sidecarSources.size()));
            binds = strings(sidecarSources);
        }
        out.append({QLatin1Char('(') + parts.join(QStringLiteral(" OR ")) + QLatin1Char(')'), binds});
    }
    if (!skip(Dim::Keyword) && !f.keywords.isEmpty()) {
        // Un mot-cle hierarchique inclut ses descendants : « Lieux » trouve « Lieux | France ».
        QStringList parts;
        QVariantList binds;
        for (const QString& k : f.keywords) {
            parts << QStringLiteral("(k.keyword = ? OR k.keyword LIKE ? ESCAPE '\\')");
            binds << k << likeEscape(k) + QStringLiteral(" | %");
        }
        out.append({versionExists(QStringLiteral("EXISTS (SELECT 1 FROM library_keywords k WHERE k.version_id = v.id AND (")
                                  + parts.join(QStringLiteral(" OR ")) + QStringLiteral("))")), binds});
    }
    return out;
}

void join(const QVector<Clause>& clauses, QString* where, QVariantList* binds) {
    QStringList sql;
    for (const Clause& c : clauses) {
        sql << c.sql;
        *binds += c.binds;
    }
    *where = sql.join(QStringLiteral(" AND "));
}

QJsonArray keywordArray(const QStringList& k) {
    QJsonArray a;
    for (const QString& s : k)
        a.append(s);
    return a;
}

} // namespace

// --- Ecriture -------------------------------------------------------------------

QHash<QString, PhotoRepository::LibraryFileState> PhotoRepository::libraryState(int folderId) {
    QHash<QString, LibraryFileState> out;
    QSqlQuery q(db());
    if (!run(q, QStringLiteral(
            "SELECT f.path, f.id, lf.fingerprint FROM files f "
            "LEFT JOIN library_files lf ON lf.file_id = f.id WHERE f.folder_id = ?"), {folderId}))
        return out;
    while (q.next())
        out.insert(q.value(0).toString(), {q.value(1).toInt(), q.value(2).toString()});
    return out;
}

QJsonObject PhotoRepository::rawExif(int fileId) {
    QSqlQuery q(db());
    if (run(q, QStringLiteral("SELECT raw_exif FROM files WHERE id = ?"), {fileId}) && q.next())
        return QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object();
    return {};
}

void PhotoRepository::writeLibrary(int fileId, const QString& fingerprint, const LibraryResult& r) {
    {
        QSqlQuery q(db());
        run(q, QStringLiteral("DELETE FROM library_versions WHERE file_id = ?"), {fileId});   // cascade : mots-cles
        QSqlQuery s(db());
        run(s, QStringLiteral("DELETE FROM library_sidecars WHERE file_id = ?"), {fileId});
    }
    for (const LibraryVersion& v : r.versions) {
        QSqlQuery q(db());
        run(q, QStringLiteral(
                 "INSERT INTO library_versions (file_id, version, is_copy, name, rating, rejected, labels, "
                 "edited, title, description, creator, rights, sources, picked) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)"),
            {fileId, v.version, v.isCopy ? 1 : 0, v.name, v.rating,
             v.rejected.isNull() ? QVariant() : QVariant(v.rejected.toBool() ? 1 : 0), v.labels,
             v.edited.isNull() ? QVariant() : QVariant(v.edited.toBool() ? 1 : 0),
             v.title, v.description, v.creator, v.rights,
             QString::fromUtf8(QJsonDocument(v.sources).toJson(QJsonDocument::Compact)),
             v.picked.isNull() ? QVariant() : QVariant(v.picked.toBool() ? 1 : 0)});
        const QVariant versionId = q.lastInsertId();
        for (const QString& k : v.keywords) {
            QSqlQuery kq(db());
            run(kq, QStringLiteral("INSERT OR IGNORE INTO library_keywords (version_id, keyword) VALUES (?,?)"),
                {versionId, k});
        }
    }
    for (const SidecarEntry& s : r.sidecars) {
        QSqlQuery q(db());
        run(q, QStringLiteral("INSERT OR REPLACE INTO library_sidecars (file_id, name, source, readable) VALUES (?,?,?,?)"),
            {fileId, s.name, s.source.isEmpty() ? QVariant() : QVariant(s.source), s.readable ? 1 : 0});
    }
    QSqlQuery q(db());
    run(q, QStringLiteral(
             "INSERT OR REPLACE INTO library_files (file_id, fingerprint, versions, main_source, "
             "gps_latitude, gps_longitude, gps_source) VALUES (?,?,?,?,?,?,?)"),
        {fileId, fingerprint, static_cast<int>(r.versions.size()),
         r.mainSource.isEmpty() ? QVariant() : QVariant(r.mainSource),
         r.latitude, r.longitude, r.gpsSource.isEmpty() ? QVariant() : QVariant(r.gpsSource)});
}

int PhotoRepository::clearLibrary() {
    int n = 0;
    {
        QSqlQuery c(db());
        if (run(c, QStringLiteral("SELECT COUNT(*) FROM library_files")) && c.next())
            n = c.value(0).toInt();
    }
    if (n == 0)
        return 0;
    for (const char* t : {"library_keywords", "library_versions", "library_sidecars", "library_files"}) {
        QSqlQuery q(db());
        run(q, QStringLiteral("DELETE FROM %1").arg(QLatin1String(t)));
    }
    return n;
}

bool PhotoRepository::beginBatch() { return db().transaction(); }
void PhotoRepository::commitBatch() { db().commit(); }

// --- Lecture d'une photo --------------------------------------------------------

namespace {

// Version -> JSON du contrat.
QJsonObject versionJson(const QSqlQuery& q, const QStringList& keywords) {
    QJsonObject o;
    o["version"] = q.value("version").toInt();
    o["is_copy"] = q.value("is_copy").toInt() != 0;
    o["name"] = nullable(q.value("name"));
    o["rating"] = nullable(q.value("rating"));
    o["rejected"] = nullableBool(q.value("rejected"));
    o["picked"] = nullableBool(q.value("picked"));
    QJsonArray labels;
    for (const QString& n : labelNames(q.value("labels").toInt()))
        labels.append(n);
    o["color_labels"] = labels;
    o["keywords"] = keywordArray(keywords);
    o["title"] = nullable(q.value("title"));
    o["description"] = nullable(q.value("description"));
    o["creator"] = nullable(q.value("creator"));
    o["rights"] = nullable(q.value("rights"));
    o["edited"] = nullableBool(q.value("edited"));
    o["sources"] = QJsonDocument::fromJson(q.value("sources").toString().toUtf8()).object();
    return o;
}

} // namespace

QJsonArray PhotoRepository::libraryPhotos(const QVector<int>& ids) {
    QJsonArray out;
    if (ids.isEmpty())
        return out;
    QVariantList idBinds;
    for (int id : ids)
        idBinds << id;
    const QString in = placeholders(ids.size());

    // Photos
    QMap<int, QJsonObject> photos;
    {
        QSqlQuery q(db());
        run(q, QStringLiteral(
                 "SELECT f.id, f.path, f.filename, f.extension, f.file_type, f.taken_at, f.camera_model, f.lens, "
                 "f.focal_length, f.aperture, f.iso, f.shutter_speed_s, f.latitude, f.longitude, "
                 "lf.gps_latitude, lf.gps_longitude, lf.gps_source "
                 "FROM files f LEFT JOIN library_files lf ON lf.file_id = f.id WHERE f.id IN (") + in + QLatin1Char(')'),
            idBinds);
        while (q.next()) {
            QJsonObject o;
            o["id"] = q.value(0).toInt();
            o["path"] = q.value(1).toString();
            o["filename"] = q.value(2).toString();
            o["extension"] = nullable(q.value(3));
            o["kind"] = nullable(q.value(4));
            o["taken_at"] = nullable(q.value(5));
            o["camera"] = nullable(q.value(6));
            o["lens"] = nullable(q.value(7));
            o["focal_length"] = nullable(q.value(8));
            o["aperture"] = nullable(q.value(9));
            o["iso"] = nullable(q.value(10));
            o["shutter_speed_s"] = nullable(q.value(11));
            // Position : le fichier fait autorite, le sidecar ne sert qu'a defaut.
            if (!q.value(12).isNull() && !q.value(13).isNull()) {
                o["gps"] = QJsonObject{{"latitude", q.value(12).toDouble()}, {"longitude", q.value(13).toDouble()}};
                o["gps_source"] = QStringLiteral("file");
            } else if (!q.value(14).isNull() && !q.value(15).isNull()) {
                o["gps"] = QJsonObject{{"latitude", q.value(14).toDouble()}, {"longitude", q.value(15).toDouble()}};
                o["gps_source"] = nullable(q.value(16));
            } else {
                o["gps"] = QJsonValue(QJsonValue::Null);
                o["gps_source"] = QJsonValue(QJsonValue::Null);
            }
            o["versions"] = QJsonArray();
            o["sidecars"] = QJsonArray();
            photos.insert(q.value(0).toInt(), o);
        }
    }

    // Mots-cles, par version.
    QHash<int, QStringList> keywords;   // id de version -> mots-cles
    {
        QSqlQuery q(db());
        run(q, QStringLiteral("SELECT k.version_id, k.keyword FROM library_keywords k "
                              "JOIN library_versions v ON v.id = k.version_id WHERE v.file_id IN (") + in
                + QStringLiteral(") ORDER BY k.keyword"), idBinds);
        while (q.next())
            keywords[q.value(0).toInt()] << q.value(1).toString();
    }
    // Versions.
    {
        QSqlQuery q(db());
        run(q, QStringLiteral("SELECT * FROM library_versions WHERE file_id IN (") + in
                + QStringLiteral(") ORDER BY file_id, version"), idBinds);
        while (q.next()) {
            const int fileId = q.value("file_id").toInt();
            if (!photos.contains(fileId))
                continue;
            QJsonArray arr = photos[fileId].value("versions").toArray();
            arr.append(versionJson(q, keywords.value(q.value("id").toInt())));
            photos[fileId]["versions"] = arr;
        }
    }
    // Sidecars (nom et logiciel seulement : jamais le chemin du serveur).
    {
        QSqlQuery q(db());
        run(q, QStringLiteral("SELECT file_id, source, name, readable FROM library_sidecars WHERE file_id IN (") + in
                + QStringLiteral(") ORDER BY file_id, name"), idBinds);
        while (q.next()) {
            const int fileId = q.value(0).toInt();
            if (!photos.contains(fileId))
                continue;
            QJsonArray arr = photos[fileId].value("sidecars").toArray();
            arr.append(QJsonObject{{"source", nullable(q.value(1))}, {"name", q.value(2).toString()},
                                   {"readable", q.value(3).toInt() != 0}});
            photos[fileId]["sidecars"] = arr;
        }
    }
    // Ordre demande par l'appelant.
    for (int id : ids)
        if (photos.contains(id))
            out.append(photos.value(id));
    return out;
}

QJsonObject PhotoRepository::libraryPhoto(int fileId, bool* found) {
    const QJsonArray a = libraryPhotos({fileId});
    if (found) *found = !a.isEmpty();
    return a.isEmpty() ? QJsonObject() : a.first().toObject();
}

// --- Liste ----------------------------------------------------------------------

QJsonObject PhotoRepository::libraryList(const LibraryFilter& filter, int page, int pageSize, bool skipTotal,
                                         const QString& cursor, bool* cursorValid) {
    QString where;
    QVariantList binds;
    join(buildClauses(filter, nullptr), &where, &binds);
    if (cursorValid) *cursorValid = true;

    QJsonObject o;
    if (!skipTotal) {
        QSqlQuery q(db());
        run(q, QStringLiteral("SELECT COUNT(*) FROM files f WHERE ") + where, binds);
        o["total"] = q.next() ? q.value(0).toInt() : 0;
    } else {
        o["total"] = QJsonValue(QJsonValue::Null);
    }
    constexpr int kMaxPage = 1000000;
    page = qBound(1, page, kMaxPage);
    pageSize = qMax(1, pageSize);

    // Cle de tri unique et totale : (sans date ?, date, identifiant). COALESCE evite le NULL,
    // qu'une comparaison de n-uplets ne sait pas ordonner ; l'ordre est celui d'avant.
    const QString sortKey = QStringLiteral("f.taken_at IS NULL, COALESCE(f.taken_at, ''), f.id");
    QVariantList pageBinds = binds;
    QString extra;
    qint64 offset = static_cast<qint64>(page - 1) * pageSize;
    if (!cursor.isEmpty()) {
        // Curseur = base64url de « <0|1>|<date>|<id> » : opaque pour le client.
        const QByteArray raw = QByteArray::fromBase64(cursor.toLatin1(), QByteArray::Base64UrlEncoding);
        const QList<QByteArray> parts = raw.split('|');
        bool okNull = false, okId = false;
        const int isNull = parts.value(0).toInt(&okNull);
        const int id = parts.value(2).toInt(&okId);
        if (parts.size() != 3 || !okNull || !okId || (isNull != 0 && isNull != 1) || id < 1) {
            if (cursorValid) *cursorValid = false;
            return {};
        }
        extra = QStringLiteral(" AND (f.taken_at IS NULL, COALESCE(f.taken_at, ''), f.id) > (?, ?, ?)");
        pageBinds << isNull << QString::fromUtf8(parts.value(1)) << id;
        offset = 0;
    }
    // Une ligne de plus que demande : sa presence dit s'il y a une page suivante.
    pageBinds << pageSize + 1 << offset;
    QVector<int> ids;
    QVector<QPair<QPair<int, QString>, int>> keys;   // ((sansDate, date), id) de chaque ligne lue
    {
        QSqlQuery q(db());
        run(q, QStringLiteral("SELECT f.id, f.taken_at IS NULL, COALESCE(f.taken_at, '') FROM files f WHERE ")
                + where + extra + QStringLiteral(" ORDER BY ") + sortKey + QStringLiteral(" LIMIT ? OFFSET ?"), pageBinds);
        while (q.next()) {
            ids.append(q.value(0).toInt());
            keys.append({{q.value(1).toInt(), q.value(2).toString()}, q.value(0).toInt()});
        }
    }
    bool more = false;
    if (ids.size() > pageSize) {
        more = true;
        ids.removeLast();
        keys.removeLast();
    }
    o["items"] = libraryPhotos(ids);
    o["page"] = page;
    o["page_size"] = pageSize;
    // Curseur de la page suivante : la position du dernier element rendu.
    if (more && !keys.isEmpty()) {
        const auto& last = keys.last();
        const QByteArray raw = QByteArray::number(last.first.first) + '|' + last.first.second.toUtf8()
                             + '|' + QByteArray::number(last.second);
        o["next"] = QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    } else {
        o["next"] = QJsonValue(QJsonValue::Null);
    }
    return o;
}

// --- Facettes -------------------------------------------------------------------

QJsonObject PhotoRepository::libraryFacets(const LibraryFilter& filter) {
    QJsonObject out;

    // Total : photos qui passent TOUS les filtres.
    {
        QString where; QVariantList binds;
        join(buildClauses(filter, nullptr), &where, &binds);
        QSqlQuery q(db());
        run(q, QStringLiteral("SELECT COUNT(*) FROM files f WHERE ") + where, binds);
        out["total"] = q.next() ? q.value(0).toInt() : 0;
    }

    // Une liste = comptes par valeur, calcules avec tous les filtres sauf le sien.
    auto facet = [&](const char* name, Dim dim, const QString& select, const QString& from, const QString& extraWhere) {
        QString where; QVariantList binds;
        join(buildClauses(filter, &dim), &where, &binds);
        QSqlQuery q(db());
        run(q, QStringLiteral("SELECT %1 AS value, COUNT(DISTINCT f.id) AS count FROM files f %2 WHERE %3 %4 "
                              "GROUP BY value ORDER BY count DESC, value")
                   .arg(select, from, where, extraWhere), binds);
        QJsonArray arr;
        while (q.next())
            arr.append(QJsonObject{{"value", nullable(q.value(0))}, {"count", q.value(1).toInt()}});
        out[QLatin1String(name)] = arr;
    };
    const QString notNull = QStringLiteral("AND %1 IS NOT NULL");
    facet("kind",     Dim::Kind,     QStringLiteral("f.file_type"),    {}, notNull.arg(QStringLiteral("f.file_type")));
    facet("camera",   Dim::Camera,   QStringLiteral("f.camera_model"), {}, notNull.arg(QStringLiteral("f.camera_model")));
    facet("lens",     Dim::Lens,     QStringLiteral("f.lens"),         {}, notNull.arg(QStringLiteral("f.lens")));
    facet("iso",      Dim::Iso,      QStringLiteral("f.iso"),          {}, notNull.arg(QStringLiteral("f.iso")));
    facet("aperture", Dim::Aperture, QStringLiteral("f.aperture"),     {}, notNull.arg(QStringLiteral("f.aperture")));
    facet("focal",    Dim::Focal,    QStringLiteral("f.focal_length"), {}, notNull.arg(QStringLiteral("f.focal_length")));
    facet("shutter",  Dim::Shutter,  QStringLiteral("f.shutter_speed"), {}, notNull.arg(QStringLiteral("f.shutter_speed")));

    // Valeurs de la bibliotheque : une photo compte une fois par valeur, meme avec
    // plusieurs versions. Les photos sans valeur connue sont comptees a part (`unknown`).
    const QString lv = QStringLiteral("JOIN library_versions v ON v.file_id = f.id");
    facet("rating", Dim::Rating, QStringLiteral("v.rating"), lv, QStringLiteral("AND v.rating IS NOT NULL"));
    facet("edited", Dim::Edited, QStringLiteral("v.edited"), lv, QStringLiteral("AND v.edited IS NOT NULL"));
    facet("source", Dim::Source, QStringLiteral("s.source"),
          QStringLiteral("JOIN library_sidecars s ON s.file_id = f.id"), QStringLiteral("AND s.source IS NOT NULL"));
    facet("keyword", Dim::Keyword, QStringLiteral("k.keyword"),
          lv + QStringLiteral(" JOIN library_keywords k ON k.version_id = v.id"), {});

    // Etiquettes : le masque est eclate en noms.
    {
        const Dim dim = Dim::ColorLabel;
        QString where; QVariantList binds;
        join(buildClauses(filter, &dim), &where, &binds);
        QJsonArray arr;
        for (const QString& name : {QStringLiteral("red"), QStringLiteral("yellow"), QStringLiteral("green"),
                                    QStringLiteral("blue"), QStringLiteral("purple")}) {
            QSqlQuery q(db());
            run(q, QStringLiteral("SELECT COUNT(DISTINCT f.id) FROM files f %1 WHERE %2 AND (v.labels & ?) != 0").arg(lv, where),
                binds + QVariantList{labelMaskFromName(name)});
            const int n = q.next() ? q.value(0).toInt() : 0;
            if (n > 0)
                arr.append(QJsonObject{{"value", name}, {"count", n}});
        }
        out["color_label"] = arr;
    }

    // Inconnus : photos qui passent tous les filtres et dont la valeur n'est portee par
    // aucune version.
    {
        QString where; QVariantList binds;
        join(buildClauses(filter, nullptr), &where, &binds);
        QJsonObject unknown;
        auto count = [&](const char* name, const QString& cond) {
            QSqlQuery q(db());
            run(q, QStringLiteral("SELECT COUNT(*) FROM files f WHERE %1 AND %2").arg(where, cond), binds);
            unknown[QLatin1String(name)] = q.next() ? q.value(0).toInt() : 0;
        };
        count("rating", QStringLiteral("NOT EXISTS (SELECT 1 FROM library_versions v WHERE v.file_id = f.id AND v.rating IS NOT NULL)"));
        count("edited", QStringLiteral("NOT EXISTS (SELECT 1 FROM library_versions v WHERE v.file_id = f.id AND v.edited IS NOT NULL)"));
        count("sidecar", QStringLiteral("NOT EXISTS (SELECT 1 FROM library_sidecars s WHERE s.file_id = f.id)"));
        out["unknown"] = unknown;
    }
    return out;
}

// --- Sous-dossiers --------------------------------------------------------------

QJsonObject PhotoRepository::librarySubfolders(const QString& folder) {
    QMap<QString, int> counts;   // nom du sous-dossier direct -> photos presentes
    int here = 0;                // photos directement dans le dossier
    QSqlQuery q(db());
    if (!run(q, QStringLiteral("SELECT directory, COUNT(*) FROM files WHERE state = 'present' "
                               "AND (directory = ? OR directory LIKE ? ESCAPE '\\') GROUP BY directory"),
             {folder, likeEscape(folder) + QStringLiteral("/%")}))
        return {};
    const QString prefix = folder + QLatin1Char('/');
    while (q.next()) {
        const QString dir = q.value(0).toString();
        const int n = q.value(1).toInt();
        if (dir == folder) { here += n; continue; }
        const QString rest = dir.mid(prefix.size());
        const int slash = rest.indexOf(QLatin1Char('/'));
        counts[slash < 0 ? rest : rest.left(slash)] += n;
    }
    QJsonArray items;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        items.append(QJsonObject{{"name", it.key()}, {"path", prefix + it.key()}, {"photos", it.value()}});
    return QJsonObject{{"folder_path", folder}, {"photos", here}, {"items", items}};
}

// --- Rapport ---------------------------------------------------------------------

QJsonObject PhotoRepository::librarySidecars() {
    QJsonObject out;
    auto scalar = [this](const QString& sql) {
        QSqlQuery q(db());
        return run(q, sql) && q.next() ? q.value(0).toInt() : 0;
    };
    const int present = scalar(QStringLiteral("SELECT COUNT(*) FROM files WHERE state = 'present'"));
    const int withSidecar = scalar(QStringLiteral(
        "SELECT COUNT(DISTINCT s.file_id) FROM library_sidecars s JOIN files f ON f.id = s.file_id "
        "WHERE f.state = 'present' AND s.readable = 1"));
    out["photos"] = present;
    out["with_sidecar"] = withSidecar;
    out["without_sidecar"] = present - withSidecar;
    out["unreadable"] = scalar(QStringLiteral("SELECT COUNT(*) FROM library_sidecars WHERE readable = 0"));
    QJsonObject bySource;
    QSqlQuery q(db());
    // Les sidecars illisibles n'ont pas de logiciel : ils sont comptes a part (`unreadable`).
    if (run(q, QStringLiteral("SELECT s.source, COUNT(DISTINCT s.file_id) FROM library_sidecars s "
                              "JOIN files f ON f.id = s.file_id WHERE f.state = 'present' AND s.readable = 1 "
                              "GROUP BY s.source")))
        while (q.next())
            bySource[q.value(0).toString()] = q.value(1).toInt();
    out["by_source"] = bySource;
    // Lightcraft ecrit dans le .xmp d'un autre logiciel : ses photos se comptent par les valeurs qu'il a donnees.
    out["with_lightcraft"] = scalar(QStringLiteral(
        "SELECT COUNT(DISTINCT v.file_id) FROM library_versions v JOIN files f ON f.id = v.file_id "
        "WHERE f.state = 'present' AND v.sources LIKE '%\"lightcraft\"%'"));
    out["with_embedded"] = scalar(QStringLiteral(
        "SELECT COUNT(DISTINCT v.file_id) FROM library_versions v JOIN files f ON f.id = v.file_id "
        "WHERE f.state = 'present' AND v.sources LIKE '%\"file\"%'"));
    return out;
}

QJsonObject PhotoRepository::libraryStatus() {
    const QJsonObject r = librarySidecars();
    QJsonObject by = r.value("by_source").toObject();
    by["file"] = r.value("with_embedded");
    if (r.contains("with_lightcraft"))
        by["lightcraft"] = r.value("with_lightcraft");
    return QJsonObject{{"photos_with_sidecar", r.value("with_sidecar")}, {"by_source", by}};
}

// --- Export compact (contrat, §9) --------------------------------------------------

} // namespace morfphoto
