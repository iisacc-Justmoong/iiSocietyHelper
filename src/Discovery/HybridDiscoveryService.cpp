#include <iiSocietyHelper/DiscoveryService.h>
#include <iiSocietyHelper/Sync/BleDiscovery.h>

namespace {
class HybridDiscoveryService final : public DiscoveryService {
    DiscoveryService *network;
    iiSocietyHelper::Sync::BleDiscovery bluetooth;
public:
    explicit HybridDiscoveryService(QObject *parent) : DiscoveryService(parent), network(createNetwork(this)) {
        connect(network, &DiscoveryService::found, this, &DiscoveryService::found);
        connect(network, &DiscoveryService::lost, this, &DiscoveryService::lost);
        connect(network, &DiscoveryService::failed, this, &DiscoveryService::failed);
        connect(&bluetooth, &iiSocietyHelper::Sync::BleDiscovery::found, this, &DiscoveryService::found);
    }
    void start(const QJsonObject &record, quint16 port) override {
        network->start(record, port); bluetooth.start(record, port);
    }
    void stop() override { bluetooth.stop(); network->stop(); }
};
}
DiscoveryService *DiscoveryService::create(QObject *parent) { return new HybridDiscoveryService(parent); }
