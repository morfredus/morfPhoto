/*
 * morfPhoto - deliberately narrow Linux privilege boundary
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Le service morfPhoto tourne sans privilege. Monter un partage SMB en cifs
 * exige root. Ce binaire setuid EST la seule porte privilegiee : une source
 * (hostname slug) = un montage lecture seule sous /mnt/photos_<slug>, un
 * fichier /etc/morfsystem/smb-photos-<slug>.cred, une ligne fstab, et
 * l'ajout idempotent de cette racine dans morfphoto.json. Rien d'autre.
 *
 * Invariants DURS :
 *   - execution root obligatoire, Linux uniquement ;
 *   - point de montage cree : uniquement /mnt/photos_<slug> ;
 *   - montage cifs LECTURE SEULE, options figees (sans nofail au test) ;
 *   - mot de passe sur stdin, jamais en argv ;
 *   - un fichier d'identifiants par slug (jamais d'ecrasement croise) ;
 *   - fstab et morfphoto.json d'une source ne touchent jamais une autre ;
 *   - la racine n'est ajoutee au JSON qu'apres validation reelle du montage.
 *
 * Verbes :
 *   mount   <host> <share> <slug>   (identifiants : 2 lignes sur stdin)
 *   unmount <mountpoint>
 *   restart-service
 */

#include "morfphoto/SmbSourceNaming.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr auto kConfigJson = "/etc/morfsystem/morfphoto/morfphoto.json";
constexpr auto kFstab      = "/etc/fstab";

// UID/GID du *appelant* (compte du service), captures avant setuid(0).
// Les fichiers CIFS doivent rester lisibles par ce compte, pas par root.
quint32 g_fileUid = 0;
quint32 g_fileGid = 0;

struct CmdResult {
    bool    ok = false;
    int     code = -1;
    QString output;
};

struct Report {
    bool       ok = false;
    bool       restartNeeded = false;
    bool       alreadyConfigured = false;
    QString    code;
    QString    message;
    QString    slug;
    QString    mountpoint;
    QString    credentials;
    QJsonArray steps;
};

void addStep(Report& r, const QString& id, bool ok, const QString& detail = {}) {
    QJsonObject s;
    s[QStringLiteral("id")] = id;
    s[QStringLiteral("ok")]  = ok;
    if (!detail.isEmpty())
        s[QStringLiteral("detail")] = detail;
    r.steps.append(s);
}

QJsonObject reportJson(const Report& r) {
    QJsonObject o;
    o[QStringLiteral("ok")]                 = r.ok;
    o[QStringLiteral("restart_needed")]     = r.restartNeeded;
    o[QStringLiteral("already_configured")] = r.alreadyConfigured;
    o[QStringLiteral("code")]               = r.code;
    o[QStringLiteral("detail")]             = r.message;
    o[QStringLiteral("slug")]               = r.slug;
    o[QStringLiteral("mountpoint")]         = r.mountpoint;
    o[QStringLiteral("credentials")]        = r.credentials;
    o[QStringLiteral("steps")]              = r.steps;
    return o;
}

int emitReport(const Report& r, int exitCode) {
    QTextStream(stdout) << QJsonDocument(reportJson(r)).toJson(QJsonDocument::Compact) << '\n';
    if (!r.ok && !r.message.isEmpty())
        QTextStream(stderr) << r.message << '\n';
    return exitCode;
}

int fail(Report& r, const QString& code, const QString& message) {
    r.ok = false;
    r.code = code;
    r.message = message;
    return emitReport(r, 2);
}

int refuse(const QString& message) {
    Report r;
    r.code = QStringLiteral("invalid_request");
    r.message = message;
    return fail(r, r.code, message);
}

