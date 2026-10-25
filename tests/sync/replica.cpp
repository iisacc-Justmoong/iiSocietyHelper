#include <iiSocietyHelper/Sync/Replica.h>
#include <iiSocietyHelper/Sync/ObjectProvider.h>
#include "TestLink.h"
#include <SocietyDrive.h>
#include <StorageMaintenance.h>
#include <TemporaryLease.h>
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QLockFile>

using namespace iiSocietyHelper::Sync;
static const QString scope(64, 'a');
static void writeFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data), data.size());
}
static QByteArray readFile(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
class MaintenanceProvider final : public ObjectProvider {
public:
    DirectoryObjectProvider directory;
    QString root, identifier;
    iiSocietyContainer::StorageMaintenance::Report report;
    bool protectedDuringGet = false, journalLockReleased = false;
    MaintenanceProvider(const QString &backing, const QString &container, const QString &id)
        : directory("maintenance-provider", backing), root(container), identifier(id) {}
    QString id() const override { return directory.id(); }
    QString kind() const override { return directory.kind(); }
    bool put(const ObjectIdentity &object, const QString &source, QString *error, ProviderCancellation cancel) override {
        return directory.put(object, source, error, cancel);
    }
    bool get(const ObjectIdentity &object, const QString &target, QString *error, ProviderCancellation cancel) override {
        if (!directory.get(object, target, error, cancel)) return false;
        journalLockReleased = !QFileInfo::exists(QDir(root).filePath(".society-sync/operation.lock"));
        report = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(root.toStdU16String()), identifier.toStdString());
        protectedDuringGet = report.skippedBusy > 0 && QFileInfo::exists(target);
        return protectedDuringGet;
    }
};
class ReplicaTests : public QObject {
    Q_OBJECT
private slots:
    void unchangedHostBindingPreservesItsDurableFile() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/binding-source-XXXXXX.societycontainer"),b(SYNC_TEST_DIRECTORY "/binding-mirror-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path()));QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host,client;QVERIFY(host.open(a.path(),scope));QVERIFY(client.open(b.path(),scope));
        QVERIFY(client.bindHost("host",host.replicaId(),host.containerId()));
        const auto path=b.filePath(".society-sync/mirror.json");const auto durable=readFile(path);
        const auto timestamp=QDateTime::fromSecsSinceEpoch(1000000);QFile file(path);QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(timestamp,QFileDevice::FileModificationTime));file.close();
        QVERIFY(client.bindHost("host",host.replicaId(),host.containerId()));
        QCOMPARE(readFile(path),durable);QCOMPARE(QFileInfo(path).lastModified(),timestamp);
        QVERIFY(QFile::remove(path));QVERIFY(client.bindHost("host",host.replicaId(),host.containerId()));QCOMPARE(readFile(path),durable);
    }
    void inlineCommitRejectsCorruptionAndPreservesThePreviousFile() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/inline-source-XXXXXX.societycontainer"),b(SYNC_TEST_DIRECTORY "/inline-target-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path()));QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        writeFile(a.filePath("Files/item"),"verified payload");Replica source,target;QVERIFY(source.open(a.path(),scope));QVERIFY(target.open(b.path(),scope));QVERIFY(source.scan());
        const auto entry=source.record("files/item");
        auto request=QJsonObject{{"action","commit"},{"entry",entry},{"inlineData",QString::fromLatin1(QByteArray("corrupt").toBase64())}};
        QCOMPARE(target.handle("source",request).value("error"),"invalid_inline_payload");QVERIFY(target.record("files/item").isEmpty());
        request.insert("inlineData",QString::fromLatin1(QByteArray("verified payload").toBase64()));
        QVERIFY(target.handle("source",request).value("complete").toBool());
        QFile file(b.filePath("Files/item"));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("verified payload"));file.close();
        request.insert("inlineData","!");QVERIFY(!target.handle("source",request).value("ok").toBool());
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("verified payload"));
    }
    void metadataPagesReadACommittedWalSnapshotWhileTheOperationLockIsHeld() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/wal-read-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));writeFile(root.filePath("Files/value"),"committed");
        Replica replica;QVERIFY(replica.open(root.path(),scope));QVERIFY(replica.scan());
        const auto changes=replica.changes(),journal=replica.journal();QVERIFY(changes.value("ok").toBool());QVERIFY(journal.value("ok").toBool());
        QVERIFY(replica.publishStorageMap());
        QLockFile operation(root.filePath(".society-sync/operation.lock"));operation.setStaleLockTime(0);QVERIFY(operation.tryLock(3000));
        // An unchanged publication is already durable and must not acquire a
        // fresh filesystem lock merely to report that fact.
        QVERIFY(replica.publishStorageMap());
        QVERIFY(replica.scan()); // A verified, unchanged walk does not create a lock file.
        const auto connection="wal-writer-"+QUuid::createUuid().toString();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(root.filePath(".society-sync/journal.sqlite"));QVERIFY(db.open());QVERIFY(db.transaction());
            QSqlQuery edit(db);QVERIFY(edit.exec("UPDATE metadata SET value='999999' WHERE key='sequence'"));
            QVERIFY(edit.exec("UPDATE entries SET data='{}',sequence=999999"));
            QCOMPARE(replica.changes(),changes);QCOMPARE(replica.journal(),journal);
            QVERIFY(QFileInfo::exists(root.filePath(".society-sync/operation.lock")));QVERIFY(db.rollback());db.close();
        }
        QSqlDatabase::removeDatabase(connection);operation.unlock();QCOMPARE(replica.changes(),changes);
    }
    void unchangedPeerCursorsDoNotCreateAnotherDurableRevision() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/peer-noop-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));Replica replica;QVERIFY(replica.open(root.path(),scope));
        const auto peer=QUuid::createUuid().toString(),container=QUuid::createUuid().toString();
        QVERIFY(replica.savePeerState("peer",peer,container,5,8));
        const auto connection="peer-observer-"+QUuid::createUuid().toString();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(root.filePath(".society-sync/journal.sqlite"));QVERIFY(db.open());
            auto revision=[&]{QSqlQuery query(db);if(!query.exec("PRAGMA data_version")||!query.next())return qint64(-1);return query.value(0).toLongLong();};
            const auto before=revision();QVERIFY(before>=0);
            QVERIFY(replica.savePeerState("peer",peer,container,5,8));QVERIFY(replica.savePeerState("peer",peer,container,1,2));
            QCOMPARE(revision(),before);QCOMPARE(replica.peerState("peer").value("pushed").toString(),"8");
            QVERIFY(replica.savePeerState("peer",peer,container,6,8));QVERIFY(revision()!=before);db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void failedNamespaceMigrationRollsBackTheWholeNewJournal() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/schema-rollback-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        QVERIFY(QDir().mkpath(root.filePath(".society-sync")));
        const auto path=root.filePath(".society-sync/journal.sqlite");
        const auto name="schema-probe-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(path);QVERIFY(db.open());
            QSqlQuery query(db);QVERIFY(query.exec("CREATE TABLE namespace_revisions(unexpected INTEGER)"));
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
        Replica replica;QVERIFY(!replica.open(root.path(),scope));replica.close();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(path);QVERIFY(db.open());
            QSqlQuery query(db);QVERIFY(query.exec("PRAGMA user_version"));QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(),0);query.finish();
            QVERIFY(!db.tables().contains("metadata"));QVERIFY(!db.tables().contains("entries"));
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    void maintenancePreservesLeasedCapabilityProbeAndRetainedHardLink() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/lease-capability-probe-XXXXXX.societycontainer");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        QVERIFY(QDir().mkpath(root.filePath(".society-sync")));
        const auto probe = root.filePath(".society-sync/capability-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto retained = root.filePath("Files/retained");
        {
            iiFileProvider::TemporaryLease lease(std::filesystem::path(root.filePath(".society-sync").toStdU16String()),
                iiFileProvider::TemporaryLease::Mode::Shared);
            QVERIFY(lease.locked()); writeFile(probe, "probe bytes");
            std::error_code error;
            std::filesystem::create_hard_link(std::filesystem::path(probe.toStdU16String()),
                std::filesystem::path((probe + ".link").toStdU16String()), error); QVERIFY(!error);
            std::filesystem::create_hard_link(std::filesystem::path(probe.toStdU16String()),
                std::filesystem::path(retained.toStdU16String()), error); QVERIFY(!error);
            const auto active = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(root.path().toStdU16String()), drive->identifier().toStdString());
            QVERIFY(active.errors.empty()); QVERIFY(active.skippedBusy > 0); QCOMPARE(active.tempRemoved, std::uint64_t(0));
            QCOMPARE(readFile(probe), QByteArray("probe bytes")); QCOMPARE(readFile(probe + ".link"), QByteArray("probe bytes"));
        }
        const auto cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(root.path().toStdU16String()), drive->identifier().toStdString());
        QVERIFY(cleanup.errors.empty()); QCOMPARE(cleanup.skippedBusy, std::uint64_t(0));
        QCOMPARE(cleanup.tempRemoved, std::uint64_t(2)); QVERIFY(!QFileInfo::exists(probe)); QVERIFY(!QFileInfo::exists(probe + ".link"));
        QCOMPARE(readFile(retained), QByteArray("probe bytes"));
    }
    void leaseSentinelNeverBecomesAReplicatedObject() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/lease-private-sentinel-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        {
            iiFileProvider::TemporaryLease lease(std::filesystem::path(root.filePath("Files").toStdU16String()),
                iiFileProvider::TemporaryLease::Mode::Shared);
            QVERIFY(lease.locked());
        }
        QVERIFY(QFileInfo::exists(root.filePath("Files/.iisacc-temporary.lock")));
        writeFile(root.filePath("Files/public"), "visible");
        Replica replica; QVERIFY(replica.open(root.path(), scope)); QVERIFY(replica.scan());
        QVERIFY(replica.record("files/.iisacc-temporary.lock").isEmpty());
        const auto entries = replica.changes().value("entries").toArray();
        for (const auto &entry : entries) QVERIFY(!entry.toObject().value("path").toString().contains(".iisacc-temporary.lock"));
        auto forged = replica.record("files/public"); forged.insert("path", "files/.iisacc-temporary.lock");
        QJsonObject payload;
        for (const auto *key : {"path", "kind", "size", "hash", "clock"}) payload.insert(key, forged.value(key));
        forged.insert("version", QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
        QVERIFY(!Replica::validRecord(forged));
        QVERIFY(!replica.handle("sender", {{"action", "begin"}, {"entry", forged}}).value("ok").toBool());
        QCOMPARE(QFileInfo(root.filePath("Files/.iisacc-temporary.lock")).size(), qint64(0));
    }
    void presentationsAreRegeneratedWhileUserClassificationStillReplicates() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/presentation-private-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        writeFile(root.filePath("Files/public.txt"), "visible original");
        writeFile(root.filePath("Files/public.txt.iisacc.json"), "{\"rating\":5,\"keywords\":[\"Work\"]}");
        writeFile(root.filePath("Files/.public.txt.iisacc-preview"), "local identity and preview bytes");
        writeFile(root.filePath("Files/.iifile-stage-fixture.iisacc-preview"), "partial preview");
        QVERIFY(QDir().mkpath(root.filePath("Files/.iisacc-file-provider")));
        writeFile(root.filePath("Files/.iisacc-file-provider/catalog.sqlite"), "private catalog");
        Replica replica; QVERIFY(replica.open(root.path(), scope)); QVERIFY(replica.scan());
        QVERIFY(!replica.record("files/public.txt").isEmpty());
        QVERIFY(!replica.record("files/public.txt.iisacc.json").isEmpty());
        QVERIFY(replica.record("files/.public.txt.iisacc-preview").isEmpty());
        QVERIFY(replica.record("files/.iifile-stage-fixture.iisacc-preview").isEmpty());
        QVERIFY(replica.record("files/.iisacc-file-provider").isEmpty());
        for (const auto &entry : replica.changes().value("entries").toArray()) {
            const auto path = entry.toObject().value("path").toString();
            QVERIFY(!path.contains(".iisacc-preview"));
            QVERIFY(!path.contains(".iisacc-file-provider"));
        }
    }
    void maintenancePreservesActiveTransferAndRemovesCompletedOrClosedOrphans() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/lease-source-XXXXXX.societycontainer"), b(SYNC_TEST_DIRECTORY "/lease-target-XXXXXX.societycontainer");
        const auto sourceDrive = iiSocietyContainer::SocietyDrive::create(a.path()); QVERIFY(sourceDrive);
        const auto targetDrive = iiSocietyContainer::SocietyDrive::create(b.path()); QVERIFY(targetDrive);
        Replica source, target; QVERIFY(source.open(a.path(), scope)); QVERIFY(target.open(b.path(), scope));
        const QByteArray bytes("active transfer bytes"); writeFile(a.filePath("Files/item"), bytes); QVERIFY(source.scan());
        const auto entry = source.record("files/item");
        const auto begin = target.handle("sender", {{"action", "begin"}, {"entry", entry}});
        QVERIFY(begin.value("ok").toBool()); QVERIFY(!begin.value("complete").toBool());
        const QDir transfers(b.filePath(".society-sync/transfers"));
        auto parts = transfers.entryList({"*.part"}, QDir::Files); QCOMPARE(parts.size(), 1);
        const auto partial = transfers.filePath(parts.first());
        const auto prefix = bytes.left(7);
        QVERIFY(target.handle("sender", {{"action", "chunk"}, {"entry", entry}, {"offset", "0"},
            {"data", QString::fromLatin1(prefix.toBase64())}}).value("ok").toBool());
        auto cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(b.path().toStdU16String()), targetDrive->identifier().toStdString());
        QVERIFY(cleanup.errors.empty()); QVERIFY(cleanup.skippedBusy > 0); QCOMPARE(cleanup.tempRemoved, std::uint64_t(0));
        QCOMPARE(readFile(partial), prefix); QVERIFY(!QFileInfo::exists(b.filePath("Files/item")));
        QVERIFY(target.handle("sender", {{"action", "chunk"}, {"entry", entry}, {"offset", QString::number(prefix.size())},
            {"data", QString::fromLatin1(bytes.mid(prefix.size()).toBase64())}}).value("ok").toBool());
        QVERIFY(target.handle("sender", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool());
        QCOMPARE(readFile(b.filePath("Files/item")), bytes); QVERIFY(!QFileInfo::exists(partial));
        const auto orphan = transfers.filePath(QString(64, 'f') + ".part"); writeFile(orphan, "orphan");
        cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(b.path().toStdU16String()), targetDrive->identifier().toStdString());
        QVERIFY(cleanup.errors.empty()); QCOMPARE(cleanup.skippedBusy, std::uint64_t(0));
        QCOMPARE(cleanup.tempRemoved, std::uint64_t(1)); QVERIFY(!QFileInfo::exists(orphan));
        // A replica stays open while idle; only unfinished transfers hold a lease.
        QVERIFY(target.isOpen());
        writeFile(a.filePath("Files/abandoned"), bytes); QVERIFY(source.scan());
        const auto abandoned = source.record("files/abandoned");
        QVERIFY(target.handle("sender", {{"action", "begin"}, {"entry", abandoned}}).value("ok").toBool());
        QVERIFY(target.handle("sender", {{"action", "chunk"}, {"entry", abandoned}, {"offset", "0"},
            {"data", QString::fromLatin1(prefix.toBase64())}}).value("ok").toBool());
        parts = transfers.entryList({"*.part"}, QDir::Files); QCOMPARE(parts.size(), 1);
        const auto abandonedPart = transfers.filePath(parts.first());
        target.close();
        cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(b.path().toStdU16String()), targetDrive->identifier().toStdString());
        QVERIFY(cleanup.errors.empty()); QCOMPARE(cleanup.skippedBusy, std::uint64_t(0));
        QCOMPARE(cleanup.tempRemoved, std::uint64_t(1)); QVERIFY(!QFileInfo::exists(abandonedPart));
        QVERIFY(!QFileInfo::exists(b.filePath("Files/abandoned")));
        QVERIFY(target.open(b.path(), scope));
        QCOMPARE(target.handle("sender", {{"action", "begin"}, {"entry", abandoned}}).value("offset").toString(), "0");
    }
    void providerRestoreKeepsTemporaryLeaseWhileJournalLockIsReleased() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/lease-provider-host-XXXXXX.societycontainer"), backing(SYNC_TEST_DIRECTORY "/lease-provider-backup-XXXXXX");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        const auto path = root.filePath("Files/stored"); const QByteArray bytes("provider restored bytes"); writeFile(path, bytes);
        Replica replica; QVERIFY(replica.open(root.path(), scope)); QVERIFY(replica.scan());
        MaintenanceProvider provider(backing.path(), root.path(), drive->identifier());
        QVERIFY(replica.placeObject("files/stored", provider)); QVERIFY(QFile::remove(path));
        QVERIFY2(replica.restoreObject("files/stored", provider), qPrintable(replica.errorString()));
        QVERIFY(provider.journalLockReleased); QVERIFY(provider.protectedDuringGet); QVERIFY(provider.report.errors.empty());
        QCOMPARE(provider.report.tempRemoved, std::uint64_t(0)); QCOMPARE(readFile(path), bytes);
        const auto orphan = root.filePath(".society-sync/transfers/provider-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".part");
        writeFile(orphan, "provider orphan");
        const auto cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(root.path().toStdU16String()), drive->identifier().toStdString());
        QVERIFY(cleanup.errors.empty()); QCOMPARE(cleanup.skippedBusy, std::uint64_t(0));
        QCOMPARE(cleanup.tempRemoved, std::uint64_t(1)); QVERIFY(!QFileInfo::exists(orphan));
        QVERIFY(replica.isOpen());
    }
    void formerDefaultNamesAcceptRemoteDeletionReplacementAndReplay() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/fixed-files-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path()));
        for (const auto *name : {"Documents", "Audios", "3D objects"})
            QVERIFY(QDir().mkpath(dir.filePath("Files/" + QString(name))));
        Replica replica; QVERIFY(replica.open(dir.path(), scope)); QVERIFY(replica.scan());
        const auto revision = [&](const QString &path, const QString &kind, const QByteArray &bytes = {}) {
            auto clock = replica.record(path).value("clock").toObject();
            clock.insert(replica.replicaId(), QString::number(clock.value(replica.replicaId()).toString().toLongLong() + 1));
            QJsonObject entry{{"path", path}, {"kind", kind}, {"size", QString::number(bytes.size())},
                {"hash", kind == "file" ? QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) : QString()}, {"clock", clock}};
            entry.insert("version", QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(entry).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
            return entry;
        };
        for (const auto name : {"Documents", "Audios", "3D objects"}) {
            const auto path = QString("files/") + name;
            const auto deleted = revision(path, "deleted"); QVERIFY(Replica::validRecord(deleted));
            for (int retry = 0; retry < 2; ++retry) {
                const auto response = replica.handle("legacy", {{"action", "apply"}, {"entry", deleted}});
                QVERIFY2(response.value("ok").toBool(), qPrintable(QJsonDocument(response).toJson()));
                QVERIFY(response.value("complete").toBool());
                QVERIFY(!QFileInfo::exists(dir.filePath("Files/" + QString(name))));
                QCOMPARE(replica.record(path).value("kind"), "deleted");
            }
        }
        const QByteArray contents("preserved legacy file");
        const auto replacement = revision("files/Documents", "file", contents);
        QVERIFY(Replica::validRecord(replacement));
        auto response = replica.handle("legacy", {{"action", "begin"}, {"entry", replacement}});
        QVERIFY(response.value("ok").toBool()); QVERIFY(!response.value("complete").toBool());
        QVERIFY(replica.handle("legacy", {{"action", "chunk"}, {"entry", replacement}, {"offset", "0"}, {"data", QString::fromLatin1(contents.toBase64())}}).value("ok").toBool());
        QVERIFY(replica.handle("legacy", {{"action", "commit"}, {"entry", replacement}}).value("ok").toBool());
        QCOMPARE(readFile(dir.filePath("Files/Documents")), contents);
        QCOMPARE(replica.record("files/Documents").value("kind"), "file");
        QVERIFY(QDir(dir.filePath("Files")).entryList({"Documents.sync-conflict-*"}, QDir::Files).isEmpty());
        QVERIFY(replica.handle("legacy", {{"action", "begin"}, {"entry", replacement}}).value("complete").toBool());
        QVERIFY(QFile::remove(dir.filePath("Files/Documents")));
        writeFile(dir.filePath("Files/Documents/movie.mp4"), "editable video"); QVERIFY(replica.scan());
        const auto child = revision("files/Documents/movie.mp4", "deleted");
        QVERIFY(replica.handle("legacy", {{"action", "apply"}, {"entry", child}}).value("ok").toBool());
        QVERIFY(!QFileInfo::exists(dir.filePath("Files/Documents/movie.mp4")));
        QVERIFY(QFileInfo(dir.filePath("Files/Documents")).isDir());
    }

    void hostAdoptionArchivesUserDirectoriesAndLeavesFilesEmpty() {
        QTemporaryDir local(SYNC_TEST_DIRECTORY "/fixed-adoption-XXXXXX.societycontainer"), host(SYNC_TEST_DIRECTORY "/fixed-host-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(local.path()));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(host.path()));
        writeFile(local.filePath("Files/Documents/old.mp4"), "local video");
        for (int i=0;i<8;++i) writeFile(local.filePath("Files/old-"+QString::number(i)+".txt"),"entry-"+QByteArray::number(i));
        Replica client, remote; QVERIFY(client.open(local.path(), scope)); QVERIFY(remote.open(host.path(), scope));
        QVERIFY(client.bindHost("host", remote.replicaId(), remote.containerId()));
        const auto recovery = client.binding().value("recovery").toString(); QVERIFY(!recovery.isEmpty());
        QCOMPARE(readFile(local.filePath(recovery + "/Files/Documents/old.mp4")), "local video");
        for (int i=0;i<8;++i)
            QCOMPARE(readFile(local.filePath(recovery+"/Files/old-"+QString::number(i)+".txt")),"entry-"+QByteArray::number(i));
        QVERIFY(client.bindHost("host",remote.replicaId(),remote.containerId()));
        QVERIFY(!QFileInfo::exists(local.filePath("Files/Documents/old.mp4")));
        QVERIFY(client.bootstrapping());
        QVERIFY(QDir(local.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        client.close(); QVERIFY(client.open(local.path(), scope));
        QVERIFY(client.completeBootstrap());
        QVERIFY(iiSocietyContainer::SocietyDrive::open(local.path())->isReady());
    }
    void manifestHashAndOrderingAreCheckedWithoutWritingFileBytes() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/manifest-source-XXXXXX.societycontainer"), b(SYNC_TEST_DIRECTORY "/manifest-host-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica sender, host; QVERIFY(sender.open(a.path(), scope)); QVERIFY(host.open(b.path(), scope)); QVERIFY(sender.scan());
        const auto baseline = sender.changes().value("through").toString().toLongLong();
        writeFile(a.filePath("Files/one"), "first"); writeFile(a.filePath("Files/two"), "second");
        QVERIFY(sender.scan());
        const auto changes = sender.changes(baseline); auto entries = changes.value("entries").toArray(); QCOMPARE(entries.size(), 2);
        for (qsizetype i = 0; i < entries.size(); ++i) {
            auto proposal = entries[i].toObject();
            for (const auto *key : {"namespace", "authority", "revision"}) proposal.remove(key);
            proposal.insert("baseRevision", ""); entries[i] = proposal;
        }
        QByteArray records; for (const auto &entry : entries) records += QJsonDocument(entry.toObject()).toJson(QJsonDocument::Compact) + '\n';
        const auto id = QString::fromLatin1(QCryptographicHash::hash(records, QCryptographicHash::Sha256).toHex());
        QJsonObject page{{"action", "manifest"}, {"container", host.containerId()}, {"replica", sender.replicaId()}, {"manifest", id},
            {"offset", "0"}, {"total", "2"}, {"after", QString::number(baseline)}, {"through", changes.value("through")}, {"entries", QJsonArray{entries[0]}}};
        auto response = host.handle("sender", page); QVERIFY(response.value("ok").toBool()); QVERIFY(!response.value("complete").toBool());
        QCOMPARE(host.handle("sender", {{"action", "begin"}, {"manifest", id}, {"entry", entries[0]}}).value("error"), "manifest_required_before_transfer");
        auto outOfOrder = page; outOfOrder.insert("offset", "2"); outOfOrder.insert("entries", QJsonArray{});
        QVERIFY(!host.handle("sender", outOfOrder).value("ok").toBool());
        page.insert("offset", "1"); page.insert("entries", QJsonArray{entries[1]});
        response = host.handle("sender", page); QVERIFY(response.value("ok").toBool()); QVERIFY(response.value("complete").toBool());
        QCOMPARE(QDir(b.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0); QVERIFY(host.record("files/one").isEmpty());
        QCOMPARE(host.changes().value("through").toString(), "0");
        page.insert("offset", "0"); page.insert("entries", entries); page.insert("manifest", QString(64, 'f'));
        QCOMPARE(host.handle("sender", page).value("error"), "manifest_hash_mismatch");
        QCOMPARE(QDir(b.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
    }
    void sectionInventoryAndLocalBoundary() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/replica-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path()));
        Replica replica; QVERIFY2(replica.open(dir.path(), scope), qPrintable(replica.errorString()));
        QVERIFY2(replica.scan(), qPrintable(replica.errorString()));
        const auto initial = replica.changes();
        for (const auto &value : initial.value("entries").toArray()) {
            const auto entry = value.toObject(); QVERIFY(Replica::validRecord(entry));
            QCOMPARE(entry.value("kind").toString(), "directory");
            const auto path = entry.value("path").toString();
            QVERIFY(path.startsWith("models/"));
        }
        const auto baseline = initial.value("through").toString().toLongLong();
        for (const auto section : iiSocietyContainer::allStoreSections())
            writeFile(dir.filePath(iiSocietyContainer::storeSectionName(section) + "/one.txt"), "data");
        writeFile(dir.filePath("private-cookie.json"), "must not cross devices");
        QVERIFY2(replica.scan(), qPrintable(replica.errorString()));
        const auto manifest = replica.changes(baseline); QVERIFY(manifest.value("ok").toBool());
        QCOMPARE(manifest.value("entries").toArray().size(), 9);
        for (const auto &value : manifest.value("entries").toArray()) QVERIFY(Replica::validRecord(value.toObject()));
        QVERIFY(replica.record("private-cookie.json").isEmpty());
        const auto id = replica.replicaId(); replica.close();
        QVERIFY(replica.open(dir.path(), scope)); QCOMPARE(replica.replicaId(), id);
        QVERIFY(!replica.open(dir.path(), QString(64, 'b')));
    }
    void chunksResumeAndBadDataCannotReplaceExistingFiles() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/send-XXXXXX.societycontainer"), b(SYNC_TEST_DIRECTORY "/receive-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        const QByteArray bytes(700000, 'x'); writeFile(a.filePath("Files/large"), bytes);
        Replica source, target; QVERIFY(source.open(a.path(), scope)); QVERIFY(target.open(b.path(), scope)); QVERIFY(source.scan());
        const auto entry = source.record("files/large");
        const QJsonObject begin{{"action", "begin"}, {"entry", entry}};
        auto result = target.handle("sender", begin); QVERIFY(result.value("ok").toBool());
        auto chunk = source.handle("receiver", {{"action", "read"}, {"entry", entry}, {"offset", "0"}});
        QVERIFY(chunk.value("ok").toBool());
        QJsonObject put{{"action", "chunk"}, {"entry", entry}, {"offset", "0"}, {"data", chunk.value("data")}};
        QVERIFY(target.handle("sender", put).value("ok").toBool());
        QVERIFY(target.handle("sender", put).value("ok").toBool()); // Identical retry is idempotent.
        put.insert("data", "!!!!"); QVERIFY(!target.handle("sender", put).value("ok").toBool());
        target.close(); QVERIFY(target.open(b.path(), scope));
        result = target.handle("sender", begin); QCOMPARE(result.value("offset").toString(), QString::number(Replica::ChunkBytes));
        QVERIFY(!QFileInfo::exists(b.filePath("Files/large")));
        for (qint64 offset = Replica::ChunkBytes; offset < bytes.size(); offset += Replica::ChunkBytes) {
            chunk = source.handle("receiver", {{"action", "read"}, {"entry", entry}, {"offset", QString::number(offset)}});
            QVERIFY(chunk.value("ok").toBool());
            QVERIFY(target.handle("sender", {{"action", "chunk"}, {"entry", entry}, {"offset", QString::number(offset)}, {"data", chunk.value("data")}}).value("ok").toBool());
        }
        QVERIFY2(target.handle("sender", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool(), qPrintable(target.errorString()));
        QCOMPARE(readFile(b.filePath("Files/large")), bytes);
        QVERIFY(target.handle("sender", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool());
        auto bad = entry; bad.insert("path", "files/../../outside"); QVERIFY(!Replica::validRecord(bad));
        QVERIFY(!target.handle("sender", {{"action", "begin"}, {"entry", bad}}).value("ok").toBool());
    }
    void deletionIsPersistentAndTheSameClockHasOneValue() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/delete-XXXXXX.societycontainer"); QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path()));
        writeFile(dir.filePath("Files/item"), "before"); Replica replica; QVERIFY(replica.open(dir.path(), scope)); QVERIFY(replica.scan());
        auto entry = replica.record("files/item");
        auto forged = entry; forged.insert("hash", QString(64, 'e'));
        QVERIFY(!replica.handle("sender", {{"action", "begin"}, {"entry", forged}}).value("ok").toBool());
        QVERIFY(QFile::remove(dir.filePath("Files/item"))); QVERIFY(replica.scan());
        QCOMPARE(replica.record("files/item").value("kind").toString(), "deleted");
        replica.close(); QVERIFY(replica.open(dir.path(), scope));
        QCOMPARE(replica.record("files/item").value("kind").toString(), "deleted");
    }
    void corruptCompleteTransferPreservesDestinationAndCanRetry() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/hash-a-XXXXXX.societycontainer"), b(SYNC_TEST_DIRECTORY "/hash-b-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica source, target; QVERIFY(source.open(a.path(), scope)); QVERIFY(target.open(b.path(), scope));
        QVERIFY(target.scan());
        const auto baseline = target.changes().value("through").toString().toLongLong();
        auto transfer = [&](const QByteArray &data, bool corrupt) {
            writeFile(a.filePath("Files/item"), data); QVERIFY(source.scan());
            const auto entry = source.record("files/item");
            QVERIFY(target.handle("source", {{"action", "begin"}, {"entry", entry}}).value("ok").toBool());
            const auto bytes = corrupt ? QByteArray(data.size(), '?') : data;
            QVERIFY(target.handle("source", {{"action", "chunk"}, {"entry", entry}, {"offset", "0"}, {"data", QString::fromLatin1(bytes.toBase64())}}).value("ok").toBool());
            QCOMPARE(target.handle("source", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool(), !corrupt);
        };
        transfer("original", false);
        transfer("replacement", true);
        QCOMPARE(readFile(b.filePath("Files/item")), QByteArray("original"));
        const auto entry = source.record("files/item");
        QCOMPARE(target.handle("source", {{"action", "begin"}, {"entry", entry}}).value("offset").toString(), "0");
        transfer("replacement", false);
        QCOMPARE(readFile(b.filePath("Files/item")), QByteArray("replacement"));
        const QDir recovery(b.filePath(".society-sync/recovery"));
        const auto metadataFiles = recovery.entryList({"*.json"}, QDir::Files); QCOMPARE(metadataFiles.size(), 1);
        const auto metadata = QJsonDocument::fromJson(readFile(recovery.filePath(metadataFiles[0]))).object();
        QCOMPARE(metadata.value("path").toString(), "Files/item");
        QCOMPARE(readFile(b.filePath(metadata.value("backup").toString())), QByteArray("original"));
        QVERIFY(target.scan()); QCOMPARE(target.changes(baseline).value("entries").toArray().size(), 1);
    }
    void redirectedFilesAndReplacedContainerFailClosed() {
        QTemporaryDir parent(SYNC_TEST_DIRECTORY "/boundary-XXXXXX");
        const auto root = parent.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(root));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root));
        writeFile(root + "/Files/item", "local"); writeFile(parent.filePath("outside"), "outside");
        Replica replica; QVERIFY(replica.open(root, scope)); QVERIFY(replica.scan());
        const auto entry = replica.record("files/item");
        QVERIFY(QFile::remove(root + "/Files/item")); QVERIFY(nativeTestLink(parent.filePath("outside"), root + "/Files/item"));
        QVERIFY(!replica.scan());
        QVERIFY(!replica.handle("peer", {{"action", "read"}, {"entry", entry}, {"offset", "0"}}).value("ok").toBool());
        QVERIFY(!replica.handle("peer", {{"action", "begin"}, {"entry", entry}}).value("ok").toBool());
        QCOMPARE(readFile(parent.filePath("outside")), QByteArray("outside"));
        QCOMPARE(replica.record("files/item").value("version"), entry.value("version"));
