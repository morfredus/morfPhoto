/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QVariantMap>

#include "morfphoto/PhotoTypes.h"

// -----------------------------------------------------------------------------
// PhotoRepository : accès SQLite du domaine photo. Toutes les requêtes SQL vivent
// ici, nulle part ailleurs. Le reste du code manipule des structs et du JSON,
// jamais des lignes brutes : le jour où le schéma change, un seul fichier bouge.
//
// Concurrence : QSqlDatabase n'est pas partageable entre threads. Chaque dépôt
// porte donc sa PROPRE connexion nommée, et un thread qui indexe crée son propre
// dépôt (sa propre connexion vers le même fichier). SQLite en WAL laisse une
// connexion écrire pendant que d'autres lisent. Un dépôt s'utilise dans le thread
// qui l'a ouvert.
// -----------------------------------------------------------------------------
namespace morfphoto {

class PhotoRepository {
public:
    explicit PhotoRepository(QString connectionName);
    ~PhotoRepository();

    // Ouvre la connexion, pose les PRAGMA et applique les migrations en attente.
    bool open(const QString& dbPath);
    void close();
    bool isOpen() const;
    QString openError() const { return m_openError; }  // detail si open() a echoue

    // --- Dossiers (sélections) ---
    int  addFolder(const QString& path, const QString& rootPath, const QVariant& label,
                   bool recursive, bool removable, const QVariant& volumeLabel,
                   const QString& addedAt);
    QVector<FolderRow> activeFolders();
    QVector<FolderRow> allFolders();
    void setFolderAutoDisabled(int folderId, bool autoDisabled);
    void setFolderScanned(int folderId, const QString& when);
    bool getFolder(int folderId, FolderRow* out = nullptr);
    QJsonArray listFoldersDetail();
    void setFolderEnabled(int folderId, bool enabled);
    // Réglages du support amovible d'une sélection (drapeau + libellé de volume).
    void setFolderMedia(int folderId, bool removable, const QVariant& volumeLabel);
    // Sortir (ou réintégrer) une sélection des analyses sans effacer ses données.
    void setFolderAnalyticsExcluded(int folderId, bool excluded);
    void softDeleteFolder(int folderId, const QString& now);
    void restoreFolder(int folderId);
    // Id d'un dossier par son chemin (UNIQUE), -1 si absent. *deleted indique s'il
    // est actuellement retiré (deleted_at non NULL). Sert à gérer proprement le
    // ré-ajout d'un chemin déjà connu.
    int  folderIdByPath(const QString& path, bool* deleted = nullptr);

    // --- Fichiers : verdict et écritures d'indexation ---
    KnownFiles existingFilesInFolder(int folderId);
    void insertFile(int folderId, const FileInfo& info, const ExifData& exif,
                    bool exifOk, const QString& now);
    void updateFile(int fileId, const FileInfo& info, const ExifData& exif,
                    bool exifOk, const QString& now);
    void touchSeen(int fileId, const QString& now);
    void markMissing(const QVector<int>& fileIds, const QString& now);

    // --- Passes d'indexation ---
    int  startRun(const QString& mode, const QString& trigger, const QString& startedAt);
    void finishRun(int runId, const QString& state, const RunCounts& counts,
                   const QString& finishedAt);
    void logError(int runId, const QVariant& path, const QString& stage,
                  const QString& message, const QString& when);

    // --- Lectures pour l'API ---
    QJsonObject summary();
    QJsonObject listPhotos(const QVariantMap& filters, int page, int pageSize);
    QJsonObject getPhoto(int fileId, bool* found);
    QJsonArray  distinctCameras();
    QJsonArray  distinctLenses();
    QJsonArray  distinctFocals();
    QJsonArray  years();
    QJsonObject latestRun(bool* found = nullptr);

    // Export compact des colonnes analytiques de toutes les photos PRESENTES, pour
    // la couche d'analyse (morfAnalytics). Format colonnaire + dictionnaires pour
    // les chaines repetees (boitier, objectif, type) : une seule reponse legere meme
    // sur 20 000+ photos. Valeurs BRUTES, aucun regroupement ; les NULL sont
    // preserves (distinguer valeur absente de valeur nulle, cf qualite des metadonnees).
    QJsonObject photoDataset();

    // --- Purge (suppression DÉFINITIVE, à distinguer du retrait doux) ---
    // Efface RÉELLEMENT des lignes de la base (aucune restauration possible). Chaque
    // méthode renvoie le nombre de fichiers supprimés. Destructif et volontaire :
    // l'appelant (API + PhotoHub) confirme explicitement avant d'y recourir.
    int purgeFolder(int folderId);            // fichiers du dossier + la sélection
    int purgeByYear(int year);                // fichiers d'une année de prise de vue
    int purgeByCamera(const QString& camera); // fichiers d'un boîtier
    int purgeAll();                           // remise à zéro : fichiers, sélections, historique

private:
    bool applyMigrations();
    QJsonArray distinct(const QString& column, const QString& label);
    QSqlDatabase db() const;

    QString m_connectionName;
    QString m_openError;
};

} // namespace morfphoto
