/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/HttpServer.h"
#include "morfphoto/ModuleRegistry.h"
#include "morfphoto/SelfDescription.h"
#include "morfphoto/Version.h"
#include "morfphoto/PhotoApi.h"
#include "morfphoto/PhotoModule.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QDateTime>
#include <QUrl>

#include <utility>

namespace morfphoto {

namespace {
constexpr int kMaxRequestBytes = 65536;

QByteArray toJson(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

int contentLength(const QByteArray& headerBlock) {
    for (const QByteArray& line : headerBlock.split('\n')) {
        const QByteArray l = line.trimmed();
        if (l.toLower().startsWith("content-length:"))
            return l.mid(l.indexOf(':') + 1).trimmed().toInt();
    }
    return 0;
}

// Libellé HTTP pour les codes que l'API photo peut renvoyer (au-delà de 200/404).
QByteArray reasonFor(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}
} // namespace

HttpServer::HttpServer(ServiceConfig config, ModuleRegistry* registry, QObject* parent)
    : QObject(parent),
      m_config(std::move(config)),
      m_registry(registry),
      m_server(new QTcpServer(this)) {
    connect(m_server, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

HttpServer::~HttpServer() = default;

bool HttpServer::start() {
    if (m_config.httpPort == 0)
        return false;
    m_uptime.start();
    QHostAddress addr(m_config.bindAddress);
    if (addr.isNull())
        addr = QHostAddress(QHostAddress::AnyIPv4);
    return m_server->listen(addr, m_config.httpPort);
}

void HttpServer::stop()            { m_server->close(); }
bool HttpServer::isListening() const { return m_server->isListening(); }
quint16 HttpServer::port() const   { return m_server->isListening() ? m_server->serverPort() : 0; }

void HttpServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket* sock = m_server->nextPendingConnection();
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() { onSocketReadyRead(sock); });
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }
}

void HttpServer::onSocketReadyRead(QTcpSocket* sock) {
    QByteArray buf = sock->property("buf").toByteArray();
    buf += sock->readAll();

    const int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buf.size() > kMaxRequestBytes) { sock->abort(); return; }
        sock->setProperty("buf", buf);
        return;
    }

    const QByteArray headerBlock = buf.left(headerEnd);
    const int needed = contentLength(headerBlock);
    const int bodyStart = headerEnd + 4;
    if (buf.size() - bodyStart < needed) {
        if (buf.size() > kMaxRequestBytes) { sock->abort(); return; }
        sock->setProperty("buf", buf);
        return;
    }

    const int lineEnd = buf.indexOf("\r\n");
    const QList<QByteArray> parts = buf.left(lineEnd).split(' ');
    const QByteArray method = parts.value(0);
    const QByteArray path   = parts.value(1);
    const QByteArray body   = buf.mid(bodyStart, needed);

    sock->setProperty("buf", QByteArray());
    handleRequest(sock, method, path, body);
}

