/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include "morfphoto/PhotoTypes.h"

class QProcess;

// -----------------------------------------------------------------------------
// ExifExtractor : enveloppe autour d'ExifTool.
//
// ExifTool est lancé en mode persistant (`-stay_open True -@ -`) : démarrer un
// process par fichier serait ruineux sur une photothèque de dizaines de milliers
// d'images. Garder un process ouvert et lui pousser les fichiers un par un, en
// envoyant des arguments sur son entrée standard puis un `-execute` numéroté ;
// ExifTool répond par le JSON du fichier suivi d'un marqueur `{readyN}`.
//
// Souveraineté : cette classe RENVOIE les valeurs telles qu'ExifTool les présente.
// Elle ne corrige rien, n'arrondit rien, ne regroupe rien. La date est demandée
// au format ISO (`-d`) pour que `taken_year` fonctionne, sans changer l'instant ;
// la vitesse est conservée sous sa forme lisible (« 1/250 ») ET numérique, l'une
// n'écrasant jamais l'autre.
//
// Non conçue pour un usage concurrent : un extracteur (donc un process ExifTool)
// par thread d'indexation.
// -----------------------------------------------------------------------------
namespace morfphoto {

class ExifExtractor {
public:
    ExifExtractor(QString binary, bool stayOpen);
    ~ExifExtractor();

    void open();     // démarre le process persistant (idempotent)
    void close();    // arrête proprement le process

    // Vérifie qu'ExifTool est joignable, en lançant « binary -ver ». Statique : le
    // service sonde le prérequis UNE fois au démarrage, pour signaler clairement un
    // binaire absent plutôt que de laisser chaque fichier échouer en silence (photos
    // indexées, métadonnées toutes nulles). En cas de succès, `detail` reçoit la
    // version (« ExifTool 12.76 ») ; sinon, la cause de l'échec.
    static bool probe(const QString& binary, QString* detail = nullptr);

    // Lit les métadonnées d'un fichier. `ok` vaut true en cas de succès ; sinon
    // `error` décrit l'échec (le runner le journalise et poursuit la passe).
    ExifData extract(const QString& path, QString* error);

    // Mappe une sortie JSON d'ExifTool sur ExifData. Statique et public pour être
    // testable sans lancer ExifTool (le cœur fragile de la souveraineté des valeurs).
    static ExifData mapTags(const QString& json, QString* error);

private:
    QString readPersistent(const QString& path, QString* error);
    QString readOnce(const QString& path, QString* error);

    QString    m_binary;
    bool       m_stayOpen;
    QProcess*  m_proc = nullptr;
    int        m_counter = 0;   // numérote les -execute pour synchroniser
};

} // namespace morfphoto