CmdResult run(const QString& program, const QStringList& arguments, int timeoutMs = 45000) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    CmdResult out;
    if (!process.waitForStarted(10000)) {
        out.output = QStringLiteral("impossible de demarrer %1").arg(program);
        return out;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        out.output = QStringLiteral("%1 n'a pas repondu dans le delai").arg(program);
        return out;
    }
    out.code   = process.exitCode();
    out.output = QString::fromUtf8(process.readAll()).trimmed();
    out.ok     = process.exitStatus() == QProcess::NormalExit && out.code == 0;
    return out;
}

QString classifyMountError(const QString& output) {
    const QString u = output.toUpper();
    // Le statut NT/SMB prime sur errno (-13 = EACCES). mount.cifs peut coller
    // « return code = -13 » après STATUS_ACCOUNT_LOCKED_OUT : ce n'est pas un ACL.
    if (u.contains(QLatin1String("STATUS_ACCOUNT_LOCKED_OUT"))
        || u.contains(QLatin1String("NT_STATUS_ACCOUNT_LOCKED_OUT")))
        return QStringLiteral("account_locked");
    if (u.contains(QLatin1String("STATUS_ACCOUNT_DISABLED"))
        || u.contains(QLatin1String("NT_STATUS_ACCOUNT_DISABLED"))
        || u.contains(QLatin1String("STATUS_ACCOUNT_EXPIRED"))
        || u.contains(QLatin1String("NT_STATUS_ACCOUNT_EXPIRED"))
        || u.contains(QLatin1String("STATUS_PASSWORD_EXPIRED"))
        || u.contains(QLatin1String("NT_STATUS_PASSWORD_EXPIRED"))
        || u.contains(QLatin1String("STATUS_PASSWORD_MUST_CHANGE"))
        || u.contains(QLatin1String("NT_STATUS_PASSWORD_MUST_CHANGE")))
        return QStringLiteral("account_locked");
    if (u.contains(QLatin1String("STATUS_LOGON_FAILURE"))
        || u.contains(QLatin1String("NT_STATUS_LOGON_FAILURE"))
        || u.contains(QLatin1String("NT_STATUS_WRONG_PASSWORD"))
        || u.contains(QLatin1String("NT_STATUS_LOGON_TYPE_NOT_GRANTED")))
        return QStringLiteral("auth_failed");
    if (u.contains(QLatin1String("NT_STATUS_BAD_NETWORK_NAME"))
        || u.contains(QLatin1String("NT_STATUS_OBJECT_NAME_NOT_FOUND"))
        || u.contains(QLatin1String("NT_STATUS_BAD_NETWORK_PATH")))
        return QStringLiteral("share_not_found");
    // mount.cifs (non root reel) : "permission denied: no match ... /etc/fstab".
    // Ce n'est PAS un refus ACL sur le partage Windows.
    if ((u.contains(QLatin1String("NO MATCH")) && u.contains(QLatin1String("FSTAB")))
        || u.contains(QLatin1String("FOUND IN /ETC/FSTAB")))
        return QStringLiteral("cifs_needs_root");
    if (u.contains(QLatin1String("NT_STATUS_ACCESS_DENIED"))
        || u.contains(QLatin1String("STATUS_ACCESS_DENIED")))
        return QStringLiteral("permission_denied");
    if (u.contains(QLatin1String("NT_STATUS_HOST_UNREACHABLE"))
        || u.contains(QLatin1String("ENETUNREACH"))
        || u.contains(QLatin1String("ETIMEDOUT"))
        || u.contains(QLatin1String("NO ROUTE"))
        || u.contains(QLatin1String("CONNECTION TIMED OUT"))
        || u.contains(QLatin1String("CONNECTION REFUSED"))
        || u.contains(QLatin1String("UNKNOWN HOST"))
        || u.contains(QLatin1String("NAME OR SERVICE NOT KNOWN")))
        return QStringLiteral("host_unreachable");
    return QStringLiteral("cifs_mount_failed");
}

