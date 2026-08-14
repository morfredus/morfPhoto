/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/ExampleModule.h"

#include <QTimer>
#include <QDateTime>

namespace morfphoto {

ExampleModule::ExampleModule(const QString& id, int periodMs, QObject* parent)
    : IModule(id, QStringLiteral("example"), parent),
      m_periodMs(periodMs > 0 ? periodMs : 5000),
      m_timer(new QTimer(this)) {
    m_timer->setInterval(m_periodMs);
    connect(m_timer, &QTimer::timeout, this, &ExampleModule::tick);
}

bool ExampleModule::start() {
    m_running = true;
    m_timer->start();
    return true;
}

void ExampleModule::stop() {
    m_running = false;
    m_timer->stop();
}

QJsonObject ExampleModule::statusJson() const {
    // >>> A REMPLACER : renvoyez ici l'etat reel de votre module. <<<
    QJsonObject o;
    o["running"] = m_running;
    o["ticks"]   = static_cast<double>(m_ticks);
    o["ts"]      = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    return o;
}

void ExampleModule::tick() {
    ++m_ticks;
    emit updated(id());
}

} // namespace morfphoto
