/*
 * morfPhoto — demon de service
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Charge une configuration JSON, demarre les modules, ouvre l'API HTTP et
 * annonce sa presence sur le LAN (morfBeacon). Squelette reutilisable : le
 * comportement propre au service vit dans les modules (voir IModule).
 */

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTextStream>

#include <morfphoto/Service.h>
#include <morfphoto/ModuleFactory.h>
#include <morfphoto/Paths.h>
#include <morfphoto/Version.h>

using morfphoto::ServiceConfig;

namespace {

// Nom court du service (dossiers /etc, /var/lib, unite systemd). >>> A ADAPTER. <<<
const QString kServiceName = QStringLiteral("morfphoto");

QTextStream& out() { static QTextStream s(stdout); return s; }
QTextStream& err() { static QTextStream s(stderr); return s; }

// Fusion recursive : les cles de `overlay` ecrasent celles de `base`. Deux objets
// se fusionnent en profondeur ; toute autre valeur (tableau, scalaire) remplace.
// Sert a superposer les surcharges editables (etat) sur la config admin (/etc).
void mergeInto(QJsonObject& base, const QJsonObject& overlay) {
    for (auto it = overlay.constBegin(); it != overlay.constEnd(); ++it) {
        const QJsonValue bv = base.value(it.key());
        if (bv.isObject() && it.value().isObject()) {
            QJsonObject merged = bv.toObject();
            mergeInto(merged, it.value().toObject());
            base[it.key()] = merged;
        } else {
            base[it.key()] = it.value();
        }
    }
}

QString findDefaultConfig() {
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir::current().filePath("morfphoto.json"),
        QDir(exeDir).filePath("morfphoto.json"),
        QDir(exeDir).filePath("config/morfphoto.json"),
#ifdef Q_OS_UNIX
        QStringLiteral("/etc/morfsystem/morfphoto/morfphoto.json"),
        QStringLiteral("/etc/morfphoto/morfphoto.json"),   // ancien emplacement (avant le regroupement sous /etc/morfsystem)
#endif
    };
    for (const QString& c : candidates)
        if (QFileInfo::exists(c))
            return c;
    return {};
}

// Config de repli : un module 'example', pour tester le squelette sans config.
ServiceConfig fallbackConfig() {
    ServiceConfig c;
    morfphoto::ModuleDef ex;
    ex.type = QStringLiteral("example");
    ex.id   = QStringLiteral("example-1");
    c.modules.push_back(ex);
    return c;
}

// Lit un fichier JSON en objet. `required` distingue une absence tolérée (les
// surcharges, optionnelles) d'une erreur dure (la config admin demandée).
bool readJsonObject(const QString& path, bool required, QJsonObject* out, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (!required) { *out = QJsonObject(); return true; }
        *error = QStringLiteral("impossible d'ouvrir %1 : %2").arg(path, f.errorString());
        return false;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("JSON invalide dans %1 : %2").arg(path, pe.errorString());
        return false;
    }
    *out = doc.object();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("morfPhoto"));
    QCoreApplication::setApplicationVersion(morfphoto::version());

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("morfPhoto — indexation permanente d'une phototheque locale "
                       "(API HTTP + annonce LAN morfBeacon)."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption configOpt({"c", "config"},
        QStringLiteral("Fichier de configuration JSON."), QStringLiteral("chemin"));
    QCommandLineOption listOpt("list-types",
        QStringLiteral("Liste les types de modules disponibles puis quitte."));
    parser.addOption(configOpt);
    parser.addOption(listOpt);
    parser.process(app);

    if (parser.isSet(listOpt)) {
        out() << "Types de modules disponibles : "
              << morfphoto::ModuleFactory::knownTypes().join(", ") << '\n';
        return 0;
    }

    ServiceConfig config;
    QString configPath = parser.value(configOpt);
    if (configPath.isEmpty())
        configPath = findDefaultConfig();

    if (configPath.isEmpty()) {
        err() << "Aucune configuration trouvee : demarrage avec un module 'example'. "
                 "Fournir --config pour votre configuration.\n";
        config = fallbackConfig();
    } else {
        QString error;
        QJsonObject base;
        if (!readJsonObject(configPath, /*required=*/true, &base, &error)) {
            err() << "Erreur de configuration : " << error << '\n';
            return 2;
        }
        out() << "Configuration chargee : " << configPath << '\n';

        // SUPERPOSITION (doctrine FILESYSTEM.md) : la config admin de /etc reste la
        // reference en lecture seule ; les preferences modifiees a l'execution
        // (UI web) vivent dans l'etat et sont appliquees PAR-DESSUS. Absence de
        // surcharges = cas normal, pas une erreur.
        const QString overridePath = morfphoto::Paths::overrideConfigFile(kServiceName);
        QJsonObject overrides;
        if (!readJsonObject(overridePath, /*required=*/false, &overrides, &error)) {
            err() << "Surcharges ignorees (" << overridePath << ") : " << error << '\n';
        } else if (!overrides.isEmpty()) {
            mergeInto(base, overrides);
            out() << "Surcharges appliquees : " << overridePath << '\n';
        }
        config = ServiceConfig::fromJson(base);
    }

    morfphoto::Service service(config);
    for (const QString& w : service.warnings())
        err() << "Avertissement : " << w << '\n';

    if (!service.start()) {
        err() << "Le serveur HTTP n'a pas pu ecouter sur le port "
              << config.httpPort << " (deja utilise ?).\n";
        return 3;
    }

    out() << "morfPhoto v" << morfphoto::version() << " demarre : "
          << service.moduleCount() << " module(s), API http://"
          << config.bindAddress << ':' << service.httpPort()
          << "/  (GET /status /healthz ; GET /api/v1/photos ; POST /api/v1/index)\n";
    out().flush();

    return app.exec();
}
