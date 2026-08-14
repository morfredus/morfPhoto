/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/PhotoApi.h"
#include "morfphoto/PhotoModule.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QStringList>

namespace morfphoto {

namespace {

constexpr int kDefaultPageSize = 50;
constexpr int kMaxPageSize     = 500;

QByteArray toBytes(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
QByteArray toBytes(const QJsonArray& a) {
    return QJsonDocument(a).toJson(QJsonDocument::Compact);
}

PhotoApi::Result ok(const QJsonObject& o)  { return {200, toBytes(o)}; }
PhotoApi::Result items(const QJsonArray& a) {
    QJsonObject o; o["items"] = a; return {200, toBytes(o)};
}
PhotoApi::Result error(int code, const QString& err, const QString& detail = {}) {
    QJsonObject o; o["error"] = err; o["detail"] = detail;
    return {code, toBytes(o)};
}

// Entier positif d'un paramètre de requête, ou défaut si absent. -1 => invalide.
int positiveIntParam(const QUrlQuery& q, const QString& key, int fallback) {
    if (!q.hasQueryItem(key))
        return fallback;
    bool ok = false;
    const int n = q.queryItemValue(key).toInt(&ok);
    return (ok && n >= 1) ? n : -1;
}

} // namespace

PhotoApi::PhotoApi(PhotoModule* module) : m_module(module) {}

PhotoApi::Result PhotoApi::handle(const QByteArray& method, const QString& path,
                                  const QString& queryString, const QByteArray& body) {
    if (!m_module)
        return error(503, QStringLiteral("unavailable"), QStringLiteral("module photo absent"));

    QString sub = path;
    if (sub.endsWith('/'))
        sub.chop(1);
    if (!sub.startsWith(QLatin1String("/api/v1")))
        return error(404, QStringLiteral("not_found"), path);
    sub = sub.mid(QStringLiteral("/api/v1").size());
    const QStringList seg = sub.split('/', Qt::SkipEmptyParts);
    const QUrlQuery query(queryString);

    // ---- /photos... ----
    if (!seg.isEmpty() && seg[0] == QLatin1String("photos")) {
        if (method != "GET")
            return error(405, QStringLiteral("method_not_allowed"));
        if (seg.size() == 1) {
            // Liste paginée + filtrée.
            const int page = positiveIntParam(query, QStringLiteral("page"), 1);
            int pageSize   = positiveIntParam(query, QStringLiteral("page_size"), kDefaultPageSize);
            if (page < 0 || pageSize < 0)
                return error(400, QStringLiteral("bad_request"), QStringLiteral("page/page_size invalide"));
            pageSize = qMin(pageSize, kMaxPageSize);
            static const QStringList kFilters =
                {"year", "camera", "lens", "type", "folder", "state"};
            QVariantMap filters;
            for (const QString& key : kFilters) {
                if (!query.hasQueryItem(key))
                    continue;
                const QString v = query.queryItemValue(key);
                if (key == QLatin1String("year") || key == QLatin1String("folder")) {
                    bool okInt = false;
                    const int n = v.toInt(&okInt);
                    if (!okInt)
                        return error(400, QStringLiteral("bad_request"),
                                     QStringLiteral("valeur entiere attendue: %1").arg(key));
                    filters[key] = n;
                } else {
                    filters[key] = v;
                }
            }
            return ok(m_module->listPhotos(filters, page, pageSize));
        }
        const QString& what = seg[1];
        if (what == QLatin1String("summary")) return ok(m_module->summary());
        if (what == QLatin1String("dataset")) return ok(m_module->photoDataset());
        if (what == QLatin1String("cameras")) return items(m_module->cameras());
        if (what == QLatin1String("lenses"))  return items(m_module->lenses());
        if (what == QLatin1String("focals"))  return items(m_module->focals());
        if (what == QLatin1String("years"))   return items(m_module->years());
        bool okId = false;
        const int id = what.toInt(&okId);
        if (!okId)
            return error(404, QStringLiteral("not_found"), path);
        bool found = false;
        const QJsonObject photo = m_module->getPhoto(id, &found);
        if (!found)
            return error(404, QStringLiteral("not_found"), QStringLiteral("photo %1 inconnue").arg(id));
        return ok(photo);
    }

    // ---- /roots : racines autorisees (lecture seule ; la config est souveraine) ----
    if (!seg.isEmpty() && seg[0] == QLatin1String("roots")) {
        if (method != "GET")
            return error(405, QStringLiteral("method_not_allowed"));
        return items(m_module->allowedRoots());
    }

    // ---- /index... ----
    if (!seg.isEmpty() && seg[0] == QLatin1String("index")) {
        if (seg.size() == 2 && seg[1] == QLatin1String("status")) {
            if (method != "GET")
                return error(405, QStringLiteral("method_not_allowed"));
            return ok(m_module->indexStatus());
        }
        if (seg.size() == 1) {
            if (method != "POST")
                return error(405, QStringLiteral("method_not_allowed"));
            const QJsonObject in = QJsonDocument::fromJson(body).object();
            const QString modeStr = in.value(QStringLiteral("mode")).toString(QStringLiteral("incremental"));
            if (modeStr != QLatin1String("incremental") && modeStr != QLatin1String("full"))
                return error(400, QStringLiteral("bad_request"), QStringLiteral("mode inconnu: %1").arg(modeStr));
            const IndexMode mode = (modeStr == QLatin1String("full")) ? IndexMode::Full
                                                                      : IndexMode::Incremental;
            QVector<int> folderIds;
            if (in.contains(QStringLiteral("folder_ids"))) {
                for (const QJsonValue& v : in.value(QStringLiteral("folder_ids")).toArray()) {
                    if (!v.isDouble())
                        return error(400, QStringLiteral("bad_request"),
                                     QStringLiteral("folder_ids doit etre une liste d'entiers"));
                    folderIds << v.toInt();
                }
            }
            const QJsonObject result = m_module->triggerIndex(mode, folderIds, QStringLiteral("api"));
            const int code = result.value(QStringLiteral("accepted")).toBool() ? 202 : 409;
            return {code, toBytes(result)};
        }
        return error(404, QStringLiteral("not_found"), path);
    }

    // ---- /folders... ----
    if (!seg.isEmpty() && seg[0] == QLatin1String("folders")) {
        if (seg.size() == 1) {
            if (method == "GET")
                return items(m_module->listFolders());
            if (method == "POST") {
                const QJsonObject in = QJsonDocument::fromJson(body).object();
                const QString folderPath = in.value(QStringLiteral("path")).toString();
                if (folderPath.isEmpty())
                    return error(400, QStringLiteral("bad_request"), QStringLiteral("`path` obligatoire"));
                const QVariant label = in.contains(QStringLiteral("label"))
                    ? QVariant(in.value(QStringLiteral("label")).toString()) : QVariant();
                const bool recursive = in.value(QStringLiteral("recursive")).toBool(true);
                QJsonObject out;
                QString err;
                if (!m_module->addFolder(folderPath, label, recursive, &out, &err)) {
                    const int code = err.contains(QStringLiteral("racine")) ? 403 : 400;
                    return error(code, code == 403 ? QStringLiteral("root_violation")
                                                   : QStringLiteral("bad_request"), err);
                }
                return {201, toBytes(out)};
            }
            return error(405, QStringLiteral("method_not_allowed"));
        }
        bool okId = false;
        const int id = seg[1].toInt(&okId);
        if (!okId)
            return error(404, QStringLiteral("not_found"), path);
        // Restauration d'un dossier retiré : POST /api/v1/folders/{id}/restore.
        if (seg.size() == 3 && seg[2] == QLatin1String("restore")) {
            if (method != "POST")
                return error(405, QStringLiteral("method_not_allowed"));
            QJsonObject out;
            if (!m_module->restoreFolder(id, &out))
                return error(404, QStringLiteral("not_found"), QStringLiteral("selection %1 introuvable").arg(id));
            return ok(out);
        }
        if (seg.size() != 2)
            return error(404, QStringLiteral("not_found"), path);
        if (method == "PATCH") {
            const QJsonObject in = QJsonDocument::fromJson(body).object();
            if (!in.value(QStringLiteral("enabled")).isBool())
                return error(400, QStringLiteral("bad_request"), QStringLiteral("`enabled` (booleen) obligatoire"));
            QJsonObject out;
            if (!m_module->setFolderEnabled(id, in.value(QStringLiteral("enabled")).toBool(), &out))
                return error(404, QStringLiteral("not_found"), QStringLiteral("selection %1 introuvable").arg(id));
            return ok(out);
        }
        if (method == "DELETE") {
            if (!m_module->removeFolder(id))
                return error(404, QStringLiteral("not_found"), QStringLiteral("selection %1 introuvable").arg(id));
            QJsonObject o; o["status"] = QStringLiteral("removed"); o["id"] = id;
            return ok(o);
        }
        return error(405, QStringLiteral("method_not_allowed"));
    }

    return error(404, QStringLiteral("not_found"), path);
}

} // namespace morfphoto