void HttpServer::handleRequest(QTcpSocket* sock, const QByteArray& method,
                               const QByteArray& rawPath, const QByteArray& body) {
    const int qpos = rawPath.indexOf('?');
    const QByteArray path = qpos < 0 ? rawPath : rawPath.left(qpos);
    const QString queryString = qpos < 0 ? QString() : QString::fromUtf8(rawPath.mid(qpos + 1));
    int        code   = 200;
    QByteArray reason = "OK";
    QByteArray out;

    // ---- API metier /api/v1 : deleguee au domaine photo (PhotoApi) ----
    if (path.startsWith("/api/")) {
        IModule* mod = m_registry ? m_registry->firstOfType(QStringLiteral("photo")) : nullptr;
        PhotoApi api(qobject_cast<PhotoModule*>(mod));
        const PhotoApi::Result r = api.handle(method, QString::fromUtf8(path), queryString, body);
        reply(sock, r.code, reasonFor(r.code), r.body);
        return;
    }

    // ---- Exemple de route POST (a remplacer par vos endpoints metier) ----
    if (path == "/example") {
        if (method != "POST") {
            code = 405; reason = "Method Not Allowed";
            out = "{\"error\":\"use POST /example\"}";
        } else {
            out = handleExamplePost(body, code, reason);
        }
    }
    // ---- Routes GET ------------------------------------------------------
    else if (method != "GET") {
        code = 405; reason = "Method Not Allowed";
        out = "{\"error\":\"method not allowed\"}";
    } else if (path == "/healthz") {
        out = "{\"status\":\"ok\"}";
    } else if (path == "/status") {
        out = buildStatusJson();
    } else if (path == "/modules") {
        QJsonObject o;
        o["modules"] = m_registry ? m_registry->modulesJson() : QJsonArray{};
        o["count"]   = m_registry ? m_registry->count() : 0;
        o["ts"]      = static_cast<double>(QDateTime::currentSecsSinceEpoch());
        out = toJson(o);
    } else if (path.startsWith("/modules/")) {
        const QString id = QUrl::fromPercentEncoding(path.mid(9));
        bool found = false;
        const QJsonObject o = m_registry ? m_registry->moduleJson(id, &found) : QJsonObject{};
        if (found) { out = toJson(o); }
        else { code = 404; reason = "Not Found"; out = "{\"error\":\"module not found\"}"; }
    } else {
        code = 404; reason = "Not Found";
        out = "{\"error\":\"not found\"}";
    }

    reply(sock, code, reason, out);
}

QByteArray HttpServer::handleExamplePost(const QByteArray& body, int& code, QByteArray& reason) const {
    // >>> EXEMPLE : parse un corps JSON et repond. A remplacer par votre metier.
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        code = 400; reason = "Bad Request";
        return "{\"error\":\"corps JSON invalide\"}";
    }
    QJsonObject o;
    o["received"] = doc.object();
    o["ts"]       = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    return toJson(o);
}

QByteArray HttpServer::buildStatusJson() const {
    QJsonObject o;
    o["app"]      = m_config.appName;
    o["host"]     = QHostInfo::localHostName();
    o["version"]  = morfphoto::version();
    o["proto"]    = QString::fromLatin1(morfphoto::kProtocol);
    o["state"]    = m_registry ? m_registry->state() : QStringLiteral("ok");
    o["uptime_s"] = static_cast<double>(m_uptime.isValid() ? m_uptime.elapsed() / 1000 : 0);
    o["ts"]       = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    o["metrics"]  = m_registry ? m_registry->metrics() : QJsonObject{};

    // Detail annonce (API) depuis le point UNIQUE. MODELE : tout service
    // morfSystem se decrit ainsi. describeService n'emet `web_ui` que si une
    // interface est declaree dans fillAnnouncedDetail ; ce squelette etant
    // sans interface, seul le bloc `api` apparait.
    morfbeacon::PresenceConfig self;
    fillAnnouncedDetail(self);
    const QJsonObject detail = morfbeacon::describeService(self, port());
    for (auto it = detail.constBegin(); it != detail.constEnd(); ++it)
        o[it.key()] = it.value();

    return toJson(o);
}

void HttpServer::reply(QTcpSocket* sock, int code, const QByteArray& reason, const QByteArray& body) {
    QByteArray resp;
    resp += "HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\n";
    resp += "Content-Type: application/json; charset=utf-8\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    resp += "Access-Control-Allow-Origin: *\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += body;
    sock->write(resp);
    // Vider le tampon d'écriture AVANT de fermer : sur une grande réponse (page HTML,
    // /status volumineux), le corps déborde du tampon socket (~20 Ko constaté) et
    // `disconnectFromHost` seul en tronque la fin côté client. On draine jusqu'à ce
    // qu'il ne reste rien à écrire, avec un délai de garde pour ne jamais bloquer.
    while (sock->bytesToWrite() > 0)
        if (!sock->waitForBytesWritten(2000))
            break;
    sock->disconnectFromHost();
}

} // namespace morfphoto