QString humanFor(const QString& code, const QString& raw) {
    if (code == QLatin1String("auth_failed"))
        return QStringLiteral(
            "Impossible d'authentifier le compte Windows aupres du partage SMB. "
            "Verifiez le nom d'utilisateur et le mot de passe utilises pour l'acces reseau. "
            "L'identifiant est le nom d'utilisateur Windows de la machine (jamais l'e-mail). "
            "Compte local : mot de passe de session Windows. Compte Microsoft : mot de passe "
            "du compte Microsoft associe. Jamais le PIN Windows Hello ni une passkey.")
            + (raw.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(raw));
    if (code == QLatin1String("account_locked"))
        return QStringLiteral(
            "Le compte utilise pour acceder au partage SMB est actuellement verrouille "
            "(STATUS_ACCOUNT_LOCKED_OUT). Deverrouillez le compte Windows avant de reessayer.")
            + (raw.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(raw));
    if (code == QLatin1String("share_not_found"))
        return QStringLiteral(
            "Le partage SMB est introuvable sur la machine source. "
            "Verifiez que le partage Windows existe et que le pare-feu autorise SMB.");
    if (code == QLatin1String("host_unreachable"))
        return QStringLiteral(
            "La machine source est inaccessible depuis le serveur. "
            "Verifiez l'adresse, le reseau local et que le poste est allume.");
    if (code == QLatin1String("cifs_needs_root"))
        return QStringLiteral(
            "mount.cifs a refuse le montage faute d'identite root reelle "
            "(helper setuid : l'UID reel restait celui du service, d'ou l'exigence "
            "d'une ligne fstab au premier essai). Mettre a jour morfPhoto et reessayer.");
    if (code == QLatin1String("permission_denied"))
        return QStringLiteral(
            "Authentification acceptee mais permissions insuffisantes pour lire le partage.");
    if (code == QLatin1String("cifs_utils_missing"))
        return QStringLiteral(
            "cifs-utils est absent sur le serveur (mount.cifs introuvable). "
            "Installer le paquet puis reessayer.");
    if (!raw.isEmpty())
        return raw;
    return QStringLiteral("montage CIFS impossible");
}

struct MountInfo {
    bool    present = false;
    QString fstype;
    QString source;
};

MountInfo readMount(const QString& mountpoint) {
    MountInfo info;
    QFile f(QStringLiteral("/proc/mounts"));
    if (!f.open(QIODevice::ReadOnly))
        return info;
    const QByteArray needle = QStringLiteral(" %1 ").arg(mountpoint).toUtf8();
    for (const QByteArray& line : f.readAll().split('\n')) {
        if (!line.contains(needle))
            continue;
        const QList<QByteArray> cols = line.simplified().split(' ');
        if (cols.size() < 3)
            continue;
        if (QString::fromUtf8(cols.at(1)) != mountpoint)
            continue;
        info.present = true;
        info.source  = QString::fromUtf8(cols.at(0));
        info.fstype  = QString::fromUtf8(cols.at(2));
        return info;
    }
    return info;
}

QByteArray credBytes(const QString& user, const QString& password) {
    return QStringLiteral("username=%1\npassword=%2\n").arg(user, password).toUtf8();
}

bool sameCredentials(const QString& path, const QByteArray& wanted) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return f.readAll() == wanted;
}

bool writeCredentials(const QString& path, const QByteArray& body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const qint64 n = f.write(body);
    f.close();
    if (n != body.size())
        return false;
#ifdef Q_OS_UNIX
    if (::chmod(path.toUtf8().constData(), S_IRUSR | S_IWUSR) != 0)
        return false;
    if (::chown(path.toUtf8().constData(), 0, 0) != 0)
        return false;
#endif
    return true;
}

QString fstabLine(const QString& host, const QString& share,
                  const QString& mountpoint, const QString& cred) {
    // nofail + automount : uniquement pour la persistance, APRES un montage
    // manuel deja valide. uid/gid = utilisateur reel (compte du service).
    return QStringLiteral(
        "//%1/%2 %3 cifs credentials=%4,ro,uid=%5,gid=%6,iocharset=utf8,vers=3.0,"
        "nofail,x-systemd.automount 0 0")
        .arg(host, share, mountpoint, cred)
        .arg(g_fileUid).arg(g_fileGid);
}

