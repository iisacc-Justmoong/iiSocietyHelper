#include "../CTestSupport.h"
#include "../QtEventWait.h"
#include "../SignalRecorder.h"
#include <QCoreApplication>
#include "../TestWorkspace.h"
#include "iiSocietyHelper/Sync/Sync.h"
#include <LanPeer.h>
#include <SocietyDrive.h>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QCryptographicHash>

using namespace iiSocietyHelper::Sync;
class SocketTransferTests : public QObject {
    static QByteArray digest(const QString &path) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
        QCryptographicHash hash(QCryptographicHash::Sha256); if (!hash.addData(&file)) return {}; return hash.result();
    }
    static void put(const QString &path, const QByteArray &bytes) {
        QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(bytes), bytes.size());
    }
public:
    void tlsBinaryMirrorResumeUploadAndRevocation() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/socket-client-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/socket-host-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        QByteArray bytes(8 * 1024 * 1024 + 17, Qt::Uninitialized);
        for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = char((i * 37 + i / 251) % 253);
        put(b.filePath("Models/bulk"), bytes); const auto expected = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        iiWebServer::LanPeer hostPeer, clientPeer;
        Controller host({}), client([&](const auto &peer, const auto &request) { return clientPeer.request(peer, request); });
        client.setContentPolicy(Synchronizer::ContentPolicy::FullReplica);
        connect(&clientPeer, &iiWebServer::LanPeer::completed, &client, [&](const auto &id, const auto &result, auto) { client.receive(id, result); });
        const QString scope(64, 'a'); host.open(b.path(), scope); client.open(a.path(), scope);
        CTEST_WAIT_DEFAULT(host.available() && client.available()); host.setPeers({"phone"}, {});
        CTEST_CHECK(hostPeer.startHost("desktop", "Desktop", [&](const auto &peer, const auto &request) {
            return request.value("op") == "society.sync" ? host.handle(peer, request) : QJsonObject{{"ok", true}, {"entries", QJsonArray()}};
        }, {"127.0.0.1"}, iiWebServer::Address::loopback()));
        CTEST_CHECK(clientPeer.join(hostPeer.createOffer(), "phone", "Phone")); CTEST_WAIT_DEFAULT(clientPeer.connected()); CTEST_CHECK(clientPeer.binaryTransferEnabled());
        bool stopped = false;
        const auto stop = connect(&client, &Controller::progress, &client, [&](auto, qint64 done, auto) {
            if (!stopped && done >= Replica::ChunkBytes) { stopped = true; client.close(); }
        });
        client.setPeers({"desktop"}, {"desktop"}); CTEST_WAIT_VERIFY(stopped, 10000); disconnect(stop);
        client.closeAndWait(); CTEST_CHECK(!QFileInfo::exists(a.filePath("Models/bulk")));
        client.open(a.path(), scope); CTEST_WAIT_DEFAULT(client.available());
        helper_ctest::SignalRecorder synchronized(&client, &Controller::synchronized); QElapsedTimer clock; clock.start();
        client.setPeers({"desktop"}, {"desktop"});
        CTEST_WAIT_VERIFY(!synchronized.isEmpty(), 30000);
        CTEST_COMPARE(digest(a.filePath("Models/bulk")), expected);
        qInfo("TCP/TLS binary resumed download bytes=%lld elapsed_ms=%lld sha256=%s", qlonglong(bytes.size()), clock.elapsed(), expected.toHex().constData());
        put(a.filePath("Files/upload"), bytes); synchronized.clear(); clock.restart(); client.synchronizeNow();
        CTEST_WAIT_COMPARE_TIMEOUT(digest(b.filePath("Files/upload")), expected, 30000);
        qInfo("TCP/TLS binary upload bytes=%lld elapsed_ms=%lld sha256=%s", qlonglong(bytes.size()), clock.elapsed(), expected.toHex().constData());
        host.setPeers({}, {});
        const auto denied = host.handle("phone", {{"op", "society.sync"}, {"protocol", 2}, {"scope", scope}});
        CTEST_CHECK(!denied.value("ok").toBool());
        client.closeAndWait(); host.closeAndWait();
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner;
    SocketTransferTests tests;
    runner.run("tlsBinaryMirrorResumeUploadAndRevocation", [&] { tests.tlsBinaryMirrorResumeUploadAndRevocation(); });
    return runner.result();
}
