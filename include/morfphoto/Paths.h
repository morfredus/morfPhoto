/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>

namespace morfphoto {

// -----------------------------------------------------------------------------
// Paths : emplacements du service selon la doctrine morfSystem
// (voir docs/fr/FILESYSTEM.md). Trois zones distinctes, jamais melangees :
//
//   /opt/<service>                    le PROGRAMME (binaire + assets embarques),
//                                     lecture seule a l'execution.
//   /etc/morfsystem/<service>         la CONFIG ADMIN, reference durable posee par
//                                     l'administrateur, JAMAIS reecrite par le
//                                     service (proprietaire root).
//   /var/lib/morfsystem/<service>     l'ETAT PERSISTANT genere par le service
//                                     (cles, coffre, curseurs, sequences de sync,
//                                     index, derniere execution). SEULE zone ou le
//                                     service ecrit ; proprietaire = user du service.
//
// Regle : ce qu'un administrateur pose et qu'on peut perdre a la reinstallation
// est de la CONFIG ; ce que le service genere et dont la perte casse une
// continuite est de l'ETAT. Un secret, une cle, une sequence monotone, un
// curseur : toujours de l'etat.
//
// Preferences modifiables a l'execution (UI web) : SUPERPOSITION. La base admin
// reste en lecture seule dans /etc ; les surcharges sont ecrites dans l'etat
// (overrideConfigFile) et appliquees par-dessus au chargement. On ne rend jamais
// /etc accessible en ecriture au service.
// -----------------------------------------------------------------------------
struct Paths {
    // Racine de l'etat persistant, garantie accessible en ecriture (creee au
    // besoin). Honore $STATE_DIRECTORY pose par systemd (StateDirectory=) ; repli
    // conforme a l'OS sinon (/var/lib/morfsystem/<service> ou %ProgramData%\...).
    static QString stateDir(const QString& serviceName);

    // Fichier des surcharges de configuration editables a l'execution. Vit dans
    // l'etat, jamais dans /etc. Superpose a la config admin (voir loadLayeredConfig).
    static QString overrideConfigFile(const QString& serviceName);
};

} // namespace morfphoto
