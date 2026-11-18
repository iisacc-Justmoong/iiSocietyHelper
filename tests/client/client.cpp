#include "../TestWorkspace.h"
#include <iiSocietyHelper/Client.h>
#include <iiSocietyHelper/FileDownload.h>
#include <iiSocietyHelper/FileHost.h>
#include <iiSocietyHelper/LocalClient.h>
#include <iiSocietyHelper/LocalService.h>
#include <iiSocietyHelper.h>
#include <iiSocietyHelper/GroupSessionStore.h>
#include <QScopeGuard>
#include <iiSocietyHelper/PairingCredentials.h>
#include <iiSocietyHelper/AccountSession.h>
#include <iiSocietyHelper/NearbyDevices.h>
#include <iiSocietyHelper/AutomaticPairing.h>
#include <iiSocietyHelper/Sync/Sync.h>
#include <Container.h>
#include <StorageMap.h>
#include "AccountServer.h"
#include "MemorySessionStore.h"
#include "FakeDiscoveryService.h"
#include "PairingCredentialsFixture.h"
#include "../../src/Client/LocalFrame.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QLockFile>
#include <QLocalSocket>
#include "../SignalRecorder.h"
#include "../QtEventWait.h"
#include <QCoreApplication>
#include <QEvent>
#include <QTemporaryDir>
#include <QImage>
#include <QBuffer>
#include <filesystem>
#include <utility>
#include <iiFileProviderPresentation.h>
class ClientTests : public QObject {
    template <typename Recorder>
    static QJsonObject driveResponse(Recorder &responses, const QString &id) {
        const auto find = [&]() {
            for (const auto &row : responses) if (row[0].toString() == id) return row[1].toJsonObject();
            return QJsonObject();
        };
        if (id.isEmpty() || !helper_ctest::waitFor([&] { return !find().isEmpty(); }, 30000))
            return QJsonObject{{"ok", false}, {"error", "test_response_timeout"}};
        return find();
    }
    void verifyNativeFileDownloads(AccountSession &account, FakeDiscoveryService &discovery,
                                   FakeDiscoveryService &hostDiscovery,
                                   const QTemporaryDir &hostRoot, const QTemporaryDir &replicaRoot) {
        iiSocietyHelper::FileSystem files; CTEST_CHECK(files.open(replicaRoot.path()));
        FakeDiscoveryService otherDiscovery;
        otherDiscovery.publishDepartures = true;
        iiSocietyHelper::Client other(&account, &otherDiscovery, QHostAddress::LocalHost);
        QTimer announcements;
        if (account.serverConfiguration().value("url").toString().isEmpty()) {
            connect(&announcements, &QTimer::timeout, this, [&] {
                if (!otherDiscovery.record.isEmpty()) otherDiscovery.announceTo(hostDiscovery);
                if (!hostDiscovery.record.isEmpty()) hostDiscovery.announceTo(otherDiscovery);
            });
            announcements.start(50);
        }
        CTEST_CHECK(other.connectHost()); CTEST_WAIT_VERIFY(other.connected(), 10000);
        helper_ctest::SignalRecorder otherResponses(&other, &iiSocietyHelper::Client::hostDriveResponse);
        iiSocietyHelper::Client client(&account, &discovery, QHostAddress::LocalHost);
        QTemporaryDir serviceRoot((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/local-service-XXXXXX"))); CTEST_CHECK(serviceRoot.isValid());
        iiSocietyHelper::LocalService service(&account, [](const QString &product, const QString &app) {
            return product == "dreamscapes" && app == "com.iisacc.dreamscapes";
        }, &discovery, QHostAddress::LocalHost);
        CTEST_CHECK(service.listen(serviceRoot.path()));
        iiSocietyHelper::LocalClient transport("dreamscapes", "com.iisacc.dreamscapes",
            account.manager()->deviceInfo().value("id").toString(), service.path());
        transport.setExpectedSubject(account.manager()->account()->sub());
        helper_ctest::SignalRecorder transferResponses(&transport, &iiSocietyHelper::FileHost::hostDriveResponse);
        iiSocietyHelper::FileDownload download(&files, transport);
        helper_ctest::SignalRecorder ready(&download, &iiSocietyHelper::FileDownload::ready);
        const auto id = files.containerId();
        // A different app may own its normal sync session. Explicit hydration
        // must still complete within this process without a queued disk request.
        QLockFile otherSession(replicaRoot.filePath(".society-sync/client-session.lock"));
        otherSession.setStaleLockTime(0); CTEST_CHECK(otherSession.tryLock());
        const auto queuedBefore = QDir(replicaRoot.filePath(".society-sync/requests")).entryList({"*.json"}, QDir::Files);
        // The application's per-request deadline remains 30 seconds; the
        // complete transfer may legitimately require several such requests.
        const auto wait = [&] { return helper_ctest::waitFor([&] { return !download.active(); }, 120000); };
        const auto contents = [](const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; };
        {
            iiSocietyHelper::FileDownload legacy(&files, &client);
            CTEST_CHECK(legacy.start("files/legacy.bin", id));
            CTEST_CHECK(helper_ctest::waitFor([&] { return !legacy.active(); }, 120000));
            CTEST_COMPARE(legacy.state().value("state").toString(), "ready");
            CTEST_COMPARE(contents(replicaRoot.filePath("Files/legacy.bin")), QByteArray(300123, 'l'));
        }
        for (const auto &key : {QString("files/nested % #/document.iisc"), QString("generation-history/download.png")}) {
            const auto relative = iiSocietyContainer::StorageMap::physicalPath(key);
            CTEST_CHECK(!QFileInfo::exists(replicaRoot.filePath(relative)));
            CTEST_CHECK(download.start(key, id)); CTEST_CHECK(download.start(key, id));
            CTEST_CHECK_MESSAGE(wait(), qPrintable(QJsonDocument::fromVariant(download.state()).toJson() + client.status().toUtf8()));
            CTEST_CHECK_MESSAGE(download.state().value("state").toString() == "ready",
                     qPrintable(QJsonDocument::fromVariant(download.state()).toJson(QJsonDocument::Compact)
                                + " client=" + QJsonDocument(transport.hostBinding()).toJson(QJsonDocument::Compact)
                                + " connected=" + QByteArray::number(transport.connected())
                                + " responses=" + QByteArray::number(transferResponses.size())
                                + " other=" + QByteArray::number(other.connected())));
            CTEST_CHECK(!ready.isEmpty()); const auto file = ready.last().first().toMap();
            CTEST_COMPARE(file.value("key").toString(), key); CTEST_CHECK(file.value("available").toBool());
            CTEST_COMPARE(file.value("source").toUrl().toLocalFile(), replicaRoot.filePath(relative));
            CTEST_COMPARE(contents(file.value("path").toString()), contents(hostRoot.filePath(relative)));
            CTEST_CHECK(files.container()->alias(key).value("available").toBool());
            CTEST_CHECK(other.connected());
            CTEST_CHECK(driveResponse(otherResponses, other.statHostEntry("models/model.safetensors")).value("ok").toBool());
        }
        CTEST_CHECK(transferResponses.size() >= 6);
        const auto beforeCancel = ready.size();
        bool cancelAfterChunk = true;
        const auto cancellation = connect(&download, &iiSocietyHelper::FileDownload::changed, this, [&] {
            if (cancelAfterChunk && download.active() && download.state().value("completed").toLongLong() > 0) {
                cancelAfterChunk = false; download.cancel();
            }
        });
        CTEST_CHECK(download.start("files/cancel.bin", id)); CTEST_CHECK(wait());
        CTEST_COMPARE(download.state().value("state").toString(), "cancelled"); CTEST_COMPARE(ready.size(), beforeCancel);
        CTEST_CHECK(!QFileInfo::exists(replicaRoot.filePath("Files/cancel.bin")));
        disconnect(cancellation);
        CTEST_CHECK(download.start("files/cancel.bin", id)); CTEST_CHECK(wait());
        CTEST_COMPARE(download.state().value("state").toString(), "ready");
        CTEST_COMPARE(contents(replicaRoot.filePath("Files/cancel.bin")), QByteArray(900123, 'c'));
        const auto beforeCorrupt = ready.size();
        QFile changed(hostRoot.filePath("Files/corrupt.bin")); CTEST_CHECK(changed.open(QIODevice::WriteOnly));
        CTEST_COMPARE(changed.write(QByteArray(900123, 'x')), 900123); changed.close();
        CTEST_CHECK(download.start("files/corrupt.bin", id)); CTEST_CHECK(wait());
        CTEST_COMPARE(download.state().value("state").toString(), "failed"); CTEST_COMPARE(ready.size(), beforeCorrupt);
        CTEST_COMPARE(download.state().value("error").toString(), "incomplete_or_corrupt_transfer");
        CTEST_CHECK(!QFileInfo::exists(replicaRoot.filePath("Files/corrupt.bin")));
        CTEST_CHECK(!download.start("../outside", id));
        CTEST_CHECK(!download.start("files/cancel.bin", "stale-container"));
        CTEST_CHECK(download.start("files/switch.bin", id));
        QTemporaryDir second((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/download-switch-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(second.path())); CTEST_CHECK(files.open(second.path()));
        CTEST_CHECK(!download.active()); CTEST_COMPARE(download.state().value("state").toString(), "cancelled");
        helper_ctest::processFor(100); CTEST_COMPARE(ready.size(), beforeCorrupt);
        CTEST_COMPARE(QDir(replicaRoot.filePath(".society-sync/requests")).entryList({"*.json"}, QDir::Files), queuedBefore);
        // Ordinary resident files do not need login, sockets, or any Client.
        QFile local(second.filePath("Files/local.txt")); CTEST_CHECK(local.open(QIODevice::WriteOnly)); local.write("local"); local.close();
        iiSocietyHelper::FileDownload offline(&files, nullptr); helper_ctest::SignalRecorder localReady(&offline, &iiSocietyHelper::FileDownload::ready);
        CTEST_CHECK(offline.start("files/local.txt", files.containerId())); CTEST_WAIT_COMPARE(localReady.size(), 1);
        CTEST_COMPARE(offline.state().value("state").toString(), "ready");
    }
    void verifyDirectHostAccess(AccountSession &account, FakeDiscoveryService &discovery, const QTemporaryDir &hostRoot,
                                int &inspectionFaults, int &injectedFaults) {
        iiSocietyHelper::Client client(&account, &discovery, QHostAddress::LocalHost);
        const auto mirror = client.container(); CTEST_CHECK(mirror); CTEST_CHECK(!mirror->available());
        CTEST_CHECK(client.metaObject()->indexOfProperty("container") >= 0);
        helper_ctest::SignalRecorder responses(&client, &iiSocietyHelper::Client::hostDriveResponse);
        CTEST_CHECK(client.connectHost());
        CTEST_WAIT_VERIFY(client.connected(), 10000);
        CTEST_WAIT_MESSAGE_TIMEOUT(mirror->available(), qPrintable(mirror->errorString() + ' ' + client.status()), 10000);
        CTEST_COMPARE(client.container(), mirror);
        CTEST_COMPARE(client.hostSections().size(), 9);
        CTEST_COMPARE(mirror->containerId(), iiSocietyContainer::SocietyDrive::open(hostRoot.path())->identifier());
        CTEST_CHECK(mirror->rootPath().startsWith("society://"));
        const auto alias = mirror->alias("models/model.safetensors");
        CTEST_CHECK(!alias.isEmpty()); CTEST_CHECK(alias.value("exists").toBool());
        CTEST_CHECK(!alias.value("available").toBool()); CTEST_CHECK(alias.value("source").toUrl().isEmpty());
        const auto response = [&](const QString &id) {
            return driveResponse(responses, id);
        };
        const auto listed = response(client.listHostDirectory("models"));
        CTEST_CHECK_MESSAGE(listed.value("ok").toBool(), qPrintable(listed.value("error").toString()));
        CTEST_CHECK(!listed.value("entries").toArray().isEmpty());
        const auto stat = response(client.statHostEntry("models/model.safetensors"));
        CTEST_CHECK(stat.value("ok").toBool()); CTEST_COMPARE(stat.value("size").toString().toLongLong(), 800123);
        CTEST_CHECK(alias.value("informationAvailable").toBool());
        const auto inspected = response(client.inspectHostFile("models/model.safetensors"));
        CTEST_CHECK_MESSAGE(inspected.value("ok").toBool(), qPrintable(inspected.value("error").toString()));
        const auto information = inspected.value("information").toObject();
        CTEST_COMPARE(information.value("format").toString(), "iiFileProvider.FileInformation");
        CTEST_COMPARE(information.value("details").toObject().value("size").toInteger(), 800123);
        CTEST_CHECK(!information.value("preview").toObject().value("data").toString().isEmpty());
        CTEST_CHECK(!information.contains("rootPath"));
        QImage image(512, 512, QImage::Format_RGB32); quint32 random = 7;
        for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            image.setPixel(x, y, 0xff000000u | (random & 0x00ffffff));
        }
        QByteArray png; QBuffer output(&png); CTEST_CHECK(output.open(QIODevice::WriteOnly)); CTEST_CHECK(image.save(&output, "PNG"));
        const iiFileProvider::native::FilePreview preview{"image/png", "image", png.toStdString(), 512, 512};
        (void)iiFileProvider::native::Presentation::attach(std::filesystem::path(hostRoot.filePath("Models/model.safetensors").toStdU16String()), preview);
        inspectionFaults = 1;
        const auto rich = response(client.inspectHostFile("models/model.safetensors"));
        CTEST_CHECK_MESSAGE(rich.value("ok").toBool(), qPrintable(rich.value("error").toString()));
        CTEST_COMPARE(injectedFaults, 1);
        const auto remotePreview = rich.value("information").toObject().value("preview").toObject();
        CTEST_COMPARE(remotePreview.value("kind").toString(), "image");
        CTEST_CHECK(remotePreview.value("data").toString().size() > iiSocietyHelper::Sync::Replica::ChunkBytes);
        CTEST_COMPARE(QByteArray::fromBase64(remotePreview.value("data").toString().toLatin1()), png);
        inspectionFaults = 100; const auto beforeFailures = injectedFaults;
        const auto unstable = response(client.inspectHostFile("models/model.safetensors"));
        CTEST_COMPARE(unstable.value("error").toString(), QStringLiteral("drive_information_changed"));
        CTEST_COMPARE(injectedFaults - beforeFailures, 3);
        inspectionFaults = 0;
        auto busy = std::make_shared<QLockFile>(hostRoot.filePath(".society-sync/operation.lock"));
        busy->setStaleLockTime(0); CTEST_CHECK(busy->tryLock(2000));
        QTimer::singleShot(300, &client, [busy] { busy->unlock(); });
        const auto deferred = response(client.statHostEntry("models/model.safetensors"));
        CTEST_CHECK_MESSAGE(deferred.value("ok").toBool(), qPrintable(deferred.value("error").toString()));
        const auto version = stat.value("version").toString(); QByteArray data;
        while (data.size() < 800123) {
            const auto read = response(client.readHostFile("models/model.safetensors", data.size(), version));
            CTEST_CHECK_MESSAGE(read.value("ok").toBool(), qPrintable(read.value("error").toString()));
            const auto chunk = QByteArray::fromBase64(read.value("data").toString().toLatin1());
            CTEST_CHECK(!chunk.isEmpty()); data += chunk;
        }
        CTEST_COMPARE(data, QByteArray(800123, 'm'));
        CTEST_CHECK(response(client.createHostDirectory("files/direct" )).value("ok").toBool());
        const QByteArray created("created through iiSocietyHelper");
        CTEST_CHECK(response(client.writeHostFile("files/direct/from-app.txt", created)).value("ok").toBool());
        QFile actual(hostRoot.filePath("Files/direct/from-app.txt")); CTEST_CHECK(actual.open(QIODevice::ReadOnly)); CTEST_COMPARE(actual.readAll(), created);
        CTEST_CHECK(!response(client.writeHostFile("files/direct/from-app.txt", "replace")).value("ok").toBool());
        CTEST_CHECK(client.statHostEntry("../outside").isEmpty());
        CTEST_CHECK(client.inspectHostFile("files/private.iisacc.json").isEmpty());
        CTEST_CHECK(client.readHostFile("files/.iisacc-temporary.lock").isEmpty());
        CTEST_CHECK(client.writeHostFile("files/oversize", QByteArray(iiSocietyHelper::Sync::Replica::ChunkBytes + 1, 'a')).isEmpty());
        mirror->refresh();
        CTEST_WAIT_VERIFY(!mirror->alias("files/direct/from-app.txt").isEmpty(), 10000);
        CTEST_CHECK(mirror->alias("files/direct").isEmpty());
        CTEST_CHECK(mirror->alias("files/direct/from-app.txt").value("modified").toDateTime().isValid());
        CTEST_COMPARE(mirror->recentFiles().first().toMap().value("key").toString(), "files/direct/from-app.txt");
        const auto pending = client.statHostEntry("models/model.safetensors"); CTEST_CHECK(!pending.isEmpty());
        const auto pendingInspection = client.inspectHostFile("models/model.safetensors"); CTEST_CHECK(!pendingInspection.isEmpty());
        client.stop();
        CTEST_CHECK(!client.connected()); CTEST_CHECK(!mirror->available()); CTEST_CHECK(mirror->aliases().isEmpty()); CTEST_CHECK(client.hostSections().isEmpty());
        CTEST_COMPARE(response(pending).value("error").toString(), "host_disconnected");
        CTEST_COMPARE(response(pendingInspection).value("error").toString(), "host_disconnected");
        CTEST_CHECK(client.listHostDirectory().isEmpty());
    }
public:
    void automaticPairingIdleDoesNotPoll() {
        FakeDiscoveryService discovery;
        NearbyDevices nearby(&discovery, QHostAddress::LocalHost);
        iiWebServer::LanPeer peer;
        AutomaticPairing pairing(&nearby, &peer, [] { return false; }, [](const QString &) { return false; }, [] {});
        auto *timer = pairing.findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        CTEST_CHECK(timer);
        CTEST_CHECK(timer->isSingleShot());
        QCoreApplication::processEvents();
        CTEST_CHECK(!timer->isActive());
        pairing.setEnabled(false);
        CTEST_CHECK(!timer->isActive());
    }
    void automaticPairingRetriesAtDeadline() {
        FakeDiscoveryService hostDiscovery, phoneDiscovery;
        phoneDiscovery.publishDepartures = true;
        NearbyDevices host(&hostDiscovery, QHostAddress::LocalHost);
        NearbyDevices phone(&phoneDiscovery, QHostAddress::LocalHost);
        iiWebServer::LanPeer peer;
        int attempts = 0;
        AutomaticPairing pairing(&host, &peer, [&] { ++attempts; return false; }, [](const QString &) { return false; }, [] {});
        const auto scope = QString(64, 'a');
        const auto credentials = pairingCredentialsFixture(scope);
        host.setIdentity(scope, "host", "Host", "pc", true);
        phone.setIdentity(scope, "phone", "Phone", "phone", false);
        host.setCredentials(credentials, true, "host");
        phone.setCredentials(credentials, false, "host");
        CTEST_CHECK(host.authenticated()); CTEST_CHECK(phone.authenticated());
        phoneDiscovery.announceTo(hostDiscovery);
        CTEST_WAIT_VERIFY(attempts >= 2, 3000);
        phoneDiscovery.stop();
        auto *timer = pairing.findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        CTEST_CHECK(timer);
        CTEST_WAIT_DEFAULT(!timer->isActive());
    }
    void applicationsOnTheRegisteredHostUseItsNativeDriveWithoutASelfPeer() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/same-device-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        QFile model(root.filePath("Models/registered.bin")); CTEST_CHECK(model.open(QIODevice::WriteOnly)); model.write("indexed model"); model.close();
        AccountServer authority; CTEST_CHECK(authority.server.listen(QHostAddress::LocalHost));
        const QString hostId(64, '1');
        authority.profile.insert("societyContainerDrive", QJsonObject{{"hostDeviceId", hostId}, {"containerId", drive->identifier()},
            {"imagePath", root.path()}, {"revision", "1d02e288-9704-4c5a-979e-f9b60d1799ca"}});
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        auto device = account.manager()->deviceInfo(); device.insert("id", hostId); CTEST_CHECK(account.manager()->setDeviceInfo(device));
        CTEST_CHECK(account.login("builder@example.com", "FixtureOnly1!")); CTEST_WAIT_DEFAULT(account.signedIn() && !account.busy());
        {
            iiSocietyHelper::Sync::Replica owner; CTEST_CHECK(owner.open(root.path(), authority.scope())); CTEST_CHECK(owner.scan());
        }
        CTEST_CHECK(iiSocietyHelper::Sync::Replica::claimPrimaryHost(root.path(), authority.scope(), hostId));
        QFile unindexed(root.filePath("Files/not-indexed.txt")); CTEST_CHECK(unindexed.open(QIODevice::WriteOnly)); unindexed.write("live native data"); unindexed.close();
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto restore = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", previous); });
        QTemporaryDir unrelated((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/unrelated-host-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(unrelated.path()));
        qputenv("SOCIETY_CONTAINER_PATH", unrelated.path().toUtf8());
        FakeDiscoveryService discovery; iiSocietyHelper::Client client(&account, &discovery, QHostAddress::LocalHost);
        helper_ctest::SignalRecorder responses(&client, &iiSocietyHelper::Client::hostDriveResponse);
        CTEST_CHECK(client.connectHost()); CTEST_CHECK(!client.connected()); CTEST_CHECK(!client.container()->available());
        CTEST_CHECK(client.readHostFile("files/not-indexed.txt").isEmpty()); CTEST_COMPARE(discovery.starts, 0);
        CTEST_CHECK(!QFileInfo::exists(unrelated.filePath(".society-sync")));
        qputenv("SOCIETY_CONTAINER_PATH", root.path().toUtf8()); client.synchronizeNow();
        CTEST_WAIT_MESSAGE_DEFAULT(client.connected(), qPrintable(client.status()));
        CTEST_WAIT_MESSAGE_DEFAULT(client.container()->available(), qPrintable(client.container()->errorString()));
        CTEST_COMPARE(discovery.starts, 0); CTEST_COMPARE(client.hostSections().size(), 9);
        CTEST_CHECK(!client.container()->alias("models/registered.bin").isEmpty());
        const auto read = driveResponse(responses, client.readHostFile("files/not-indexed.txt"));
        CTEST_CHECK_MESSAGE(read.value("ok").toBool(), qPrintable(read.value("error").toString()));
        CTEST_COMPARE(QByteArray::fromBase64(read.value("data").toString().toLatin1()), QByteArray("live native data"));
        const auto write = driveResponse(responses, client.writeHostFile("files/from-local-app.txt", "local publication"));
        CTEST_CHECK_MESSAGE(write.value("ok").toBool(), qPrintable(write.value("error").toString()));
        QFile published(root.filePath("Files/from-local-app.txt")); CTEST_CHECK(published.open(QIODevice::ReadOnly)); CTEST_COMPARE(published.readAll(), QByteArray("local publication"));
        helper_ctest::processFor(150);
        iiSocietyHelper::Sync::Replica inspected; CTEST_CHECK(inspected.open(root.path(), authority.scope()));
        CTEST_CHECK(inspected.record("files/not-indexed.txt").isEmpty()); // No second background indexer.
        CTEST_COMPARE(discovery.starts, 0);
        CTEST_CHECK(client.requestHost({{"op", "society.generation"}}).isEmpty());
        CTEST_CHECK(account.logout()); CTEST_WAIT_DEFAULT(!account.signedIn());
        CTEST_CHECK(!client.connected()); CTEST_CHECK(!client.container()->available());
        client.stop();
    }
    void directHostConnectionRequiresASignedInAccount() {
        AccountServer authority; CTEST_CHECK(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        FakeDiscoveryService discovery; iiSocietyHelper::Client client(&account, &discovery, QHostAddress::LocalHost);
        CTEST_CHECK(!client.connectHost()); CTEST_CHECK(!client.container()->available());
    }
    void stateCreatedBeforeSelectionSavesAfterThePackageAppears() {
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/late-group-XXXXXX.societycontainer")));
        CTEST_CHECK(fixture.isValid());
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto cleanup = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", previous); });
        qputenv("SOCIETY_CONTAINER_PATH", fixture.path().toUtf8());
        MemorySessionStore keys;
        GroupSessionStore store({}, &keys, nullptr);
        CTEST_CHECK(store.directory().isEmpty());
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(fixture.path()));
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        store.write("late-selection-fixture", "synthetic record", [&](auto value) { result = value; done = true; });
        CTEST_WAIT_DEFAULT(done); CTEST_COMPARE(result.error, Store::Error::None);
        CTEST_COMPARE(store.directory(), fixture.filePath(".society-runtime/state"));
        done = false; store.read("late-selection-fixture", [&](auto value) { result = value; done = true; });
        CTEST_WAIT_DEFAULT(done); CTEST_COMPARE(result.data, QByteArray("synthetic record"));
    }

    void stateRemovalReportsLegacyFailure() {
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/group-remove-XXXXXX"))); CTEST_CHECK(fixture.isValid());
        MemorySessionStore keys, legacy; legacy.failRemove = true;
        GroupSessionStore store(fixture.path(), &keys, &legacy);
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        store.remove("synthetic", [&](auto value) { result = value; done = true; });
        CTEST_WAIT_DEFAULT(done); CTEST_COMPARE(result.error, Store::Error::Unavailable);
    }

    void sessionRecordRevisionTracksReplacementAndRevocation() {
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/group-revision-XXXXXX")));
        CTEST_CHECK(fixture.isValid());
        MemorySessionStore keys;
        GroupSessionStore first(fixture.path(), &keys, nullptr), second(fixture.path(), &keys, nullptr);
        CTEST_CHECK(first.recordRevision("account-session").isEmpty());
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        first.write("account-session", "session-a", [&](auto value) {result=value;done=true;});
        CTEST_WAIT_DEFAULT(done); CTEST_COMPARE(result.error, Store::Error::None);
        const auto initial=first.recordRevision("account-session");
        CTEST_COMPARE(initial.size(),32);CTEST_COMPARE(second.recordRevision("account-session"),initial);
        done=false;second.write("account-session", "session-b", [&](auto value) {result=value;done=true;});
        CTEST_WAIT_DEFAULT(done);CTEST_COMPARE(result.error,Store::Error::None);
        CTEST_CHECK(first.recordRevision("account-session")!=initial);
        done=false;second.remove("account-session", [&](auto value) {result=value;done=true;});
        CTEST_WAIT_DEFAULT(done);CTEST_COMPARE(result.error,Store::Error::None);
        CTEST_CHECK(first.recordRevision("account-session").isEmpty());
        // The provider may create preview sidecars while removing state files.
        // Remove only this test's freshly created private fixture tree.
        fixture.setAutoRemove(false);
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(fixture.path().toStdString()), error);
        CTEST_CHECK_MESSAGE(!error, qPrintable(QString::fromStdString(error.message())));
    }

    void stateRemovalAcceptsMissingLegacyEntry() {
        class MissingLegacy final : public MemorySessionStore {
            void remove(const QString &, Completion done) override { done({Error::Missing, {}}); }
        } legacy;
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/group-missing-XXXXXX"))); CTEST_CHECK(fixture.isValid());
        MemorySessionStore keys;
        GroupSessionStore store(fixture.path(), &keys, &legacy);
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        store.remove("synthetic", [&](auto value) { result = value; done = true; });
        CTEST_WAIT_DEFAULT(done); CTEST_COMPARE(result.error, Store::Error::None);
    }

    void stateRemovalReportsTombstoneFailure() {
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/group-tombstone-XXXXXX"))); CTEST_CHECK(fixture.isValid());
        MemorySessionStore keys;
        GroupSessionStore store(fixture.path(), &keys, nullptr);
        const auto name = QString::fromLatin1(QCryptographicHash::hash(QByteArray("synthetic"), QCryptographicHash::Sha256).toHex()) + ".state";
        const auto target = fixture.filePath("untouched-target");
        QFile file(target); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
        CTEST_CHECK(QFile::link(target, fixture.filePath(name)));
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        store.remove("synthetic", [&](auto value) { result = value; done = true; });
        CTEST_WAIT_DEFAULT(done); CTEST_COMPARE(result.error, Store::Error::Unavailable);
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), QByteArray("fixture"));
    }

    void savedHostRoutesReconnectWithoutBonjourAndRequireFreshProof() {
        FakeDiscoveryService hostService, firstService;
        NearbyDevices host(&hostService, QHostAddress::LocalHost), first(&firstService, QHostAddress::LocalHost);
        const QString scope(64, 'a');
        const auto credentials = localPairingCredentialsFixture(scope);
        host.setIdentity(scope, "host", "Host", "pc", true);
        host.setCredentials(credentials, true, "host");
        first.setIdentity(scope, "phone", "Phone", "phone", false);
        first.setCredentials(credentials, false, "host");
        hostService.announceTo(firstService);
        firstService.announceTo(hostService);
        const auto routes = first.connectionHints("host");
        CTEST_COMPARE(routes.size(), 1);
        const auto staleRecord = firstService.record;
        const auto stalePort = firstService.port;
        iiWebServer::LanPeer transport;
        CTEST_CHECK(transport.startHost("host", "Host", [](const auto &, const auto &) { return QJsonObject{}; },
            {"127.0.0.1"}, iiWebServer::Address::loopback()));
        CTEST_CHECK(host.invite("phone", transport.createDeviceOffer("phone"), true));
        helper_ctest::SignalRecorder retired(&host, &NearbyDevices::invitationEnded);
        first.clear();
        FakeDiscoveryService restoredService;
        NearbyDevices restored(&restoredService, QHostAddress::LocalHost);
        restored.setIdentity(scope, "phone", "Phone", "phone", false);
        restored.setCredentials(credentials, false, "host");
        restored.resumeHost("unregistered-host", routes);
        helper_ctest::processFor(50); CTEST_CHECK(restored.devices().isEmpty());
        restored.resumeHost("host", routes);
        CTEST_WAIT_COMPARE(restored.devices().size(), 1);
        CTEST_CHECK(restored.devices().first().toMap().value("verified").toBool());
        CTEST_WAIT_COMPARE(host.devices().size(), 1);
        CTEST_CHECK(host.devices().first().toMap().value("verified").toBool());
        CTEST_COMPARE(retired.size(), 1); // No 20-second wait for an old process invitation.
        // A delayed Bonjour callback must not replace the freshly challenged
        // process with the prior launch and restart its pairing timeout.
        emit hostService.found("delayed-bonjour", staleRecord, QHostAddress::LocalHost, stalePort);
        CTEST_COMPARE(host.connectionHints("phone"), QJsonArray({QJsonObject{
            {"address", "127.0.0.1"}, {"port", restoredService.port}}}));
        // Hints only route a fresh challenge. They never restore authentication.
        FakeDiscoveryService wrongService;
        NearbyDevices wrong(&wrongService, QHostAddress::LocalHost);
        wrong.setIdentity(scope, "other", "Other", "phone", false);
        wrong.setCredentials(localPairingCredentialsFixture(scope, 'f'), false, "host");
        wrong.resumeHost("host", routes);
        helper_ctest::processFor(150);
        CTEST_CHECK(wrong.devices().isEmpty());
        CTEST_COMPARE(host.devices().size(), 1);
        restored.clear();
        CTEST_CHECK(restored.connectionHints("host").isEmpty());
    }
    void hostConnectionIsPersistedAndBoundToTheAccountDrive() {
        AccountServer authority; CTEST_CHECK(authority.server.listen(QHostAddress::LocalHost));
        const QString host(64, 'a'), container("b413adf3-0642-4565-941f-aa1940d25c51");
        authority.profile.insert("societyContainerDrive", QJsonObject{{"hostDeviceId", host}, {"containerId", container},
            {"imagePath", "/Volumes/Society.sparsebundle"}, {"revision", "1d02e288-9704-4c5a-979e-f9b60d1799ca"}});
        MemorySessionStore state;
        const QJsonArray routes{QJsonObject{{"address", "192.168.1.5"}, {"port", 4000}}};
        {
            AccountSession account(authority.url(), &state, nullptr);
            CTEST_CHECK(account.login("builder@example.com", "FixtureOnly1!"));
            CTEST_WAIT_DEFAULT(account.signedIn() && !account.busy());
            CTEST_WAIT_DEFAULT(account.rememberHostConnection(host, container, routes));
            CTEST_COMPARE(account.savedHostConnection().value("routes").toArray(), routes);
            CTEST_CHECK(!account.rememberHostConnection("another-host", container, routes));
            helper_ctest::processFor(50);
        }
        AccountSession restored(authority.url(), &state, nullptr);
        CTEST_WAIT_DEFAULT(restored.signedIn());
        CTEST_WAIT_COMPARE(restored.savedHostConnection().value("routes").toArray(), routes);
        auto changedDrive = authority.profile.value("societyContainerDrive").toObject();
        changedDrive.insert("hostDeviceId", QString(64, 'b'));
        authority.profile.insert("societyContainerDrive", changedDrive);
        bool refreshed = false;
        CTEST_WAIT_DEFAULT(refreshed || (refreshed = restored.manager()->refreshContainerDrive()));
        CTEST_WAIT_DEFAULT(restored.savedHostConnection().isEmpty());
        CTEST_CHECK(restored.logout()); CTEST_WAIT_DEFAULT(!restored.signedIn());
        CTEST_CHECK(restored.savedHostConnection().isEmpty());
    }
    void nativeDiscoveryLifecycleWithoutNetwork() {
        QObject owner;
        auto *discovery = DiscoveryService::createNetwork(&owner);
        CTEST_CHECK(discovery);
        CTEST_COMPARE(discovery->parent(), &owner);
        discovery->stop();
        discovery->stop();
    }
    void localServiceEnforcesDeviceAndCurrentEntitlement() {
#if defined(Q_OS_UNIX)
        AccountServer authority; CTEST_CHECK(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        CTEST_CHECK(account.login("builder@example.com", "FixtureOnly1!")); CTEST_WAIT_DEFAULT(account.signedIn()); CTEST_WAIT_DEFAULT(!account.busy());
        QTemporaryDir root((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/broker-XXXXXX"))); CTEST_CHECK(root.isValid());
        bool authorized = true; int checks = 0;
        iiSocietyHelper::LocalService service(&account, [&](const QString &product, const QString &app) {
            ++checks;
            return authorized && product == "dreamscapes" && app == "com.iisacc.dreamscapes";
        });
        CTEST_CHECK(service.listen(root.path()));
        const auto hello = [&](const QString &device) { return iiSocietyHelper::LocalFrame::encode({{"op", "hello"},
            {"productId", "dreamscapes"}, {"appId", "com.iisacc.dreamscapes"}, {"deviceId", device}}); };
        QLocalSocket wrong; wrong.connectToServer(service.path()); CTEST_WAIT_COMPARE(wrong.state(), QLocalSocket::ConnectedState);
        CTEST_CHECK(wrong.write(hello(QString(64, 'f'))) > 0); wrong.flush(); CTEST_WAIT_COMPARE(wrong.state(), QLocalSocket::UnconnectedState);
        QLocalSocket socket; socket.connectToServer(service.path()); CTEST_WAIT_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        CTEST_CHECK(socket.write(hello(account.manager()->deviceInfo().value("id").toString())) > 0); socket.flush();
        iiSocietyHelper::LocalFrame responses; CTEST_WAIT_DEFAULT(socket.bytesAvailable() > 0);
        CTEST_CHECK(responses.append(socket.readAll()));
        QJsonObject ready;
        CTEST_WAIT_DEFAULT(([&] { if (!ready.isEmpty()) return true; if (auto value = responses.take()) { ready = *value; return true; }
            if (socket.bytesAvailable()) responses.append(socket.readAll()); return false; })());
        CTEST_COMPARE(ready.value("type").toString(), "ready");
        authorized = false;
        CTEST_CHECK(socket.write(iiSocietyHelper::LocalFrame::encode({{"op", "sync"}})) > 0); socket.flush();
        CTEST_WAIT_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
        authorized = true; const auto before = checks;
        iiSocietyHelper::LocalClient proxy("dreamscapes", "com.iisacc.dreamscapes",
            account.manager()->deviceInfo().value("id").toString(), service.path());
        proxy.setExpectedSubject("different-account"); CTEST_CHECK(proxy.observe());
        CTEST_WAIT_DEFAULT(checks > before); helper_ctest::processFor(100); CTEST_CHECK(!proxy.sessionBound());
        proxy.setExpectedSubject(account.manager()->account()->sub()); CTEST_WAIT_DEFAULT(proxy.sessionBound());
        helper_ctest::SignalRecorder boundChanges(&proxy, &iiSocietyHelper::LocalClient::sessionChanged);
        authorized = false; proxy.synchronizeNow(); CTEST_WAIT_DEFAULT(!proxy.sessionBound());
        CTEST_CHECK(!boundChanges.isEmpty()); CTEST_COMPARE(boundChanges.last().first().toBool(), false);
        service.stop();
#endif
    }
    void pairingRequestedDuringRestoreIsRetriedAfterRestore() {
        AccountServer authority; CTEST_CHECK(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; state.holdRead = true;
        AccountSession account(authority.url(), &state, nullptr);
        CTEST_CHECK(account.login("builder@example.com", "FixtureOnly1!")); CTEST_WAIT_DEFAULT(account.signedIn());
        CTEST_WAIT_DEFAULT(!account.busy()); CTEST_WAIT_DEFAULT(bool(state.releaseRead));
        account.requestPairingCredentials();
        CTEST_CHECK(account.pairingCredentials().isEmpty());
        const auto release = std::move(state.releaseRead); release();
        CTEST_WAIT_DEFAULT(!account.pairingCredentials().isEmpty());
        int requests = 0;
        for (const auto &request : authority.requests) if (request.value("intent") == "pairing") ++requests;
        CTEST_COMPARE(requests, 1);
    }
    void pairingRequestedDuringRestoreIsDiscardedOnLogout() {
        AccountServer authority; CTEST_CHECK(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; state.holdRead = true;
        AccountSession account(authority.url(), &state, nullptr);
        CTEST_CHECK(account.login("builder@example.com", "FixtureOnly1!")); CTEST_WAIT_DEFAULT(account.signedIn());
        CTEST_WAIT_DEFAULT(!account.busy()); CTEST_WAIT_DEFAULT(bool(state.releaseRead));
        account.requestPairingCredentials(); CTEST_CHECK(account.logout());
        const auto release = std::move(state.releaseRead); release();
        CTEST_WAIT_DEFAULT(!account.signedIn()); helper_ctest::processFor(100);
        CTEST_CHECK(account.pairingCredentials().isEmpty());
        for (const auto &request : authority.requests) CTEST_CHECK(request.value("intent") != "pairing");
    }
    void accountProofRejectsTamperingExpiryAndCrossAccount() {
        AccountServer authority; CTEST_CHECK(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        CTEST_CHECK(account.login("builder@example.com", "FixtureOnly1!")); CTEST_WAIT_DEFAULT(account.signedIn());
        CTEST_WAIT_DEFAULT(!account.busy()); account.requestPairingCredentials(); CTEST_WAIT_DEFAULT(!account.pairingCredentials().isEmpty());
        int pairingRequest = -1;
        for (qsizetype i = 0; i < authority.requests.size(); ++i)
            if (authority.requests[i].value("intent") == "pairing") pairingRequest = int(i);
        CTEST_CHECK(pairingRequest >= 0);
        const auto headers = authority.headers.at(pairingRequest).toLower();
        CTEST_CHECK(headers.contains("origin: " + authority.url().toEncoded()));
        CTEST_CHECK(headers.contains("iisacc_login_session=fixture-session"));
        CTEST_CHECK(headers.contains("cache-control: no-store"));
        const auto credentials = account.pairingCredentials(); const QByteArray context("client\nunique-offer-and-nonce");
        const auto proof = SocietyPairingCredentials::sign(credentials, context); CTEST_CHECK(!proof.isEmpty());
        CTEST_CHECK(SocietyPairingCredentials::verify(credentials, context, proof));
        CTEST_CHECK(!SocietyPairingCredentials::verify(credentials, "host\nunique-offer-and-nonce", proof));
        CTEST_CHECK(!SocietyPairingCredentials::verify(credentials, context + "other-offer", proof));
        auto other = credentials; other.insert("scope", QString(64, 'f'));
        CTEST_CHECK(!SocietyPairingCredentials::verify(other, context, proof));
        auto expired = credentials; expired.insert("expiresAt", QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODate));
        CTEST_CHECK(SocietyPairingCredentials::sign(expired, context).isEmpty());
        CTEST_CHECK(!SocietyPairingCredentials::verify(expired, context, proof));
        CTEST_CHECK(!SocietyPairingCredentials::verify(credentials, context, {}));
    }
    void accountSessionAutomaticallyFetchesItsDriveLocation() {
        AccountServer authority; CTEST_CHECK(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        helper_ctest::SignalRecorder synced(account.manager(), &iisacc::accounts::AccountManager::containerDriveSynchronized);
        CTEST_CHECK(account.login("builder@example.com", "FixtureOnly1!"));
        CTEST_WAIT_DEFAULT(account.signedIn()); CTEST_WAIT_DEFAULT(!synced.isEmpty());
        CTEST_CHECK(account.manager()->account()->societyContainerDrive().isEmpty());
        const QJsonObject location{{"containerId", "b413adf3-0642-4565-941f-aa1940d25c51"},
            {"imagePath", "/Volumes/Moved/Society.sparsebundle"}, {"hostDeviceId", QString(64, 'a')},
            {"revision", "1d02e288-9704-4c5a-979e-f9b60d1799ca"}};
        authority.profile.insert("societyContainerDrive", location);
        bool refreshed = false;
        CTEST_WAIT_DEFAULT(refreshed || (refreshed = account.manager()->refreshContainerDrive()));
        CTEST_WAIT_COMPARE(account.manager()->account()->societyContainerDrive(), location.toVariantMap());
        CTEST_CHECK(account.logout()); CTEST_WAIT_DEFAULT(!account.signedIn());
        CTEST_CHECK(account.manager()->account()->societyContainerDrive().isEmpty());
    }

    void downloadsAndUploadsWithoutASocietyApplicationProcess(bool useRelay, bool verifyDirect) {
        AccountServer authority; CTEST_CHECK(authority.server.listen(QHostAddress::LocalHost)); CTEST_CHECK(!authority.profile.isEmpty());
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        auto device = account.manager()->deviceInfo(); device["id"] = QString(64, '2');
        CTEST_CHECK(account.manager()->setDeviceInfo(device));
        CTEST_CHECK(account.login("builder@example.com", "FixtureOnly1!")); CTEST_WAIT_DEFAULT(account.signedIn());
        CTEST_WAIT_DEFAULT(!account.busy());
        QTemporaryDir hostRoot((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/host-XXXXXX.societycontainer"))), phoneRoot((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/phone-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(hostRoot.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(phoneRoot.path()));
        QFile model(hostRoot.filePath("Models/model.safetensors")); CTEST_CHECK(model.open(QIODevice::WriteOnly)); model.write(QByteArray(800123, 'm')); model.close();
        CTEST_CHECK(QDir().mkpath(hostRoot.filePath("Files/nested % #")));
        const QMap<QString, QByteArray> downloadFiles{{"Files/nested % #/document.iisc", QByteArray(600123, 'f')},
            {"Generation History/download.png", QByteArray(700123, 'h')}, {"Files/cancel.bin", QByteArray(900123, 'c')},
            {"Files/corrupt.bin", QByteArray(900123, 'o')}, {"Files/switch.bin", QByteArray(900123, 's')},
            {"Files/legacy.bin", QByteArray(300123, 'l')}};
        for (auto it = downloadFiles.cbegin(); it != downloadFiles.cend(); ++it) {
            QFile file(hostRoot.filePath(it.key())); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(it.value()), it.value().size());
        }
        const auto hostId = QString(64, '1'); const auto phoneId = device.value("id").toString(); const auto scope = authority.scope();
        authority.profile.insert("societyContainerDrive", QJsonObject{{"hostDeviceId", hostId},
            {"containerId", iiSocietyContainer::SocietyDrive::open(hostRoot.path())->identifier()},
            {"revision", "1d02e288-9704-4c5a-979e-f9b60d1799ca"}, {"imagePath", "/Volumes/Society.sparsebundle"}});
        bool refreshed = false;
        CTEST_WAIT_DEFAULT(refreshed || (refreshed = account.manager()->refreshContainerDrive()));
        CTEST_WAIT_COMPARE(account.manager()->account()->societyContainerDrive().value("hostDeviceId").toString(), hostId);
        // Fixture for the one-time Society setup: bind to the authenticated
        // host and receive its storage map, without downloading its payload.
        {
            iiSocietyHelper::Sync::Replica host, phone;
            CTEST_CHECK(host.open(hostRoot.path(), scope)); CTEST_CHECK(phone.open(phoneRoot.path(), scope));
            iiSocietyHelper::Sync::Synchronizer initial(&phone); initial.setContentPolicy(iiSocietyHelper::Sync::Synchronizer::ContentPolicy::MetadataFirst);
            helper_ctest::SignalRecorder done(&initial, &iiSocietyHelper::Sync::Synchronizer::finished);
            connect(&initial, &iiSocietyHelper::Sync::Synchronizer::requestReady, this, [&](auto id, auto, auto wire) {
                initial.receive(id, {{"ok", true}, {"result", host.handle(phoneId, wire.value("message").toObject())}});
            });
            CTEST_CHECK(initial.start(hostId)); CTEST_WAIT_COMPARE(done.size(), 1); CTEST_CHECK(done[0][1].toBool());
        }
        iiWebServer::RelayServer relay([&](const auto &, auto done) { done({authority.profile.value("sub").toString(), QDateTime::currentDateTimeUtc().addSecs(300)}); });
        CTEST_CHECK(relay.listen(iiWebServer::Address::loopback()));
        const QUrl endpoint(QString("ws://127.0.0.1:%1").arg(relay.port()));
        if (useRelay) CTEST_WAIT_DEFAULT(account.setServerConfiguration({{"url", endpoint.toString()}, {"host", false}}));
        iiSocietyHelper::Sync::Controller hosting({}); hosting.open(hostRoot.path(), scope); CTEST_WAIT_DEFAULT(hosting.available());
        hosting.setPeers({phoneId}, {});
        QSet<QString> authorizedSessions{phoneId};
        iiWebServer::Peer host;
        int inspectionFaults = 0, injectedFaults = 0;
        const auto handle = [&](const QString &peer, const QJsonObject &payload) {
            if (payload.value("op") == "society.generation")
                return QJsonObject{{"ok", true}, {"peer", peer}, {"echo", payload}};
            const auto message = payload.value("message").toObject();
            if (message.value("action") == "drive.inspect" && message.value("offset").toString().toLongLong() > 0
                && inspectionFaults > 0) {
                --inspectionFaults; ++injectedFaults;
                return QJsonObject{{"ok", true}, {"result", QJsonObject{{"ok", false}, {"error", "drive_information_changed"}}}};
            }
            // As in Society's host, the LAN/relay handler receives only an
            // authenticated account peer. Direct clients need not advertise a
            // hosted file service in the relay's host directory.
            authorizedSessions.insert(peer); hosting.setPeers(authorizedSessions.values(), {});
            return hosting.handle(peer, payload);
        };
        iiWebServer::PeerOptions options; options.relayUrl = endpoint; options.credential = "fixture";
        options.peerId = hostId; options.name = "Society host"; options.service = "com.iisacc.society.files"; options.localEnabled = false;
        if (useRelay) {
            CTEST_CHECK(host.start(options, handle)); CTEST_WAIT_DEFAULT(host.isReady());
        }
        FakeDiscoveryService discovery, hostDiscovery;
        discovery.publishDepartures = hostDiscovery.publishDepartures = true;
        NearbyDevices hostNearby(&hostDiscovery, QHostAddress::LocalHost);
        iiWebServer::LanPeer hostLan;
        hostLan.setPairingAuthentication([&](const QByteArray &context) { return SocietyPairingCredentials::sign(account.pairingCredentials(), context); },
            [&](const QByteArray &context, const QJsonObject &proof) { return SocietyPairingCredentials::verify(account.pairingCredentials(), context, proof); });
        connect(&hostLan, &iiWebServer::LanPeer::completed, &hosting, [&](auto id, auto response, auto) { hosting.receive(id, response); });
        AutomaticPairing hostPairing(&hostNearby, &hostLan, [&] {
            return hostLan.hosting() || hostLan.startHost(hostId, "Society host", [&](auto peer, auto payload) {
                if (payload.value("op") == "list") return QJsonObject{{"ok", true}, {"entries", QJsonArray()}};
                return handle(peer, payload);
            }, {"127.0.0.1"}, iiWebServer::Address::loopback());
        }, [](auto) { return false; }, [&] { hostLan.stop(); });
        QTimer announcements;
        if (!useRelay) {
            account.requestPairingCredentials(); CTEST_WAIT_DEFAULT(!account.pairingCredentials().isEmpty());
            hostNearby.setIdentity(scope, hostId, "Society host", "pc", true);
            hostNearby.setCredentials(account.pairingCredentials(), true, hostId);
            CTEST_CHECK(hostNearby.authenticated());
            connect(&announcements, &QTimer::timeout, this, [&] {
                if (!discovery.record.isEmpty()) discovery.announceTo(hostDiscovery);
                if (!hostDiscovery.record.isEmpty()) hostDiscovery.announceTo(discovery);
            });
            announcements.start(50);
        }
        if (verifyDirect) {
            verifyDirectHostAccess(account, discovery, hostRoot, inspectionFaults, injectedFaults);
        }
        verifyNativeFileDownloads(account, discovery, hostDiscovery, hostRoot, phoneRoot);
        QTemporaryDir localRoot((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/local-generation-XXXXXX"))); CTEST_CHECK(localRoot.isValid());
        iiSocietyHelper::LocalService gateway(&account, [](const QString &product, const QString &app) {
            return product == "dreamscapes" && app == "com.iisacc.dreamscapes";
        }, &discovery, QHostAddress::LocalHost);
        CTEST_CHECK(gateway.listen(localRoot.path()));
        iiSocietyHelper::LocalClient consumer("dreamscapes", "com.iisacc.dreamscapes", phoneId, gateway.path());
        consumer.setExpectedSubject(account.manager()->account()->sub());
        CTEST_CHECK(consumer.requestHost({{"op", "society.generation"}}).isEmpty());
        // Completed direct-session fixtures are retired by the bounded nearby
        // endpoint lifetime; reconnect using the SDK's 30-second open budget.
        CTEST_CHECK(consumer.start(phoneRoot.path())); CTEST_WAIT_MESSAGE_TIMEOUT(consumer.connected(),
            qPrintable(hostPairing.status() + ' ' + hostLan.phase() + ' ' + hostLan.errorString()), 30000);
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(phoneRoot.path()));
        helper_ctest::SignalRecorder responses(&consumer, &iiSocietyHelper::LocalClient::hostResponse);
        const auto rpc = consumer.requestHost({{"op", "society.generation"}, {"action", "submit"}});
        CTEST_CHECK(!rpc.isEmpty()); CTEST_WAIT_COMPARE(responses.size(), 1);
        CTEST_COMPARE(responses[0][0].toString(), rpc);
        CTEST_COMPARE(responses[0][1].toJsonObject().value("peer").toString(), phoneId);
        CTEST_CHECK(!QFileInfo::exists(phoneRoot.filePath("Models/model.safetensors")));
        const auto request = map.request({"models/model.safetensors"}); CTEST_CHECK(!request.isEmpty()); consumer.synchronizeNow();
        CTEST_WAIT_COMPARE_TIMEOUT(map.requestState(request).value("state").toString(), QString("ready"), 30000);
        CTEST_CHECK(map.available({"models/model.safetensors"}));
        QFile image(phoneRoot.filePath("Generation History/generated.png")); CTEST_CHECK(image.open(QIODevice::WriteOnly)); image.write("completed output"); image.close();
        consumer.synchronizeNow();
        CTEST_WAIT_VERIFY(QFileInfo::exists(hostRoot.filePath("Generation History/generated.png")), 30000);
        consumer.setExpectedSubject("different-account"); CTEST_CHECK(!consumer.connected());
        helper_ctest::processFor(200); CTEST_CHECK(!consumer.connected());
        CTEST_CHECK(consumer.requestHost({{"op", "society.generation"}}).isEmpty());
        consumer.setExpectedSubject(account.manager()->account()->sub());
        CTEST_WAIT_VERIFY(consumer.connected(), 30000);
        CTEST_CHECK(account.logout()); CTEST_WAIT_DEFAULT(!account.signedIn()); CTEST_WAIT_DEFAULT(!consumer.connected());
        CTEST_CHECK(consumer.hostBinding().isEmpty()); CTEST_CHECK(consumer.statHostEntry("models/model.safetensors").isEmpty());
        CTEST_CHECK(consumer.requestHost({{"op", "society.generation"}}).isEmpty());
        consumer.stop(); hosting.closeAndWait(); host.stop(); relay.stop();
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    ClientTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("automaticPairingIdleDoesNotPoll", [&] { FlushDeferredDeletes cleanup; tests.automaticPairingIdleDoesNotPoll(); });
    runner.run("automaticPairingRetriesAtDeadline", [&] { FlushDeferredDeletes cleanup; tests.automaticPairingRetriesAtDeadline(); });
    runner.run("applicationsOnTheRegisteredHostUseItsNativeDriveWithoutASelfPeer", [&] { FlushDeferredDeletes cleanup; tests.applicationsOnTheRegisteredHostUseItsNativeDriveWithoutASelfPeer(); });
    runner.run("directHostConnectionRequiresASignedInAccount", [&] { FlushDeferredDeletes cleanup; tests.directHostConnectionRequiresASignedInAccount(); });
    runner.run("stateCreatedBeforeSelectionSavesAfterThePackageAppears", [&] { FlushDeferredDeletes cleanup; tests.stateCreatedBeforeSelectionSavesAfterThePackageAppears(); });
    runner.run("stateRemovalReportsLegacyFailure", [&] { FlushDeferredDeletes cleanup; tests.stateRemovalReportsLegacyFailure(); });
    runner.run("sessionRecordRevisionTracksReplacementAndRevocation", [&] { FlushDeferredDeletes cleanup; tests.sessionRecordRevisionTracksReplacementAndRevocation(); });
    runner.run("stateRemovalAcceptsMissingLegacyEntry", [&] { FlushDeferredDeletes cleanup; tests.stateRemovalAcceptsMissingLegacyEntry(); });
    runner.run("stateRemovalReportsTombstoneFailure", [&] { FlushDeferredDeletes cleanup; tests.stateRemovalReportsTombstoneFailure(); });
    runner.run("savedHostRoutesReconnectWithoutBonjourAndRequireFreshProof", [&] { FlushDeferredDeletes cleanup; tests.savedHostRoutesReconnectWithoutBonjourAndRequireFreshProof(); });
    runner.run("hostConnectionIsPersistedAndBoundToTheAccountDrive", [&] { FlushDeferredDeletes cleanup; tests.hostConnectionIsPersistedAndBoundToTheAccountDrive(); });
    runner.run("nativeDiscoveryLifecycleWithoutNetwork", [&] { FlushDeferredDeletes cleanup; tests.nativeDiscoveryLifecycleWithoutNetwork(); });
    runner.run("localServiceEnforcesDeviceAndCurrentEntitlement", [&] { FlushDeferredDeletes cleanup; tests.localServiceEnforcesDeviceAndCurrentEntitlement(); });
    runner.run("pairingRequestedDuringRestoreIsRetriedAfterRestore", [&] { FlushDeferredDeletes cleanup; tests.pairingRequestedDuringRestoreIsRetriedAfterRestore(); });
    runner.run("pairingRequestedDuringRestoreIsDiscardedOnLogout", [&] { FlushDeferredDeletes cleanup; tests.pairingRequestedDuringRestoreIsDiscardedOnLogout(); });
    runner.run("accountProofRejectsTamperingExpiryAndCrossAccount", [&] { FlushDeferredDeletes cleanup; tests.accountProofRejectsTamperingExpiryAndCrossAccount(); });
    runner.run("accountSessionAutomaticallyFetchesItsDriveLocation", [&] { FlushDeferredDeletes cleanup; tests.accountSessionAutomaticallyFetchesItsDriveLocation(); });
    runner.run("downloadsAndUploadsWithoutASocietyApplicationProcess:saved-server", [&] { FlushDeferredDeletes cleanup; tests.downloadsAndUploadsWithoutASocietyApplicationProcess(true, true); });
    runner.run("downloadsAndUploadsWithoutASocietyApplicationProcess:automatic-local-network", [&] { FlushDeferredDeletes cleanup; tests.downloadsAndUploadsWithoutASocietyApplicationProcess(false, true); });
    runner.run("downloadsAndUploadsWithoutASocietyApplicationProcess:gateway-relay", [&] { FlushDeferredDeletes cleanup; tests.downloadsAndUploadsWithoutASocietyApplicationProcess(true, false); });
    runner.run("downloadsAndUploadsWithoutASocietyApplicationProcess:gateway-lan", [&] { FlushDeferredDeletes cleanup; tests.downloadsAndUploadsWithoutASocietyApplicationProcess(false, false); });
    return runner.result();
}
