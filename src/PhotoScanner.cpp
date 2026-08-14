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

QVector<FileInfo> scanFolder(const QString& folder, bool recursive) {
    QVector<FileInfo> out;
    const QDirIterator::IteratorFlags flags =
        recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    QDirIterator it(folder, QDir::Files | QDir::NoDotAndDotDot, flags);
    while (it.hasNext()) {
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
        out.push_back(info);
    }
    return out;
}

} // namespace morfphoto