bool ensureFstab(const QString& host, const QString& share,
                 const QString& mountpoint, const QString& cred, bool* changed) {
    const QString line = fstabLine(host, share, mountpoint, cred);
    const QByteArray needle = QStringLiteral(" %1 ").arg(mountpoint).toUtf8();

    QFile f(QString::fromLatin1(kFstab));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QList<QByteArray> lines = f.readAll().split('\n');
    f.close();

    QByteArray kept;
    bool found = false;
    bool identical = false;
    for (const QByteArray& raw : lines) {
        if (raw.trimmed().isEmpty() && kept.isEmpty())
            continue;
        if (raw.contains(needle)) {
            found = true;
            if (QString::fromUtf8(raw.trimmed()) == line) {
                identical = true;
                kept += raw;
                kept += '\n';
            } else {
                kept += line.toUtf8();
                kept += '\n';
            }
            continue;
        }
        kept += raw;
        kept += '\n';
    }
    if (!found) {
        if (!kept.isEmpty() && !kept.endsWith('\n'))
            kept += '\n';
        kept += line.toUtf8();
        kept += '\n';
    }
    const bool needWrite = !found || !identical;
    if (changed)
        *changed = needWrite;
    if (!needWrite)
        return true;
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(kept);
    f.close();
    return true;
}

bool dirIsReadable(const QString& mountpoint) {
    QDir dir(mountpoint);
    if (!dir.exists())
        return false;
    // entryList force une lecture du repertoire : un dossier local vide non monte
    // "reussit" aussi, d'ou le controle /proc/mounts + type cifs AVANT cet appel.
    (void)dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    return dir.isReadable();
}

bool addRootToConfig(const QString& mountpoint, bool* changed, QString* error) {
    QFile f(QString::fromLatin1(kConfigJson));
    if (!f.exists()) {
        *error = QStringLiteral("%1 est introuvable").arg(QLatin1String(kConfigJson));
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("lecture de %1 impossible").arg(QLatin1String(kConfigJson));
        return false;
    }
    const QByteArray original = f.readAll();
    f.close();

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(original, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("JSON invalide avant modification : %1").arg(pe.errorString());
        return false;
    }

    QJsonObject root = doc.object();
    QJsonArray modules = root.value(QStringLiteral("modules")).toArray();
    bool foundRoots = false;
    bool already = false;
    QJsonArray newModules;
    for (const QJsonValue& v : modules) {
        QJsonObject mod = v.toObject();
        if (!mod.contains(QStringLiteral("roots"))) {
            newModules.append(mod);
            continue;
        }
        foundRoots = true;
        QJsonArray roots = mod.value(QStringLiteral("roots")).toArray();
        for (const QJsonValue& r : roots) {
            if (r.toString() == mountpoint)
                already = true;
        }
        if (!already)
            roots.append(mountpoint);
        mod[QStringLiteral("roots")] = roots;
        newModules.append(mod);
    }
    if (!foundRoots) {
        *error = QStringLiteral("aucune section roots dans morfphoto.json");
        return false;
    }
    if (changed)
        *changed = !already;
    if (already)
        return true;

    root[QStringLiteral("modules")] = newModules;
    const QByteArray next = QJsonDocument(root).toJson(QJsonDocument::Indented);

    // Revalider AVANT d'ecraser : une generation cassee ne doit jamais remplacer
    // le fichier en place.
    QJsonParseError pe2{};
    if (QJsonDocument::fromJson(next, &pe2).isNull()
        || pe2.error != QJsonParseError::NoError) {
        *error = QStringLiteral("JSON genere invalide : %1").arg(pe2.errorString());
        return false;
    }

    const QString bak = QString::fromLatin1(kConfigJson) + QStringLiteral(".bak-smb");
    QFile::remove(bak);
    QFile::copy(QString::fromLatin1(kConfigJson), bak);

    const QString tmp = QString::fromLatin1(kConfigJson) + QStringLiteral(".tmp");
    QFile tf(tmp);
    if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("ecriture temporaire de morfphoto.json impossible");
        return false;
    }
    tf.write(next);
    tf.close();

    QFile check(tmp);
    if (!check.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("relecture du JSON temporaire impossible");
        return false;
    }
    QJsonParseError pe3{};
    const QJsonDocument verify = QJsonDocument::fromJson(check.readAll(), &pe3);
    check.close();
    if (pe3.error != QJsonParseError::NoError || !verify.isObject()) {
        QFile::remove(tmp);
        *error = QStringLiteral("JSON temporaire invalide, fichier original conserve");
        return false;
    }

    if (!QFile::remove(QString::fromLatin1(kConfigJson))
        || !QFile::rename(tmp, QString::fromLatin1(kConfigJson))) {
        QFile::rename(bak, QString::fromLatin1(kConfigJson));
        *error = QStringLiteral("remplacement de morfphoto.json impossible ; sauvegarde restauree");
        return false;
    }
    return true;
}

