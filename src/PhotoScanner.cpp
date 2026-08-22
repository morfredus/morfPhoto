/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/PhotoScanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace morfphoto {

namespace {

// Extensions RAW connues (regroupées sous le type "raw"). Mapping figé, documenté.
const QSet<QString>& rawExtensions() {
    static const QSet<QString> s = {
        ".arw", ".cr2", ".cr3", ".nef", ".nrw", ".raf", ".rw2", ".orf", ".dng",
        ".pef", ".srw", ".x3f", ".raw", ".3fr", ".mos", ".iiq", ".gpr", ".mrw",
        ".dcr", ".kdc", ".erf", ".mef", ".rwl",
    };
    return s;
}

// Types explicites hors RAW.
QString explicitType(const QString& ext) {
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".jpe") return QStringLiteral("jpeg");
    if (ext == ".tif" || ext == ".tiff") return QStringLiteral("tiff");
    if (ext == ".heic" || ext == ".heif") return QStringLiteral("heic");
    if (ext == ".png")  return QStringLiteral("png");
    if (ext == ".webp") return QStringLiteral("webp");
    return {};
}

// Périmètre des fichiers pris en compte pendant le parcours.
bool isImageExtension(const QString& ext) {
    return rawExtensions().contains(ext) || !explicitType(ext).isEmpty();
}

// Normalise un chemin en absolu : canonique si le chemin existe (résout liens et
// `..`), sinon nettoyage lexical (résout `..` textuellement). Suffisant pour le
// contrôle de périmètre.
QString normalizePath(const QString& path) {
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : canonical;
}

} // namespace

QString fileTypeFor(const QString& extension) {
    const QString ext = extension.toLower();
    if (rawExtensions().contains(ext))
        return QStringLiteral("raw");
    const QString t = explicitType(ext);
    return t.isEmpty() ? QStringLiteral("other") : t;
}

bool isWithinRoots(const QString& candidate, const QStringList& roots) {
    const QString target = normalizePath(candidate);
    for (const QString& root : roots) {
        const QString base = normalizePath(root);
        if (base.isEmpty())
            continue;
        if (target == base || target.startsWith(base + QLatin1Char('/')))
            return true;
    }
    return false;
}

bool probeAccessible(const QString& path, int timeoutMs) {
    // Le stat vit sur un thread détaché : c'est LUI qui peut rester bloqué le
    // temps du timeout du montage réseau. La promesse est partagée par shared_ptr
    // pour survivre au retour de cette fonction : quand l'appel finit enfin par
    // revenir (déblocage tardif du montage), le thread écrit dans une promesse
    // encore vivante, sans écrire dans une pile disparue.
    auto promise = std::make_shared<std::promise<bool>>();
    std::future<bool> fut = promise->get_future();
    std::thread([promise, path]() {
        // QFileInfo neuf à chaque appel (pas de cache) : force un vrai stat.
        const QFileInfo fi(path);
        promise->set_value(fi.exists() && fi.isDir());
    }).detach();

    if (timeoutMs <= 0)
        return fut.get();
    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready)
        return fut.get();
    // Délai dépassé : la source ne répond pas assez vite, on la considère
    // indisponible plutôt que de rester figé. Le thread détaché se terminera seul.
    return false;
}

ScanResult scanFolder(const QString& folder, bool recursive,
                      const std::function<bool()>& stillAvailable) {
    // Intervalle de contrôle : consulter stillAvailable toutes les N entrées suffit
    // à borner la réactivité sans transformer chaque fichier en sonde réseau.
    constexpr int kCheckEvery = 256;

    ScanResult res;
    const QDirIterator::IteratorFlags flags =
        recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    QDirIterator it(folder, QDir::Files | QDir::NoDotAndDotDot, flags);
    int since = 0;
    while (it.hasNext()) {
        // Vérifier AVANT de toucher l'entrée suivante : si la source a disparu, on
        // sort tout de suite, `completed` reste false, et le verdict de disparition
        // sera écarté par l'appelant (jamais de markMissing sur un scan partiel).
        if (stillAvailable && ++since >= kCheckEvery) {
            since = 0;
            if (!stillAvailable())
                return res;   // completed == false
        }
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString ext = QStringLiteral(".") + fi.suffix().toLower();
        if (!isImageExtension(ext))
            continue;
        FileInfo info;
        info.path      = fi.absoluteFilePath();
        info.directory = fi.absolutePath();
        info.filename  = fi.fileName();
        info.extension = ext;
        info.fileType  = fileTypeFor(ext);
        info.size      = fi.size();
        info.mtime     = fi.lastModified().toSecsSinceEpoch();
        res.files.push_back(info);
    }
    // Arrivé au bout du parcours : le scan est fiable et complet.
    res.completed = true;
    return res;
}

qint64 countImageFiles(const QString& folder, bool recursive,
                       const std::function<bool()>& stillAvailable, bool* completed) {
    constexpr int kCheckEvery = 256;
    const QDirIterator::IteratorFlags flags =
        recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    QDirIterator it(folder, QDir::Files | QDir::NoDotAndDotDot, flags);
    qint64 n = 0;
    int since = 0;
    while (it.hasNext()) {
        if (stillAvailable && ++since >= kCheckEvery) {
            since = 0;
            if (!stillAvailable()) {
                if (completed)
                    *completed = false;
                return n;
            }
        }
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString ext = QStringLiteral(".") + fi.suffix().toLower();
        if (!isImageExtension(ext))
            continue;
        ++n;
    }
    if (completed)
        *completed = true;
    return n;
}

} // namespace morfphoto
