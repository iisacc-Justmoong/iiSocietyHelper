#pragma once
#include <iiSocietyHelper/DiscoveryService.h>
class FakeDiscoveryService final : public DiscoveryService {
public:
    QJsonObject record;
    quint16 port = 0;
    int starts = 0, stops = 0;
    void start(const QJsonObject &value, quint16 socketPort) override { record = value; port = socketPort; ++starts; }
    void stop() override { record = {}; port = 0; ++stops; }
    void announceTo(FakeDiscoveryService &other) {
        emit other.found(record.value("id").toString(), record, QHostAddress::LocalHost, port);
    }
};
