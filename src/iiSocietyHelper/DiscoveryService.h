#pragma once
#include "Export.h"
#include <QObject>
#include <QHostAddress>
#include <QJsonObject>

// BLE bootstrap and OS DNS-SD produce untrusted LAN candidates. Pairing still
// verifies account proofs and a TLS pin before opening the file channel.
class IISOCIETYHELPER_EXPORT DiscoveryService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start(const QJsonObject &record, quint16 port) = 0;
    virtual void stop() = 0;
    static DiscoveryService *create(QObject *parent);
    static DiscoveryService *createNetwork(QObject *parent);
signals:
    void found(QString service, QJsonObject record, QHostAddress address, quint16 port);
    void lost(QString service);
    void failed(QString message);
};
