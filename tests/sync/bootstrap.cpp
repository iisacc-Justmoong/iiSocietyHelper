#include <iiSocietyHelper/Sync/NearbyBootstrap.h>
#include <iiSocietyHelper/Sync/BleDiscovery.h>
#include "../CTestSupport.h"
#include <QCoreApplication>
#include <string_view>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QScopeGuard>
using namespace iiSocietyHelper::Sync;
class BootstrapTests {
    NearbyBootstrap record() const {
        return {{{"v", "1"}, {"scope", QString(64, 'a')}, {"id", "desktop"}, {"nonce", "nonce"},
            {"name", QString::fromUtf8("작업실")}, {"kind", "pc"}, {"host", "1"}}, {"192.168.1.2", "10.0.0.1"}, 12345};
    }
public:
    void pagesRoundTripAndDetectMixedRefresh() {
        const auto original = record(); const auto bytes = original.encode(); CTEST_CHECK(!bytes.isEmpty());
        QByteArray joined;
        for (int page = 0; page < 4; ++page) joined += bytes.mid(page * 500, 500);
        const auto decoded = NearbyBootstrap::decode(joined); CTEST_CHECK(decoded);
        CTEST_COMPARE(decoded->record, original.record); CTEST_COMPARE(decoded->addresses, original.addresses); CTEST_COMPARE(decoded->port, original.port);
        joined[joined.size() - 10] ^= 1; CTEST_CHECK(!NearbyBootstrap::decode(joined));
        CTEST_CHECK(!NearbyBootstrap::decode(bytes + ' ')); CTEST_CHECK(!NearbyBootstrap::decode(bytes.left(32)));
    }
    void rejectsCredentialsFilesAndPublicEndpoints() {
        for (const auto &field : {"cookie", "secret", "keys", "data", "files", "password"}) {
            auto value = record(); value.record.insert(field, "must never be advertised"); CTEST_CHECK(value.encode().isEmpty());
        }
        for (const auto &address : {"8.8.8.8", "127.0.0.1", "169.254.169.254", "iisacc.com", "0.0.0.0"}) {
            auto value = record(); value.addresses = {address}; CTEST_CHECK(value.encode().isEmpty());
        }
        auto value = record(); value.port = 0; CTEST_CHECK(value.encode().isEmpty());
        value = record(); value.record.insert("name", QString(129, 'x')); CTEST_CHECK(value.encode().isEmpty());
        CTEST_CHECK(!NearbyBootstrap::decode(QByteArray(2001, 'x')));
    }
    void digestDoesNotAuthorizeUntrustedMetadata() {
        auto value = record(); value.record.insert("proof", QString(64, '0'));
        const auto decoded = NearbyBootstrap::decode(value.encode()); CTEST_CHECK(decoded);
        CTEST_COMPARE(decoded->record.value("proof").toString(), QString(64, '0')); // Consumer must verify HMAC.
    }
    void stopIsIdempotentWithoutStartingRadio(bool disabled) {
        const auto previous=qgetenv("IISOCIETYSYNC_DISABLE_BLE");
        const auto restore=qScopeGuard([&] {
            if(previous.isNull())qunsetenv("IISOCIETYSYNC_DISABLE_BLE");else qputenv("IISOCIETYSYNC_DISABLE_BLE",previous);
        });
        qputenv("IISOCIETYSYNC_DISABLE_BLE",disabled?"1":"0");
        BleDiscovery discovery; CTEST_COMPARE(discovery.status(), "stopped"); discovery.stop(); discovery.stop();
        // An invalid endpoint returns before radio creation or permission calls.
        discovery.start({}, 0);
        if(disabled)CTEST_COMPARE(discovery.status(),"disabled");
        else CTEST_CHECK(discovery.status().startsWith("unavailable"));
        discovery.stop(); CTEST_COMPARE(discovery.status(), "stopped");
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--offline")) {
        std::cerr << "Expected no argument or --offline" << std::endl;
        return 1;
    }
    const bool offline = argc == 2;
    helper_ctest::CTestRunner runner;
    BootstrapTests tests;
    runner.run("pagesRoundTripAndDetectMixedRefresh", [&] { tests.pagesRoundTripAndDetectMixedRefresh(); });
    runner.run("rejectsCredentialsFilesAndPublicEndpoints", [&] { tests.rejectsCredentialsFilesAndPublicEndpoints(); });
    runner.run("digestDoesNotAuthorizeUntrustedMetadata", [&] { tests.digestDoesNotAuthorizeUntrustedMetadata(); });
    if (!offline)
        runner.run("stop.invalid-endpoint", [&] { tests.stopIsIdempotentWithoutStartingRadio(false); });
    runner.run("stop.disabled", [&] { tests.stopIsIdempotentWithoutStartingRadio(true); });
    return runner.result();
}
