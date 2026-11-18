#include "../TestWorkspace.h"
#include "iiSocietyHelper/Sync/Sync.h"
#include "TestLink.h"
#include "../../example/ManualStatusExecutor.h"
#include <SocietyDrive.h>
#include <SharedStorage.h>
#include <QDir>
#include <QFile>
#include "../SignalRecorder.h"
#include "../QtEventWait.h"
#include "../QtWarningCapture.h"
#include <QCoreApplication>
#include <QEvent>
#include <QScopeGuard>
#include <memory>
#include <QTemporaryDir>
#include <QUuid>
#include <QElapsedTimer>
#include <QThread>
#include <QLockFile>

using namespace iiSocietyHelper::Sync;
class ControllerTests : public QObject {
public:
    void transientOperationLockKeepsTheOriginalRequestPending() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/pending-lock-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));const QString scope(64,'a');
        Controller hosting({});hosting.open(root.path(),scope);CTEST_WAIT_DEFAULT(hosting.available());hosting.setPeers({"phone"},{});
        QLockFile lock(root.filePath(".society-sync/operation.lock"));lock.setStaleLockTime(0);CTEST_CHECK(lock.tryLock(3000));
        const QJsonObject envelope{{"op","society.sync"},{"protocol",2},{"namespaceVersion",1},{"scope",scope},
            {"token",QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"message",QJsonObject{{"action","drive.list"},{"container",iiSocietyContainer::SocietyDrive::open(root.path())->identifier()},{"path","files"}}}};
        CTEST_CHECK(hosting.handle("phone",envelope).value("pending").toBool());helper_ctest::processFor(25);
        CTEST_CHECK(hosting.handle("phone",envelope).value("pending").toBool());lock.unlock();
        QJsonObject reply;CTEST_WAIT_DEFAULT(!(reply=hosting.handle("phone",envelope)).value("pending").toBool());
        CTEST_CHECK(reply.value("ok").toBool());CTEST_CHECK(reply.value("result").toObject().value("ok").toBool());hosting.closeAndWait();
    }
    void hostSelectionRetriesTheLocalOperationLockWithoutBlockingTheCaller() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/primary-claim-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path())); const QString scope(64, 'a');
        Controller sync({}); sync.open(root.path(), scope); CTEST_WAIT_DEFAULT(sync.available());
        QLockFile lock(root.filePath(".society-sync/operation.lock")); lock.setStaleLockTime(0); CTEST_CHECK(lock.tryLock(3000));
        const auto priorTrace = qgetenv("SOCIETY_SYNC_TRACE");
        const auto restoreTrace = qScopeGuard([&] { priorTrace.isNull() ? qunsetenv("SOCIETY_SYNC_TRACE") : qputenv("SOCIETY_SYNC_TRACE", priorTrace); });
        qputenv("SOCIETY_SYNC_TRACE", "1");
        helper_ctest::ExpectedWarning expectedWarning(QStringLiteral("Society sync error: primary_host_unavailable"));
        QElapsedTimer elapsed; elapsed.start(); sync.claimPrimaryHost("desktop"); CTEST_CHECK(elapsed.elapsed() < 100);
        CTEST_WAIT_COMPARE(sync.errorString(), QString("primary_host_unavailable"));
        CTEST_CHECK(expectedWarning.seen());
        qunsetenv("SOCIETY_SYNC_TRACE");
        CTEST_CHECK(Replica::primaryHost(root.path(), scope).isEmpty());
        lock.unlock(); CTEST_WAIT_COMPARE(Replica::primaryHost(root.path(), scope), QString("desktop"));
        CTEST_WAIT_DEFAULT(sync.errorString().isEmpty()); sync.closeAndWait();
    }
    void failedStartupCanRetryTheSameContainer() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/startup-retry-XXXXXX")));
        const auto path = root.filePath("later.societycontainer"); const QString scope(64, 'a');
        Controller sync({}); sync.open(path, scope);
        CTEST_WAIT_DEFAULT(!sync.errorString().isEmpty()); CTEST_CHECK(!sync.available());
        CTEST_CHECK(QDir().mkpath(path));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(path));
        sync.open(path, scope);
        CTEST_WAIT_MESSAGE_DEFAULT(sync.available(), qPrintable(sync.errorString()));
        CTEST_CHECK(sync.errorString().isEmpty());
    }
    void remoteDownloadsOpenWriteAndCommitAsynchronously() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/async-download-XXXXXX")));
        QList<QPair<QString, QJsonObject>> requests;
        RemoteFiles remote([&](const QString &, const QJsonObject &payload) {
            const auto id = QUuid::createUuid().toString(); requests.append({id, payload}); return id;
        });
        remote.browse("desktop");
        remote.receive(requests.last().first, {{"ok", true}, {"entries", QJsonArray{}}}, "local");
        helper_ctest::SignalRecorder completed(&remote, &RemoteFiles::downloadFinished);
        const auto destination = QUrl::fromLocalFile(root.filePath("result"));
        remote.download("remote", destination);
        CTEST_COMPARE(requests.size(), 1); // Opening the destination cannot synchronously call the transport.
        CTEST_CHECK(remote.busy());
        CTEST_WAIT_COMPARE(requests.size(), 2);
        CTEST_COMPARE(requests.last().second.value("op").toString(), "stat");
        remote.receive(requests.last().first, {{"ok", true}, {"size", "6"}, {"version", "v1"}}, "local");
        CTEST_COMPARE(requests.last().second.value("op").toString(), "read");
        remote.receive(requests.last().first, {{"ok", true}, {"data", "YWJjZGVm"}, {"offset", "0"},
            {"size", "6"}, {"version", "v1"}, {"eof", true}}, "local");
        CTEST_CHECK(remote.busy()); CTEST_CHECK(completed.isEmpty());
        CTEST_WAIT_COMPARE(completed.size(), 1); CTEST_CHECK(!remote.busy());
        QFile file(destination.toLocalFile()); CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), "abcdef"); file.close();

        remote.download("remote", destination);
        CTEST_WAIT_COMPARE(requests.last().second.value("op").toString(), "stat");
        remote.receive(requests.last().first, {{"ok", true}, {"size", "6"}, {"version", "v1"}}, "local");
        remote.receive(requests.last().first, {{"ok", true}, {"data", "eHl6"}, {"offset", "0"},
            {"size", "6"}, {"version", "v1"}, {"eof", true}}, "local");
        CTEST_WAIT_DEFAULT(!remote.busy()); CTEST_COMPARE(completed.size(), 1);
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), "abcdef"); file.close();

        const auto sent = requests.size();
        remote.download("cancelled", destination); remote.reset();
        helper_ctest::processFor(30); CTEST_COMPARE(requests.size(), sent); CTEST_COMPARE(completed.size(), 1);
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), "abcdef");
    }
    void inspectionsAreQueuedReadOnlyAndDiscardSupersededContainers() {
        QTemporaryDir first((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/inspect-first-XXXXXX.societycontainer"))), second((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/inspect-next-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(first.path()));
        const auto drive = iiSocietyContainer::SocietyDrive::create(second.path()); CTEST_CHECK(drive);
        Controller sync({}); helper_ctest::SignalRecorder inspected(&sync, &Controller::containerInspected);
        auto inspectionOnOwner = std::make_shared<bool>(true);
        connect(&sync, &Controller::containerInspected, this, [this, inspectionOnOwner] { *inspectionOnOwner = *inspectionOnOwner && QThread::currentThread() == thread(); });
        sync.inspectContainer(first.path(), QString(64, 'a'));
        sync.inspectContainer(second.path(), QString(64, 'b'));
        CTEST_CHECK(inspected.isEmpty());
        CTEST_WAIT_COMPARE(inspected.size(), 1);
        CTEST_CHECK(*inspectionOnOwner);
        CTEST_COMPARE(inspected[0][0].toString(), second.path());
        CTEST_COMPARE(inspected[0][1].toString(), QString(64, 'b'));
        CTEST_COMPARE(inspected[0][3].toString(), drive->identifier());
        CTEST_CHECK(!QFileInfo::exists(second.filePath(".society-sync"))); // No replica/database creation for a UI snapshot.
        sync.inspectContainer(first.path(), {});
        sync.shutdownAsync();
        helper_ctest::processFor(20); CTEST_COMPARE(inspected.size(), 1);
        sync.open(second.path(), QString(64, 'b')); CTEST_CHECK(!sync.available());
    }
    void asynchronousShutdownDoesNotWaitForAnActiveScan() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/async-stop-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        QFile file(root.filePath("Models/large")); CTEST_CHECK(file.open(QIODevice::WriteOnly));
        CTEST_CHECK(file.resize(1024LL * 1024 * 1024)); file.close();
        auto sync = std::make_unique<Controller>(RequestSender{});
        helper_ctest::SignalRecorder verification(sync.get(), &Controller::verificationProgress);
        const QString scope(64, 'a'); sync->open(root.path(), scope); CTEST_WAIT_DEFAULT(sync->available());
        sync->setPeers({"trusted"}, {});
        CTEST_CHECK(sync->handle("trusted", {{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1}, {"scope", scope},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"message", QJsonObject{{"action", "changes"}, {"container", drive->identifier()}}}}).value("pending").toBool());
        CTEST_WAIT_DEFAULT(!verification.isEmpty());
        QElapsedTimer elapsed; elapsed.start(); sync->shutdownAsync(); sync.reset();
        CTEST_CHECK_MESSAGE(elapsed.elapsed() < 100, "Mobile shutdown waited for the replica worker");
        CTEST_WAIT_DEFAULT(!QFileInfo::exists(root.filePath(".society-sync/operation.lock")));
        Replica next; CTEST_CHECK(next.open(root.path(), scope));
    }
    void processHandoffDrainsAnActiveFileScan() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/handoff-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        QFile file(root.filePath("Models/large")); CTEST_CHECK(file.open(QIODevice::WriteOnly));
        CTEST_CHECK(file.resize(1024LL * 1024 * 1024)); file.close();
        Controller sync({}); helper_ctest::SignalRecorder verification(&sync, &Controller::verificationProgress);
        const QString scope(64, 'a'); sync.open(root.path(), scope); CTEST_WAIT_DEFAULT(sync.available());
        sync.setPeers({"trusted"}, {});
        const QJsonObject request{{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1}, {"scope", scope},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"message", QJsonObject{{"action", "changes"}, {"container", drive->identifier()}}}};
        CTEST_CHECK(sync.handle("trusted", request).value("pending").toBool());
        CTEST_WAIT_VERIFY(!verification.isEmpty(), 5000);
        sync.closeAndWait(); CTEST_CHECK(!sync.available());
        CTEST_CHECK(!QFileInfo::exists(root.filePath(".society-sync/operation.lock")));
        Replica successor; CTEST_CHECK_MESSAGE(successor.open(root.path(), scope), qPrintable(successor.errorString()));
        CTEST_CHECK(!sync.handle("trusted", request).value("ok").toBool());
    }
    void filesProjectionSupportsCreateReadAndPaginationWithoutExposingOtherSections() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/files-service-XXXXXX.societycontainer"))); CTEST_CHECK(root.isValid());
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        QFile model(root.filePath("Models/private")); CTEST_CHECK(model.open(QIODevice::WriteOnly)); model.write("model"); model.close();
        const auto files = filesHandler(root.path());
        const auto defaults = files("peer", {{"op", "list"}, {"path", ""}}).value("entries").toArray();
        CTEST_CHECK(defaults.isEmpty());
        CTEST_CHECK(!files("peer", {{"op", "list"}, {"path", "../Models"}}).value("ok").toBool());
        CTEST_CHECK(!files("peer", {{"op", "stat"}, {"path", QJsonArray{}}}).value("ok").toBool());
        CTEST_CHECK(!files("peer", {{"op", "list"}, {"cursor", "-1"}}).value("ok").toBool());
        CTEST_CHECK(files("peer", {{"op", "mkdir"}, {"path", "folder"}}).value("ok").toBool());
        CTEST_CHECK(!files("peer", {{"op", "mkdir"}, {"path", "folder"}}).value("ok").toBool());
        CTEST_CHECK(!files("peer", {{"op", "write"}, {"path", "missing/child"}, {"data", "Ynl0ZXM="}}).value("ok").toBool());
        CTEST_CHECK(files("peer", {{"op", "write"}, {"path", "folder/item"}, {"data", "Ynl0ZXM="}}).value("ok").toBool());
        CTEST_CHECK(!files("peer", {{"op", "write"}, {"path", "folder/item"}, {"data", "bmV3"}}).value("ok").toBool());
        const auto metadata = files("peer", {{"op", "stat"}, {"path", "folder/item"}});
        CTEST_CHECK(metadata.value("ok").toBool()); CTEST_COMPARE(metadata.value("size").toString(), "5");
        const auto data = files("peer", {{"op", "read"}, {"path", "folder/item"}, {"version", metadata.value("version")}, {"offset", "1"}});
        CTEST_CHECK(data.value("ok").toBool()); CTEST_COMPARE(QByteArray::fromBase64(data.value("data").toString().toLatin1()), QByteArray("ytes"));
        CTEST_CHECK(data.value("eof").toBool());
        CTEST_CHECK(!files("peer", {{"op", "read"}, {"path", "folder/item"}, {"version", "old"}}).value("ok").toBool());
        CTEST_CHECK(nativeTestLink(root.filePath("Models/private"), root.filePath("Files/link")));
        CTEST_CHECK(!files("peer", {{"op", "read"}, {"path", "link"}}).value("ok").toBool());
        for (int i = 0; i < 260; ++i) {
            QFile file(root.filePath("Files/" + QString::number(i))); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("data");
        }
        auto page = files("peer", {{"op", "list"}, {"path", ""}}); CTEST_CHECK(page.value("ok").toBool());
        CTEST_COMPARE(page.value("entries").toArray().size(), 256); CTEST_CHECK(!page.value("nextCursor").toString().isEmpty());
        auto next = files("peer", {{"op", "list"}, {"path", ""}, {"cursor", page.value("nextCursor")}});
        CTEST_COMPARE(next.value("entries").toArray().size(), 5); CTEST_CHECK(next.value("nextCursor").toString().isEmpty());
        for (const auto &entry : page.value("entries").toArray() + next.value("entries").toArray())
            CTEST_CHECK(entry.toObject().value("name").toString() != "link");
        CTEST_CHECK(removeNativeTestLink(root.filePath("Files/link")));
    }
    void endpointRequiresTheAuthenticatedPeerAndRejectsTokenMutation() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/endpoint-XXXXXX.societycontainer"))); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        Controller sync({}); sync.open(root.path(), QString(64, 'a')); CTEST_WAIT_DEFAULT(sync.available());
        QFile payload(root.filePath("Models/progress")); CTEST_CHECK(payload.open(QIODevice::WriteOnly));
        payload.write(QByteArray(2 * 1024 * 1024 + 51, 'p')); payload.close();
        helper_ctest::SignalRecorder verification(&sync, &Controller::verificationProgress);
        auto verificationOnOwner = std::make_shared<bool>(true);
        connect(&sync, &Controller::verificationProgress, this, [this, verificationOnOwner] { *verificationOnOwner = *verificationOnOwner && QThread::currentThread() == thread(); });
        QJsonObject message{{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1}, {"scope", QString(64, 'a')},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"message", QJsonObject{{"action", "changes"}, {"container", iiSocietyContainer::SocietyDrive::open(root.path())->identifier()}}}};
        CTEST_CHECK(!sync.handle("other", message).value("ok").toBool());
        sync.setPeers({"trusted"}, {});
        auto legacy = message; legacy.remove("namespaceVersion");
        CTEST_COMPARE(sync.handle("trusted", legacy).value("error"), "namespace_authority_protocol_required");
        auto forged = message; forged.insert("message", QJsonObject{{"action", "begin"}, {"entry", QJsonObject{{"revision", QString(64, 'f')}}}});
        CTEST_COMPARE(sync.handle("trusted", forged).value("error"), "namespace_proposal_required");
        auto otherScope = message; otherScope.insert("scope", QString(64, 'b'));
        CTEST_CHECK(!sync.handle("trusted", otherScope).value("ok").toBool());
        CTEST_CHECK(sync.handle("trusted", message).value("pending").toBool());
        CTEST_WAIT_DEFAULT(sync.handle("trusted", message).value("result").toObject().value("ok").toBool());
        CTEST_WAIT_DEFAULT(verification.size() >= 2);
        CTEST_CHECK(*verificationOnOwner);
        CTEST_COMPARE(verification.last()[1].toLongLong(), qint64(2 * 1024 * 1024 + 51));
        CTEST_COMPARE(verification.last()[1], verification.last()[2]);
        auto changed = message; changed.insert("message", QJsonObject{{"action", "changes"}, {"container", iiSocietyContainer::SocietyDrive::open(root.path())->identifier()}, {"after", "1"}});
        CTEST_COMPARE(sync.handle("trusted", changed).value("error").toString(), "different_request_token_reuse");
        sync.close(); CTEST_CHECK(!sync.available()); CTEST_CHECK(!sync.handle("trusted", message).value("ok").toBool());
    }
    void aClientSynchronizesBothDirectionsOverRealLocalTls() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/tls-host-XXXXXX.societycontainer"))), mobile((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/tls-client-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(mobile.path()));
        QFile file(root.filePath("Models/host.bin")); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write(QByteArray(700000, 'h')); file.close();
        QFile local(mobile.filePath("Files/mobile.txt")); CTEST_CHECK(local.open(QIODevice::WriteOnly)); local.write("mobile edit"); local.close();
        iiWebServer::LanPeer host, client;
        Controller hosting({}), syncing([&](const auto &peer, const auto &message) { return client.request(peer, message); });
        ManualStatusExecutor receiver;
        QList<int> hostStates, clientStates;
        auto hostWatch = hosting.status().subscribe(receiver, [&](auto event) { hostStates.append(event.value); });
        auto clientWatch = syncing.status().subscribe(receiver, [&](auto event) { clientStates.append(event.value); });
        CTEST_CHECK(hostWatch); CTEST_CHECK(clientWatch);
        syncing.setContentPolicy(Synchronizer::ContentPolicy::FullReplica);
        const auto files = filesHandler(root.path());
        CTEST_CHECK(host.startHost("desktop", "Desktop", [&](const auto &peer, const auto &message) {
            return message.value("op") == "society.sync" ? hosting.handle(peer, message) : files(peer, message);
        }, {"127.0.0.1"}, iiWebServer::Address::loopback()));
        CTEST_CHECK_MESSAGE(client.join(host.createOffer(), "phone", "Phone"), qPrintable(client.errorString()));
        CTEST_WAIT_MESSAGE_DEFAULT(client.connected(), qPrintable(client.errorString()));
        hosting.open(root.path(), QString(64, 'a')); syncing.open(mobile.path(), QString(64, 'a'));
        CTEST_WAIT_DEFAULT(hosting.available() && syncing.available());
        // The product obtains these peers from account-authenticated pairing.
        // This transport test supplies an isolated synthetic authorization set.
        hosting.setPeers({"phone"}, {});
        auto allTransportsLocal = std::make_shared<bool>(true);
        connect(&client, &iiWebServer::LanPeer::completed, &syncing, [&, allTransportsLocal](auto id, auto result, auto transport) {
            *allTransportsLocal = *allTransportsLocal && transport == "local"; syncing.receive(id, result);
        });
        helper_ctest::SignalRecorder completed(&syncing, &Controller::synchronized);
        syncing.setPeers({"desktop"}, {"desktop"});
        CTEST_WAIT_MESSAGE_TIMEOUT(completed.size() > 0, qPrintable(syncing.errorString()), 30000);
        receiver.drain();
        CTEST_CHECK(hostStates.contains(1)); CTEST_CHECK(hostStates.contains(2));
        CTEST_CHECK(clientStates.contains(1)); CTEST_CHECK(clientStates.contains(2));
        // A completed snapshot can precede the authority's next indexing batch.
        // Wait for the actual payload, not the first empty metadata round trip.
        CTEST_WAIT_MESSAGE_TIMEOUT(QFileInfo::exists(mobile.filePath("Models/host.bin")), qPrintable(syncing.errorString()), 30000);
        QFile copy(mobile.filePath("Models/host.bin")); CTEST_CHECK(copy.open(QIODevice::ReadOnly)); CTEST_COMPARE(copy.readAll(), QByteArray(700000, 'h'));
        CTEST_COMPARE(iiSocietyContainer::SocietyDrive::open(mobile.path())->identifier(), iiSocietyContainer::SocietyDrive::open(root.path())->identifier());
        CTEST_CHECK(!QFileInfo::exists(root.filePath("Files/mobile.txt")));
        CTEST_CHECK(local.open(QIODevice::WriteOnly)); local.write("mobile edit"); local.close();
        completed.clear();
        // A local filesystem edit must propagate without a UI/manual sync call.
        CTEST_WAIT_VERIFY(QFileInfo::exists(root.filePath("Files/mobile.txt")), 5000);
        QFile uploaded(root.filePath("Files/mobile.txt")); CTEST_CHECK(uploaded.open(QIODevice::ReadOnly)); CTEST_COMPARE(uploaded.readAll(), QByteArray("mobile edit"));
        uploaded.close();
        // Host file visibility precedes the canonical revision round trip.
        // Measure each idle filesystem event after that round trip completes;
        // NamespaceTests separately edits during the upload acknowledgement.
        CTEST_WAIT_VERIFY(!completed.isEmpty(), 30000); completed.clear();
        CTEST_CHECK(local.open(QIODevice::WriteOnly | QIODevice::Truncate)); local.write("same file changed"); local.close();
        CTEST_WAIT_VERIFY(([&] { QFile f(root.filePath("Files/mobile.txt")); return f.open(QIODevice::ReadOnly) && f.readAll() == "same file changed"; })(), 1800);
        CTEST_WAIT_VERIFY(!completed.isEmpty(), 30000);
        QFile hostEdit(root.filePath("Files/host-edit")); CTEST_CHECK(hostEdit.open(QIODevice::WriteOnly)); hostEdit.write("host event"); hostEdit.close();
        CTEST_WAIT_VERIFY(QFileInfo::exists(mobile.filePath("Files/host-edit")), 5000);
        CTEST_CHECK(!client.hosting());
        CTEST_CHECK(!files("phone", {{"op", "list"}, {"path", "../Models"}}).value("ok").toBool());
        CTEST_CHECK(*allTransportsLocal);
        syncing.close(); hosting.close(); client.stop(); host.stop();
    }
    void anIdleHostArchivesExternalFilesAfterPublishingItsNamespace() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/provider-intake-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        QFile file(root.filePath("Files/external.txt")); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("external input"); file.close();
        Controller controller({}); controller.open(root.path(), QString(64, 'a'));
        CTEST_WAIT_DEFAULT(controller.available());
        const auto storage = iiSocietyContainer::SharedStorage::open(root.path()); CTEST_CHECK(storage);
        QJsonObject tracked;
        CTEST_WAIT_VERIFY((tracked = storage->fileTracking(iiSocietyContainer::StoreSection::Files, "external.txt")).value("status") == "ready", 30000);
        CTEST_CHECK(!tracked.value("rawKey").toString().isEmpty());
        CTEST_CHECK(!tracked.value("sessionKey").toString().isEmpty());
        controller.close();
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    ControllerTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("transientOperationLockKeepsTheOriginalRequestPending", [&] { FlushDeferredDeletes cleanup; tests.transientOperationLockKeepsTheOriginalRequestPending(); });
    runner.run("hostSelectionRetriesTheLocalOperationLockWithoutBlockingTheCaller", [&] { FlushDeferredDeletes cleanup; tests.hostSelectionRetriesTheLocalOperationLockWithoutBlockingTheCaller(); });
    runner.run("failedStartupCanRetryTheSameContainer", [&] { FlushDeferredDeletes cleanup; tests.failedStartupCanRetryTheSameContainer(); });
    runner.run("remoteDownloadsOpenWriteAndCommitAsynchronously", [&] { FlushDeferredDeletes cleanup; tests.remoteDownloadsOpenWriteAndCommitAsynchronously(); });
    runner.run("inspectionsAreQueuedReadOnlyAndDiscardSupersededContainers", [&] { FlushDeferredDeletes cleanup; tests.inspectionsAreQueuedReadOnlyAndDiscardSupersededContainers(); });
    runner.run("asynchronousShutdownDoesNotWaitForAnActiveScan", [&] { FlushDeferredDeletes cleanup; tests.asynchronousShutdownDoesNotWaitForAnActiveScan(); });
    runner.run("processHandoffDrainsAnActiveFileScan", [&] { FlushDeferredDeletes cleanup; tests.processHandoffDrainsAnActiveFileScan(); });
    runner.run("filesProjectionSupportsCreateReadAndPaginationWithoutExposingOtherSections", [&] { FlushDeferredDeletes cleanup; tests.filesProjectionSupportsCreateReadAndPaginationWithoutExposingOtherSections(); });
    runner.run("endpointRequiresTheAuthenticatedPeerAndRejectsTokenMutation", [&] { FlushDeferredDeletes cleanup; tests.endpointRequiresTheAuthenticatedPeerAndRejectsTokenMutation(); });
    runner.run("aClientSynchronizesBothDirectionsOverRealLocalTls", [&] { FlushDeferredDeletes cleanup; tests.aClientSynchronizesBothDirectionsOverRealLocalTls(); });
    runner.run("anIdleHostArchivesExternalFilesAfterPublishingItsNamespace", [&] { FlushDeferredDeletes cleanup; tests.anIdleHostArchivesExternalFilesAfterPublishingItsNamespace(); });
    return runner.result();
}
