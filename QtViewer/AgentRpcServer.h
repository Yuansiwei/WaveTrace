#pragma once

#include <QObject>
#include <QHash>
#include <QByteArray>
#include <QString>

class MainWindow;
class QLocalServer;
class QLocalSocket;
class QTimer;

// Local-process-only, line-delimited JSON-RPC bridge for waveform analysis agents.
// The bridge deliberately lives on the GUI thread so Viewer operations use the
// same code paths as toolbar and mouse actions.
class AgentRpcServer final : public QObject {
public:
    explicit AgentRpcServer(MainWindow* window, QObject* parent = nullptr);
    ~AgentRpcServer() override;

    bool start(const QString& requestedName = QString(), QString* error = nullptr);
    QString serverName() const;

private:
    void acceptConnections();
    void pollConnections();
    void readRequests(QLocalSocket* socket);
    void processRequest(QLocalSocket* socket, const QByteArray& line);
    void writeDiscoveryFiles();
    void removeDiscoveryFiles();

    MainWindow* m_window = nullptr;
    QLocalServer* m_server = nullptr;
    QTimer* m_pollTimer = nullptr;
    QHash<QLocalSocket*, QByteArray> m_buffers;
    QString m_serverName;
    QString m_instanceDiscoveryPath;
    QString m_latestDiscoveryPath;
};
