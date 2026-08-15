/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include "morfphoto/IModule.h"
#include "morfphoto/Indexer.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QMutex>
#include <QFuture>

class QTimer;

// -----------------------------------------------------------------------------
// PhotoModule : LE module métier de morfPhoto. Il assemble tout le domaine et
// maintient en permanence une base qui représente l'état réel des dossiers
// surveillés. C'est un service permanent : tant qu'il tourne, il réconcilie.
//
// Threads :
//   - les LECTURES (API HTTP) et la gestion des dossiers passent par une
//     connexion SQLite propre au thread principal (m_readRepo) ;
//   - chaque PASSE d'indexation s'exécute sur un thread du pool (QtConcurrent)
//     avec SA PROPRE connexion et SON PROPRE process ExifTool. SQLite en WAL
//     laisse une passe écrire pendant que l'API lit.
//
// Garde « une seule passe à la fois » : un drapeau protégé par mutex. Une
// nouvelle demande pendant une passe est refusée (« already_running »), jamais
// mise en file. Le watcher périodique passe alors simplement son tour.
// -----------------------------------------------------------------------------
namespace morfphoto {

class PhotoRepository;

class PhotoModule : public IModule {
    Q_OBJECT
public:
    PhotoModule(const QString& id, const QJsonObject& params, QObject* parent = nullptr);
    ~PhotoModule() override;

    bool start() override;
    void stop() override;
    QJsonObject statusJson() const override;

    // --- Lectures (API HTTP, thread principal) ---
    QJsonObject summary() const;
    QJsonObject listPhotos(const QVariantMap& filters, int page, int pageSize) const;
    QJsonObject getPhoto(int fileId, bool* found) const;
    QJsonArray  cameras() const;
    QJsonArray  lenses() const;
    QJsonArray  focals() const;
    QJsonArray  years() const;
    QJsonObject indexStatus() const;
    // Export compact des photos presentes pour la couche d'analyse (morfAnalytics).
    QJsonObject photoDataset() const;

    // --- Dossiers (thread principal) ---
    QJsonArray  listFolders() const;
    // Racines AUTORISEES (perimetre defini par la config) : PhotoHub les affiche
    // pour guider l'utilisateur, qui ne peut declarer qu'a l'interieur.
    QJsonArray  allowedRoots() const;
    // Ajoute une sélection. Refuse (false + *error) si hors racine autorisée.
    bool addFolder(const QString& path, const QVariant& label, bool recursive,
                   QJsonObject* out, QString* error);
    bool setFolderEnabled(int folderId, bool enabled, QJsonObject* out); // false si absent
    bool removeFolder(int folderId);                                     // false si absent
    // Restaure un dossier retiré (retrait doux) : il redevient surveillé et une
    // passe le ravive. false si l'id est inconnu.
    bool restoreFolder(int folderId, QJsonObject* out);

    // --- Indexation ---
    // Déclenche une passe ASYNCHRONE. Retourne { accepted, state, started_at }.
    QJsonObject triggerIndex(IndexMode mode, const QVector<int>& folderIds, const QString& trigger);

private:
    void reconcileRoots();
    void doPass(IndexMode mode, QVector<int> folderIds, QString trigger); // thread du pool
    QString matchingRoot(const QString& path) const;
    QJsonObject observableState() const;

    // Configuration (lue depuis les params du module).
    QString     m_dbPath;
    QStringList m_roots;
    QString     m_exiftoolBinary;
    bool        m_stayOpen = true;
    // Cadence de la réconciliation automatique. Défaut : 0 => passe automatique
    // DÉSACTIVÉE, indexation UNIQUEMENT à la demande (API /index ou bouton PhotoHub).
    // C'est le mode qui ne met AUCUNE pression de fond sur la machine — le choix par
    // défaut. Une valeur positive réactive une passe périodique (ex. 86400000 = une
    // fois par jour) ; attention, une passe re-parcourt et `stat` tout l'arbre
    // surveillé plus une sonde par racine, coûteux surtout sur un montage réseau.
    int         m_intervalMs = 0;   // 0 = à la demande uniquement (défaut)
    // Délai borné des sondes d'accessibilité d'une racine (montages distants) :
    // au-delà, la racine est déclarée indisponible plutôt que de rester figé.
    int         m_probeTimeoutMs = 5000;

    // Prérequis ExifTool, sondé une fois au démarrage. Sans lui, les fichiers sont
    // indexés mais l'extraction EXIF échoue en silence : on le rend explicite.
    bool        m_exiftoolReady = false;
    QString     m_exiftoolDetail;        // version si prêt, sinon cause de l'échec

    PhotoRepository* m_readRepo = nullptr;  // connexion du thread principal
    QTimer*          m_watchTimer = nullptr;

    mutable QMutex m_stateMutex;         // protège le drapeau et l'état observable
    bool           m_indexing = false;
    QString        m_startedAt;
    QString        m_lastError;
    int            m_passCounter = 0;
    QFuture<void>  m_pass;

    // Progression de la passe en cours, alimentée par le callback de l'Indexer
    // (thread de passe) et lue par observableState() (thread HTTP) : sous m_stateMutex.
    int     m_progFoldersTotal = 0;
    int     m_progFoldersDone  = 0;
    qint64  m_progFilesSeen    = 0;
    QString m_progCurrentFolder;
    bool           m_started = false;
};

} // namespace morfphoto