int doMount(const QString& host, const QString& share, const QString& slug) {
    Report r;
    r.slug        = slug;
    r.mountpoint  = morfphoto::mountpointForSlug(slug);
    r.credentials = morfphoto::credentialsPathForSlug(slug);

    addStep(r, QStringLiteral("source_identified"), true, host);
    addStep(r, QStringLiteral("hostname_normalized"), true, slug);

    QTextStream in(stdin);
    const QString user     = in.readLine();
    const QString password = in.readLine();
    if (user.isEmpty()) {
        addStep(r, QStringLiteral("smb_auth"), false, QStringLiteral("username manquant"));
        return fail(r, QStringLiteral("auth_failed"),
                    QStringLiteral("username manquant sur stdin"));
    }

    if (!QFile::exists(QStringLiteral("/sbin/mount.cifs"))
        && !QFile::exists(QStringLiteral("/usr/sbin/mount.cifs"))) {
        addStep(r, QStringLiteral("cifs_mounted"), false,
                QStringLiteral("mount.cifs introuvable"));
        return fail(r, QStringLiteral("cifs_utils_missing"),
                    humanFor(QStringLiteral("cifs_utils_missing"), {}));
    }

    if (!QDir().mkpath(r.mountpoint) || !QDir(r.mountpoint).exists()) {
        addStep(r, QStringLiteral("mountpoint_created"), false, r.mountpoint);
        return fail(r, QStringLiteral("mountpoint_create_failed"),
                    QStringLiteral("impossible de creer le point de montage %1")
                        .arg(r.mountpoint));
    }
    addStep(r, QStringLiteral("mountpoint_created"), true, r.mountpoint);

    const QByteArray wanted = credBytes(user, password);
    const bool credsUnchanged = sameCredentials(r.credentials, wanted);
    if (!writeCredentials(r.credentials, wanted)) {
        addStep(r, QStringLiteral("credentials_written"), false, r.credentials);
        return fail(r, QStringLiteral("credentials_failed"),
                    QStringLiteral("ecriture du fichier d'identifiants %1 impossible")
                        .arg(r.credentials));
    }
    addStep(r, QStringLiteral("credentials_written"), true, r.credentials);
    addStep(r, QStringLiteral("credentials_permissions"), true,
            QStringLiteral("0600 root:root"));

    const QString unc = QStringLiteral("//%1/%2").arg(host, share);
    MountInfo before = readMount(r.mountpoint);
        const bool sameSource = before.present
        && before.fstype == QLatin1String("cifs")
        && (before.source.compare(unc, Qt::CaseInsensitive) == 0
            || before.source.contains(share, Qt::CaseInsensitive));

    bool remounted = false;
    if (!(sameSource && credsUnchanged && dirIsReadable(r.mountpoint))) {
        if (before.present)
            run(QStringLiteral("umount"), {r.mountpoint}, 15000);

        // Test reel : PAS de nofail, sinon mount peut "reussir" sur un dossier vide.
        const QString options = QStringLiteral(
            "ro,uid=%1,gid=%2,credentials=%3,iocharset=utf8,vers=3.0")
            .arg(g_fileUid).arg(g_fileGid).arg(r.credentials);
        const CmdResult mounted = run(QStringLiteral("mount"), {
            QStringLiteral("-t"), QStringLiteral("cifs"), unc, r.mountpoint,
            QStringLiteral("-o"), options});
        if (!mounted.ok) {
            const QString code = classifyMountError(mounted.output);
            const bool authStepOk = code != QLatin1String("auth_failed")
                                    && code != QLatin1String("account_locked");
            addStep(r, QStringLiteral("smb_auth"), authStepOk, mounted.output);
            addStep(r, QStringLiteral("cifs_mounted"), false, mounted.output);
            return fail(r, code, humanFor(code, mounted.output));
        }
        remounted = true;
    }
    addStep(r, QStringLiteral("smb_auth"), true);

    const MountInfo after = readMount(r.mountpoint);
    if (!after.present || after.fstype != QLatin1String("cifs")) {
        addStep(r, QStringLiteral("cifs_mounted"), false,
                QStringLiteral("le point de montage existe mais aucun filesystem CIFS n'y est monte"));
        return fail(r, QStringLiteral("cifs_mount_failed"),
                    QStringLiteral("Le point de montage existe mais aucun filesystem CIFS n'y est monte."));
    }
    addStep(r, QStringLiteral("cifs_mounted"), true, after.source);

    if (!dirIsReadable(r.mountpoint)) {
        addStep(r, QStringLiteral("share_readable"), false, r.mountpoint);
        if (remounted)
            run(QStringLiteral("umount"), {r.mountpoint}, 15000);
        return fail(r, QStringLiteral("share_unreadable"),
                    QStringLiteral("le partage est monte mais illisible (%1)").arg(r.mountpoint));
    }
    addStep(r, QStringLiteral("share_readable"), true);

    bool fstabChanged = false;
    if (!ensureFstab(host, share, r.mountpoint, r.credentials, &fstabChanged)) {
        addStep(r, QStringLiteral("fstab_configured"), false,
                QString::fromLatin1(kFstab));
        return fail(r, QStringLiteral("fstab_failed"),
                    QStringLiteral("modification de /etc/fstab impossible (cette source uniquement)"));
    }
    if (fstabChanged)
        run(QStringLiteral("systemctl"), {QStringLiteral("daemon-reload")}, 20000);
    addStep(r, QStringLiteral("fstab_configured"), true);

    bool jsonChanged = false;
    QString jsonErr;
    if (!addRootToConfig(r.mountpoint, &jsonChanged, &jsonErr)) {
        addStep(r, QStringLiteral("root_added"), false, jsonErr);
        return fail(r, jsonErr.contains(QLatin1String("invalide"))
                        ? QStringLiteral("json_invalid")
                        : QStringLiteral("json_failed"),
                    jsonErr);
    }
    addStep(r, QStringLiteral("root_added"), true, r.mountpoint);
    addStep(r, QStringLiteral("json_valid"), true,
            QString::fromLatin1(kConfigJson));

    r.ok = true;
    r.restartNeeded = jsonChanged;
    r.alreadyConfigured = !remounted && !fstabChanged && !jsonChanged && credsUnchanged;
    r.code = r.alreadyConfigured ? QStringLiteral("already_configured")
                                 : QStringLiteral("ok");
    r.message = r.alreadyConfigured
        ? QStringLiteral("configuration deja operationnelle")
        : QStringLiteral("source montee et racine declaree");
    return emitReport(r, 0);
}

