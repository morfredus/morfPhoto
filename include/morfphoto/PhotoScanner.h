/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "morfphoto/PhotoTypes.h"

// -----------------------------------------------------------------------------
// Parcours du système de fichiers et garde-fou de périmètre.
//
// Le triplet (path, size, mtime) décide seul si un fichier doit être retraité
// (pas de hash tant qu'un besoin réel ne le justifie pas). Le scan ne lit JAMAIS
// l'EXIF : il reste rapide (un stat par fichier), l'extraction vient après et
// seulement pour les fichiers neufs ou modifiés.
//
// Tolérance aux montages distants : une racine peut être un partage réseau
// (SMB/CIFS, NFS...) qui disparaît en cours de passe. La couche FS reste
// GÉNÉRIQUE (aucune notion de SMB) mais expose de quoi ne pas rester bloquée sur
// une source muette : une sonde bornée dans le temps (probeAccessible) et un scan
// qui dit s'il est allé au bout (ScanResult), interruptible en cours de route.
// -----------------------------------------------------------------------------
namespace morfphoto {

// Résultat d'un parcours. `completed` distingue un scan mené jusqu'au bout de
// façon fiable d'un scan interrompu (source devenue muette) : SEUL un scan
// complet autorise ensuite un verdict de disparition (voir Indexer). Un scan
// partiel ne doit jamais servir de référence pour marquer des fichiers absents.
struct ScanResult {
    bool              completed = false;
    QVector<FileInfo> files;
};

// Classe une extension en type de fichier (RAW regroupés, reste explicite).
// Classification DÉTERMINISTE (ne dépend que de l'extension), pas une analyse.
QString fileTypeFor(const QString& extension);

// Vrai si `candidate` est situé sous une des racines autorisées. Empêche
// PhotoHub (ou une requête forgée) de faire indexer un chemin arbitraire.
// Vérifie l'ascendance sur chemins normalisés : un `..` ne peut pas s'échapper.
bool isWithinRoots(const QString& candidate, const QStringList& roots);

// Vrai si `path` répond comme un dossier accessible dans le délai imparti.
// Le stat potentiellement bloquant est déporté sur un thread détaché : si la
// source ne répond pas, on cesse d'attendre au bout de `timeoutMs` au lieu de
// rester figé le temps du timeout du montage (jusqu'à ~180 s en CIFS). Le thread
// se termine seul quand l'appel finit par revenir ; on ne le tue jamais. Un
// `timeoutMs <= 0` attend indéfiniment (à réserver aux chemins locaux sûrs).
bool probeAccessible(const QString& path, int timeoutMs);

// Génère les FileInfo des fichiers image d'un dossier (récursif si demandé).
// Un fichier illisible au stat est simplement ignoré.
//
// `stillAvailable`, s'il est fourni, est consulté périodiquement pendant le
// parcours : dès qu'il renvoie false, le scan s'arrête et `completed` reste
// false (source perdue en cours de route). Sans callback, le scan va toujours au
// bout et `completed` vaut true. C'est le mécanisme qui borne le « ça a l'air
// figé » sans imposer de deadline arbitraire à un gros parcours légitime.
ScanResult scanFolder(const QString& folder, bool recursive,
                      const std::function<bool()>& stillAvailable = {});

} // namespace morfphoto
