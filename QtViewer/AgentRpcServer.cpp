#include "AgentRpcServer.h"
#include "MainWindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QTimer>
#include <QLocalServer>
#include <QLocalSocket>

namespace {
constexpr qsizetype kMaxBufferedRequest = 8 * 1024 * 1024;

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message) {
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    QJsonObject response;
    response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("error"), error);
    return response;
}

void sendJson(QLocalSocket* socket, const QJsonObject& object) {
    if (!socket) return;
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    socket->write("\n", 1);
}

bool writeJsonFile(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0) return false;
    return file.commit();
}
}

AgentRpcServer::AgentRpcServer(MainWindow* window, QObject* parent)
    : QObject(parent), m_window(window), m_server(new QLocalServer(this)), m_pollTimer(new QTimer(this)) {
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    m_pollTimer->setInterval(10);
    QObject::connect(m_pollTimer, &QTimer::timeout, this, [this]() { pollConnections(); });
}

AgentRpcServer::~AgentRpcServer() {
    removeDiscoveryFiles();
}

bool AgentRpcServer::start(const QString& requestedName, QString* error) {
    if (m_server->isListening()) return true;
    m_serverName = requestedName.trimmed();
    if (m_serverName.isEmpty()) {
        m_serverName = QStringLiteral("WaveViewer-agent-%1").arg(QCoreApplication::applicationPid());
    }
    QLocalServer::removeServer(m_serverName);
    if (m_server->listen(m_serverName)) {
        m_pollTimer->start();
        writeDiscoveryFiles();
        return true;
    }
    if (error) *error = m_server->errorString();
    return false;
}

QString AgentRpcServer::serverName() const {
    return m_server->isListening() ? m_serverName : QString();
}

void AgentRpcServer::acceptConnections() {
    while (m_server->hasPendingConnections()) {
        QLocalSocket* socket = m_server->nextPendingConnection();
        if (!socket) continue;
        m_buffers.insert(socket, QByteArray());
    }
}

void AgentRpcServer::pollConnections() {
    acceptConnections();
    const QList<QLocalSocket*> sockets = m_buffers.keys();
    for (QLocalSocket* socket : sockets) {
        if (!socket) continue;
        if (socket->bytesAvailable() > 0) readRequests(socket);
        if (socket->state() == QLocalSocket::UnconnectedState && socket->bytesAvailable() == 0) {
            m_buffers.remove(socket);
            socket->deleteLater();
        }
    }
}

void AgentRpcServer::readRequests(QLocalSocket* socket) {
    QByteArray& buffer = m_buffers[socket];
    buffer += socket->readAll();
    if (buffer.size() > kMaxBufferedRequest) {
        sendJson(socket, rpcError(QJsonValue(), -32600, QStringLiteral("Request exceeds 8 MiB limit.")));
        socket->disconnectFromServer();
        return;
    }
    for (;;) {
        const qsizetype newline = buffer.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (!line.isEmpty()) processRequest(socket, line);
    }
}

void AgentRpcServer::processRequest(QLocalSocket* socket, const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        sendJson(socket, rpcError(QJsonValue(), -32700, QStringLiteral("Parse error: %1").arg(parseError.errorString())));
        return;
    }

    const QJsonObject request = document.object();
    const bool hasId = request.contains(QStringLiteral("id"));
    const QJsonValue id = request.value(QStringLiteral("id"));
    if (request.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0") ||
        !request.value(QStringLiteral("method")).isString() ||
        (request.contains(QStringLiteral("params")) && !request.value(QStringLiteral("params")).isObject())) {
        if (hasId) sendJson(socket, rpcError(id, -32600, QStringLiteral("Invalid JSON-RPC request.")));
        return;
    }

    int errorCode = 0;
    QString errorMessage;
    const QJsonValue result = m_window->handleAgentRpc(
        request.value(QStringLiteral("method")).toString(),
        request.value(QStringLiteral("params")).toObject(),
        &errorCode,
        &errorMessage);
    if (!hasId) return;
    if (errorCode != 0) {
        sendJson(socket, rpcError(id, errorCode, errorMessage));
        return;
    }
    QJsonObject response;
    response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("result"), result);
    sendJson(socket, response);
}

void AgentRpcServer::writeDiscoveryFiles() {
    const qint64 pid = QCoreApplication::applicationPid();
    const QString temp = QDir::tempPath();
    m_instanceDiscoveryPath = QDir(temp).filePath(QStringLiteral("WaveViewer-agent-%1.json").arg(pid));
    m_latestDiscoveryPath = QDir(temp).filePath(QStringLiteral("WaveViewer-agent-latest.json"));
    QJsonObject info;
    info.insert(QStringLiteral("transport"), QStringLiteral("local-socket"));
    info.insert(QStringLiteral("server_name"), serverName());
    info.insert(QStringLiteral("pid"), QString::number(pid));
    info.insert(QStringLiteral("protocol"), QStringLiteral("jsonrpc-2.0-line"));
    info.insert(QStringLiteral("api_version"), 1);
    writeJsonFile(m_instanceDiscoveryPath, info);
    writeJsonFile(m_latestDiscoveryPath, info);
}

void AgentRpcServer::removeDiscoveryFiles() {
    if (m_server && m_server->isListening()) m_server->close();
    if (!m_serverName.isEmpty()) QLocalServer::removeServer(m_serverName);
    if (!m_instanceDiscoveryPath.isEmpty()) QFile::remove(m_instanceDiscoveryPath);
    if (m_latestDiscoveryPath.isEmpty()) return;
    QFile latest(m_latestDiscoveryPath);
    if (!latest.open(QIODevice::ReadOnly)) return;
    const QJsonObject object = QJsonDocument::fromJson(latest.readAll()).object();
    latest.close();
    if (object.value(QStringLiteral("pid")).toString() == QString::number(QCoreApplication::applicationPid())) {
        QFile::remove(m_latestDiscoveryPath);
    }
}
