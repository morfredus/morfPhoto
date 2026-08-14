/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QStringList>

// -----------------------------------------------------------------------------
// Schéma SQLite v1 du domaine photo (source de vérité du schéma).
//
// Le numéro de schéma courant est porté par `PRAGMA user_version` : le dépôt
// applique les migrations dont le numéro dépasse la version courante, puis scelle
// la nouvelle version. Le driver SQLite exécute UNE instruction par requête ;
// chaque migration est donc une liste d'instructions séparées.
//
// Souveraineté : les valeurs EXIF sont stockées BRUTES, jamais corrigées ni
// regroupées. 49, 50 et 51 mm restent 49, 50 et 51. Un RAW et son JPEG sont deux
// lignes distinctes : morfPhoto ne décide jamais qu'ils sont un même déclenchement
// (c'est le rôle de la couche d'analyse, ailleurs).
// -----------------------------------------------------------------------------
namespace morfphoto {

inline constexpr int kSchemaVersion = 3;

// Instructions de la migration 001 (création initiale). La colonne calculée
// `taken_year` permet de regrouper par année sans reparser la date à chaque fois.
inline QStringList schemaV1Statements() {
    return {
        // folders : les sélections que l'utilisateur demande à surveiller.
        // Modèle hybride : /etc définit les RACINES AUTORISÉES, cette table ce que
        // l'utilisateur demande effectivement, toujours sous une racine autorisée.
        QStringLiteral(
            "CREATE TABLE folders ("
            "  id            INTEGER PRIMARY KEY,"
            "  path          TEXT    NOT NULL UNIQUE,"
            "  root_path     TEXT    NOT NULL,"
            "  label         TEXT,"
            "  enabled       INTEGER NOT NULL DEFAULT 1,"
            "  auto_disabled INTEGER NOT NULL DEFAULT 0,"
            "  recursive     INTEGER NOT NULL DEFAULT 1,"
            "  added_at      TEXT    NOT NULL,"
            "  last_scan_at  TEXT"
            ")"),

        // files : le cœur. Une ligne par fichier présent sur le disque. Le triplet
        // (path, size, mtime) suffit à décider d'un retraitement (pas de hash).
        QStringLiteral(
            "CREATE TABLE files ("
            "  id                    INTEGER PRIMARY KEY,"
            "  folder_id             INTEGER NOT NULL REFERENCES folders(id),"
            "  path                  TEXT    NOT NULL UNIQUE,"
            "  directory             TEXT    NOT NULL,"
            "  filename              TEXT    NOT NULL,"
            "  extension             TEXT,"
            "  file_type             TEXT,"
            "  size                  INTEGER NOT NULL,"
            "  mtime                 INTEGER NOT NULL,"
            "  taken_at              TEXT,"
            "  taken_year            INTEGER GENERATED ALWAYS AS"
            "                        (CAST(strftime('%Y', taken_at) AS INTEGER)) STORED,"
            "  make                  TEXT,"
            "  camera_model          TEXT,"
            "  lens                  TEXT,"
            "  focal_length          REAL,"
            "  focal_length_35mm     REAL,"
            "  aperture              REAL,"
            "  shutter_speed         TEXT,"
            "  shutter_speed_s       REAL,"
            "  iso                   INTEGER,"
            "  exposure_compensation REAL,"
            "  rating                INTEGER,"
            "  latitude              REAL,"
            "  longitude             REAL,"
            "  raw_exif              TEXT,"
            "  state                 TEXT    NOT NULL DEFAULT 'present',"
            "  exif_ok               INTEGER NOT NULL DEFAULT 0,"
            "  first_seen_at         TEXT    NOT NULL,"
            "  last_seen_at          TEXT    NOT NULL,"
            "  last_indexed_at       TEXT    NOT NULL,"
            "  disappeared_at        TEXT"
            ")"),

        QStringLiteral("CREATE INDEX idx_files_folder ON files(folder_id)"),
        QStringLiteral("CREATE INDEX idx_files_state  ON files(state)"),
        QStringLiteral("CREATE INDEX idx_files_year   ON files(taken_year)"),
        QStringLiteral("CREATE INDEX idx_files_camera ON files(camera_model)"),
        QStringLiteral("CREATE INDEX idx_files_lens   ON files(lens)"),
        QStringLiteral("CREATE INDEX idx_files_type   ON files(file_type)"),

        // index_runs : historique des passes (progression, compteurs, déclencheur).
        QStringLiteral(
            "CREATE TABLE index_runs ("
            "  id            INTEGER PRIMARY KEY,"
            "  started_at    TEXT    NOT NULL,"
            "  finished_at   TEXT,"
            "  mode          TEXT    NOT NULL,"
            "  trigger       TEXT    NOT NULL DEFAULT 'watch',"
            "  state         TEXT    NOT NULL,"
            "  files_seen    INTEGER NOT NULL DEFAULT 0,"
            "  files_new     INTEGER NOT NULL DEFAULT 0,"
            "  files_updated INTEGER NOT NULL DEFAULT 0,"
            "  files_missing INTEGER NOT NULL DEFAULT 0,"
            "  errors_count  INTEGER NOT NULL DEFAULT 0"
            ")"),

        // index_errors : erreurs par fichier, jamais avalées en silence.
        QStringLiteral(
            "CREATE TABLE index_errors ("
            "  id          INTEGER PRIMARY KEY,"
            "  run_id      INTEGER NOT NULL REFERENCES index_runs(id),"
            "  path        TEXT,"
            "  stage       TEXT    NOT NULL,"
            "  message     TEXT    NOT NULL,"
            "  occurred_at TEXT    NOT NULL"
            ")"),
    };
}

// Migration 002 : distinguer un dossier RETIRÉ (suppression douce) d'un dossier
// simplement désactivé. Jusque-là les deux valaient `enabled = 0`, donc PhotoHub
// ne pouvait pas les séparer. `deleted_at` est NULL tant que le dossier est
// surveillé (actif ou en pause) et horodaté au moment du retrait doux. La donnée
// n'est jamais supprimée : un dossier retiré peut être restauré.
inline QStringList schemaV2Statements() {
    return {
        QStringLiteral("ALTER TABLE folders ADD COLUMN deleted_at TEXT"),
    };
}

// Migration 003 : rendre visible une passe INTERROMPUE par une source indisponible.
// Une racine réseau (SMB/CIFS, NFS...) valide au départ peut disparaître pendant
// une passe. On ne marque alors AUCUN fichier disparu (un scan partiel n'est pas
// une référence fiable) : on compte les sélections sautées ici, et `index_runs.state`
// vaut alors 'interrupted' au lieu de 'done'. Colonne additive, valeur 0 par défaut
// pour tout l'historique existant.
inline QStringList schemaV3Statements() {
    return {
        QStringLiteral("ALTER TABLE index_runs ADD COLUMN files_unavailable INTEGER NOT NULL DEFAULT 0"),
    };
}

// Instructions à exécuter pour atteindre une version de schéma donnée. Le dépôt
// applique dans l'ordre chaque version manquante (voir applyMigrations).
inline QStringList migrationStatements(int version) {
    switch (version) {
    case 1:  return schemaV1Statements();
    case 2:  return schemaV2Statements();
    case 3:  return schemaV3Statements();
    default: return {};
    }
}

} // namespace morfphoto
