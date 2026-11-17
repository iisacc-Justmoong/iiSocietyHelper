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
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QImage>
#include <QBuffer>
#include <filesystem>
#include <utility>
#include <iiFileProviderPresentation.h>
class ClientTests : public QObject {
    Q_OBJECT
    static QJsonObject driveResponse(QSignalSpy &responses, const QString &id) {
        const auto find = [&]() {
            for (const auto &row : responses) if (row[0].toString() == id) return row[1].toJsonObject();
            return QJsonObject();
        };
        if (id.isEmpty() || !QTest::qWaitFor([&] { return !find().isEmpty(); }, 30000))
            return QJsonObject{{"ok", false}, {"error", "test_response_timeout"}};
        return find();
    }
    void verifyNativeFileDownloads(AccountSession &account, FakeDiscoveryService &discovery,
                                   FakeDiscoveryService &hostDiscovery,
                                   const QTemporaryDir &hostRoot, const QTemporaryDir &replicaRoot) {
        iiSocietyHelper::FileSystem files; QVERIFY(files.open(replicaRoot.path()));
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
        QVERIFY(other.connectHost()); QTRY_VERIFY_WITH_TIMEOUT(other.connected(), 10000);
        QSignalSpy otherResponses(&other, &iiSocietyHelper::Client::hostDriveResponse);
        iiSocietyHelper::Client client(&account, &discovery, QHostAddress::LocalHost);
        QTemporaryDir serviceRoot((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/local-service-XXXXXX"))); QVERIFY(serviceRoot.isValid());
        iiSocietyHelper::LocalService service(&account, [](const QString &product, const QString &app) {
            return product == "dreamscapes" && app == "com.iisacc.dreamscapes";
        }, &discovery, QHostAddress::LocalHost);
        QVERIFY(service.listen(serviceRoot.path()));
        iiSocietyHelper::LocalClient transport("dreamscapes", "com.iisacc.dreamscapes",
            account.manager()->deviceInfo().value("id").toString(), service.path());
        transport.setExpectedSubject(account.manager()->account()->sub());
        QSignalSpy transferResponses(&transport, &iiSocietyHelper::FileHost::hostDriveResponse);
        iiSocietyHelper::FileDownload download(&files, transport);
        QSignalSpy ready(&download, &iiSocietyHelper::FileDownload::ready);
        const auto id = files.containerId();
        // A different app may own its normal sync session. Explicit hydration
        // must still complete within this process without a queued disk request.
        QLockFile otherSession(replicaRoot.filePath(".society-sync/client-session.lock"));
        otherSession.setStaleLockTime(0); QVERIFY(otherSession.tryLock());
        const auto queuedBefore = QDir(replicaRoot.filePath(".society-sync/requests")).entryList({"*.json"}, QDir::Files);
        // The application's per-request deadline remains 30 seconds; the
        // complete transfer may legitimately require several such requests.
        const auto wait = [&] { return QTest::qWaitFor([&] { return !download.active(); }, 120000); };
        const auto contents = [](const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; };
        {
            iiSocietyHelper::FileDownload legacy(&files, &client);
            QVERIFY(legacy.start("files/legacy.bin", id));
            QVERIFY(QTest::qWaitFor([&] { return !legacy.active(); }, 120000));
            QCOMPARE(legacy.state().value("state").toString(), "ready");
            QCOMPARE(contents(replicaRoot.filePath("Files/legacy.bin")), QByteArray(300123, 'l'));
        }
        for (const auto &key : {QString("files/nested % #/document.iisc"), QString("generation-history/download.png")}) {
            const auto relative = iiSocietyContainer::StorageMap::physicalPath(key);
            QVERIFY(!QFileInfo::exists(replicaRoot.filePath(relative)));
            QVERIFY(download.start(key, id)); QVERIFY(download.start(key, id));
            QVERIFY2(wait(), qPrintable(QJsonDocument::fromVariant(download.state()).toJson() + client.status().toUtf8()));
            QVERIFY2(download.state().value("state").toString() == "ready",
                     qPrintable(QJsonDocument::fromVariant(download.state()).toJson(QJsonDocument::Compact)
                                + " client=" + QJsonDocument(transport.hostBinding()).toJson(QJsonDocument::Compact)
                                + " connected=" + QByteArray::number(transport.connected())
                                + " responses=" + QByteArray::number(transferResponses.size())
                                + " other=" + QByteArray::number(other.connected())));
            QVERIFY(!ready.isEmpty()); const auto file = ready.last().first().toMap();
            QCOMPARE(file.value("key").toString(), key); QVERIFY(file.value("available").toBool());
            QCOMPARE(file.value("source").toUrl().toLocalFile(), replicaRoot.filePath(relative));
            QCOMPARE(contents(file.value("path").toString()), contents(hostRoot.filePath(relative)));
            QVERIFY(files.container()->alias(key).value("available").toBool());
            QVERIFY(other.connected());
            QVERIFY(driveResponse(otherResponses, other.statHostEntry("models/model.safetensors")).value("ok").toBool());
        }
        QVERIFY(transferResponses.size() >= 6);
        const auto beforeCancel = ready.size();
        bool cancelAfterChunk = true;
        const auto cancellation = connect(&download, &iiSocietyHelper::FileDownload::changed, this, [&] {
            if (cancelAfterChunk && download.active() && download.state().value("completed").toLongLong() > 0) {
                cancelAfterChunk = false; download.cancel();
            }
        });
        QVERIFY(download.start("files/cancel.bin", id)); QVERIFY(wait());
        QCOMPARE(download.state().value("state").toString(), "cancelled"); QCOMPARE(ready.size(), beforeCancel);
        QVERIFY(!QFileInfo::exists(replicaRoot.filePath("Files/cancel.bin")));
        disconnect(cancellation);
        QVERIFY(download.start("files/cancel.bin", id)); QVERIFY(wait());
        QCOMPARE(download.state().value("state").toString(), "ready");
        QCOMPARE(contents(replicaRoot.filePath("Files/cancel.bin")), QByteArray(900123, 'c'));
        const auto beforeCorrupt = ready.size();
        QFile changed(hostRoot.filePath("Files/corrupt.bin")); QVERIFY(changed.open(QIODevice::WriteOnly));
        QCOMPARE(changed.write(QByteArray(900123, 'x')), 900123); changed.close();
        QVERIFY(download.start("files/corrupt.bin", id)); QVERIFY(wait());
        QCOMPARE(download.state().value("state").toString(), "failed"); QCOMPARE(ready.size(), beforeCorrupt);
        QCOMPARE(download.state().value("error").toString(), "incomplete_or_corrupt_transfer");
        QVERIFY(!QFileInfo::exists(replicaRoot.filePath("Files/corrupt.bin")));
        QVERIFY(!download.start("../outside", id));
        QVERIFY(!download.start("files/cancel.bin", "stale-container"));
        QVERIFY(download.start("files/switch.bin", id));
        QTemporaryDir second((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/download-switch-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(second.path())); QVERIFY(files.open(second.path()));
        QVERIFY(!download.active()); QCOMPARE(download.state().value("state").toString(), "cancelled");
        QTest::qWait(100); QCOMPARE(ready.size(), beforeCorrupt);
        QCOMPARE(QDir(replicaRoot.filePath(".society-sync/requests")).entryList({"*.json"}, QDir::Files), queuedBefore);
        // Ordinary resident files do not need login, sockets, or any Client.
        QFile local(second.filePath("Files/local.txt")); QVERIFY(local.open(QIODevice::WriteOnly)); local.write("local"); local.close();
        iiSocietyHelper::FileDownload offline(&files, nullptr); QSignalSpy localReady(&offline, &iiSocietyHelper::FileDownload::ready);
        QVERIFY(offline.start("files/local.txt", files.containerId())); QTRY_COMPARE(localReady.size(), 1);
        QCOMPARE(offline.state().value("state").toString(), "ready");
    }
    void verifyDirectHostAccess(AccountSession &account, FakeDiscoveryService &discovery, const QTemporaryDir &hostRoot,
                                int &inspectionFaults, int &injectedFaults) {
        iiSocietyHelper::Client client(&account, &discovery, QHostAddress::LocalHost);
        const auto mirror = client.container(); QVERIFY(mirror); QVERIFY(!mirror->available());
        QVERIFY(client.metaObject()->indexOfProperty("container") >= 0);
        QSignalSpy responses(&client, &iiSocietyHelper::Client::hostDriveResponse);
        QVERIFY(client.connectHost());
        QTRY_VERIFY_WITH_TIMEOUT(client.connected(), 10000);
        QTRY_VERIFY2_WITH_TIMEOUT(mirror->available(), qPrintable(mirror->errorString() + ' ' + client.status()), 10000);
        QCOMPARE(client.container(), mirror);
        QCOMPARE(client.hostSections().size(), 9);
        QCOMPARE(mirror->containerId(), iiSocietyContainer::SocietyDrive::open(hostRoot.path())->identifier());
        QVERIFY(mirror->rootPath().startsWith("society://"));
        const auto alias = mirror->alias("models/model.safetensors");
        QVERIFY(!alias.isEmpty()); QVERIFY(alias.value("exists").toBool());
        QVERIFY(!alias.value("available").toBool()); QVERIFY(alias.value("source").toUrl().isEmpty());
        const auto response = [&](const QString &id) {
            return driveResponse(responses, id);
        };
        const auto listed = response(client.listHostDirectory("models"));
        QVERIFY2(listed.value("ok").toBool(), qPrintable(listed.value("error").toString()));
        QVERIFY(!listed.value("entries").toArray().isEmpty());
        const auto stat = response(client.statHostEntry("models/model.safetensors"));
        QVERIFY(stat.value("ok").toBool()); QCOMPARE(stat.value("size").toString().toLongLong(), 800123);
        QVERIFY(alias.value("informationAvailable").toBool());
        const auto inspected = response(client.inspectHostFile("models/model.safetensors"));
        QVERIFY2(inspected.value("ok").toBool(), qPrintable(inspected.value("error").toString()));
        const auto information = inspected.value("information").toObject();
        QCOMPARE(information.value("format").toString(), "iiFileProvider.FileInformation");
        QCOMPARE(information.value("details").toObject().value("size").toInteger(), 800123);
        QVERIFY(!information.value("preview").toObject().value("data").toString().isEmpty());
        QVERIFY(!information.contains("rootPath"));
        QImage image(512, 512, QImage::Format_RGB32); quint32 random = 7;
        for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            image.setPixel(x, y, 0xff000000u | (random & 0x00ffffff));
        }
        QByteArray png; QBuffer output(&png); QVERIFY(output.open(QIODevice::WriteOnly)); QVERIFY(image.save(&output, "PNG"));
        const iiFileProvider::native::FilePreview preview{"image/png", "image", png.toStdString(), 512, 512};
        (void)iiFileProvider::native::Presentation::attach(std::filesystem::path(hostRoot.filePath("Models/model.safetensors").toStdU16String()), preview);
        inspectionFaults = 1;
        const auto rich = response(client.inspectHostFile("models/model.safetensors"));
        QVERIFY2(rich.value("ok").toBool(), qPrintable(rich.value("error").toString()));
        QCOMPARE(injectedFaults, 1);
        const auto remotePreview = rich.value("information").toObject().value("preview").toObject();
        QCOMPARE(remotePreview.value("kind").toString(), "image");
        QVERIFY(remotePreview.value("data").toString().size() > iiSocietyHelper::Sync::Replica::ChunkBytes);
        QCOMPARE(QByteArray::fromBase64(remotePreview.value("data").toString().toLatin1()), png);
        inspectionFaults = 100; const auto beforeFailures = injectedFaults;
        const auto unstable = response(client.inspectHostFile("models/model.safetensors"));
        QCOMPARE(unstable.value("error").toString(), QStringLiteral("drive_information_changed"));
        QCOMPARE(injectedFaults - beforeFailures, 3);
        inspectionFaults = 0;
        auto busy = std::make_shared<QLockFile>(hostRoot.filePath(".society-sync/operation.lock"));
        busy->setStaleLockTime(0); QVERIFY(busy->tryLock(2000));
        QTimer::singleShot(300, &client, [busy] { busy->unlock(); });
        const auto deferred = response(client.statHostEntry("models/model.safetensors"));
        QVERIFY2(deferred.value("ok").toBool(), qPrintable(deferred.value("error").toString()));
        const auto version = stat.value("version").toString(); QByteArray data;
        while (data.size() < 800123) {
            const auto read = response(client.readHostFile("models/model.safetensors", data.size(), version));
            QVERIFY2(read.value("ok").toBool(), qPrintable(read.value("error").toString()));
            const auto chunk = QByteArray::fromBase64(read.value("data").toString().toLatin1());
            QVERIFY(!chunk.isEmpty()); data += chunk;
        }
        QCOMPARE(data, QByteArray(800123, 'm'));
        QVERIFY(response(client.createHostDirectory("files/direct" )).value("ok").toBool());
        const QByteArray created("created through iiSocietyHelper");
        QVERIFY(response(client.writeHostFile("files/direct/from-app.txt", created)).value("ok").toBool());
        QFile actual(hostRoot.filePath("Files/direct/from-app.txt")); QVERIFY(actual.open(QIODevice::ReadOnly)); QCOMPARE(actual.readAll(), created);
        QVERIFY(!response(client.writeHostFile("files/direct/from-app.txt", "replace")).value("ok").toBool());
        QVERIFY(client.statHostEntry("../outside").isEmpty());
        QVERIFY(client.inspectHostFile("files/private.iisacc.json").isEmpty());
        QVERIFY(client.readHostFile("files/.iisacc-temporary.lock").isEmpty());
        QVERIFY(client.writeHostFile("files/oversize", QByteArray(iiSocietyHelper::Sync::Replica::ChunkBytes + 1, 'a')).isEmpty());
        mirror->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!mirror->alias("files/direct/from-app.txt").isEmpty(), 10000);
        QVERIFY(mirror->alias("files/direct").isEmpty());
        QVERIFY(mirror->alias("files/direct/from-app.txt").value("modified").toDateTime().isValid());
        QCOMPARE(mirror->recentFiles().first().toMap().value("key").toString(), "files/direct/from-app.txt");
        const auto pending = client.statHostEntry("models/model.safetensors"); QVERIFY(!pending.isEmpty());
        const auto pendingInspection = client.inspectHostFile("models/model.safetensors"); QVERIFY(!pendingInspection.isEmpty());
        client.stop();
        QVERIFY(!client.connected()); QVERIFY(!mirror->available()); QVERIFY(mirror->aliases().isEmpty()); QVERIFY(client.hostSections().isEmpty());
        QCOMPARE(response(pending).value("error").toString(), "host_disconnected");
        QCOMPARE(response(pendingInspection).value("error").toString(), "host_disconnected");
        QVERIFY(client.listHostDirectory().isEmpty());
    }
private slots:
    void automaticPairingIdleDoesNotPoll() {
        FakeDiscoveryService discovery;
        NearbyDevices nearby(&discovery, QHostAddress::LocalHost);
        iiWebServer::LanPeer peer;
        AutomaticPairing pairing(&nearby, &peer, [] { return false; }, [](const QString &) { return false; }, [] {});
        auto *timer = pairing.findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(timer);
        QVERIFY(timer->isSingleShot());
        QCoreApplication::processEvents();
        QVERIFY(!timer->isActive());
        pairing.setEnabled(false);
        QVERIFY(!timer->isActive());
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
        QVERIFY(host.authenticated()); QVERIFY(phone.authenticated());
        phoneDiscovery.announceTo(hostDiscovery);
        QTRY_VERIFY_WITH_TIMEOUT(attempts >= 2, 3000);
        phoneDiscovery.stop();
        auto *timer = pairing.findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(timer);
        QTRY_VERIFY(!timer->isActive());
    }
    void applicationsOnTheRegisteredHostUseItsNativeDriveWithoutASelfPeer() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/same-device-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        QFile model(root.filePath("Models/registered.bin")); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("indexed model"); model.close();
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost));
        const QString hostId(64, '1');
        authority.profile.insert("societyContainerDrive", QJsonObject{{"hostDeviceId", hostId}, {"containerId", drive->identifier()},
            {"imagePath", root.path()}, {"revision", "1d02e288-9704-4c5a-979e-f9b60d1799ca"}});
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        auto device = account.manager()->deviceInfo(); device.insert("id", hostId); QVERIFY(account.manager()->setDeviceInfo(device));
        QVERIFY(account.login("builder@example.com", "FixtureOnly1!")); QTRY_VERIFY(account.signedIn() && !account.busy());
        {
            iiSocietyHelper::Sync::Replica owner; QVERIFY(owner.open(root.path(), authority.scope())); QVERIFY(owner.scan());
        }
        QVERIFY(iiSocietyHelper::Sync::Replica::claimPrimaryHost(root.path(), authority.scope(), hostId));
        QFile unindexed(root.filePath("Files/not-indexed.txt")); QVERIFY(unindexed.open(QIODevice::WriteOnly)); unindexed.write("live native data"); unindexed.close();
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto restore = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", previous); });
        QTemporaryDir unrelated((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/unrelated-host-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(unrelated.path()));
        qputenv("SOCIETY_CONTAINER_PATH", unrelated.path().toUtf8());
        FakeDiscoveryService discovery; iiSocietyHelper::Client client(&account, &discovery, QHostAddress::LocalHost);
        QSignalSpy responses(&client, &iiSocietyHelper::Client::hostDriveResponse);
        QVERIFY(client.connectHost()); QVERIFY(!client.connected()); QVERIFY(!client.container()->available());
        QVERIFY(client.readHostFile("files/not-indexed.txt").isEmpty()); QCOMPARE(discovery.starts, 0);
        QVERIFY(!QFileInfo::exists(unrelated.filePath(".society-sync")));
        qputenv("SOCIETY_CONTAINER_PATH", root.path().toUtf8()); client.synchronizeNow();
        QTRY_VERIFY2(client.connected(), qPrintable(client.status()));
        QTRY_VERIFY2(client.container()->available(), qPrintable(client.container()->errorString()));
        QCOMPARE(discovery.starts, 0); QCOMPARE(client.hostSections().size(), 9);
        QVERIFY(!client.container()->alias("models/registered.bin").isEmpty());
        const auto read = driveResponse(responses, client.readHostFile("files/not-indexed.txt"));
        QVERIFY2(read.value("ok").toBool(), qPrintable(read.value("error").toString()));
        QCOMPARE(QByteArray::fromBase64(read.value("data").toString().toLatin1()), QByteArray("live native data"));
        const auto write = driveResponse(responses, client.writeHostFile("files/from-local-app.txt", "local publication"));
        QVERIFY2(write.value("ok").toBool(), qPrintable(write.value("error").toString()));
        QFile published(root.filePath("Files/from-local-app.txt")); QVERIFY(published.open(QIODevice::ReadOnly)); QCOMPARE(published.readAll(), QByteArray("local publication"));
        QTest::qWait(150);
        iiSocietyHelper::Sync::Replica inspected; QVERIFY(inspected.open(root.path(), authority.scope()));
        QVERIFY(inspected.record("files/not-indexed.txt").isEmpty()); // No second background indexer.
        QCOMPARE(discovery.starts, 0);
        QVERIFY(client.requestHost({{"op", "society.generation"}}).isEmpty());
        QVERIFY(account.logout()); QTRY_VERIFY(!account.signedIn());
        QVERIFY(!client.connected()); QVERIFY(!client.container()->available());
        client.stop();
    }
    void directHostConnectionRequiresASignedInAccount() {
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        FakeDiscoveryService discovery; iiSocietyHelper::Client client(&account, &discovery, QHostAddress::LocalHost);
        QVERIFY(!client.connectHost()); QVERIFY(!client.container()->available());
    }
    void stateCreatedBeforeSelectionSavesAfterThePackageAppears() {
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/late-group-XXXXXX.societycontainer")));
        QVERIFY(fixture.isValid());
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto cleanup = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", previous); });
        qputenv("SOCIETY_CONTAINER_PATH", fixture.path().toUtf8());
        MemorySessionStore keys;
        GroupSessionStore store({}, &keys, nullptr);
        QVERIFY(store.directory().isEmpty());
        QVERIFY(iiSocietyContainer::SocietyDrive::create(fixture.path()));
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        store.write("late-selection-fixture", "synthetic record", [&](auto value) { result = value; done = true; });
        QTRY_VERIFY(done); QCOMPARE(result.error, Store::Error::None);
        QCOMPARE(store.directory(), fixture.filePath(".society-runtime/state"));
        done = false; store.read("late-selection-fixture", [&](auto value) { result = value; done = true; });
        QTRY_VERIFY(done); QCOMPARE(result.data, QByteArray("synthetic record"));
    }

    void stateRemovalReportsLegacyFailure() {
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/group-remove-XXXXXX"))); QVERIFY(fixture.isValid());
        MemorySessionStore keys, legacy; legacy.failRemove = true;
        GroupSessionStore store(fixture.path(), &keys, &legacy);
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        store.remove("synthetic", [&](auto value) { result = value; done = true; });
        QTRY_VERIFY(done); QCOMPARE(result.error, Store::Error::Unavailable);
    }

    void sessionRecordRevisionTracksReplacementAndRevocation() {
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/group-revision-XXXXXX")));
        QVERIFY(fixture.isValid());
        MemorySessionStore keys;
        GroupSessionStore first(fixture.path(), &keys, nullptr), second(fixture.path(), &keys, nullptr);
        QVERIFY(first.recordRevision("account-session").isEmpty());
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        first.write("account-session", "session-a", [&](auto value) {result=value;done=true;});
        QTRY_VERIFY(done); QCOMPARE(result.error, Store::Error::None);
        const auto initial=first.recordRevision("account-session");
        QCOMPARE(initial.size(),32);QCOMPARE(second.recordRevision("account-session"),initial);
        done=false;second.write("account-session", "session-b", [&](auto value) {result=value;done=true;});
        QTRY_VERIFY(done);QCOMPARE(result.error,Store::Error::None);
        QVERIFY(first.recordRevision("account-session")!=initial);
        done=false;second.remove("account-session", [&](auto value) {result=value;done=true;});
        QTRY_VERIFY(done);QCOMPARE(result.error,Store::Error::None);
        QVERIFY(first.recordRevision("account-session").isEmpty());
        // The provider may create preview sidecars while removing state files.
        // Remove only this test's freshly created private fixture tree.
        fixture.setAutoRemove(false);
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(fixture.path().toStdString()), error);
        QVERIFY2(!error, qPrintable(QString::fromStdString(error.message())));
    }

    void stateRemovalAcceptsMissingLegacyEntry() {
        class MissingLegacy final : public MemorySessionStore {
            void remove(const QString &, Completion done) override { done({Error::Missing, {}}); }
        } legacy;
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/group-missing-XXXXXX"))); QVERIFY(fixture.isValid());
        MemorySessionStore keys;
        GroupSessionStore store(fixture.path(), &keys, &legacy);
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        store.remove("synthetic", [&](auto value) { result = value; done = true; });
        QTRY_VERIFY(done); QCOMPARE(result.error, Store::Error::None);
    }

    void stateRemovalReportsTombstoneFailure() {
        QTemporaryDir fixture((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/group-tombstone-XXXXXX"))); QVERIFY(fixture.isValid());
        MemorySessionStore keys;
        GroupSessionStore store(fixture.path(), &keys, nullptr);
        const auto name = QString::fromLatin1(QCryptographicHash::hash(QByteArray("synthetic"), QCryptographicHash::Sha256).toHex()) + ".state";
        const auto target = fixture.filePath("untouched-target");
        QFile file(target); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
        QVERIFY(QFile::link(target, fixture.filePath(name)));
        using Store = iisacc::accounts::SessionStore;
        Store::Result result; bool done = false;
        store.remove("synthetic", [&](auto value) { result = value; done = true; });
        QTRY_VERIFY(done); QCOMPARE(result.error, Store::Error::Unavailable);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("fixture"));
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
        QCOMPARE(routes.size(), 1);
        const auto staleRecord = firstService.record;
        const auto stalePort = firstService.port;
        iiWebServer::LanPeer transport;
        QVERIFY(transport.startHost("host", "Host", [](const auto &, const auto &) { return QJsonObject{}; },
            {"127.0.0.1"}, iiWebServer::Address::loopback()));
        QVERIFY(host.invite("phone", transport.createDeviceOffer("phone"), true));
        QSignalSpy retired(&host, &NearbyDevices::invitationEnded);
        first.clear();
        FakeDiscoveryService restoredService;
        NearbyDevices restored(&restoredService, QHostAddress::LocalHost);
        restored.setIdentity(scope, "phone", "Phone", "phone", false);
        restored.setCredentials(credentials, false, "host");
        restored.resumeHost("unregistered-host", routes);
        QTest::qWait(50); QVERIFY(restored.devices().isEmpty());
        restored.resumeHost("host", routes);
        QTRY_COMPARE(restored.devices().size(), 1);
        QVERIFY(restored.devices().first().toMap().value("verified").toBool());
        QTRY_COMPARE(host.devices().size(), 1);
        QVERIFY(host.devices().first().toMap().value("verified").toBool());
        QCOMPARE(retired.size(), 1); // No 20-second wait for an old process invitation.
        // A delayed Bonjour callback must not replace the freshly challenged
        // process with the prior launch and restart its pairing timeout.
        emit hostService.found("delayed-bonjour", staleRecord, QHostAddress::LocalHost, stalePort);
        QCOMPARE(host.connectionHints("phone"), QJsonArray({QJsonObject{
            {"address", "127.0.0.1"}, {"port", restoredService.port}}}));
        // Hints only route a fresh challenge. They never restore authentication.
        FakeDiscoveryService wrongService;
        NearbyDevices wrong(&wrongService, QHostAddress::LocalHost);
        wrong.setIdentity(scope, "other", "Other", "phone", false);
        wrong.setCredentials(localPairingCredentialsFixture(scope, 'f'), false, "host");
        wrong.resumeHost("host", routes);
        QTest::qWait(150);
        QVERIFY(wrong.devices().isEmpty());
        QCOMPARE(host.devices().size(), 1);
        restored.clear();
        QVERIFY(restored.connectionHints("host").isEmpty());
    }
    void hostConnectionIsPersistedAndBoundToTheAccountDrive() {
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost));
        const QString host(64, 'a'), container("b413adf3-0642-4565-941f-aa1940d25c51");
        authority.profile.insert("societyContainerDrive", QJsonObject{{"hostDeviceId", host}, {"containerId", container},
            {"imagePath", "/Volumes/Society.sparsebundle"}, {"revision", "1d02e288-9704-4c5a-979e-f9b60d1799ca"}});
        MemorySessionStore state;
        const QJsonArray routes{QJsonObject{{"address", "192.168.1.5"}, {"port", 4000}}};
        {
            AccountSession account(authority.url(), &state, nullptr);
            QVERIFY(account.login("builder@example.com", "FixtureOnly1!"));
            QTRY_VERIFY(account.signedIn() && !account.busy());
            QTRY_VERIFY(account.rememberHostConnection(host, container, routes));
            QCOMPARE(account.savedHostConnection().value("routes").toArray(), routes);
            QVERIFY(!account.rememberHostConnection("another-host", container, routes));
            QTest::qWait(50);
        }
        AccountSession restored(authority.url(), &state, nullptr);
        QTRY_VERIFY(restored.signedIn());
        QTRY_COMPARE(restored.savedHostConnection().value("routes").toArray(), routes);
        auto changedDrive = authority.profile.value("societyContainerDrive").toObject();
        changedDrive.insert("hostDeviceId", QString(64, 'b'));
        authority.profile.insert("societyContainerDrive", changedDrive);
        bool refreshed = false;
        QTRY_VERIFY(refreshed || (refreshed = restored.manager()->refreshContainerDrive()));
        QTRY_VERIFY(restored.savedHostConnection().isEmpty());
        QVERIFY(restored.logout()); QTRY_VERIFY(!restored.signedIn());
        QVERIFY(restored.savedHostConnection().isEmpty());
    }
    void nativeDiscoveryLifecycleWithoutNetwork() {
        QObject owner;
        auto *discovery = DiscoveryService::createNetwork(&owner);
        QVERIFY(discovery);
        QCOMPARE(discovery->parent(), &owner);
        discovery->stop();
        discovery->stop();
    }
    void localServiceEnforcesDeviceAndCurrentEntitlement() {
#if defined(Q_OS_UNIX)
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        QVERIFY(account.login("builder@example.com", "FixtureOnly1!")); QTRY_VERIFY(account.signedIn()); QTRY_VERIFY(!account.busy());
        QTemporaryDir root((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/broker-XXXXXX"))); QVERIFY(root.isValid());
        bool authorized = true; int checks = 0;
        iiSocietyHelper::LocalService service(&account, [&](const QString &product, const QString &app) {
            ++checks;
            return authorized && product == "dreamscapes" && app == "com.iisacc.dreamscapes";
        });
        QVERIFY(service.listen(root.path()));
        const auto hello = [&](const QString &device) { return iiSocietyHelper::LocalFrame::encode({{"op", "hello"},
            {"productId", "dreamscapes"}, {"appId", "com.iisacc.dreamscapes"}, {"deviceId", device}}); };
        QLocalSocket wrong; wrong.connectToServer(service.path()); QTRY_COMPARE(wrong.state(), QLocalSocket::ConnectedState);
        QVERIFY(wrong.write(hello(QString(64, 'f'))) > 0); wrong.flush(); QTRY_COMPARE(wrong.state(), QLocalSocket::UnconnectedState);
        QLocalSocket socket; socket.connectToServer(service.path()); QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        QVERIFY(socket.write(hello(account.manager()->deviceInfo().value("id").toString())) > 0); socket.flush();
        iiSocietyHelper::LocalFrame responses; QTRY_VERIFY(socket.bytesAvailable() > 0);
        QVERIFY(responses.append(socket.readAll()));
        QJsonObject ready;
        QTRY_VERIFY(([&] { if (!ready.isEmpty()) return true; if (auto value = responses.take()) { ready = *value; return true; }
            if (socket.bytesAvailable()) responses.append(socket.readAll()); return false; })());
        QCOMPARE(ready.value("type").toString(), "ready");
        authorized = false;
        QVERIFY(socket.write(iiSocietyHelper::LocalFrame::encode({{"op", "sync"}})) > 0); socket.flush();
        QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
        authorized = true; const auto before = checks;
        iiSocietyHelper::LocalClient proxy("dreamscapes", "com.iisacc.dreamscapes",
            account.manager()->deviceInfo().value("id").toString(), service.path());
        proxy.setExpectedSubject("different-account"); QVERIFY(proxy.observe());
        QTRY_VERIFY(checks > before); QTest::qWait(100); QVERIFY(!proxy.sessionBound());
        proxy.setExpectedSubject(account.manager()->account()->sub()); QTRY_VERIFY(proxy.sessionBound());
        QSignalSpy boundChanges(&proxy, &iiSocietyHelper::LocalClient::sessionChanged);
        authorized = false; proxy.synchronizeNow(); QTRY_VERIFY(!proxy.sessionBound());
        QVERIFY(!boundChanges.isEmpty()); QCOMPARE(boundChanges.last().first().toBool(), false);
        service.stop();
#endif
    }
    void pairingRequestedDuringRestoreIsRetriedAfterRestore() {
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; state.holdRead = true;
        AccountSession account(authority.url(), &state, nullptr);
        QVERIFY(account.login("builder@example.com", "FixtureOnly1!")); QTRY_VERIFY(account.signedIn());
        QTRY_VERIFY(!account.busy()); QTRY_VERIFY(bool(state.releaseRead));
        account.requestPairingCredentials();
        QVERIFY(account.pairingCredentials().isEmpty());
        const auto release = std::move(state.releaseRead); release();
        QTRY_VERIFY(!account.pairingCredentials().isEmpty());
        int requests = 0;
        for (const auto &request : authority.requests) if (request.value("intent") == "pairing") ++requests;
        QCOMPARE(requests, 1);
    }
    void pairingRequestedDuringRestoreIsDiscardedOnLogout() {
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; state.holdRead = true;
        AccountSession account(authority.url(), &state, nullptr);
        QVERIFY(account.login("builder@example.com", "FixtureOnly1!")); QTRY_VERIFY(account.signedIn());
        QTRY_VERIFY(!account.busy()); QTRY_VERIFY(bool(state.releaseRead));
        account.requestPairingCredentials(); QVERIFY(account.logout());
        const auto release = std::move(state.releaseRead); release();
        QTRY_VERIFY(!account.signedIn()); QTest::qWait(100);
        QVERIFY(account.pairingCredentials().isEmpty());
        for (const auto &request : authority.requests) QVERIFY(request.value("intent") != "pairing");
    }
    void accountProofRejectsTamperingExpiryAndCrossAccount() {
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        QVERIFY(account.login("builder@example.com", "FixtureOnly1!")); QTRY_VERIFY(account.signedIn());
        QTRY_VERIFY(!account.busy()); account.requestPairingCredentials(); QTRY_VERIFY(!account.pairingCredentials().isEmpty());
        int pairingRequest = -1;
        for (qsizetype i = 0; i < authority.requests.size(); ++i)
            if (authority.requests[i].value("intent") == "pairing") pairingRequest = int(i);
        QVERIFY(pairingRequest >= 0);
        const auto headers = authority.headers.at(pairingRequest).toLower();
        QVERIFY(headers.contains("origin: " + authority.url().toEncoded()));
        QVERIFY(headers.contains("iisacc_login_session=fixture-session"));
        QVERIFY(headers.contains("cache-control: no-store"));
        const auto credentials = account.pairingCredentials(); const QByteArray context("client\nunique-offer-and-nonce");
        const auto proof = SocietyPairingCredentials::sign(credentials, context); QVERIFY(!proof.isEmpty());
        QVERIFY(SocietyPairingCredentials::verify(credentials, context, proof));
        QVERIFY(!SocietyPairingCredentials::verify(credentials, "host\nunique-offer-and-nonce", proof));
        QVERIFY(!SocietyPairingCredentials::verify(credentials, context + "other-offer", proof));
        auto other = credentials; other.insert("scope", QString(64, 'f'));
        QVERIFY(!SocietyPairingCredentials::verify(other, context, proof));
        auto expired = credentials; expired.insert("expiresAt", QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODate));
        QVERIFY(SocietyPairingCredentials::sign(expired, context).isEmpty());
        QVERIFY(!SocietyPairingCredentials::verify(expired, context, proof));
        QVERIFY(!SocietyPairingCredentials::verify(credentials, context, {}));
    }
    void accountSessionAutomaticallyFetchesItsDriveLocation() {
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        QSignalSpy synced(account.manager(), &iisacc::accounts::AccountManager::containerDriveSynchronized);
        QVERIFY(account.login("builder@example.com", "FixtureOnly1!"));
        QTRY_VERIFY(account.signedIn()); QTRY_VERIFY(!synced.isEmpty());
        QVERIFY(account.manager()->account()->societyContainerDrive().isEmpty());
        const QJsonObject location{{"containerId", "b413adf3-0642-4565-941f-aa1940d25c51"},
            {"imagePath", "/Volumes/Moved/Society.sparsebundle"}, {"hostDeviceId", QString(64, 'a')},
            {"revision", "1d02e288-9704-4c5a-979e-f9b60d1799ca"}};
        authority.profile.insert("societyContainerDrive", location);
        bool refreshed = false;
        QTRY_VERIFY(refreshed || (refreshed = account.manager()->refreshContainerDrive()));
        QTRY_COMPARE(account.manager()->account()->societyContainerDrive(), location.toVariantMap());
        QVERIFY(account.logout()); QTRY_VERIFY(!account.signedIn());
        QVERIFY(account.manager()->account()->societyContainerDrive().isEmpty());
    }

    void downloadsAndUploadsWithoutASocietyApplicationProcess_data() {
        QTest::addColumn<bool>("useRelay");
        QTest::addColumn<bool>("verifyDirect");
        QTest::newRow("saved-server") << true << true;
        QTest::newRow("automatic-local-network") << false << true;
        // Isolate gateway coverage from unrelated provider-preview inspection.
        QTest::newRow("gateway-relay") << true << false;
        QTest::newRow("gateway-lan") << false << false;
    }
    void downloadsAndUploadsWithoutASocietyApplicationProcess() {
        QFETCH(bool, useRelay);
        QFETCH(bool, verifyDirect);
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost)); QVERIFY(!authority.profile.isEmpty());
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        auto device = account.manager()->deviceInfo(); device["id"] = QString(64, '2');
        QVERIFY(account.manager()->setDeviceInfo(device));
        QVERIFY(account.login("builder@example.com", "FixtureOnly1!")); QTRY_VERIFY(account.signedIn());
        QTRY_VERIFY(!account.busy());
        QTemporaryDir hostRoot((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/host-XXXXXX.societycontainer"))), phoneRoot((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/phone-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(hostRoot.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(phoneRoot.path()));
        QFile model(hostRoot.filePath("Models/model.safetensors")); QVERIFY(model.open(QIODevice::WriteOnly)); model.write(QByteArray(800123, 'm')); model.close();
        QVERIFY(QDir().mkpath(hostRoot.filePath("Files/nested % #")));
        const QMap<QString, QByteArray> downloadFiles{{"Files/nested % #/document.iisc", QByteArray(600123, 'f')},
            {"Generation History/download.png", QByteArray(700123, 'h')}, {"Files/cancel.bin", QByteArray(900123, 'c')},
            {"Files/corrupt.bin", QByteArray(900123, 'o')}, {"Files/switch.bin", QByteArray(900123, 's')},
            {"Files/legacy.bin", QByteArray(300123, 'l')}};
        for (auto it = downloadFiles.cbegin(); it != downloadFiles.cend(); ++it) {
            QFile file(hostRoot.filePath(it.key())); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(it.value()), it.value().size());
        }
        const auto hostId = QString(64, '1'); const auto phoneId = device.value("id").toString(); const auto scope = authority.scope();
        authority.profile.insert("societyContainerDrive", QJsonObject{{"hostDeviceId", hostId},
            {"containerId", iiSocietyContainer::SocietyDrive::open(hostRoot.path())->identifier()},
            {"revision", "1d02e288-9704-4c5a-979e-f9b60d1799ca"}, {"imagePath", "/Volumes/Society.sparsebundle"}});
        bool refreshed = false;
        QTRY_VERIFY(refreshed || (refreshed = account.manager()->refreshContainerDrive()));
        QTRY_COMPARE(account.manager()->account()->societyContainerDrive().value("hostDeviceId").toString(), hostId);
        // Fixture for the one-time Society setup: bind to the authenticated
        // host and receive its storage map, without downloading its payload.
        {
            iiSocietyHelper::Sync::Replica host, phone;
            QVERIFY(host.open(hostRoot.path(), scope)); QVERIFY(phone.open(phoneRoot.path(), scope));
            iiSocietyHelper::Sync::Synchronizer initial(&phone); initial.setContentPolicy(iiSocietyHelper::Sync::Synchronizer::ContentPolicy::MetadataFirst);
            QSignalSpy done(&initial, &iiSocietyHelper::Sync::Synchronizer::finished);
            connect(&initial, &iiSocietyHelper::Sync::Synchronizer::requestReady, this, [&](auto id, auto, auto wire) {
                initial.receive(id, {{"ok", true}, {"result", host.handle(phoneId, wire.value("message").toObject())}});
            });
            QVERIFY(initial.start(hostId)); QTRY_COMPARE(done.size(), 1); QVERIFY(done[0][1].toBool());
        }
        iiWebServer::RelayServer relay([&](const auto &, auto done) { done({authority.profile.value("sub").toString(), QDateTime::currentDateTimeUtc().addSecs(300)}); });
        QVERIFY(relay.listen(iiWebServer::Address::loopback()));
        const QUrl endpoint(QString("ws://127.0.0.1:%1").arg(relay.port()));
        if (useRelay) QTRY_VERIFY(account.setServerConfiguration({{"url", endpoint.toString()}, {"host", false}}));
        iiSocietyHelper::Sync::Controller hosting({}); hosting.open(hostRoot.path(), scope); QTRY_VERIFY(hosting.available());
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
            QVERIFY(host.start(options, handle)); QTRY_VERIFY(host.isReady());
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
            account.requestPairingCredentials(); QTRY_VERIFY(!account.pairingCredentials().isEmpty());
            hostNearby.setIdentity(scope, hostId, "Society host", "pc", true);
            hostNearby.setCredentials(account.pairingCredentials(), true, hostId);
            QVERIFY(hostNearby.authenticated());
            connect(&announcements, &QTimer::timeout, this, [&] {
                if (!discovery.record.isEmpty()) discovery.announceTo(hostDiscovery);
                if (!hostDiscovery.record.isEmpty()) hostDiscovery.announceTo(discovery);
            });
            announcements.start(50);
        }
        if (verifyDirect) {
            verifyDirectHostAccess(account, discovery, hostRoot, inspectionFaults, injectedFaults);
            if (QTest::currentTestFailed()) return;
        }
        verifyNativeFileDownloads(account, discovery, hostDiscovery, hostRoot, phoneRoot);
        if (QTest::currentTestFailed()) return;
        QTemporaryDir localRoot((society_test::root(QString::fromUtf8(CLIENT_TEST_DIRECTORY)) + QStringLiteral("/local-generation-XXXXXX"))); QVERIFY(localRoot.isValid());
        iiSocietyHelper::LocalService gateway(&account, [](const QString &product, const QString &app) {
            return product == "dreamscapes" && app == "com.iisacc.dreamscapes";
        }, &discovery, QHostAddress::LocalHost);
        QVERIFY(gateway.listen(localRoot.path()));
        iiSocietyHelper::LocalClient consumer("dreamscapes", "com.iisacc.dreamscapes", phoneId, gateway.path());
        consumer.setExpectedSubject(account.manager()->account()->sub());
        QVERIFY(consumer.requestHost({{"op", "society.generation"}}).isEmpty());
        // Completed direct-session fixtures are retired by the bounded nearby
        // endpoint lifetime; reconnect using the SDK's 30-second open budget.
        QVERIFY(consumer.start(phoneRoot.path())); QTRY_VERIFY2_WITH_TIMEOUT(consumer.connected(),
            qPrintable(hostPairing.status() + ' ' + hostLan.phase() + ' ' + hostLan.errorString()), 30000);
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(phoneRoot.path()));
        QSignalSpy responses(&consumer, &iiSocietyHelper::LocalClient::hostResponse);
        const auto rpc = consumer.requestHost({{"op", "society.generation"}, {"action", "submit"}});
        QVERIFY(!rpc.isEmpty()); QTRY_COMPARE(responses.size(), 1);
        QCOMPARE(responses[0][0].toString(), rpc);
        QCOMPARE(responses[0][1].toJsonObject().value("peer").toString(), phoneId);
        QVERIFY(!QFileInfo::exists(phoneRoot.filePath("Models/model.safetensors")));
        const auto request = map.request({"models/model.safetensors"}); QVERIFY(!request.isEmpty()); consumer.synchronizeNow();
        QTRY_COMPARE_WITH_TIMEOUT(map.requestState(request).value("state").toString(), QString("ready"), 30000);
        QVERIFY(map.available({"models/model.safetensors"}));
        QFile image(phoneRoot.filePath("Generation History/generated.png")); QVERIFY(image.open(QIODevice::WriteOnly)); image.write("completed output"); image.close();
        consumer.synchronizeNow();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(hostRoot.filePath("Generation History/generated.png")), 30000);
        consumer.setExpectedSubject("different-account"); QVERIFY(!consumer.connected());
        QTest::qWait(200); QVERIFY(!consumer.connected());
        QVERIFY(consumer.requestHost({{"op", "society.generation"}}).isEmpty());
        consumer.setExpectedSubject(account.manager()->account()->sub());
        QTRY_VERIFY_WITH_TIMEOUT(consumer.connected(), 30000);
        QVERIFY(account.logout()); QTRY_VERIFY(!account.signedIn()); QTRY_VERIFY(!consumer.connected());
        QVERIFY(consumer.hostBinding().isEmpty()); QVERIFY(consumer.statHostEntry("models/model.safetensors").isEmpty());
        QVERIFY(consumer.requestHost({{"op", "society.generation"}}).isEmpty());
        consumer.stop(); hosting.closeAndWait(); host.stop(); relay.stop();
    }
};
QTEST_GUILESS_MAIN(ClientTests)
#include "client.moc"
