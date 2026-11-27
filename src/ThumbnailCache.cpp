/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/ThumbnailCache.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStorageInfo>

#include <algorithm>
#include <vector>

namespace morfphoto {

namespace {

// Un JPEG deja a la bonne taille est rendu tel quel : le recompresser abimerait
// l'image pour rien. Au-dela de ce poids, on reencode (un apercu de 400 px de 3 Mo
// serait un gaspillage de bande passante pour une grille).
constexpr qint64 kMaxPassthroughBytes = 1024 * 1024;

// Version du format de vignette, melangee a la cle : changer la taille ou la qualite
// invalide d'un coup toutes les vignettes en cache, sans migration.
constexpr auto kCacheVersion = "grid-400-v1";

// Le plafond n'est verifie qu'une ecriture sur N : parcourir le cache a chaque
// vignette coute plus cher que la vignette elle-meme.
constexpr qint64 kTrimEveryWrites = 64;

// Lit une image deja ouverte en la reduisant AVANT decodage complet quand le format
// le permet (JPEG : reduction DCT), puis applique l'orientation EXIF.
QImage readReduced(QImageReader& reader) {
    const QSize size = reader.size();
    if (size.isEmpty())
        return {};
    if (qMax(size.width(), size.height()) > kThumbnailEdge)
        reader.setScaledSize(size.scaled(kThumbnailEdge, kThumbnailEdge, Qt::KeepAspectRatio)
                                 .expandedTo(QSize(1, 1)));
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    // La reduction DCT tombe sur des multiples de 1/8 : si le resultat depasse encore
    // la grille (rare), on termine proprement plutot que de servir trop grand.
    if (qMax(image.width(), image.height()) > kThumbnailEdge)
        return image.scaled(kThumbnailEdge, kThumbnailEdge, Qt::KeepAspectRatio,
                            Qt::SmoothTransformation);
    return image;
}

QByteArray encodeJpeg(const QImage& image) {
    if (image.isNull())
        return {};
    QByteArray jpeg;
    QBuffer out(&jpeg);
    out.open(QIODevice::WriteOnly);
    QImageWriter writer(&out, "jpeg");
    writer.setQuality(85);
    return writer.write(image) ? jpeg : QByteArray();
}

} // namespace

QByteArray gridThumbnailFromBytes(const QByteArray& jpeg) {
    if (jpeg.isEmpty())
        return {};
    QByteArray copy = jpeg;   // QBuffer veut un QByteArray modifiable
    QBuffer in(&copy);
    in.open(QIODevice::ReadOnly);
    QImageReader reader(&in, "jpeg");
    const QSize original = reader.size();
    if (original.isEmpty())
        return {};
    if (qMax(original.width(), original.height()) <= kThumbnailEdge
        && jpeg.size() <= kMaxPassthroughBytes)
        return jpeg;
    return encodeJpeg(readReduced(reader));
}

QByteArray gridThumbnailFromFile(const QString& path) {
    QImageReader reader(path, "jpeg");
    return encodeJpeg(readReduced(reader));
}

// --- ThumbnailCache -----------------------------------------------------------

ThumbnailCache::ThumbnailCache(const QString& dir, qint64 maxBytes, qint64 minFreeBytes)
    : m_dir(dir), m_maxBytes(maxBytes), m_minFreeBytes(minFreeBytes) {
    QDir().mkpath(m_dir);
}

QString ThumbnailCache::key(const QString& path, qint64 size, qint64 mtime) {
    const QByteArray raw = QStringLiteral("%1|%2|%3|%4")
                               .arg(QString::fromLatin1(kCacheVersion), path)
                               .arg(size).arg(mtime).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha1).toHex());
}

QString ThumbnailCache::filePath(const QString& key) const {
    // Deux caracteres de sous-dossier : evite un dossier de dizaines de milliers de
    // fichiers, lent a lister sur la plupart des systemes de fichiers.
    return QStringLiteral("%1/%2/%3.jpg").arg(m_dir, key.left(2), key);
}

QByteArray ThumbnailCache::load(const QString& key) {
    QFile f(filePath(key));
    if (!f.open(QIODevice::ReadOnly)) {
        m_misses.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty()) {
        m_misses.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    // Date de modification = date de dernier usage : le plafond retire donc d'abord
    // ce qu'on n'a pas regarde depuis le plus longtemps.
    QFile touch(filePath(key));
    if (touch.open(QIODevice::ReadWrite))
        touch.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    m_hits.fetch_add(1, std::memory_order_relaxed);
    return data;
}

bool ThumbnailCache::store(const QString& key, const QByteArray& jpeg) {
    if (jpeg.isEmpty() || m_full.load(std::memory_order_relaxed))
        return false;
    const QString path = filePath(key);
    QDir().mkpath(QFileInfo(path).absolutePath());
    // QSaveFile : ecriture dans un fichier temporaire puis renommage, donc jamais de
    // vignette a moitie ecrite si le service s'arrete ou si deux requetes se croisent.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(jpeg);
    if (!out.commit())
        return false;
    const qint64 n = m_writes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % kTrimEveryWrites == 0)
        trim();
    return true;
}

void ThumbnailCache::trim() {
    // Un seul passage a la fois : les autres requetes continuent sans attendre.
    if (!m_trimMutex.tryLock())
        return;
    const auto release = qScopeGuard([this] { m_trimMutex.unlock(); });

    // Garde-fou d'espace libre : evalue ici (rarement) plutot qu'a chaque ecriture.
    if (m_minFreeBytes > 0) {
        const QStorageInfo disk(m_dir);
        m_full.store(disk.isValid() && disk.bytesAvailable() < m_minFreeBytes,
                     std::memory_order_relaxed);
    }
    if (m_maxBytes <= 0)
        return;

    struct Entry { QString path; qint64 size; QDateTime used; };
    std::vector<Entry> entries;
    qint64 total = 0;
    QDirIterator it(m_dir, {QStringLiteral("*.jpg")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        entries.push_back({fi.absoluteFilePath(), fi.size(), fi.lastModified()});
        total += fi.size();
    }
    if (total <= m_maxBytes)
        return;
    // On redescend a 90 % du plafond : evite de relancer un nettoyage a chaque ecriture.
    const qint64 target = m_maxBytes / 10 * 9;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const Entry& e : entries) {
        if (total <= target)
            break;
        if (QFile::remove(e.path))
            total -= e.size;
    }
}

QJsonObject ThumbnailCache::stats() const {
    QJsonObject o;
    o[QStringLiteral("enabled")] = true;
    o[QStringLiteral("dir")]     = m_dir;
    o[QStringLiteral("hits")]    = static_cast<double>(m_hits.load(std::memory_order_relaxed));
    o[QStringLiteral("misses")]  = static_cast<double>(m_misses.load(std::memory_order_relaxed));
    o[QStringLiteral("writes")]  = static_cast<double>(m_writes.load(std::memory_order_relaxed));
    o[QStringLiteral("full")]    = m_full.load(std::memory_order_relaxed);
    return o;
}

} // namespace morfphoto
