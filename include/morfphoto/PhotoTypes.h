/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QVariant>
#include <QJsonObject>
#include <QHash>

// -----------------------------------------------------------------------------
// Types partagés du domaine photo : ce que le scan sait d'un fichier (FileInfo),
// ce que l'extraction EXIF en tire (ExifData), et les projections de la base
// utiles au verdict d'indexation (KnownFile, FolderRow, RunCounts).
//
// Souveraineté : ExifData porte les valeurs BRUTES telles qu'ExifTool les rend.
// Les champs numériques sont des QVariant : un QVariant vide devient NULL en base,
// ce qui distingue proprement « valeur absente » de « valeur nulle ».
// -----------------------------------------------------------------------------
namespace morfphoto {

// Ce que le parcours du disque connaît d'un fichier, sans avoir lu son EXIF.
struct FileInfo {
    QString path;          // chemin complet
    QString directory;     // dossier parent
    QString filename;
    QString extension;     // en minuscules, avec le point (« .arw »)
    QString fileType;      // raw|jpeg|tiff|heic|png|webp|other
    qint64  size  = 0;
    qint64  mtime = 0;     // epoch entier, tel que le système de fichiers
};

// Métadonnées extraites d'un fichier, mappées sur les colonnes de `files`.
// Tous les champs sont optionnels (QVariant vide => NULL) : un fichier peut
// n'avoir aucune donnée EXIF.
struct ExifData {
    QVariant takenAt;              // date de prise de vue (ISO, non inventée)
    QVariant make;                 // constructeur
    QVariant cameraModel;          // boîtier
    QVariant lens;                 // objectif
    QVariant focalLength;          // focale réelle brute (mm)
    QVariant focalLength35mm;      // équivalent 24x36 si dispo
    QVariant aperture;             // ouverture (f-number)
    QVariant shutterSpeed;         // vitesse lisible (« 1/250 »)
    QVariant shutterSpeedS;        // vitesse numérique en secondes
    QVariant iso;
    QVariant exposureCompensation;
    QVariant rating;               // notation
    QVariant latitude;
    QVariant longitude;
    QJsonObject raw;               // tags renvoyés, pour extension future
    bool ok = false;               // extraction réussie ?
};

// Projection minimale d'un fichier connu, pour trancher le verdict et repérer
// les disparus sans charger tout l'EXIF.
struct KnownFile {
    int     id    = 0;
    qint64  size  = 0;
    qint64  mtime = 0;
    QString state;                 // present|missing|deleted
};

// Une sélection de dossier à surveiller (projection de la table `folders`).
struct FolderRow {
    int     id = 0;
    QString path;
    QString rootPath;
    bool    recursive = true;
    bool    enabled   = true;
    // Support amovible (CD/DVD, disque d'archive) : l'absence du dossier est NORMALE
    // et ne doit jamais valoir suppression. Une passe ne marque JAMAIS disparu un
    // fichier d'un tel dossier (voir Indexer::reconcileFolder).
    bool    removable = false;
};

// Compteurs d'une passe de réconciliation, tenus à jour puis écrits dans index_runs.
struct RunCounts {
    int seen        = 0;
    int created     = 0;   // « new » (mot réservé en C++)
    int updated     = 0;
    int missing     = 0;
    int errors      = 0;
    int unavailable = 0;   // sélections sautées car leur racine était indisponible
};

using KnownFiles = QHash<QString, KnownFile>;   // chemin complet -> état connu

} // namespace morfphoto
