#include <iiSocietyHelper/Client.h>
#include <iiSocietyHelper/FileDownload.h>
#include <iiSocietyHelper.h>
#include <iiSocietyHelper/GroupSessionStore.h>
#include <QScopeGuard>
#include <iiSocietyHelper/PairingCredentials.h>
#include <iiSocietyHelper/AccountSession.h>
#include <iiSocietyHelper/NearbyDevices.h>
#include <iiSocietyHelper/AutomaticPairing.h>
#include <iiSocietySync.h>
#include <Container.h>
#include <StorageMap.h>
#include "AccountServer.h"
#include "MemorySessionStore.h"
#include "FakeDiscoveryService.h"
#include "PairingCredentialsFixture.h"
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QImage>
#include <QBuffer>
#include <iiFileProviderPresentation.h>
class ClientTests : public QObject {
    Q_OBJECT
    static QJsonObject driveResponse(QSignalSpy &responses, const QString &id) {
        const auto find = [&]() {
            for (const auto &row : responses) if (row[0].toString() == id) return row[1].toJsonObject();
            return QJsonObject();
        };
        if (id.isEmpty() || !QTest::qWaitFor([&] { return !find().isEmpty(); }, 10000))
            return QJsonObject{{"ok", false}, {"error", "test_response_timeout"}};
        return find();
    }
    void verifyNativeFileDownloads(AccountSession &account, FakeDiscoveryService &discovery,
                                   FakeDiscoveryService &hostDiscovery,
                                   const QTemporaryDir &hostRoot, const QTemporaryDir &replicaRoot) {
        iiSocietyHelper::FileSystem files; QVERIFY(files.open(replicaRoot.path()));
        FakeDiscoveryService otherDiscovery;
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
        iiSocietyHelper::FileDownload download(&files, &client);
        QSignalSpy ready(&download, &iiSocietyHelper::FileDownload::ready);
        const auto id = files.containerId();
        // A different app may own its normal sync session. Explicit hydration
        // must still complete within this process without a queued disk request.
        QLockFile otherSession(replicaRoot.filePath(".society-sync/client-session.lock"));
        otherSession.setStaleLockTime(0); QVERIFY(otherSession.tryLock());
        const auto queuedBefore = QDir(replicaRoot.filePath(".society-sync/requests")).entryList({"*.json"}, QDir::Files);
        const auto wait = [&] { return QTest::qWaitFor([&] { return !download.active(); }, 15000); };
        const auto contents = [](const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; };
        for (const auto &key : {QString("files/nested % #/document.iisc"), QString("generation-history/download.png")}) {
            const auto relative = iiSocietyContainer::StorageMap::physicalPath(key);
            QVERIFY(!QFileInfo::exists(replicaRoot.filePath(relative)));
            QVERIFY(download.start(key, id)); QVERIFY(download.start(key, id));
            QVERIFY(wait()); QCOMPARE(download.state().value("state").toString(), "ready");
            QVERIFY(!ready.isEmpty()); const auto file = ready.last().first().toMap();
            QCOMPARE(file.value("key").toString(), key); QVERIFY(file.value("available").toBool());
            QCOMPARE(file.value("source").toUrl().toLocalFile(), replicaRoot.filePath(relative));
            QCOMPARE(contents(file.value("path").toString()), contents(hostRoot.filePath(relative)));
            QVERIFY(files.container()->alias(key).value("available").toBool());
            QVERIFY(other.connected());
            QVERIFY(driveResponse(otherResponses, other.statHostEntry("models/model.safetensors")).value("ok").toBool());
        }
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
        QTemporaryDir second(CLIENT_TEST_DIRECTORY "/download-switch-XXXXXX.societycontainer");
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
    void verifyDirectHostAccess(AccountSession &account, FakeDiscoveryService &discovery, const QTemporaryDir &hostRoot) {
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
        const auto rich = response(client.inspectHostFile("models/model.safetensors"));
        QVERIFY2(rich.value("ok").toBool(), qPrintable(rich.value("error").toString()));
        const auto remotePreview = rich.value("information").toObject().value("preview").toObject();
        QCOMPARE(remotePreview.value("kind").toString(), "image");
        QVERIFY(remotePreview.value("data").toString().size() > iiSocietySync::Replica::ChunkBytes);
        QCOMPARE(QByteArray::fromBase64(remotePreview.value("data").toString().toLatin1()), png);
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
        QVERIFY(client.writeHostFile("files/oversize", QByteArray(iiSocietySync::Replica::ChunkBytes + 1, 'a')).isEmpty());
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
    void applicationsOnTheRegisteredHostUseItsNativeDriveWithoutASelfPeer() {
        QTemporaryDir root(CLIENT_TEST_DIRECTORY "/same-device-XXXXXX.societycontainer");
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
            iiSocietySync::Replica owner; QVERIFY(owner.open(root.path(), authority.scope())); QVERIFY(owner.scan());
        }
        QVERIFY(iiSocietySync::Replica::claimPrimaryHost(root.path(), authority.scope(), hostId));
        QFile unindexed(root.filePath("Files/not-indexed.txt")); QVERIFY(unindexed.open(QIODevice::WriteOnly)); unindexed.write("live native data"); unindexed.close();
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto restore = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", previous); });
        QTemporaryDir unrelated(CLIENT_TEST_DIRECTORY "/unrelated-host-XXXXXX.societycontainer");
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
        iiSocietySync::Replica inspected; QVERIFY(inspected.open(root.path(), authority.scope()));
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
        QTemporaryDir fixture(CLIENT_TEST_DIRECTORY "/late-group-XXXXXX.societycontainer");
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
        iiServerHost::LanPeer transport;
        QVERIFY(transport.startHost("host", "Host", [](const auto &, const auto &) { return QJsonObject{}; },
            {"127.0.0.1"}, QHostAddress::LocalHost));
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
    void accountProofRejectsTamperingExpiryAndCrossAccount() {
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost));
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        QVERIFY(account.login("builder@example.com", "FixtureOnly1!")); QTRY_VERIFY(account.signedIn());
        QTRY_VERIFY(!account.busy()); account.requestPairingCredentials(); QTRY_VERIFY(!account.pairingCredentials().isEmpty());
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
        QTest::newRow("saved-server") << true;
        QTest::newRow("automatic-local-network") << false;
    }
    void downloadsAndUploadsWithoutASocietyApplicationProcess() {
        QFETCH(bool, useRelay);
        AccountServer authority; QVERIFY(authority.server.listen(QHostAddress::LocalHost)); QVERIFY(!authority.profile.isEmpty());
        MemorySessionStore state; AccountSession account(authority.url(), &state, nullptr);
        auto device = account.manager()->deviceInfo(); device["id"] = QString(64, '2');
        QVERIFY(account.manager()->setDeviceInfo(device));
        QVERIFY(account.login("builder@example.com", "FixtureOnly1!")); QTRY_VERIFY(account.signedIn());
        QTRY_VERIFY(!account.busy());
        QTemporaryDir hostRoot(CLIENT_TEST_DIRECTORY "/host-XXXXXX.societycontainer"), phoneRoot(CLIENT_TEST_DIRECTORY "/phone-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(hostRoot.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(phoneRoot.path()));
        QFile model(hostRoot.filePath("Models/model.safetensors")); QVERIFY(model.open(QIODevice::WriteOnly)); model.write(QByteArray(800123, 'm')); model.close();
        QVERIFY(QDir().mkpath(hostRoot.filePath("Files/nested % #")));
        const QMap<QString, QByteArray> downloadFiles{{"Files/nested % #/document.iisc", QByteArray(600123, 'f')},
            {"Generation History/download.png", QByteArray(700123, 'h')}, {"Files/cancel.bin", QByteArray(900123, 'c')},
            {"Files/corrupt.bin", QByteArray(900123, 'o')}, {"Files/switch.bin", QByteArray(900123, 's')}};
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
            iiSocietySync::Replica host, phone;
            QVERIFY(host.open(hostRoot.path(), scope)); QVERIFY(phone.open(phoneRoot.path(), scope));
            iiSocietySync::Synchronizer initial(&phone); initial.setContentPolicy(iiSocietySync::Synchronizer::ContentPolicy::MetadataFirst);
            QSignalSpy done(&initial, &iiSocietySync::Synchronizer::finished);
            connect(&initial, &iiSocietySync::Synchronizer::requestReady, this, [&](auto id, auto, auto wire) {
                initial.receive(id, {{"ok", true}, {"result", host.handle(phoneId, wire.value("message").toObject())}});
            });
            QVERIFY(initial.start(hostId)); QTRY_COMPARE(done.size(), 1); QVERIFY(done[0][1].toBool());
        }
        iiServerHost::RelayServer relay([&](const auto &, auto done) { done({authority.profile.value("sub").toString(), QDateTime::currentDateTimeUtc().addSecs(300)}); });
        QVERIFY(relay.listen(QHostAddress::LocalHost));
        const QUrl endpoint(QString("ws://127.0.0.1:%1").arg(relay.port()));
        if (useRelay) QTRY_VERIFY(account.setServerConfiguration({{"url", endpoint.toString()}, {"host", false}}));
        iiSocietySync::Controller hosting({}); hosting.open(hostRoot.path(), scope); QTRY_VERIFY(hosting.available());
        hosting.setPeers({phoneId}, {});
        QSet<QString> authorizedSessions{phoneId};
        iiServerHost::Peer host;
        const auto handle = [&](const QString &peer, const QJsonObject &payload) {
            if (payload.value("op") == "society.generation")
                return QJsonObject{{"ok", true}, {"peer", peer}, {"echo", payload}};
            // As in Society's host, the LAN/relay handler receives only an
            // authenticated account peer. Direct clients need not advertise a
            // hosted file service in the relay's host directory.
            authorizedSessions.insert(peer); hosting.setPeers(authorizedSessions.values(), {});
            return hosting.handle(peer, payload);
        };
        iiServerHost::PeerOptions options; options.relayUrl = endpoint; options.credential = "fixture";
        options.peerId = hostId; options.name = "Society host"; options.service = "com.iisacc.society.files"; options.localEnabled = false;
        if (useRelay) {
            QVERIFY(host.start(options, handle)); QTRY_VERIFY(host.isReady());
        }
        FakeDiscoveryService discovery, hostDiscovery;
        NearbyDevices hostNearby(&hostDiscovery, QHostAddress::LocalHost);
        iiServerHost::LanPeer hostLan;
        hostLan.setPairingAuthentication([&](const QByteArray &context) { return SocietyPairingCredentials::sign(account.pairingCredentials(), context); },
            [&](const QByteArray &context, const QJsonObject &proof) { return SocietyPairingCredentials::verify(account.pairingCredentials(), context, proof); });
        connect(&hostLan, &iiServerHost::LanPeer::completed, &hosting, [&](auto id, auto response, auto) { hosting.receive(id, response); });
        AutomaticPairing hostPairing(&hostNearby, &hostLan, [&] {
            return hostLan.hosting() || hostLan.startHost(hostId, "Society host", [&](auto peer, auto payload) {
                if (payload.value("op") == "list") return QJsonObject{{"ok", true}, {"entries", QJsonArray()}};
                return handle(peer, payload);
            }, {"127.0.0.1"}, QHostAddress::LocalHost);
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
        verifyDirectHostAccess(account, discovery, hostRoot);
        if (QTest::currentTestFailed()) return;
        verifyNativeFileDownloads(account, discovery, hostDiscovery, hostRoot, phoneRoot);
        if (QTest::currentTestFailed()) return;
        iiSocietyHelper::Client consumer(&account, &discovery, QHostAddress::LocalHost);
        QVERIFY(consumer.requestHost({{"op", "society.generation"}}).isEmpty());
        QVERIFY(consumer.start(phoneRoot.path())); QTRY_VERIFY2_WITH_TIMEOUT(consumer.connected(),
            qPrintable(consumer.status() + ' ' + hostPairing.status() + ' ' + hostLan.phase() + ' ' + hostLan.errorString()), 10000);
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(phoneRoot.path()));
        QSignalSpy responses(&consumer, &iiSocietyHelper::Client::hostResponse);
        const auto rpc = consumer.requestHost({{"op", "society.generation"}, {"action", "submit"}});
        QVERIFY(!rpc.isEmpty()); QTRY_COMPARE(responses.size(), 1);
        QCOMPARE(responses[0][0].toString(), rpc);
        QCOMPARE(responses[0][1].toJsonObject().value("peer").toString(), phoneId);
        QVERIFY(!QFileInfo::exists(phoneRoot.filePath("Models/model.safetensors")));
        const auto request = map.request({"models/model.safetensors"}); QVERIFY(!request.isEmpty()); consumer.synchronizeNow();
        QTRY_COMPARE_WITH_TIMEOUT(map.requestState(request).value("state").toString(), QString("ready"), 15000);
        QVERIFY(map.available({"models/model.safetensors"}));
        QFile image(phoneRoot.filePath("Generation History/generated.png")); QVERIFY(image.open(QIODevice::WriteOnly)); image.write("completed output"); image.close();
        consumer.synchronizeNow();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(hostRoot.filePath("Generation History/generated.png")), 15000);
        QVERIFY(account.logout()); QTRY_VERIFY(!account.signedIn()); QTRY_VERIFY(!consumer.connected());
        QVERIFY(!consumer.container()->available()); QVERIFY(consumer.container()->aliases().isEmpty());
        QVERIFY(consumer.hostSections().isEmpty()); QVERIFY(consumer.statHostEntry("models/model.safetensors").isEmpty());
        QVERIFY(consumer.requestHost({{"op", "society.generation"}}).isEmpty());
        consumer.stop(); hosting.closeAndWait(); host.stop(); relay.stop();
    }
};
QTEST_GUILESS_MAIN(ClientTests)
#include "client.moc"