int doUnmount(const QString& mountpoint) {
    Report r;
    r.mountpoint = mountpoint;
    run(QStringLiteral("umount"), {mountpoint}, 15000);

    QFile f(QString::fromLatin1(kFstab));
    if (f.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = f.readAll().split('\n');
        f.close();
        const QByteArray needle = QStringLiteral(" %1 ").arg(mountpoint).toUtf8();
        QByteArray kept;
        for (const QByteArray& l : lines) {
            if (l.contains(needle))
                continue;
            kept += l;
            kept += '\n';
        }
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(kept);
            f.close();
        }
    }
    r.ok = true;
    r.code = QStringLiteral("ok");
    return emitReport(r, 0);
}

int doRestart() {
    Report r;
    const CmdResult restarted = run(QStringLiteral("systemctl"),
                                    {QStringLiteral("restart"), QStringLiteral("morfphoto")},
                                    60000);
    if (!restarted.ok) {
        addStep(r, QStringLiteral("service_restarted"), false, restarted.output);
        return fail(r, QStringLiteral("restart_failed"),
                    QStringLiteral("redemarrage de morfPhoto impossible : %1")
                        .arg(restarted.output));
    }
    const CmdResult active = run(QStringLiteral("systemctl"),
                                 {QStringLiteral("is-active"), QStringLiteral("morfphoto")},
                                 15000);
    const QString state = active.output.trimmed();
    if (state != QLatin1String("active")) {
        addStep(r, QStringLiteral("service_restarted"), true);
        addStep(r, QStringLiteral("service_active"), false, state);
        return fail(r, QStringLiteral("restart_failed"),
                    QStringLiteral("morfPhoto a redemarre mais n'est pas actif (%1)").arg(state));
    }
    addStep(r, QStringLiteral("service_restarted"), true);
    addStep(r, QStringLiteral("service_active"), true, state);
    r.ok = true;
    r.code = QStringLiteral("ok");
    return emitReport(r, 0);
}

