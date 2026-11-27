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
#include <QDate>
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

// Criteres de GET /api/v1/library (contrat morfphoto-library/1, §8). Un parametre repete
// vaut « l'un de » ; false + *detail si une valeur est invalide.
bool parseLibraryFilter(const QUrlQuery& query, LibraryFilter* f, QString* detail) {
    auto list = [&query](const char* key) {
        return query.allQueryItemValues(QLatin1String(key), QUrl::FullyDecoded);
    };
    auto text = [&query](const char* key) {
        return query.queryItemValue(QLatin1String(key), QUrl::FullyDecoded);
    };
    auto flag = [&text](const char* key) {
        return text(key) == QLatin1String("true") || text(key) == QLatin1String("1");
    };
    if (!text("min_rating").isEmpty()) {
        bool ok = false;
        f->minRating = text("min_rating").toInt(&ok);
        if (!ok || f->minRating < 0 || f->minRating > 5) {
            *detail = QStringLiteral("`min_rating` doit etre compris entre 0 et 5");
            return false;
        }
    }
    if (!text("rejected").isEmpty()) {
        f->rejected = text("rejected");
        if (!QStringList{"hide", "only", "all"}.contains(f->rejected)) {
            *detail = QStringLiteral("`rejected` : hide, only ou all");
            return false;
        }
    }
    f->edited = text("edited");
    if (!QStringList{"", "all", "edited", "unedited"}.contains(f->edited)) {
        *detail = QStringLiteral("`edited` : all, edited ou unedited");
        return false;
    }
    f->picked = text("picked");
    if (!QStringList{"", "true", "false"}.contains(f->picked)) {
        *detail = QStringLiteral("`picked` : true ou false");
        return false;
    }
    f->colorLabels = list("color_label");
    for (const QString& label : f->colorLabels) {
        if (labelMaskFromName(label) == 0) {
            *detail = QStringLiteral("`color_label` : red, yellow, green, blue ou purple (recu : %1)").arg(label);
            return false;
        }
    }
    f->takenFrom = text("taken_from");
    f->takenTo   = text("taken_to");
    for (const QString& date : {f->takenFrom, f->takenTo}) {
        if (!date.isEmpty() && !QDate::fromString(date, QStringLiteral("yyyy-MM-dd")).isValid()) {
            *detail = QStringLiteral("date attendue au format AAAA-MM-JJ : %1").arg(date);
            return false;
        }
    }
    f->sources   = list("source");
    for (const QString& s : f->sources) {
        if (!knownProviders().contains(s) && s != embeddedProviderId()) {
            *detail = QStringLiteral("`source` inconnue : %1").arg(s);
            return false;
        }
    }
    f->unlabeled = flag("unlabeled");
    f->recursive = flag("recursive");
    f->keywords  = list("keyword");
    f->kinds     = list("kind");
    f->cameras   = list("camera");
    f->lenses    = list("lens");
    f->iso       = list("iso");
    f->aperture  = list("aperture");
    f->focal     = list("focal");
    f->shutter   = list("shutter");
    f->filename  = text("filename");
    f->folderPath = text("folder_path");
    while (f->folderPath.size() > 1 && f->folderPath.endsWith(QLatin1Char('/')))
        f->folderPath.chop(1);
    return true;
}

} // namespace

PhotoApi::PhotoApi(PhotoModule* module) : m_module(module) {}

