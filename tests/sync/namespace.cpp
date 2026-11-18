#include "../TestWorkspace.h"
#include <iiSocietyHelper/Sync/Replica.h>
#include <iiSocietyHelper/Sync/Synchronizer.h>
#include <iiSocietyHelper/Sync/ObjectProvider.h>
#include <iiSocietyHelper/Sync/Controller.h>
#include <SocietyDrive.h>
#include <StorageMap.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include "../SignalRecorder.h"
#include "../QtEventWait.h"
#include <QCoreApplication>
#include <QEvent>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QLockFile>
#include <atomic>

using namespace iiSocietyHelper::Sync;
static const QString scope(64, 'a');
static void put(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    CTEST_CHECK(f.open(QIODevice::WriteOnly)); CTEST_COMPARE(f.write(bytes), bytes.size());
}
static QByteArray get(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
class DelayedProvider final : public ObjectProvider {
public:
    DirectoryObjectProvider disk;
    std::atomic<bool> entered{false}, released{false};
    explicit DelayedProvider(const QString &directory) : disk("slow-nas", directory, true) {}
    QString id() const override { return disk.id(); }
    QString kind() const override { return disk.kind(); }
    bool put(const ObjectIdentity &o, const QString &file, QString *error, ProviderCancellation cancel) override {
        entered = true; QElapsedTimer elapsed; elapsed.start();
        while (!released && elapsed.elapsed() < 5000 && !(cancel && cancel())) QThread::msleep(10);
        return disk.put(o, file, error, cancel);
    }
    bool get(const ObjectIdentity &o, const QString &file, QString *error, ProviderCancellation cancel) override { return disk.get(o, file, error, cancel); }
};
class NamespaceTests : public QObject {
    void cycle(Replica &client, Replica &host, const QString &peer = "client") {
        Synchronizer sync(&client); helper_ctest::SignalRecorder completed(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            sync.receive(id, {{"ok", true}, {"result", host.handle(peer, wire.value("message").toObject())}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE_TIMEOUT(completed.size(), 1, 10000);
        CTEST_CHECK_MESSAGE(completed[0][1].toBool(), qPrintable(completed[0][2].toString()));
    }
public:
    void authorityKeepsHistoryAndStableBoundedSnapshotsOffline() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/namespace-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(dir.path()));
        Replica host; CTEST_CHECK(host.open(dir.path(), scope)); put(dir.filePath("Files/note"), "first"); CTEST_CHECK(host.scan());
        const auto first = host.objectMetadata("files/note"); const auto bound = host.namespaceState().value("sequence").toString().toLongLong();
        put(dir.filePath("Files/note"), "second"); CTEST_CHECK(host.scan());
        const auto second = host.objectMetadata("files/note");
        CTEST_CHECK(host.revision(second.value("revision").toString()).value("parents").toArray().contains(first.value("revision")));
        const auto bounded = host.changes(0, bound).value("entries").toArray();
        bool found = false; for (const auto &e : bounded) if (e.toObject().value("path") == "files/note") { found = true; CTEST_COMPARE(e.toObject().value("version"), first.value("version")); }
        CTEST_CHECK(found);
        CTEST_CHECK(QFile::remove(dir.filePath("Files/note"))); CTEST_CHECK(host.scan());
        CTEST_COMPARE(host.objectMetadata("files/note").value("kind"), "deleted");
        CTEST_COMPARE(host.revision(first.value("revision").toString()).value("object").toObject().value("hash"), first.value("hash"));
        const auto head = host.namespaceState(); host.close(); CTEST_CHECK(host.open(dir.path(), scope)); CTEST_COMPARE(host.namespaceState(), head);
    }
    void clientsOnlyProposeAndAllNodesReceiveTheHostDecision() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/authority-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/mirror-XXXXXX.societycontainer"))), c((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/other-XXXXXX.societycontainer")));
        for (auto dir : {&a, &b, &c}) CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(dir->path()));
        Replica host, phone, pc; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(phone.open(b.path(), scope)); CTEST_CHECK(pc.open(c.path(), scope));
        put(a.filePath("Files/note"), "initial"); cycle(phone, host); cycle(pc, host, "pc");
        const auto confirmed = phone.objectMetadata("files/note"); const auto cursor = phone.namespaceState().value("sequence");
        put(b.filePath("Files/note"), "offline phone edit"); CTEST_CHECK(phone.scan());
        CTEST_COMPARE(phone.namespaceState().value("sequence"), cursor); CTEST_COMPARE(phone.objectMetadata("files/note"), confirmed);
        CTEST_CHECK(!phone.record("files/note").contains("revision")); CTEST_COMPARE(phone.record("files/note").value("baseRevision"), confirmed.value("revision"));
        put(a.filePath("Files/note"), "host truth"); cycle(phone, host); cycle(pc, host, "pc");
        CTEST_COMPARE(get(a.filePath("Files/note")), "host truth"); CTEST_COMPARE(get(b.filePath("Files/note")), "host truth"); CTEST_COMPARE(get(c.filePath("Files/note")), "host truth");
        const auto conflicts = QDir(b.filePath("Files")).entryList({"note.sync-conflict-*"}, QDir::Files); CTEST_COMPARE(conflicts.size(), 1);
        CTEST_COMPARE(get(c.filePath("Files/" + conflicts[0])), "offline phone edit");
        for (auto replica : {&phone, &pc}) {
            CTEST_COMPARE(replica->namespaceState().value("head"), host.namespaceState().value("head"));
            CTEST_COMPARE(replica->objectMetadata("files/note").value("revision"), host.objectMetadata("files/note").value("revision"));
            CTEST_COMPARE(replica->changes().value("entries").toArray().size(), 0);
        }
        CTEST_CHECK(!phone.handle("other", {{"action", "describe"}}).value("ok").toBool());
    }
    void anEditDuringUploadAcknowledgementSurvivesTheHostRevision() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/ack-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/ack-client-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(client.open(b.path(), scope)); cycle(client, host);
        put(b.filePath("Files/note"), "first proposal");
        Synchronizer sync(&client); helper_ctest::SignalRecorder completed(&sync, &Synchronizer::finished); bool edited = false, originalWasExpected = false, editSucceeded = false;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto request = wire.value("message").toObject(); const auto response = host.handle("client", request);
            if (!edited && request.value("action") == "commit" && request.value("entry").toObject().value("path") == "files/note"
                && response.value("ok").toBool()) {
                originalWasExpected = get(a.filePath("Files/note")) == "first proposal";
                QFile pendingEdit(b.filePath("Files/note"));
                const QByteArray bytes("new edit before acknowledgement");
                editSucceeded = pendingEdit.open(QIODevice::WriteOnly) && pendingEdit.write(bytes) == bytes.size();
                edited = true;
            }
            sync.receive(id, {{"ok", true}, {"result", response}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE_TIMEOUT(completed.size(), 1, 10000);
        CTEST_CHECK_MESSAGE(completed[0][1].toBool(), qPrintable(completed[0][2].toString())); CTEST_CHECK(edited);
        CTEST_CHECK(originalWasExpected); CTEST_CHECK(editSucceeded);
        CTEST_COMPARE(get(a.filePath("Files/note")), "new edit before acknowledgement");
        CTEST_COMPARE(get(b.filePath("Files/note")), "new edit before acknowledgement");
        CTEST_COMPARE(client.namespaceState().value("head"), host.namespaceState().value("head"));
        CTEST_CHECK(client.record("files/note").contains("revision"));
        CTEST_CHECK(QDir(b.filePath("Files")).entryList({"note.sync-conflict-*"}, QDir::Files).isEmpty());
    }
    void anEmptyCommittedJournalPageDoesNotNeedTheMutationLock() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/empty-journal-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/empty-journal-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path()));CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host,client;CTEST_CHECK(host.open(a.path(),scope));CTEST_CHECK(client.open(b.path(),scope));
        CTEST_CHECK(client.bindHost("host",host.replicaId(),host.containerId()));
        const auto page=host.journal();CTEST_CHECK(page.value("entries").toArray().isEmpty());
        QLockFile lock(b.filePath(".society-sync/operation.lock"));lock.setStaleLockTime(0);CTEST_CHECK(lock.tryLock(3000));
        CTEST_CHECK(client.acceptJournal("host",page));CTEST_CHECK(QFileInfo::exists(b.filePath(".society-sync/operation.lock")));
        auto forged=page;forged.insert("authority",client.replicaId());CTEST_CHECK(!client.acceptJournal("host",forged));
        CTEST_CHECK(!client.acceptJournal("other",page));
        CTEST_COMPARE(client.namespaceState().value("sequence"),"0");
    }
    void journalRejectsForgedGapsWrongAuthorityAndRollsBackWholePage() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/journal-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/journal-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(client.open(b.path(), scope));
        put(a.filePath("Files/item"), "bytes"); CTEST_CHECK(host.scan()); CTEST_CHECK(client.bindHost("host", host.replicaId(), host.containerId()));
        const auto page = host.journal();
        CTEST_CHECK(!client.acceptJournal("stranger", page));
        auto bad = page; auto entries = bad.value("entries").toArray(); auto last = entries.last().toObject(); last.insert("revision", QString(64, '0')); entries[entries.size() - 1] = last; bad.insert("entries", entries);
        CTEST_CHECK(!client.acceptJournal("host", bad)); CTEST_COMPARE(client.namespaceState().value("sequence"), "0");
        bad = page; bad.insert("authority", client.replicaId()); CTEST_CHECK(!client.acceptJournal("host", bad));
        CTEST_CHECK(client.acceptJournal("host", page));
        auto forged = host.record("files/item"); forged.insert("revision", QString(64, 'f'));
        CTEST_COMPARE(client.handle("host", {{"action", "begin"}, {"entry", forged}}).value("error"), "host_revision_required");
        CTEST_CHECK(!client.acceptJournal("host", page)); // Replay cannot append history again.
        CTEST_COMPARE(client.namespaceState().value("head"), host.namespaceState().value("head"));
    }
    void authorityResolvesAFileAgainstOfflineDirectoryChildrenWithoutLoss() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/file-authority-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/directory-client-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(client.open(b.path(), scope)); cycle(client, host);
        put(a.filePath("Files/object"), "host file"); put(b.filePath("Files/object/child"), "offline child");
        cycle(client, host);
        CTEST_COMPARE(get(a.filePath("Files/object/child")), "offline child"); CTEST_COMPARE(get(b.filePath("Files/object/child")), "offline child");
        const auto conflicts = QDir(a.filePath("Files")).entryList({"object.sync-conflict-*"}, QDir::Files); CTEST_COMPARE(conflicts.size(), 1);
        CTEST_COMPARE(get(b.filePath("Files/" + conflicts[0])), "host file");
        CTEST_COMPARE(client.namespaceState().value("head"), host.namespaceState().value("head"));
    }
    void journalMigrationRetainsCurrentStateWithoutInventingOldHistory() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/migration-XXXXXX.societycontainer"))); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(dir.path()));
        Replica host; CTEST_CHECK(host.open(dir.path(), scope)); put(dir.filePath("Files/item"), "old journal"); CTEST_CHECK(host.scan());
        auto old = host.record("files/item"); const auto identity = host.containerId(); host.close();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "namespace-migration-fixture"); db.setDatabaseName(dir.filePath(".society-sync/journal.sqlite")); CTEST_CHECK(db.open());
            QSqlQuery q(db); CTEST_CHECK(q.exec("DROP TABLE namespace_revisions")); CTEST_CHECK(q.exec("DROP TABLE object_locations")); CTEST_CHECK(q.exec("PRAGMA user_version=2"));
            db.close();
        }
        QSqlDatabase::removeDatabase("namespace-migration-fixture");
        CTEST_CHECK_MESSAGE(host.open(dir.path(), scope), qPrintable(host.errorString())); CTEST_COMPARE(host.containerId(), identity);
        CTEST_COMPARE(host.objectMetadata("files/item").value("version"), old.value("version"));
        const auto r = host.revision(host.objectMetadata("files/item").value("revision").toString()); CTEST_CHECK(r.value("parents").toArray().isEmpty());
        CTEST_COMPARE(get(dir.filePath("Files/item")), "old journal");
    }
    void journalPagesRejectGapsAndResumeFromTheDurableCursor() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/pages-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/pages-client-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(client.open(b.path(), scope));
        for (int i = 0; i < 150; ++i) put(a.filePath("Files/item-" + QString::number(i)), "item");
        CTEST_CHECK(host.scan()); CTEST_CHECK(client.bindHost("host", host.replicaId(), host.containerId()));
        const auto first = host.journal(); CTEST_CHECK(first.value("more").toBool()); CTEST_COMPARE(first.value("entries").toArray().size(), 128);
        auto bad = first; auto rows = bad.value("entries").toArray(); auto r = rows[0].toObject();
        r.remove("revision"); r.insert("sequence", "2");
        r.insert("revision", QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(r).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
        rows[0] = r; bad.insert("entries", rows);
        CTEST_CHECK(!client.acceptJournal("host", bad)); CTEST_COMPARE(client.namespaceState().value("sequence"), "0");
        CTEST_CHECK(client.acceptJournal("host", first)); const auto cursor = client.namespaceState().value("sequence").toString().toLongLong();
        client.close(); CTEST_CHECK(client.open(b.path(), scope)); CTEST_COMPARE(client.namespaceState().value("sequence").toString().toLongLong(), cursor);
        CTEST_CHECK(client.acceptJournal("host", host.journal(cursor, first.value("through").toString().toLongLong())));
        CTEST_COMPARE(client.namespaceState().value("head"), host.namespaceState().value("head"));
        CTEST_CHECK(Replica::claimPrimaryHost(a.path(), scope, "host"));
        CTEST_CHECK(!host.bindHost("other", client.replicaId(), client.containerId())); CTEST_COMPARE(host.errorString(), "authority_cannot_become_mirror");
    }
    void providerCopiesAndRestoresWithoutChangingNamespaceOrRevision() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/provider-host-XXXXXX.societycontainer"))), backing((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/provider-nas-XXXXXX")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(dir.path()));
        Replica host; CTEST_CHECK(host.open(dir.path(), scope)); const auto path = dir.filePath("Models/model.bin");
        const QByteArray bytes(1024 * 1024 + 37, 'm'); put(path, bytes); CTEST_CHECK(host.scan());
        const auto head = host.namespaceState(); const auto metadata = host.objectMetadata("models/model.bin");
        DirectoryObjectProvider nas("nas-a", backing.path(), true);
        CTEST_CHECK_MESSAGE(host.placeObject("models/model.bin", nas), qPrintable(host.errorString()));
        CTEST_COMPARE(host.namespaceState(), head); CTEST_COMPARE(host.objectMetadata("models/model.bin").value("locations").toArray().size(), 2);
        const ObjectIdentity object{host.containerId(), metadata.value("hash").toString(), bytes.size()};
        CTEST_COMPARE(get(backing.filePath(object.key())), bytes);
        CTEST_CHECK(!host.restoreObject("models/model.bin", nas)); CTEST_COMPARE(get(path), bytes);
        CTEST_CHECK(QFile::remove(path)); CTEST_CHECK_MESSAGE(host.restoreObject("models/model.bin", nas), qPrintable(host.errorString()));
        CTEST_COMPARE(get(path), bytes); CTEST_COMPARE(host.namespaceState(), head);
        put(backing.filePath(object.key()), QByteArray(bytes.size(), 'x')); CTEST_CHECK(QFile::remove(path));
        CTEST_CHECK(!host.restoreObject("models/model.bin", nas)); CTEST_CHECK(!QFileInfo::exists(path)); CTEST_COMPARE(host.namespaceState(), head);
        put(path, bytes); CTEST_CHECK(host.scan());
        DirectoryObjectProvider offline("offline", backing.filePath("unmounted"), true);
        CTEST_CHECK(!host.placeObject("models/model.bin", offline)); CTEST_CHECK(host.scan()); CTEST_COMPARE(host.namespaceState(), head);
        QString error; const auto output = dir.filePath("restore.bin"); put(output, "keep");
        CTEST_CHECK(!nas.get(object, output, &error)); CTEST_COMPARE(get(output), "keep");
        CTEST_CHECK(!nas.put(object, path, &error, [] { return true; })); CTEST_COMPARE(error, "provider_cancelled");
    }
    void offlineControllerCommitsWithoutAnyPeersOrInternet() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/offline-controller-XXXXXX.societycontainer"))); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(dir.path()));
        int requests = 0; Controller controller([&](auto, auto) { ++requests; return QString(); });
        helper_ctest::SignalRecorder changed(&controller, &Controller::namespaceChanged); controller.open(dir.path(), scope);
        CTEST_WAIT_DEFAULT(controller.available()); CTEST_WAIT_DEFAULT(!changed.isEmpty()); changed.clear();
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(dir.path()));
        put(dir.filePath("Files/offline"), "host offline");
        // A root/directory batch can publish a head before this file's batch.
        // Wait for the actual committed object before closing the indexer.
        CTEST_WAIT_COMPARE(map.object("files/offline").value("kind"), "file");
        CTEST_WAIT_DEFAULT(!changed.isEmpty());
        CTEST_COMPARE(changed.last()[0].toJsonObject().value("role"), "authority"); CTEST_COMPARE(requests, 0);
        controller.closeAndWait(); Replica host; CTEST_CHECK(host.open(dir.path(), scope)); CTEST_COMPARE(host.objectMetadata("files/offline").value("kind"), "file");
    }
    void slowProviderDoesNotBlockLocalAuthorityCommits() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/slow-provider-host-XXXXXX.societycontainer"))), remote((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/slow-provider-XXXXXX")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(dir.path())); put(dir.filePath("Files/stored"), "stored bytes");
        Controller controller({}); helper_ctest::SignalRecorder changed(&controller, &Controller::namespaceChanged); helper_ctest::SignalRecorder done(&controller, &Controller::providerFinished);
        controller.open(dir.path(), scope); CTEST_WAIT_DEFAULT(!changed.isEmpty());
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(dir.path()));
        CTEST_WAIT_COMPARE(map.object("files/stored").value("kind"), "file");
        auto provider = std::make_shared<DelayedProvider>(remote.path()); const auto release = qScopeGuard([&] { provider->released = true; });
        controller.placeObject("files/stored", provider); CTEST_WAIT_DEFAULT(provider->entered.load());
        const auto before = changed.last()[0].toJsonObject().value("head");
        put(dir.filePath("Files/while-nas-waits"), "local commit");
        CTEST_WAIT_COMPARE_TIMEOUT(map.object("files/while-nas-waits").value("kind"), "file", 3000);
        CTEST_WAIT_DEFAULT(changed.last()[0].toJsonObject().value("head") != before);
        CTEST_CHECK(done.isEmpty()); provider->released = true;
        CTEST_WAIT_COMPARE(done.size(), 1); CTEST_CHECK_MESSAGE(done[0][3].toBool(), qPrintable(done[0][4].toString()));
        controller.closeAndWait();
    }
    void realS3CompatibleProviderRoundTrip() {
        const auto rclone = qEnvironmentVariable("IIWEBSERVER_TEST_RCLONE"); if (rclone.isEmpty()) helper_ctest::skip("Set IIWEBSERVER_TEST_RCLONE for the local S3 fixture");
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/s3-provider-XXXXXX"))); CTEST_CHECK(QDir(dir.path()).mkdir("runtime")); CTEST_CHECK(QDir(dir.path()).mkpath("buckets/bucket"));
        CTEST_CHECK(QFile::setPermissions(dir.filePath("runtime"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QTcpServer probe; CTEST_CHECK(probe.listen(QHostAddress::LocalHost)); const auto port = probe.serverPort(); probe.close();
        iiWebServer::StorageBridgeOptions backend{rclone, {}, {}, dir.filePath("runtime")};
        iiWebServer::FileProtocolServer server(backend); iiWebServer::FileServerOptions options;
        options.protocol = iiWebServer::FileServerProtocol::S3; options.root = dir.filePath("buckets"); options.port = port;
        options.username = "fixture-key"; options.password = "fixture-secret"; options.readOnly = false;
        CTEST_CHECK_MESSAGE(server.start(options), qPrintable(server.errorString()));
        // A TCP connect can precede protocol readiness (and a recycled ephemeral
        // port can self-connect). Require an actual HTTP response from S3.
        CTEST_WAIT_VERIFY(([&] {
            QTcpSocket socket; socket.connectToHost(QHostAddress::LocalHost, port);
            if (!socket.waitForConnected(100) || socket.localPort() == port) return false;
            socket.write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
            return socket.waitForReadyRead(200) && socket.readAll().startsWith("HTTP/1.");
        })(), 30000); // External backend cold start is distinct from transfer latency.
        const auto config = dir.filePath("rclone.conf");
        put(config, QString("[s3fixture]\ntype = s3\nprovider = Other\naccess_key_id = fixture-key\nsecret_access_key = fixture-secret\nendpoint = http://127.0.0.1:%1\nregion = us-east-1\nforce_path_style = true\nno_check_bucket = true\n").arg(port).toUtf8());
        CTEST_CHECK(QFile::setPermissions(config, QFile::ReadOwner | QFile::WriteOwner)); backend.configFile = config;
        RemoteObjectProvider provider("s3-a", "s3", "s3fixture:bucket/society", backend);
        const QByteArray bytes(2 * 1024 * 1024 + 17, 's'); const auto source = dir.filePath("source"), target = dir.filePath("target"); put(source, bytes);
        ObjectIdentity object{QUuid::createUuid().toString(QUuid::WithoutBraces), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()), bytes.size()};
        QString error; CTEST_CHECK_MESSAGE(provider.put(object, source, &error), qPrintable(error)); CTEST_CHECK_MESSAGE(provider.get(object, target, &error), qPrintable(error));
        CTEST_COMPARE(get(target), bytes); server.stop();
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    NamespaceTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("authorityKeepsHistoryAndStableBoundedSnapshotsOffline", [&] { FlushDeferredDeletes cleanup; tests.authorityKeepsHistoryAndStableBoundedSnapshotsOffline(); });
    runner.run("clientsOnlyProposeAndAllNodesReceiveTheHostDecision", [&] { FlushDeferredDeletes cleanup; tests.clientsOnlyProposeAndAllNodesReceiveTheHostDecision(); });
    runner.run("anEditDuringUploadAcknowledgementSurvivesTheHostRevision", [&] { FlushDeferredDeletes cleanup; tests.anEditDuringUploadAcknowledgementSurvivesTheHostRevision(); });
    runner.run("anEmptyCommittedJournalPageDoesNotNeedTheMutationLock", [&] { FlushDeferredDeletes cleanup; tests.anEmptyCommittedJournalPageDoesNotNeedTheMutationLock(); });
    runner.run("journalRejectsForgedGapsWrongAuthorityAndRollsBackWholePage", [&] { FlushDeferredDeletes cleanup; tests.journalRejectsForgedGapsWrongAuthorityAndRollsBackWholePage(); });
    runner.run("authorityResolvesAFileAgainstOfflineDirectoryChildrenWithoutLoss", [&] { FlushDeferredDeletes cleanup; tests.authorityResolvesAFileAgainstOfflineDirectoryChildrenWithoutLoss(); });
    runner.run("journalMigrationRetainsCurrentStateWithoutInventingOldHistory", [&] { FlushDeferredDeletes cleanup; tests.journalMigrationRetainsCurrentStateWithoutInventingOldHistory(); });
    runner.run("journalPagesRejectGapsAndResumeFromTheDurableCursor", [&] { FlushDeferredDeletes cleanup; tests.journalPagesRejectGapsAndResumeFromTheDurableCursor(); });
    runner.run("providerCopiesAndRestoresWithoutChangingNamespaceOrRevision", [&] { FlushDeferredDeletes cleanup; tests.providerCopiesAndRestoresWithoutChangingNamespaceOrRevision(); });
    runner.run("offlineControllerCommitsWithoutAnyPeersOrInternet", [&] { FlushDeferredDeletes cleanup; tests.offlineControllerCommitsWithoutAnyPeersOrInternet(); });
    runner.run("slowProviderDoesNotBlockLocalAuthorityCommits", [&] { FlushDeferredDeletes cleanup; tests.slowProviderDoesNotBlockLocalAuthorityCommits(); });
    runner.run("realS3CompatibleProviderRoundTrip", [&] { FlushDeferredDeletes cleanup; tests.realS3CompatibleProviderRoundTrip(); });
    return runner.result();
}
