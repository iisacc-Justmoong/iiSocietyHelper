#pragma once
#include <iiSocietyHelper/DiscoveryService.h>
#include <QPointer>
class FakeDiscoveryService final : public DiscoveryService {
public:
    QJsonObject record;
    quint16 port = 0;
    int starts = 0, stops = 0;
    QList<QPointer<FakeDiscoveryService>> observers;
    void start(const QJsonObject &value, quint16 socketPort) override { record = value; port = socketPort; ++starts; }
    void stop() override {
        const auto id = record.value("id").toString();
        record = {}; port = 0; ++stops;
        const auto targets = observers; observers.clear();
        for (const auto &other : targets) if (other && !id.isEmpty()) emit other->lost(id);
    }
    void announceTo(FakeDiscoveryService &other) {
        if (!observers.contains(&other)) observers.append(&other);
        emit other.found(record.value("id").toString(), record, QHostAddress::LocalHost, port);
    }
};
