/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QObject>
#include <QString>
#include <QJsonObject>

namespace morfphoto {

// -----------------------------------------------------------------------------
// IModule : LE point d'extension du service. C'est ICI que vit le METIER.
//
// Un "module" est une unite enfichable du service : un capteur (comme dans
// morfSensor), une destination de notification (comme dans morfNotify), une
// tache de collecte, etc. Chaque module :
//   - porte un id() unique et un type() (identifiant de fabrique) ;
//   - demarre / s'arrete proprement (start / stop) ;
//   - expose son etat courant en JSON (statusJson), agrege dans /modules ;
//   - signale ses mises a jour (updated) pour une reaction eventuelle.
//
// >>> POUR CODER VOTRE SERVICE : creez une ou plusieurs sous-classes d'IModule,
//     enregistrez-les dans ModuleFactory, adaptez les routes HTTP si besoin. <<<
// Voir ExampleModule pour un squelette minimal fonctionnel.
// -----------------------------------------------------------------------------
class IModule : public QObject {
    Q_OBJECT
public:
    IModule(QString id, QString type, QObject* parent = nullptr)
        : QObject(parent), m_id(std::move(id)), m_type(std::move(type)) {}
    ~IModule() override = default;

    QString id() const   { return m_id; }
    QString type() const { return m_type; }

    // Demarre le module. false si l'initialisation echoue immediatement ; un
    // module peut aussi demarrer "en attente" et devenir operationnel plus tard.
    virtual bool start() = 0;

    // Arrete proprement (ferme fichiers/sockets, stoppe timers).
    virtual void stop() = 0;

    // Etat courant du module, expose via l'API HTTP (/modules). Toujours sur,
    // meme avant toute activite. Format libre, propre a votre metier.
    virtual QJsonObject statusJson() const = 0;

    // Activite EN COURS du module, selon le contrat generique `activity/1`
    // (morfSystem/docs/CONTRAT-ACTIVITE.md). Un module qui effectue un travail
    // long ou identifiable (indexation, collecte, compilation...) renvoie ici un
    // objet decrivant ce qu'il fait : { type, state, started_at, current, total,
    // progress_percent, detail }. Rien en cours => objet vide (defaut).
    // Expose de facon optionnelle dans /status (jamais dans le heartbeat beacon)
    // et lu en temps reel par morfMonitor.
    virtual QJsonObject activityJson() const { return {}; }

signals:
    // Emis quand l'etat du module change (facultatif a exploiter).
    void updated(const QString& id);

private:
    QString m_id;
    QString m_type;
};

} // namespace morfphoto
