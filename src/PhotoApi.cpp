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
#include <QUrl>
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
                {"year", "camera", "lens", "type", "folder", "directory", "state"};
            QVariantMap filters;
            for (const QString& key : kFilters) {
                if (!query.hasQueryItem(key))
                    continue;
                // Un chemin de repertoire porte des '/' encodes en %2F que QUrlQuery garde
                // encodes par defaut : le decoder entierement pour retrouver le chemin reel.
                const QString v = (key == QLatin1String("directory"))
                    ? query.queryItemValue(key, QUrl::FullyDecoded)
                    : query.queryItemValue(key);
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

    // ---- /contexts : repertoires + leur contexte (ecran de qualification PhotoHub) ----
    if (!seg.isEmpty() && seg[0] == QLatin1String("contexts")) {
        if (seg.size() != 1)
            return error(404, QStringLiteral("not_found"), path);
        if (method != "GET")
            return error(405, QStringLiteral("method_not_allowed"));
        QString status;
        if (query.hasQueryItem(QStringLiteral("status"))) {
            status = query.queryItemValue(QStringLiteral("status"));
            static const QStringList kStates = {"qualified", "unqualified", "invalid"};
            if (!kStates.contains(status))
                return error(400, QStringLiteral("bad_request"),
                             QStringLiteral("status inconnu (qualified|unqualified|invalid): %1").arg(status));
        }
        return items(m_module->listContexts(status));
    }

    // ---- /context : contexte d'UN repertoire (lecture, ecriture, retrait) ----
    // GET  ?directory=... : le contexte, ou { status: "unqualified" }.
    // PUT  {directory, context, subject, motif?, description?} : ecrit `.morfphoto.json`
    //      (morfPhoto est l'unique ecrivain). context ET subject obligatoires (contrat V2).
    // DELETE ?directory=... : retire le fichier (le dossier redevient non qualifie).
    if (!seg.isEmpty() && seg[0] == QLatin1String("context")) {
        if (seg.size() != 1)
            return error(404, QStringLiteral("not_found"), path);

        if (method == "GET") {
            // FullyDecoded : un chemin porte des '/' encodes en %2F que QUrlQuery
            // garde encodes par defaut (semantique des delimiteurs) ; on veut le chemin reel.
            const QString directory = query.queryItemValue(QStringLiteral("directory"), QUrl::FullyDecoded);
            if (directory.isEmpty())
                return error(400, QStringLiteral("bad_request"), QStringLiteral("`directory` obligatoire"));
            return ok(m_module->getContext(directory));
        }
        if (method == "PUT") {
            const QJsonObject in = QJsonDocument::fromJson(body).object();
            const QString directory = in.value(QStringLiteral("directory")).toString();
            const QString context   = in.value(QStringLiteral("context")).toString();
            const QString subject   = in.value(QStringLiteral("subject")).toString();
            if (directory.isEmpty())
                return error(400, QStringLiteral("bad_request"), QStringLiteral("`directory` obligatoire"));
            if (context.isEmpty() || subject.isEmpty())
                return error(400, QStringLiteral("bad_request"),
                             QStringLiteral("`context` et `subject` sont obligatoires (contrat V2)"));
            const QVariant motif = in.contains(QStringLiteral("motif"))
                ? QVariant(in.value(QStringLiteral("motif")).toString()) : QVariant();
            const QVariant description = in.contains(QStringLiteral("description"))
                ? QVariant(in.value(QStringLiteral("description")).toString()) : QVariant();
            QJsonObject out;
            QString err;
            if (!m_module->putContext(directory, context, subject, motif, description, &out, &err)) {
                const int code = err.contains(QStringLiteral("racine")) ? 403 : 400;
                return error(code, code == 403 ? QStringLiteral("root_violation")
                                               : QStringLiteral("bad_request"), err);
            }
            return ok(out);
        }
        if (method == "DELETE") {
            // FullyDecoded : un chemin porte des '/' encodes en %2F que QUrlQuery
            // garde encodes par defaut (semantique des delimiteurs) ; on veut le chemin reel.
            const QString directory = query.queryItemValue(QStringLiteral("directory"), QUrl::FullyDecoded);
            if (directory.isEmpty())
                return error(400, QStringLiteral("bad_request"), QStringLiteral("`directory` obligatoire"));
            QJsonObject out;
            QString err;
            if (!m_module->deleteContext(directory, &out, &err)) {
                const int code = err.contains(QStringLiteral("racine")) ? 403 : 400;
                return error(code, code == 403 ? QStringLiteral("root_violation")
                                               : QStringLiteral("bad_request"), err);
            }
            return ok(out);
        }
        return error(405, QStringLiteral("method_not_allowed"));
    }

    // ---- /thumbnail : vignette JPEG d'un fichier (apercu, ex. pour PhotoHub) ----
    // GET ?path=<chemin absolu> : renvoie une vignette image/jpeg (apercu embarque
    // extrait par exiftool, JPEG comme RAW). morfPhoto reste souverain sur le disque :
    // PhotoHub, client pur, ne lit jamais les fichiers -- il demande la vignette ici.
    if (!seg.isEmpty() && seg[0] == QLatin1String("thumbnail")) {
        if (seg.size() != 1)
            return error(404, QStringLiteral("not_found"), path);
        if (method != "GET")
            return error(405, QStringLiteral("method_not_allowed"));
        const QString file = query.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);
        if (file.isEmpty())
            return error(400, QStringLiteral("bad_request"), QStringLiteral("`path` obligatoire"));
        bool okThumb = false;
        const QByteArray jpeg = m_module->thumbnail(file, &okThumb);
        if (!okThumb)
            return error(404, QStringLiteral("not_found"),
                         QStringLiteral("aucune vignette disponible pour ce fichier"));
        return {200, jpeg, QByteArrayLiteral("image/jpeg")};
    }

    // ---- /roots : racines autorisees (lecture seule ; la config est souveraine) ----
    if (!seg.isEmpty() && seg[0] == QLatin1String("roots")) {
        if (method != "GET")
            return error(405, QStringLiteral("method_not_allowed"));
        return items(m_module->allowedRoots());
    }

    // ---- /sources : sources SMB poussées par PhotoHub (« Envoyer la config ») ----
    // GET  : liste les sources connues (hostname, slug, mountpoint, mounted).
    // POST : {host, share, username, password, hostname} monte sous
    //        /mnt/photos_<hostname> via le helper, valide le CIFS, persiste
    //        fstab + racine. Le mot de passe n'est jamais stocké par le service.
    if (!seg.isEmpty() && seg[0] == QLatin1String("sources")) {
        if (seg.size() == 2 && seg[1] == QLatin1String("ready")) {
            if (method != "GET")
                return error(405, QStringLiteral("method_not_allowed"));
            QJsonObject out;
            QString err;
            if (!m_module->helperReady(&out, &err)) {
                if (!out.contains(QStringLiteral("detail")))
                    out[QStringLiteral("detail")] = err;
                out[QStringLiteral("ok")] = false;
                return {503, toBytes(out)};
            }
            return {200, toBytes(out)};
        }
        if (seg.size() != 1)
            return error(404, QStringLiteral("not_found"), path);
        if (method == "GET")
            return items(m_module->listSources());
        if (method == "POST") {
            const QJsonObject in = QJsonDocument::fromJson(body).object();
            const QString host     = in.value(QStringLiteral("host")).toString().trimmed();
            const QString share    = in.value(QStringLiteral("share")).toString().trimmed();
            const QString username = in.value(QStringLiteral("username")).toString();
            const QString password = in.value(QStringLiteral("password")).toString();
            QString hostname = in.value(QStringLiteral("hostname")).toString().trimmed();
            if (hostname.isEmpty())
                hostname = in.value(QStringLiteral("label")).toString().trimmed();
            if (host.isEmpty() || share.isEmpty() || username.isEmpty())
                return error(400, QStringLiteral("bad_request"),
                             QStringLiteral("`host`, `share` et `username` sont obligatoires"));
            if (hostname.isEmpty())
                return error(400, QStringLiteral("bad_request"),
                             QStringLiteral("`hostname` de la machine source est obligatoire"));
            QJsonObject out;
            QString err;
            if (!m_module->addSource(host, share, username, password, hostname, &out, &err)) {
                if (!out.contains(QStringLiteral("error")))
                    out[QStringLiteral("error")] = out.value(QStringLiteral("code")).toString(
                        QStringLiteral("mount_failed"));
                if (!out.contains(QStringLiteral("detail")) ||
                    out.value(QStringLiteral("detail")).toString().isEmpty())
                    out[QStringLiteral("detail")] = err;
                out[QStringLiteral("ok")] = false;
                return {502, toBytes(out)};
            }
            return {201, toBytes(out)};
        }
        return error(405, QStringLiteral("method_not_allowed"));
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
                // Support amovible (CD/DVD, archive) : optionnel, défaut = non amovible.
                const bool removable = in.value(QStringLiteral("removable")).toBool(false);
                const QVariant volumeLabel = in.contains(QStringLiteral("volume_label"))
                    ? QVariant(in.value(QStringLiteral("volume_label")).toString()) : QVariant();
                QJsonObject out;
                QString err;
                if (!m_module->addFolder(folderPath, label, recursive, removable, volumeLabel,
                                         &out, &err)) {
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
            // PATCH partiel : n'importe quel sous-ensemble de champs réglables. Au
            // moins un champ reconnu doit être présent, sinon la requête ne fait rien.
            const bool hasEnabled    = in.contains(QStringLiteral("enabled"));
            const bool hasRemovable  = in.contains(QStringLiteral("removable"));
            const bool hasVolume     = in.contains(QStringLiteral("volume_label"));
            const bool hasExcluded   = in.contains(QStringLiteral("analytics_excluded"));
            if (!hasEnabled && !hasRemovable && !hasVolume && !hasExcluded)
                return error(400, QStringLiteral("bad_request"),
                             QStringLiteral("aucun champ modifiable (enabled, removable, "
                                            "volume_label, analytics_excluded)"));
            QJsonObject out;
            if (hasEnabled) {
                if (!in.value(QStringLiteral("enabled")).isBool())
                    return error(400, QStringLiteral("bad_request"), QStringLiteral("`enabled` doit etre booleen"));
                if (!m_module->setFolderEnabled(id, in.value(QStringLiteral("enabled")).toBool(), &out))
                    return error(404, QStringLiteral("not_found"), QStringLiteral("selection %1 introuvable").arg(id));
            }
            if (hasRemovable || hasVolume) {
                // removable et volume_label vont ensemble côté base : compléter le champ
                // absent avec la valeur courante de la sélection pour ne rien écraser.
                QJsonObject current;
                for (const QJsonValue& v : m_module->listFolders())
                    if (v.toObject().value(QStringLiteral("id")).toInt() == id) { current = v.toObject(); break; }
                if (current.isEmpty())
                    return error(404, QStringLiteral("not_found"), QStringLiteral("selection %1 introuvable").arg(id));
                if (hasRemovable && !in.value(QStringLiteral("removable")).isBool())
                    return error(400, QStringLiteral("bad_request"), QStringLiteral("`removable` doit etre booleen"));
                const bool removable = hasRemovable
                    ? in.value(QStringLiteral("removable")).toBool()
                    : (current.value(QStringLiteral("removable")).toInt() == 1);
                QVariant volumeLabel;
                if (hasVolume) {
                    const QJsonValue vl = in.value(QStringLiteral("volume_label"));
                    volumeLabel = vl.isNull() ? QVariant() : QVariant(vl.toString());
                } else {
                    const QJsonValue vl = current.value(QStringLiteral("volume_label"));
                    volumeLabel = vl.isNull() ? QVariant() : QVariant(vl.toString());
                }
                if (!m_module->setFolderMedia(id, removable, volumeLabel, &out))
                    return error(404, QStringLiteral("not_found"), QStringLiteral("selection %1 introuvable").arg(id));
            }
            if (hasExcluded) {
                if (!in.value(QStringLiteral("analytics_excluded")).isBool())
                    return error(400, QStringLiteral("bad_request"), QStringLiteral("`analytics_excluded` doit etre booleen"));
                if (!m_module->setFolderAnalyticsExcluded(id, in.value(QStringLiteral("analytics_excluded")).toBool(), &out))
                    return error(404, QStringLiteral("not_found"), QStringLiteral("selection %1 introuvable").arg(id));
            }
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

    // ---- /purge : suppression DÉFINITIVE de données (irréversible) ----
    // Distincte du retrait doux d'un dossier (DELETE /folders/{id}, réversible). Efface
    // vraiment des lignes selon une portée. Réservée à un choix explicite du client.
    if (!seg.isEmpty() && seg[0] == QLatin1String("purge")) {
        if (seg.size() != 1)
            return error(404, QStringLiteral("not_found"), path);
        if (method != "POST")
            return error(405, QStringLiteral("method_not_allowed"));
        const QJsonObject in = QJsonDocument::fromJson(body).object();
        const QString scope = in.value(QStringLiteral("scope")).toString();
        if (scope.isEmpty())
            return error(400, QStringLiteral("bad_request"),
                         QStringLiteral("`scope` obligatoire (folder|year|camera|all)"));
        // La valeur porte l'identifiant/critère ; ignorée pour scope=all. On la passe
        // en QVariant : le module valide selon la portée (entier pour folder/year, etc.).
        const QJsonValue v = in.value(QStringLiteral("value"));
        const QVariant value = v.isNull() ? QVariant() : v.toVariant();
        const QJsonObject result = m_module->purge(scope, value);
        if (result.contains(QStringLiteral("error")))
            return error(400, QStringLiteral("bad_request"),
                         result.value(QStringLiteral("error")).toString());
        return ok(result);
    }

    return error(404, QStringLiteral("not_found"), path);
}

} // namespace morfphoto
