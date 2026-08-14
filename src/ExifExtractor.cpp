/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/ExifExtractor.h"

#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <utility>

namespace morfphoto {

namespace {

// Tags demandés à ExifTool, un par ligne (protocole argfile). Le suffixe `#`
// force la valeur NUMÉRIQUE brute (pas la conversion lisible). Plusieurs alias
// d'objectif et de date : tous les boîtiers ne les nomment pas pareil.
const char* const kTags[] = {
    "-DateTimeOriginal", "-CreateDate", "-Make", "-Model",
    "-LensModel", "-LensID", "-Lens",
    "-FocalLength#", "-FocalLengthIn35mmFormat#", "-FNumber#",
    "-ExposureTime#",          // vitesse numérique en secondes -> shutter_speed_s
    "-ShutterSpeed",           // vitesse lisible « 1/250 » -> shutter_speed
    "-ISO#", "-ExposureCompensation#", "-Rating#",
    "-GPSLatitude#", "-GPSLongitude#",
};

// Format de date ISO imposé : « 2024-06-15 14:23:01 ». SQLite sait le lire, et
// la colonne calculée `taken_year` peut en extraire l'année.
const char* const kDateFormat = "%Y-%m-%d %H:%M:%S";
constexpr int kTimeoutMs = 15000;

// Sonde de disponibilité (« -ver ») : plus courte, elle ne lit pas de fichier.
constexpr int kProbeTimeoutMs = 5000;

// Assemble le bloc d'arguments commun (JSON, charset, format de date, tags, fichier).
QStringList argBlock(const QString& path) {
    QStringList args;
    args << QStringLiteral("-json")
         << QStringLiteral("-charset") << QStringLiteral("filename=UTF8")
         << QStringLiteral("-d") << QString::fromLatin1(kDateFormat);
    for (const char* t : kTags)
        args << QString::fromLatin1(t);
    args << path;
    return args;
}

// Premier alias texte présent et non vide (objectif, date...).
QVariant firstStr(const QJsonObject& o, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const QString s = o.value(QLatin1String(k)).toString().trimmed();
        if (!s.isEmpty())
            return s;
    }
    return {};
}

QVariant strVar(const QJsonObject& o, const char* key) {
    const QString s = o.value(QLatin1String(key)).toString().trimmed();
    return s.isEmpty() ? QVariant() : QVariant(s);
}

// Valeur numérique tolérante : nombre JSON, ou chaîne convertible, sinon NULL.
QVariant numVar(const QJsonObject& o, const char* key) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        if (ok) return d;
    }
    return {};
}

QVariant intVar(const QJsonObject& o, const char* key) {
    const QVariant n = numVar(o, key);
    return n.isNull() ? QVariant() : QVariant(qRound(n.toDouble()));
}

} // namespace

bool ExifExtractor::probe(const QString& binary, QString* detail) {
    QProcess proc;
    proc.start(binary, {QStringLiteral("-ver")});
    // Binaire absent du PATH, ou droits manquants : le process ne démarre même pas.
    if (!proc.waitForStarted(kProbeTimeoutMs)) {
        if (detail)
            *detail = QStringLiteral("binaire introuvable ou non demarrable : %1").arg(binary);
        return false;
    }
    if (!proc.waitForFinished(kProbeTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(kProbeTimeoutMs);
        if (detail)
            *detail = QStringLiteral("pas de reponse a « %1 -ver »").arg(binary);
        return false;
    }
    // « -ver » n'écrit que le numéro de version sur stdout et sort avec 0.
    const QString ver = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 || ver.isEmpty()) {
        if (detail)
            *detail = QStringLiteral("« %1 -ver » a echoue").arg(binary);
        return false;
    }
    if (detail)
        *detail = QStringLiteral("ExifTool %1").arg(ver);
    return true;
}

ExifExtractor::ExifExtractor(QString binary, bool stayOpen)
    : m_binary(std::move(binary)), m_stayOpen(stayOpen) {}

ExifExtractor::~ExifExtractor() {
    close();
}

void ExifExtractor::open() {
    if (!m_stayOpen || m_proc)
        return;
    // `-@ -` : ExifTool lit ses arguments sur stdin, jusqu'à un `-execute`.
    m_proc = new QProcess();
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    m_proc->start(m_binary, {QStringLiteral("-stay_open"), QStringLiteral("True"),
                             QStringLiteral("-@"), QStringLiteral("-")});
    if (!m_proc->waitForStarted(kTimeoutMs)) {
        delete m_proc;
        m_proc = nullptr;
    }
}

