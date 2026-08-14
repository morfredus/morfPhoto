/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include "morfphoto/PhotoTypes.h"

// -----------------------------------------------------------------------------
// Parcours du système de fichiers et garde-fou de périmètre.
//
// Le triplet (path, size, mtime) décide seul si un fichier doit être retraité
// (pas de hash tant qu'un besoin réel ne le justifie pas). Le scan ne lit JAMAIS
// l'EXIF : il reste rapide (un stat par fichier), l'extraction vient après et
// seulement pour les fichiers neufs ou modifiés.
// -----------------------------------------------------------------------------
namespace morfphoto {

// Classe une extension en type de fichier (RAW regroupés, reste explicite).
// Classification DÉTERMINISTE (ne dépend que de l'extension), pas une analyse.
QString fileTypeFor(const QString& extension);

// Vrai si `candidate` est situé sous une des racines autorisées. Empêche
// PhotoHub (ou une requête forgée) de faire indexer un chemin arbitraire.
// Vérifie l'ascendance sur chemins normalisés : un `..` ne peut pas s'échapper.
bool isWithinRoots(const QString& candidate, const QStringList& roots);

// Génère les FileInfo des fichiers image d'un dossier (récursif si demandé).
// Un fichier illisible au stat est simplement ignoré.
QVector<FileInfo> scanFolder(const QString& folder, bool recursive);

} // namespace morfphoto
