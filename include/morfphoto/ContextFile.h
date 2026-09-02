/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

#include "morfphoto/PhotoTypes.h"

// -----------------------------------------------------------------------------
// ContextFile : lecture, validation et écriture du fichier `.morfphoto.json`
// (contrat morfphoto-context/2). C'est la couche DISQUE du contexte, distincte de
// la projection SQLite (PhotoRepository).
//
// Souveraineté : le fichier sur disque est la source de vérité ; morfPhoto en est
// l'UNIQUE écrivain. Le parseur est TOLÉRANT : il ne lève jamais, un JSON absent ou
// invalide ne bloque jamais l'indexation. Une valeur `context`/`subject` hors
// vocabulaire est conservée telle quelle et signalée (warnings), jamais convertie.
//
// Deux dimensions INDÉPENDANTES, aucune matrice restrictive : n'importe quel
// `subject` est valide avec n'importe quel `context`.
// -----------------------------------------------------------------------------
namespace morfphoto {
namespace ContextFile {

// Nom du fichier posé dans chaque répertoire de photos.
QString fileName();

// Vocabulaires GELÉS du contrat V2 (majuscules).
QStringList knownContexts();   // LIBRE, DECOUVERTE, EVENEMENT, SPECTACLE, MISSION, SPECIALISEE, INCONNU
QStringList knownSubjects();   // GENERAL, PERSONNES, ANIMAUX, PAYSAGE, ARCHITECTURE, DETAIL
bool isKnownContext(const QString& value);
bool isKnownSubject(const QString& value);

// Analyse des octets bruts d'un `.morfphoto.json` en FolderContext. Ne lève JAMAIS.
// `status` vaut "ok" ou "invalid" ; `warnings` porte les signalements non bloquants ;
// `error` décrit la cause si "invalid". `directory` et `sourceMtime` restent à
// l'appelant (il connaît le chemin réel et le mtime constaté).
FolderContext parse(const QByteArray& bytes);

// Sérialise un contexte QUALIFIÉ (schema 2) en JSON indenté. `context` et `subject`
// sont obligatoires (contrat V2) ; motif/description/created/updated sont ajoutés
// seulement s'ils sont fournis (QVariant non vide).
QByteArray serialize(const QString& context, const QString& subject,
                     const QVariant& motif, const QVariant& description,
                     const QVariant& created, const QVariant& updated);

// Écrit ATOMIQUEMENT `bytes` dans `<directory>/.morfphoto.json` (QSaveFile : écriture
// dans un temporaire puis rename, multi-plateforme). false + *error si l'écriture
// échoue (répertoire absent, droits...).
bool write(const QString& directory, const QByteArray& bytes, QString* error);

// Retire `<directory>/.morfphoto.json`. true si retiré ou déjà absent ; false +
// *error si le retrait échoue alors que le fichier existe.
bool remove(const QString& directory, QString* error);

} // namespace ContextFile
} // namespace morfphoto
