/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include "morfphoto/IModule.h"

class QTimer;

namespace morfphoto {

// -----------------------------------------------------------------------------
// ExampleModule : module de DEMONSTRATION (a remplacer par votre metier).
//
// Incremente un compteur a intervalle regulier et l'expose dans statusJson().
// Il ne sert qu'a montrer le pattern complet (config -> fabrique -> registre ->
// HTTP) et a garantir que le squelette compile et tourne tel quel.
//
// Parametres (ModuleDef::params) :
//   "period_ms" : periode du tick (defaut 5000).
// -----------------------------------------------------------------------------
class ExampleModule : public IModule {
    Q_OBJECT
public:
    ExampleModule(const QString& id, int periodMs = 5000, QObject* parent = nullptr);

    bool start() override;
    void stop() override;
    QJsonObject statusJson() const override;

private:
    void tick();

    int     m_periodMs;
    QTimer* m_timer;
    qint64  m_ticks = 0;
    bool    m_running = false;
};

} // namespace morfphoto
