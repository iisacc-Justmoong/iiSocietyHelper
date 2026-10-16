#include "DiscoveryService.h"
#include <BleDiscovery.h>

namespace {
class HybridDiscoveryService final : public DiscoveryService {
    DiscoveryService *network;
    iiSocietySync::BleDiscovery bluetooth;
public:
    explicit HybridDiscoveryService(QObject *parent) : DiscoveryService(parent), network(createNetwork(this)) {
        connect(network, &DiscoveryService::found, this, &DiscoveryService::found);
        connect(network, &DiscoveryService::lost, this, &DiscoveryService::lost);
        connect(network, &DiscoveryService::failed, this, &DiscoveryService::failed);
        connect(&bluetooth, &iiSocietySync::BleDiscovery::found, this, &DiscoveryService::found);
    }
    void start(const QJsonObject &record, quint16 port) override {
        network->start(record, port); bluetooth.start(record, port);
    }
    void stop() override { bluetooth.stop(); network->stop(); }
};
}
DiscoveryService *DiscoveryService::create(QObject *parent) { return new HybridDiscoveryService(parent); }
