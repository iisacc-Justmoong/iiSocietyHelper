#include <iiSocietyHelper/Monitoring/Monitor.h>
#include <iostream>

int main()
{
    namespace monitoring = iiSocietyHelper::Monitoring;
    monitoring::Publisher publisher;
    monitoring::Subscription subscription;
    monitoring::State snapshot;
    snapshot.instanceId = "installed-native-consumer";
    snapshot.containerId = "11111111-2222-4333-8444-555555555555";
    const auto json = monitoring::toJson(snapshot);
    subscription.stop();
    publisher.stop();
    if (json.find("installed-native-consumer") == std::string::npos
        || json.find("schemaVersion") == std::string::npos) return 1;
    std::cout << json << '\n';
}
