/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/ContextFile.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace morfphoto {
namespace ContextFile {

// Version de format que morfPhoto ÉCRIT et connaît pleinement. Un `schema` supérieur
// est lu au mieux (signalé future_schema), un `schema` absent rend le fichier invalide.
namespace {
constexpr int kCurrentSchema = 2;

// Lit une chaîne optionnelle : QVariant vide si absente ou vide (=> NULL en base).
QVariant optString(const QJsonObject& o, const QString& key) {
    if (!o.contains(key))
        return {};
    const QString s = o.value(key).toString().trimmed();
    return s.isEmpty() ? QVariant() : QVariant(s);
}
} // namespace

QString fileName() { return QStringLiteral(".morfphoto.json"); }

QStringList knownContexts() {
    // Ordre du contrat morfphoto-context/2 (7 valeurs).
    return {QStringLiteral("LIBRE"), QStringLiteral("DECOUVERTE"), QStringLiteral("EVENEMENT"),
            QStringLiteral("SPECTACLE"), QStringLiteral("MISSION"), QStringLiteral("SPECIALISEE"),
            QStringLiteral("INCONNU")};
}

QStringList knownSubjects() {
    // Ordre du contrat morfphoto-context/2 (6 valeurs).
    return {QStringLiteral("GENERAL"), QStringLiteral("PERSONNES"), QStringLiteral("ANIMAUX"),
            QStringLiteral("PAYSAGE"), QStringLiteral("ARCHITECTURE"), QStringLiteral("DETAIL")};
}

bool isKnownContext(const QString& value) { return knownContexts().contains(value); }
bool isKnownSubject(const QString& value) { return knownSubjects().contains(value); }

FolderContext parse(const QByteArray& bytes) {
    FolderContext fc;

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        fc.status = QStringLiteral("invalid");
        fc.error  = perr.error != QJsonParseError::NoError
            ? QStringLiteral("json illisible: %1").arg(perr.errorString())
            : QStringLiteral("json: objet attendu a la racine");
        return fc;
    }
    const QJsonObject o = doc.object();

    // schema : obligatoire. Absent ou non entier => invalide (on ne devine pas la version).
    if (!o.contains(QStringLiteral("schema")) || !o.value(QStringLiteral("schema")).isDouble()) {
        fc.status = QStringLiteral("invalid");
        fc.error  = QStringLiteral("champ `schema` manquant ou non entier");
        return fc;
    }
    fc.schema = o.value(QStringLiteral("schema")).toInt();
    if (fc.schema > kCurrentSchema)
        fc.warnings << QStringLiteral("future_schema");   // lu au mieux, jamais bloquant

    // context : obligatoire pour un fichier présent. Valeur normalisée (trim + majuscule) ;
    // si hors vocabulaire, CONSERVÉE telle quelle et signalée (jamais convertie).
    const QString ctxRaw = o.value(QStringLiteral("context")).toString().trimmed();
    if (ctxRaw.isEmpty()) {
        fc.status = QStringLiteral("invalid");
        fc.error  = QStringLiteral("champ `context` manquant (obligatoire)");
        return fc;
    }
    const QString ctx = ctxRaw.toUpper();
    fc.context = ctx;
    if (!isKnownContext(ctx))
        fc.warnings << QStringLiteral("context_unknown");

    // subject : obligatoire dès schema >= 2 (deux dimensions fondamentales du contexte).
    const QString subjRaw = o.value(QStringLiteral("subject")).toString().trimmed();
    if (subjRaw.isEmpty()) {
        if (fc.schema >= 2) {
            fc.status = QStringLiteral("invalid");
            fc.error  = QStringLiteral("champ `subject` manquant (obligatoire en schema 2)");
            return fc;
        }
        // Tolérance d'un éventuel format antérieur : subject laissé nul, signalé.
        fc.warnings << QStringLiteral("subject_missing");
    } else {
        const QString subj = subjRaw.toUpper();
        fc.subject = subj;
        if (!isKnownSubject(subj))
            fc.warnings << QStringLiteral("subject_unknown");
    }

    fc.motif       = optString(o, QStringLiteral("motif"));
    fc.description = optString(o, QStringLiteral("description"));
    fc.created     = optString(o, QStringLiteral("created"));
    fc.updated     = optString(o, QStringLiteral("updated"));

    fc.status = QStringLiteral("ok");
    return fc;
}

QByteArray serialize(const QString& context, const QString& subject,
                     const QVariant& motif, const QVariant& description,
                     const QVariant& created, const QVariant& updated) {
    QJsonObject o;
    o[QStringLiteral("schema")]  = kCurrentSchema;
    o[QStringLiteral("context")] = context;
    o[QStringLiteral("subject")] = subject;
    if (!motif.isNull() && !motif.toString().isEmpty())
        o[QStringLiteral("motif")] = motif.toString();
    if (!description.isNull() && !description.toString().isEmpty())
        o[QStringLiteral("description")] = description.toString();
    if (!created.isNull() && !created.toString().isEmpty())
        o[QStringLiteral("created")] = created.toString();
    if (!updated.isNull() && !updated.toString().isEmpty())
        o[QStringLiteral("updated")] = updated.toString();
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

bool write(const QString& directory, const QByteArray& bytes, QString* error) {
    const QString path = QDir(directory).filePath(fileName());
    QSaveFile f(path);                 // écriture atomique : temporaire + rename
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("ouverture impossible: %1").arg(f.errorString());
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        if (error) *error = QStringLiteral("ecriture partielle: %1").arg(f.errorString());
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        if (error) *error = QStringLiteral("validation impossible: %1").arg(f.errorString());
        return false;
    }
    return true;
}

bool remove(const QString& directory, QString* error) {
    const QString path = QDir(directory).filePath(fileName());
    QFileInfo fi(path);
    if (!fi.exists())
        return true;                   // déjà absent : idempotent
    if (!QFile::remove(path)) {
        if (error) *error = QStringLiteral("suppression impossible: %1").arg(path);
        return false;
    }
    return true;
}

} // namespace ContextFile
} // namespace morfphoto