void ExifExtractor::close() {
    if (!m_proc)
        return;
    if (m_proc->state() == QProcess::Running) {
        m_proc->write("-stay_open\nFalse\n");
        m_proc->waitForBytesWritten(kTimeoutMs);
        if (!m_proc->waitForFinished(kTimeoutMs)) {
            m_proc->kill();
            m_proc->waitForFinished(kTimeoutMs);
        }
    }
    delete m_proc;
    m_proc = nullptr;
}

ExifData ExifExtractor::extract(const QString& path, QString* error) {
    QString localError;
    QString* err = error ? error : &localError;
    err->clear();

    const QString json = (m_stayOpen && m_proc) ? readPersistent(path, err)
                                                : readOnce(path, err);
    if (!err->isEmpty())
        return {};
    return mapTags(json, err);
}

QString ExifExtractor::readPersistent(const QString& path, QString* error) {
    ++m_counter;
    const QByteArray marker = "{ready" + QByteArray::number(m_counter) + "}";

    // Pousser le bloc d'arguments, un par ligne, puis l'ordre d'exécution numéroté.
    QByteArray payload;
    for (const QString& a : argBlock(path))
        payload += a.toUtf8() + "\n";
    payload += "-execute" + QByteArray::number(m_counter) + "\n";
    m_proc->write(payload);
    m_proc->waitForBytesWritten(kTimeoutMs);

    // Lire jusqu'au marqueur {readyN} (exclu de la sortie).
    QByteArray buf;
    while (!buf.contains(marker)) {
        if (!m_proc->waitForReadyRead(kTimeoutMs)) {
            *error = QStringLiteral("ExifTool: pas de reponse pour %1").arg(path);
            return {};
        }
        buf += m_proc->readAllStandardOutput();
    }
    return QString::fromUtf8(buf.left(buf.indexOf(marker)));
}

QString ExifExtractor::readOnce(const QString& path, QString* error) {
    QProcess proc;
    proc.start(m_binary, argBlock(path));
    if (!proc.waitForFinished(kTimeoutMs)) {
        *error = QStringLiteral("ExifTool: delai depasse pour %1").arg(path);
        return {};
    }
    const QByteArray out = proc.readAllStandardOutput();
    if (out.trimmed().isEmpty()) {
        *error = QStringLiteral("ExifTool: %1").arg(QString::fromUtf8(proc.readAllStandardError()).trimmed());
        return {};
    }
    return QString::fromUtf8(out);
}

ExifData ExifExtractor::mapTags(const QString& json, QString* error) {
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isArray() || doc.array().isEmpty()) {
        *error = QStringLiteral("ExifTool: sortie illisible");
        return {};
    }
    const QJsonObject o = doc.array().first().toObject();
    if (o.contains(QStringLiteral("Error"))) {
        *error = QStringLiteral("ExifTool: %1").arg(o.value(QStringLiteral("Error")).toString());
        return {};
    }

    ExifData d;
    d.ok                   = true;
    d.takenAt              = firstStr(o, {"DateTimeOriginal", "CreateDate"});
    d.make                 = strVar(o, "Make");
    d.cameraModel          = strVar(o, "Model");
    d.lens                 = firstStr(o, {"LensModel", "LensID", "Lens"});
    d.focalLength          = numVar(o, "FocalLength");
    d.focalLength35mm      = numVar(o, "FocalLengthIn35mmFormat");
    d.aperture             = numVar(o, "FNumber");
    d.shutterSpeed         = strVar(o, "ShutterSpeed");
    d.shutterSpeedS        = numVar(o, "ExposureTime");
    d.iso                  = intVar(o, "ISO");
    d.exposureCompensation = numVar(o, "ExposureCompensation");
    d.rating               = intVar(o, "Rating");
    d.latitude             = numVar(o, "GPSLatitude");
    d.longitude            = numVar(o, "GPSLongitude");

    // raw_exif : tags renvoyés, hors SourceFile, pour extension future.
    QJsonObject raw = o;
    raw.remove(QStringLiteral("SourceFile"));
    d.raw = raw;
    return d;
}

} // namespace morfphoto
