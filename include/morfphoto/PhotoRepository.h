/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QVariantMap>

#include "morfphoto/PhotoTypes.h"
#include "morfphoto/LibraryMetadata.h"

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

    // --- Contexte photographique par répertoire (morfphoto-context/2) ---
    // Projection reconstructible des `.morfphoto.json`. Clé = le répertoire réel
    // (files.directory). L'ABSENCE de fichier = absence de ligne (état « non qualifié »).
    void upsertContext(const FolderContext& ctx);        // insert ou remplace la ligne du répertoire
    void deleteContextRow(const QString& directory);     // retire la ligne (fichier disparu)
    // mtime source connu par répertoire, pour rendre la découverte IDEMPOTENTE (ne
    // relire un `.morfphoto.json` que s'il a changé). Chargé une fois par passe.
    QHash<QString, qint64> knownContextMtimes();
    // Un répertoire contient-il au moins une photo PRÉSENTE ? Garde-fou de l'écriture
    // de contexte (ne qualifier qu'un dossier réellement indexé).
    bool directoryHasPhotos(const QString& directory);
    // Liste les répertoires contenant des photos présentes + leur contexte (jointure).
    // `statusFilter` vide = tous ; sinon "qualified" | "unqualified" | "invalid".
    // Chaque entrée : { directory, label, photo_count, date, status, context, subject,
    // motif, description, updated, warnings }. Moteur de l'écran de qualification PhotoHub.
    QJsonArray listContexts(const QString& statusFilter);
    // Contexte d'UN répertoire, ou { directory, status:"unqualified" } s'il n'y en a pas.
    QJsonObject getContext(const QString& directory);

    // --- Passes d'indexation ---
    int  startRun(const QString& mode, const QString& trigger, const QString& startedAt);
    void finishRun(int runId, const QString& state, const RunCounts& counts,
                   const QString& finishedAt);
    void logError(int runId, const QVariant& path, const QString& stage,
                  const QString& message, const QString& when);

    // --- Lectures pour l'API ---
    QJsonObject summary();
    // Compteurs LEGERS pour /status (3 COUNT simples, sans DISTINCT ni sous-requete).
    QJsonObject statusCounts();
    QJsonObject listPhotos(const QVariantMap& filters, int page, int pageSize);
    // Echantillon REPRESENTATIF d'un dossier pour l'apercu : `count` chemins repartis
    // sur toute la journee (pas les N premiers), et dedupliques par radical (nom sans
    // extension) pour ne pas prendre le RAW ET le JPEG d'une meme prise. Voir la note
    // dans PhotoRepository.cpp.
    QStringList directorySample(const QString& directory, int count);
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
    // `withLibrary` : ajoute les colonnes de la bibliotheque (rating, edited, keywords...),
    // une ligne par photo (contrat, §9). Sans lui, la reponse est celle d'avant.
    // `withGps` : ajoute latitude, longitude et leur provenance (le fichier d'abord, a defaut un
    // sidecar), pour une carte. Independant de `withLibrary` : l'EXIF seul suffit.
    QJsonObject photoDataset(bool withLibrary = false, bool withGps = false);

    // --- Bibliotheque (contrat morfphoto-library/1) -------------------------------
    // Etat de lecture par fichier d'un dossier : identifiant et empreinte de la derniere
    // lecture (vide si le fichier n'a jamais ete lu par la bibliotheque).
    struct LibraryFileState { int id = 0; QString fingerprint; };
    QHash<QString, LibraryFileState> libraryState(int folderId);
    // Tags ExifTool bruts d'un fichier (raw_exif), pour lire l'XMP embarque sans relancer
    // ExifTool.
    QJsonObject rawExif(int fileId);
    // Remplace tout ce que la bibliotheque sait de ce fichier.
    void writeLibrary(int fileId, const QString& fingerprint, const LibraryResult& result);
    // Retire tout (bibliotheque eteinte) ; renvoie le nombre de fichiers concernes.
    int  clearLibrary();
    // Transaction englobante : une ecriture par fichier sans elle serait lente (un fsync
    // chacune). Un dossier = une transaction.
    bool beginBatch();
    void commitBatch();
    // Lectures de l'API /library.
    QJsonObject libraryPhoto(int fileId, bool* found);
    QJsonArray  libraryPhotos(const QVector<int>& ids);
    // Avec `cursor`, la page suit la position du curseur (cle de tri : date de prise de vue,
    // les sans date a la fin, puis identifiant) au lieu d'un decalage : pas de photo sautee ou
    // doublee si le catalogue change entre deux pages. `next` rend le curseur suivant, ou null.
    QJsonObject libraryList(const LibraryFilter& filter, int page, int pageSize, bool skipTotal,
                            const QString& cursor = {}, bool* cursorValid = nullptr);
    QJsonObject libraryFacets(const LibraryFilter& filter);
    QJsonObject librarySubfolders(const QString& folder);
    QJsonObject librarySidecars();
    QJsonObject libraryStatus();     // bloc `library` de /index/status (comptes par source)

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
