/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include "morfphoto/IModule.h"
#include "morfphoto/Indexer.h"
#include "morfphoto/SourceManager.h"

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
    // Activite en cours (contrat `activity/1`) : { type:"indexation", ... } pendant
    // une passe, objet vide sinon. Lu en temps reel par morfMonitor via /status.
    QJsonObject activityJson() const override;

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

    // --- Contexte photographique par répertoire (morfphoto-context/2) ---
    // Lectures : la liste des répertoires + leur contexte (écran de qualification
    // PhotoHub), et le contexte d'un répertoire précis. `status` : ""=tous, sinon
    // "qualified" | "unqualified" | "invalid".
    QJsonArray  listContexts(const QString& status) const;
    QJsonObject getContext(const QString& directory) const;
    // Vignette JPEG d'un fichier (apercu embarque via exiftool, JPEG comme RAW). Sert
    // l'apercu des clients (PhotoHub). Valide que `path` est sous une racine autorisee et
    // pointe un fichier existant. *ok=false + retour vide si indisponible.
    QByteArray  thumbnail(const QString& path, bool* ok) const;
    // Écriture (morfPhoto est l'UNIQUE écrivain de `.morfphoto.json`). `context` et
    // `subject` sont OBLIGATOIRES et doivent appartenir au vocabulaire gelé. Écrit le
    // fichier disque (atomique) puis met à jour la projection. false + *error si le
    // répertoire est hors racine, inconnu, ou si les valeurs sont invalides. *out reçoit
    // le contexte stocké.
    bool putContext(const QString& directory, const QString& context, const QString& subject,
                    const QVariant& motif, const QVariant& description,
                    QJsonObject* out, QString* error);
    // Retire `.morfphoto.json` du répertoire (redevient non qualifié). false + *error si
    // hors racine ou si la suppression échoue.
    bool deleteContext(const QString& directory, QJsonObject* out, QString* error);

    // --- Dossiers (thread principal) ---
    QJsonArray  listFolders() const;
    // Racines AUTORISEES (perimetre defini par la config) : PhotoHub les affiche
    // pour guider l'utilisateur, qui ne peut declarer qu'a l'interieur.
    QJsonArray  allowedRoots() const;

    // --- Sources SMB poussees (bouton « Envoyer la config » de PhotoHub) ---
    // Monte un partage distant sous /mnt/photos_<hostname> via le helper, valide
    // le CIFS, persiste fstab + racine, puis (si le JSON a change) programme un
    // redemarrage. false + *error si une etape obligatoire echoue.
    bool        addSource(const QString& host, const QString& share, const QString& username,
                          const QString& password, const QString& hostname, bool writable,
                          QJsonObject* out, QString* error);
    // Sources connues, chacune annotee `mounted`.
    QJsonArray  listSources() const;

    // Verifie que le helper privilegie est present, traversable et demarrable
    // (verbe `probe`) AVANT d'accepter un mot de passe SMB.
    bool        helperReady(QJsonObject* out, QString* error) const;
    // Ajoute une sélection. Refuse (false + *error) si hors racine autorisée.
    // `removable` : le dossier vit sur un support amovible (archive), son absence ne
    // vaudra jamais suppression. `volumeLabel` : nom du support (QVariant vide = aucun).
    bool addFolder(const QString& path, const QVariant& label, bool recursive,
                   bool removable, const QVariant& volumeLabel,
                   QJsonObject* out, QString* error);
    bool setFolderEnabled(int folderId, bool enabled, QJsonObject* out); // false si absent
    // Règle le support amovible (drapeau + libellé) d'une sélection. false si absent.
    bool setFolderMedia(int folderId, bool removable, const QVariant& volumeLabel, QJsonObject* out);
    // Sort/réintègre une sélection des analyses sans effacer ses données. false si absent.
    bool setFolderAnalyticsExcluded(int folderId, bool excluded, QJsonObject* out);
    bool removeFolder(int folderId);                                     // false si absent
    // Restaure un dossier retiré (retrait doux) : il redevient surveillé et une
    // passe le ravive. false si l'id est inconnu.
    bool restoreFolder(int folderId, QJsonObject* out);

    // Purge DÉFINITIVE (irréversible, distincte du retrait doux) selon une portée :
    //   scope="folder" (value = id), "year" (value = année), "camera" (value = nom),
    //   "all" (value ignorée). Retourne { deleted, scope } ou { error } si portée
    //   inconnue / valeur invalide.
    QJsonObject purge(const QString& scope, const QVariant& value);

    // --- Indexation ---
    // Déclenche une passe ASYNCHRONE. Retourne { accepted, state, started_at }.
    QJsonObject triggerIndex(IndexMode mode, const QVector<int>& folderIds, const QString& trigger);

private:
    void reconcileRoots();
    void doPass(IndexMode mode, QVector<int> folderIds, QString trigger); // thread du pool
    QString matchingRoot(const QString& path) const;
    QJsonObject observableState() const;
    // Declare une indexation TERMINEE a morfAnalytics (historique), best-effort et
    // jamais bloquant : contrat `activity/1` §6. URL d'ingestion lue dans
    // l'environnement (MORFANALYTICS_ACTIVITY_URL) ou le fichier admin partage
    // /etc/morfsystem/monitor-activity-url. Aucune source => rien n'est emis.
    // Appelee depuis le thread de passe, apres la fin de l'indexation.
    void reportIndexActivity(qint64 startEpoch, qint64 filesSeen,
                             int foldersTotal, const QString& error) const;

    // Configuration (lue depuis les params du module).
    QString     m_dbPath;
    QStringList m_roots;
    // Sources SMB poussees par PhotoHub : leurs points de montage completent
    // m_roots. Persistees dans l'etat, jamais le mot de passe (voir SourceManager).
    SourceManager m_sourceManager{QStringLiteral("morfphoto")};
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
    QString        m_startedAt;          // ISO UTC, lisible (API existante)
    qint64         m_startedAtEpoch = 0; // meme instant en epoch s (contrat activity)
    QString        m_lastError;
    int            m_passCounter = 0;
    QFuture<void>  m_pass;

    // Progression de la passe en cours, alimentée par le callback de l'Indexer
    // (thread de passe) et lue par observableState() (thread HTTP) : sous m_stateMutex.
    int     m_progFoldersTotal = 0;
    int     m_progFoldersDone  = 0;
    qint64  m_progFilesSeen    = 0;
    qint64  m_progFilesTotal   = -1;
    bool    m_progFilesTotalFinal = false;
    QString m_progPhase;
    QString m_progCurrentFolder;
    int     m_lastFoldersTotal = 0;
    bool           m_started = false;
};

} // namespace morfphoto