bool takeRealRoot() {
    g_fileUid = ::getuid();
    g_fileGid = ::getgid();
    return ::setgid(0) == 0 && ::setuid(0) == 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setSetuidAllowed(true);
    QCoreApplication app(argc, argv);
#ifndef Q_OS_UNIX
    return refuse(QStringLiteral("le helper privilegie ne sert que sous Linux"));
#else
    if (geteuid() != 0)
        return refuse(QStringLiteral("execution root requise"));
    // mount.cifs teste getuid() (UID reel), pas geteuid(). Un binaire setuid
    // a euid=0 mais uid=compte du service : sans setuid(0), le premier montage
    // echoue ("no match ... /etc/fstab") alors que l'auth SMB a deja reussi.
    if (!takeRealRoot())
        return refuse(QStringLiteral(
            "impossible de prendre l'identite root reelle (setuid/setgid)"));
    const QStringList args = app.arguments();

    static const QRegularExpression kHost(QStringLiteral("^[A-Za-z0-9._-]+$"));
    static const QRegularExpression kShare(QStringLiteral("^[A-Za-z0-9._$-]+$"));

    if (args.size() == 5 && args.at(1) == QStringLiteral("mount")) {
        const QString host  = args.at(2);
        const QString share = args.at(3);
        const QString slug  = args.at(4);
        if (!kHost.match(host).hasMatch())
            return refuse(QStringLiteral("hote invalide"));
        if (!kShare.match(share).hasMatch())
            return refuse(QStringLiteral("partage invalide"));
        if (!morfphoto::isValidSourceSlug(slug))
            return refuse(QStringLiteral("identifiant de machine invalide"));
        return doMount(host, share, slug);
    }

    if (args.size() == 3 && args.at(1) == QStringLiteral("unmount")) {
        const QString mountpoint = args.at(2);
        if (!morfphoto::isManagedMountpoint(mountpoint))
            return refuse(QStringLiteral("point de montage hors /mnt/photos_*"));
        return doUnmount(mountpoint);
    }

    if (args.size() == 2 && args.at(1) == QStringLiteral("restart-service"))
        return doRestart();

    return refuse(QStringLiteral(
        "usage : mount <host> <share> <slug> | unmount <mountpoint> | restart-service"));
#endif
}
