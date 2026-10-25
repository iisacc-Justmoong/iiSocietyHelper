#pragma once
#include "iiSocietyHelper/Export.h"
#include "Synchronizer.h"
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

namespace iiSocietyHelper::Sync {
class ObjectProvider;
using RequestSender = std::function<QString(const QString &peer, const QJsonObject &payload)>;
// Owns device-to-device synchronization and its worker. The app supplies only
// an already authenticated transport and its current authorized peer set.
class IISOCIETYHELPER_EXPORT Controller final : public QObject {
    Q_OBJECT
public:
    explicit Controller(RequestSender send, QObject *parent = nullptr);
    ~Controller() override;
    void open(const QString &container, const QString &accountScope);
    // Direct access to an existing authority without another indexer or replica
    // synchronization loop. Only drive.* requests are accepted in this mode.
    void openHostAccess(const QString &container, const QString &accountScope);
    void close();
    // Desktop process handoff: cancel and drain the worker before releasing a lease.
    void closeAndWait();
    // Read-only startup/foreground metadata, serialized on the replica worker.
    // Only the most recent inspection result is delivered on this object's thread.
    void inspectContainer(const QString &container, const QString &accountScope = {});
    // Permanently cancel this controller and retire its worker without blocking
    // the caller. Desktop ownership handoff must still use closeAndWait().
    void shutdownAsync();
    void setPeers(const QStringList &authorizedPeers, const QStringList &remoteHosts, const QString &accountHost = {}, const QString &accountContainer = {});
    // Queue an authenticated host selection on the replica worker. A busy local
    // operation lock is retried; no replication is served until it is durable.
    void claimPrimaryHost(const QString &device);
    bool available() const;
    bool busy() const;
    Status &status();
    QString errorString() const;
    void synchronizeNow();
    void setContentPolicy(Synchronizer::ContentPolicy policy);
    void placeObject(const QString &path, std::shared_ptr<ObjectProvider> provider);
    void restoreObject(const QString &path, std::shared_ptr<ObjectProvider> provider);
    QJsonObject handle(const QString &peer, const QJsonObject &envelope);
    void receive(const QString &transportRequestId, const QJsonObject &response);
signals:
    void namespaceChanged(QJsonObject state);
    void providerFinished(QString path, QString provider, bool restored, bool success, QString error);
    void mirrorChanged(QJsonObject binding);
    void changed();
    void synchronized(QString peer);
    void hostValidated(QString peer);
    void progress(QString path, qint64 completedBytes, qint64 totalBytes);
    void verificationProgress(QString path, qint64 completedBytes, qint64 totalBytes);
    void containerInspected(QString path, QString scope, QJsonObject binding,
        QString containerIdentifier, QString primaryHost);
private:
    void openInternal(const QString &container, const QString &accountScope, bool hostAccess);
    class Private;
    std::unique_ptr<Private> d;
};
}