#ifdef Q_OS_WIN
        QVERIFY(!QDir(parent.path()).rename("drive.societycontainer", "old"));
        QVERIFY(QFileInfo::exists(root + "/.society-drive.json"));
        replica.close();
        QVERIFY(removeNativeTestLink(root + "/Files/item"));
#else
        QVERIFY(QDir(parent.path()).rename("drive.societycontainer", "old"));
        QVERIFY(QDir().mkdir(root));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root));
        QVERIFY(!replica.scan());
        QVERIFY(!replica.handle("peer", {{"action", "begin"}, {"entry", entry}}).value("ok").toBool());
        QVERIFY(!QFileInfo::exists(root + "/Files/item"));
#endif
    }
    void redirectedJournalCannotWriteLocksOutsideContainer() {
        QTemporaryDir parent(SYNC_TEST_DIRECTORY "/journal-XXXXXX");
        const auto root = parent.filePath("drive.societycontainer"); QVERIFY(QDir().mkdir(root)); QVERIFY(iiSocietyContainer::SocietyDrive::create(root));
        Replica replica; QVERIFY(replica.open(root, scope));
#ifdef Q_OS_WIN
        QVERIFY(!QDir(root).rename(".society-sync", "saved-state"));
        QVERIFY(replica.scan());
        replica.close();
        QVERIFY(QDir(root).rename(".society-sync", "saved-state"));
#else
        QVERIFY(QDir(root).rename(".society-sync", "saved-state"));
        QVERIFY(QDir().mkdir(parent.filePath("outside")));
        QVERIFY(nativeTestLink(parent.filePath("outside"), root + "/.society-sync"));
        QVERIFY(!replica.scan());
        QCOMPARE(QDir(parent.filePath("outside")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
        replica.close();
        QVERIFY(removeNativeTestLink(root + "/.society-sync"));
#endif
    }
    void filenameAliasesCannotOverwriteDistinctNames() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/case-a-XXXXXX.societycontainer"), b(SYNC_TEST_DIRECTORY "/case-b-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        writeFile(a.filePath("Files/item"), "incoming"); writeFile(b.filePath("Files/ITEM"), "original");
        if (!QFileInfo::exists(b.filePath("Files/item"))) QSKIP("This filesystem distinguishes filename case.");
        Replica source, target; QVERIFY(source.open(a.path(), scope)); QVERIFY(target.open(b.path(), scope)); QVERIFY(source.scan());
        const auto result = target.handle("source", {{"action", "begin"}, {"entry", source.record("files/item")}});
        QVERIFY(!result.value("ok").toBool()); QCOMPARE(result.value("error").toString(), "filename_normalization_collision");
        QCOMPARE(readFile(b.filePath("Files/ITEM")), QByteArray("original"));
        writeFile(a.filePath("Files/sub/new"), "child"); QVERIFY(QDir().mkdir(b.filePath("Files/SUB"))); QVERIFY(source.scan());
        const auto child = target.handle("source", {{"action", "begin"}, {"entry", source.record("files/sub/new")}});
        QCOMPARE(child.value("error").toString(), "filename_normalization_collision");
        QVERIFY(!QFileInfo::exists(b.filePath("Files/SUB/new")));
    }
    void manifestPagesKeepAStableCursorWhileNewChangesWaitForNextCycle() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/pages-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        Replica replica; QVERIFY(replica.open(root.path(), scope)); QVERIFY(replica.scan());
        const auto baseline = replica.changes().value("through").toString().toLongLong();
        for (int i = 0; i < 260; ++i) writeFile(root.filePath("Files/" + QString::number(i)), "data");
        QVERIFY(replica.scan());
        auto page = replica.changes(baseline); QCOMPARE(page.value("entries").toArray().size(), 128);
        const auto upper = page.value("through").toString().toLongLong();
        writeFile(root.filePath("Files/259"), "changed after page one"); QVERIFY(replica.scan());
        QSet<QString> paths;
        while (true) {
            QVERIFY(page.value("ok").toBool());
            QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() < 300000);
            for (const auto &entry : page.value("entries").toArray()) {
                const auto path = entry.toObject().value("path").toString(); QVERIFY(!paths.contains(path)); paths.insert(path);
            }
            if (!page.value("more").toBool()) break;
            page = replica.changes(page.value("next").toString().toLongLong(), upper);
        }
        QCOMPARE(page.value("next").toString().toLongLong(), upper);
        page = replica.changes(upper); QCOMPARE(page.value("entries").toArray().size(), 1);
        QCOMPARE(page.value("entries").toArray()[0].toObject().value("path").toString(), "files/259");
        paths.insert("files/259"); QCOMPARE(paths.size(), 260);
    }
};
QTEST_GUILESS_MAIN(ReplicaTests)
#include "replica.moc"