PhotoApi::Result PhotoApi::handle(const QByteArray& method, const QString& path,
                                  const QString& queryString, const QByteArray& body,
                                  const QByteArray& ifNoneMatch) {
    if (!m_module)
        return error(503, QStringLiteral("unavailable"), QStringLiteral("module photo absent"));

    // Une ecriture (declarer un dossier, purger, qualifier un contexte, declencher une passe)
    // peut changer ce que les lectures renvoient : invalider les ETag deja distribues.
    if (method != "GET")
        m_module->bumpCatalogRevision();

    // ETag / 304 sur les lectures lourdes de la bibliotheque et l'export d'analyse. Le
    // validateur est la revision du catalogue : tant qu'elle est la meme, la reponse l'est
    // aussi, et un client qui la connait ne retelecharge rien. On repond 304 AVANT de
    // calculer quoi que ce soit : c'est le travail du serveur qui est economise, pas
    // seulement la bande passante.
    const bool cacheable = method == "GET"
        && (path.startsWith(QLatin1String("/api/v1/library"))
            || path == QLatin1String("/api/v1/photos/dataset"));
    QByteArray etag;
    if (cacheable) {
        etag = '"' + m_module->catalogRevision().toLatin1() + '"';
        // Une bibliotheque eteinte repond 404 : ne pas la masquer derriere un 304.
        const bool libraryOff = path.startsWith(QLatin1String("/api/v1/library")) && !m_module->libraryEnabled();
        if (!libraryOff && !ifNoneMatch.isEmpty()
            && (ifNoneMatch.trimmed() == "*" || ifNoneMatch.contains(etag))) {
            Result r{304, QByteArray()};
            r.etag = etag;
            return r;
        }
    }
    Result r = handleRoute(method, path, queryString, body);
    if (cacheable && r.code == 200)
        r.etag = etag;
    return r;
}

