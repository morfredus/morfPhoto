/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/SourceManager.h"
#include "morfphoto/SmbSourceNaming.h"
#include "morfphoto/Paths.h"

#include <QtGlobal>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QDateTime>
#include <QProcess>
#include <QFileInfo>

#include <utility>

namespace morfphoto {

SourceManager::SourceManager(QString serviceName, QString helperPath)
    : m_serviceName(std::move(serviceName)), m_helperPath(std::move(helperPath)) {}

QString SourceManager::storePath() const {
    return QDir(Paths::stateDir(m_serviceName)).filePath(QStringLiteral("sources.json"));
}

bool SourceManager::isMounted(const QString& mountpoint) {
    QFile f(QStringLiteral("/proc/mounts"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray needle = (QStringLiteral(" ") + mountpoint + QStringLiteral(" ")).toUtf8();
    return f.readAll().contains(needle);
}

void SourceManager::load() {
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly)) { m_sources = {}; return; }
    m_sources = QJsonDocument::fromJson(f.readAll()).array();
}

void SourceManager::save() const {
    QFile f(storePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(m_sources).toJson(QJsonDocument::Indented));
}

QStringList SourceManager::mountpoints() const {
    QStringList mp;
    for (const QJsonValue& v : m_sources) {
        const QString m = v.toObject().value(QStringLiteral("mountpoint")).toString();
        if (!m.isEmpty()) mp << m;
    }
    return mp;
}

QJsonArray SourceManager::listSources() const {
    QJsonArray out;
    for (const QJsonValue& v : m_sources) {
        QJsonObject o = v.toObject();
        o["mounted"] = isMounted(o.value(QStringLiteral("mountpoint")).toString());
        out.append(o);
    }
    return out;
}

bool SourceManager::helperReady(QJsonObject* out, QString* error) const {
    auto fail = [&](const QString& code, const QString& detail) {
        if (error) *error = detail;
        if (out) {
            (*out)[QStringLiteral("ok")] = false;
            (*out)[QStringLiteral("code")] = code;
            (*out)[QStringLiteral("detail")] = detail;
            (*out)[QStringLiteral("path")] = m_helperPath;
        }
        return false;
    };
#ifndef Q_OS_UNIX
    if (out) {
        (*out)[QStringLiteral("ok")] = false;
        (*out)[QStringLiteral("code")] = QStringLiteral("linux_only");
        (*out)[QStringLiteral("detail")] = QStringLiteral(
            "le helper privilegie ne sert que sur l'hote Linux de morfPhoto");
    }
    if (error) *error = QStringLiteral("le helper privilegie ne sert que sous Linux");
    return false;
#else
    const QFileInfo fi(m_helperPath);
    const QFileInfo dir(fi.absolutePath());
    if (!dir.exists() || !dir.isDir())
        return fail(QStringLiteral("helper_dir_absent"),
                    QStringLiteral("dossier du helper absent (%1) : reinstaller le paquet morfPhoto")
                        .arg(dir.absoluteFilePath()));
    // x sur un dossier = le parcourir. 750 root:root : le service ne voit pas le binaire.
    if (!dir.isReadable() || !dir.isExecutable())
        return fail(QStringLiteral("helper_dir_inaccessible"),
                    QStringLiteral(
                        "dossier du helper non traversable (%1). Attendu : 750 root:<compte du service>")
                        .arg(dir.absoluteFilePath()));
    if (!fi.exists() || !fi.isFile())
        return fail(QStringLiteral("helper_absent"),
                    QStringLiteral("helper privilegie absent (%1) : reinstaller le paquet morfPhoto")
                        .arg(m_helperPath));
    if (!fi.isExecutable())
        return fail(QStringLiteral("helper_not_executable"),
                    QStringLiteral(
                        "helper present mais non executable par ce compte (%1). "
                        "Attendu : 4750 root:<compte du service>")
                        .arg(m_helperPath));

    QProcess helper;
    helper.start(m_helperPath, {QStringLiteral("probe")});
    if (!helper.waitForStarted(8000)) {
        return fail(QStringLiteral("helper_not_startable"),
                    QStringLiteral("helper non demarrable (%1) : %2")
                        .arg(m_helperPath, helper.errorString()));
    }
    helper.waitForFinished(8000);
    const QJsonObject report = QJsonDocument::fromJson(helper.readAllStandardOutput()).object();
    const bool ok = helper.exitStatus() == QProcess::NormalExit
        && helper.exitCode() == 0
        && report.value(QStringLiteral("ok")).toBool();
    if (!ok) {
        const QString detail = report.value(QStringLiteral("detail")).toString().trimmed();
        const QString errOut = QString::fromUtf8(helper.readAllStandardError()).trimmed();
        return fail(report.value(QStringLiteral("code")).toString(QStringLiteral("helper_probe_failed")),
                    !detail.isEmpty() ? detail
                    : (!errOut.isEmpty() ? errOut
                       : QStringLiteral("le helper a refuse le controle probe")));
    }
    if (out) {
        *out = report;
        (*out)[QStringLiteral("ok")] = true;
        (*out)[QStringLiteral("path")] = m_helperPath;
        (*out)[QStringLiteral("code")] = QStringLiteral("probe_ok");
    }
    return true;
#endif
}

void SourceManager::scheduleServiceRestart() const {
#ifdef Q_OS_UNIX
    QProcess::startDetached(m_helperPath, {QStringLiteral("restart-service")});
#endif
}

bool SourceManager::addSource(const QString& host, const QString& share, const QString& username,
                              const QString& password, const QString& hostname,
                              QJsonObject* out, QString* error) {
#ifndef Q_OS_UNIX
    Q_UNUSED(host); Q_UNUSED(share); Q_UNUSED(username);
    Q_UNUSED(password); Q_UNUSED(hostname); Q_UNUSED(out);
    if (error) *error = QStringLiteral("les sources SMB poussees ne sont montables que sur un hote Linux");
    return false;
#else
    if (host.isEmpty() || share.isEmpty() || username.isEmpty()) {
        if (error) *error = QStringLiteral("host, share et username sont obligatoires");
        return false;
    }
    // Identite = hostname Windows, jamais l'IP de connexion. Toutes les machines
    // suivent la meme convention (pas de premiere source "generique").
    if (hostname.trimmed().isEmpty() || looksLikeIpv4(hostname)) {
        if (error) *error = QStringLiteral(
            "le hostname de la machine source est obligatoire "
            "(identifiant canonique, distinct de l'adresse IP)");
        return false;
    }
    const QString slug = hostnameSlug(hostname);
    if (!isValidSourceSlug(slug)) {
        if (error) *error = QStringLiteral("hostname inexploitable pour nommer le montage");
        return false;
    }
    const QString mountpoint = mountpointForSlug(slug);

    QJsonObject ready;
    if (!helperReady(&ready, error)) {
        if (out) *out = ready;
        return false;
    }

    QProcess helper;
    helper.start(m_helperPath, {QStringLiteral("mount"), host, share, slug});
    if (!helper.waitForStarted(10000)) {
        if (error) *error = QStringLiteral("helper privilegie introuvable ou non demarrable (%1)")
                                .arg(m_helperPath);
        return false;
    }
    helper.write((username + QStringLiteral("\n") + password + QStringLiteral("\n")).toUtf8());
    helper.closeWriteChannel();
    helper.waitForFinished(-1);

    const QByteArray stdoutBytes = helper.readAllStandardOutput();
    const QByteArray stderrBytes = helper.readAllStandardError();
    const QJsonObject report = QJsonDocument::fromJson(stdoutBytes).object();
    const bool helperOk = helper.exitStatus() == QProcess::NormalExit
        && helper.exitCode() == 0
        && report.value(QStringLiteral("ok")).toBool();

    if (!helperOk) {
        const QString detail = report.value(QStringLiteral("detail")).toString().trimmed();
        const QString errOut = QString::fromUtf8(stderrBytes).trimmed();
        if (error) {
            *error = !detail.isEmpty() ? detail
                   : (!errOut.isEmpty() ? errOut
                   : QStringLiteral("le helper a refuse ou echoue le montage"));
        }
        if (out) {
            *out = report;
            if (!out->contains(QStringLiteral("code")))
                (*out)[QStringLiteral("code")] = QStringLiteral("cifs_mount_failed");
            (*out)[QStringLiteral("ok")] = false;
        }
        return false;
    }

    // Metadonnees non secretes. Upsert par slug/mountpoint : re-pousser la meme
    // machine ne duplique rien et ne touche pas une autre source.
    QJsonObject entry;
    entry[QStringLiteral("host")]        = host;
    entry[QStringLiteral("hostname")]    = hostname.trimmed();
    entry[QStringLiteral("slug")]        = slug;
    entry[QStringLiteral("share")]       = share;
    entry[QStringLiteral("mountpoint")]  = mountpoint;
    entry[QStringLiteral("credentials")] = credentialsPathForSlug(slug);
    entry[QStringLiteral("added_at")]    = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonArray updated;
    bool replaced = false;
    for (const QJsonValue& v : m_sources) {
        const QString existing = v.toObject().value(QStringLiteral("mountpoint")).toString();
        const QString existingSlug = v.toObject().value(QStringLiteral("slug")).toString();
        if (existing == mountpoint || existingSlug == slug) {
            updated.append(entry);
            replaced = true;
        } else {
            updated.append(v);
        }
    }
    if (!replaced) updated.append(entry);
    m_sources = updated;
    save();

    if (out) {
        *out = report;
        for (auto it = entry.constBegin(); it != entry.constEnd(); ++it)
            if (!out->contains(it.key()))
                (*out)[it.key()] = it.value();
        (*out)[QStringLiteral("mounted")] = isMounted(mountpoint);
        (*out)[QStringLiteral("ok")] = true;
    }
    return true;
#endif
}

} // namespace morfphoto