PhotoApi::Result PhotoApi::handleRoute(const QByteArray& method, const QString& path,
                                       const QString& queryString, const QByteArray& body) {

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
            // Echantillon representatif d'un dossier (apercu de qualification) : `count`
            // chemins repartis sur toute la journee et dedupliques par radical (jamais
            // le RAW ET le JPEG d'une meme prise). Distinct de la pagination : on ne veut
            // PAS les N premieres, qui sont souvent la meme scene.
            if (query.hasQueryItem(QStringLiteral("sample"))) {
                const QString dir = query.queryItemValue(QStringLiteral("directory"), QUrl::FullyDecoded);
                if (dir.isEmpty())
                    return error(400, QStringLiteral("bad_request"),
                                 QStringLiteral("`sample` requiert `directory`"));
                bool okN = false;
                int n = query.queryItemValue(QStringLiteral("sample")).toInt(&okN);
                if (!okN || n < 1)
                    return error(400, QStringLiteral("bad_request"),
                                 QStringLiteral("`sample` doit etre un entier positif"));
                n = qMin(n, 100);
                QJsonArray arr;
                for (const QString& p : m_module->directorySample(dir, n))
                    arr.append(QJsonObject{{QStringLiteral("path"), p}});
                return items(arr);
            }
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
        if (what == QLatin1String("dataset")) {
            // `with` : liste separee par des virgules d'options. `library` ajoute les colonnes de la
            // bibliotheque, `gps` la position (carte) ; sans option, rien ne change.
            const QStringList with = query.queryItemValue(QStringLiteral("with")).split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString& w : with)
                if (w != QLatin1String("library") && w != QLatin1String("gps"))
                    return error(400, QStringLiteral("bad_request"), QStringLiteral("`with` : library, gps"));
            return ok(m_module->photoDataset(with.contains(QStringLiteral("library")),
                                             with.contains(QStringLiteral("gps"))));
        }
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

    // ---- /library... : bibliotheque (contrat morfphoto-library/1), lecture seule ----
    // Eteinte tant qu'aucun fournisseur de sidecars n'est configure : 404 explicite plutot
    // qu'une liste vide qui laisserait croire qu'il n'y a aucun sidecar.
    if (!seg.isEmpty() && seg[0] == QLatin1String("library")) {
        if (!m_module->libraryEnabled())
            return error(404, QStringLiteral("library_disabled"),
                         QStringLiteral("activer au moins un fournisseur dans `sidecars.providers`"));
        if (method != "GET")
            return error(405, QStringLiteral("method_not_allowed"));
        if (seg.size() == 2 && seg[1] == QLatin1String("sidecars"))
            return ok(m_module->librarySidecars());
        if (seg.size() == 2 && seg[1] == QLatin1String("subfolders")) {
            QString folder = query.queryItemValue(QStringLiteral("folder_path"), QUrl::FullyDecoded);
            while (folder.size() > 1 && folder.endsWith(QLatin1Char('/')))
                folder.chop(1);
            // Chemin absolu : « /... » (Linux, UNC) ou « C:/... » (morfPhoto sous Windows).
            const bool absolute = folder.startsWith(QLatin1Char('/'))
                || (folder.size() >= 3 && folder.at(0).isLetter() && folder.at(1) == QLatin1Char(':')
                    && (folder.at(2) == QLatin1Char('/') || folder.at(2) == QLatin1Char('\\')));
            if (!absolute)
                return error(400, QStringLiteral("invalid_folder_path"),
                             QStringLiteral("`folder_path` : chemin absolu attendu"));
            const QJsonObject result = m_module->librarySubfolders(folder);
            return result.isEmpty() ? error(503, QStringLiteral("catalog_unavailable")) : ok(result);
        }
        if (seg.size() == 2 && seg[1] != QLatin1String("facets")) {
            bool okId = false;
            const int id = seg[1].toInt(&okId);
            bool found = false;
            const QJsonObject photo = okId ? m_module->libraryPhoto(id, &found) : QJsonObject();
            if (!found)
                return error(404, QStringLiteral("not_found"), path);
            return ok(photo);
        }
        if (seg.size() > 2)
            return error(404, QStringLiteral("not_found"), path);
        if (seg.size() == 1 && query.hasQueryItem(QStringLiteral("ids"))) {
            const QString rawIds = query.queryItemValue(QStringLiteral("ids"), QUrl::FullyDecoded).trimmed();
            if (rawIds.isEmpty())
                return items(QJsonArray{});
            const QStringList parts = rawIds.split(QLatin1Char(','));
            if (parts.size() > kMaxPageSize)
                return error(400, QStringLiteral("bad_request"), QStringLiteral("ids : %1 au plus").arg(kMaxPageSize));
            QVector<int> ids;
            for (const QString& part : parts) {
                bool okId = false;
                const int id = part.trimmed().toInt(&okId);
                if (!okId || id < 1)
                    return error(400, QStringLiteral("bad_request"), QStringLiteral("ids invalide"));
                ids.append(id);
            }
            return items(m_module->libraryPhotos(ids));
        }
        LibraryFilter filter;
        QString detail;
        if (!parseLibraryFilter(query, &filter, &detail))
            return error(400, QStringLiteral("bad_request"), detail);
        if (seg.size() == 2)
            return ok(m_module->libraryFacets(filter));
        const int page = positiveIntParam(query, QStringLiteral("page"), 1);
        int pageSize   = positiveIntParam(query, QStringLiteral("page_size"), kDefaultPageSize);
        if (page < 0 || pageSize < 0)
            return error(400, QStringLiteral("bad_request"), QStringLiteral("page/page_size invalide"));
        pageSize = qMin(pageSize, kMaxPageSize);
        const QString skip = query.queryItemValue(QStringLiteral("skip_total"));
        // `cursor` : pagination stable (voir contrat §7) ; il prime sur `page`.
        const QString cursor = query.queryItemValue(QStringLiteral("cursor"), QUrl::FullyDecoded).trimmed();
        bool cursorValid = true;
        const QJsonObject list = m_module->libraryList(filter, page, pageSize,
                                                       skip == QLatin1String("true") || skip == QLatin1String("1"),
                                                       cursor, &cursorValid);
        if (!cursorValid)
            return error(400, QStringLiteral("bad_request"), QStringLiteral("cursor invalide"));
        return ok(list);
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
            // `writable` : source qualifiable, montee en lecture/ecriture pour que
            // morfPhoto y ecrive `.morfphoto.json`. Absent = false (lecture seule),
            // retro-compatible avec un client plus ancien.
            const bool writable = in.value(QStringLiteral("writable")).toBool(false);
            QJsonObject out;
            QString err;
            if (!m_module->addSource(host, share, username, password, hostname, writable, &out, &err)) {
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
