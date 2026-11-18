#include "../TestWorkspace.h"
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
#include "../CTestSupport.h"
#include <QCoreApplication>
#include <QEvent>
#include <QScopeGuard>
#include <QUuid>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QLockFile>

using namespace iiSocietyHelper::Sync;
static const QString scope(64, 'a');
static void writeFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    CTEST_CHECK(f.open(QIODevice::WriteOnly)); CTEST_COMPARE(f.write(data), data.size());
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
public:
    void unchangedHostBindingPreservesItsDurableFile() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/binding-source-XXXXXX.societycontainer"))),b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/binding-mirror-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path()));CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host,client;CTEST_CHECK(host.open(a.path(),scope));CTEST_CHECK(client.open(b.path(),scope));
        CTEST_CHECK(client.bindHost("host",host.replicaId(),host.containerId()));
        const auto path=b.filePath(".society-sync/mirror.json");const auto durable=readFile(path);
        const auto timestamp=QDateTime::fromSecsSinceEpoch(1000000);QFile file(path);CTEST_CHECK(file.open(QIODevice::ReadWrite));
        CTEST_CHECK(file.setFileTime(timestamp,QFileDevice::FileModificationTime));file.close();
        CTEST_CHECK(client.bindHost("host",host.replicaId(),host.containerId()));
        CTEST_COMPARE(readFile(path),durable);CTEST_COMPARE(QFileInfo(path).lastModified(),timestamp);
        CTEST_CHECK(QFile::remove(path));CTEST_CHECK(client.bindHost("host",host.replicaId(),host.containerId()));CTEST_COMPARE(readFile(path),durable);
    }
    void inlineCommitRejectsCorruptionAndPreservesThePreviousFile() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/inline-source-XXXXXX.societycontainer"))),b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/inline-target-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path()));CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        writeFile(a.filePath("Files/item"),"verified payload");Replica source,target;CTEST_CHECK(source.open(a.path(),scope));CTEST_CHECK(target.open(b.path(),scope));CTEST_CHECK(source.scan());
        const auto entry=source.record("files/item");
        auto request=QJsonObject{{"action","commit"},{"entry",entry},{"inlineData",QString::fromLatin1(QByteArray("corrupt").toBase64())}};
        CTEST_COMPARE(target.handle("source",request).value("error"),"invalid_inline_payload");CTEST_CHECK(target.record("files/item").isEmpty());
        request.insert("inlineData",QString::fromLatin1(QByteArray("verified payload").toBase64()));
        CTEST_CHECK(target.handle("source",request).value("complete").toBool());
        QFile file(b.filePath("Files/item"));CTEST_CHECK(file.open(QIODevice::ReadOnly));CTEST_COMPARE(file.readAll(),QByteArray("verified payload"));file.close();
        request.insert("inlineData","!");CTEST_CHECK(!target.handle("source",request).value("ok").toBool());
        CTEST_CHECK(file.open(QIODevice::ReadOnly));CTEST_COMPARE(file.readAll(),QByteArray("verified payload"));
    }
    void metadataPagesReadACommittedWalSnapshotWhileTheOperationLockIsHeld() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/wal-read-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));writeFile(root.filePath("Files/value"),"committed");
        Replica replica;CTEST_CHECK(replica.open(root.path(),scope));CTEST_CHECK(replica.scan());
        const auto changes=replica.changes(),journal=replica.journal();CTEST_CHECK(changes.value("ok").toBool());CTEST_CHECK(journal.value("ok").toBool());
        CTEST_CHECK(replica.publishStorageMap());
        QLockFile operation(root.filePath(".society-sync/operation.lock"));operation.setStaleLockTime(0);CTEST_CHECK(operation.tryLock(3000));
        // An unchanged publication is already durable and must not acquire a
        // fresh filesystem lock merely to report that fact.
        CTEST_CHECK(replica.publishStorageMap());
        CTEST_CHECK(replica.scan()); // A verified, unchanged walk does not create a lock file.
        const auto connection="wal-writer-"+QUuid::createUuid().toString();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(root.filePath(".society-sync/journal.sqlite"));CTEST_CHECK(db.open());CTEST_CHECK(db.transaction());
            QSqlQuery edit(db);CTEST_CHECK(edit.exec("UPDATE metadata SET value='999999' WHERE key='sequence'"));
            CTEST_CHECK(edit.exec("UPDATE entries SET data='{}',sequence=999999"));
            CTEST_COMPARE(replica.changes(),changes);CTEST_COMPARE(replica.journal(),journal);
            CTEST_CHECK(QFileInfo::exists(root.filePath(".society-sync/operation.lock")));CTEST_CHECK(db.rollback());db.close();
        }
        QSqlDatabase::removeDatabase(connection);operation.unlock();CTEST_COMPARE(replica.changes(),changes);
    }
    void unchangedPeerCursorsDoNotCreateAnotherDurableRevision() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/peer-noop-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));Replica replica;CTEST_CHECK(replica.open(root.path(),scope));
        const auto peer=QUuid::createUuid().toString(),container=QUuid::createUuid().toString();
        CTEST_CHECK(replica.savePeerState("peer",peer,container,5,8));
        const auto connection="peer-observer-"+QUuid::createUuid().toString();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(root.filePath(".society-sync/journal.sqlite"));CTEST_CHECK(db.open());
            auto revision=[&]{QSqlQuery query(db);if(!query.exec("PRAGMA data_version")||!query.next())return qint64(-1);return query.value(0).toLongLong();};
            const auto before=revision();CTEST_CHECK(before>=0);
            CTEST_CHECK(replica.savePeerState("peer",peer,container,5,8));CTEST_CHECK(replica.savePeerState("peer",peer,container,1,2));
            CTEST_COMPARE(revision(),before);CTEST_COMPARE(replica.peerState("peer").value("pushed").toString(),"8");
            CTEST_CHECK(replica.savePeerState("peer",peer,container,6,8));CTEST_CHECK(revision()!=before);db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void failedNamespaceMigrationRollsBackTheWholeNewJournal() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/schema-rollback-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        CTEST_CHECK(QDir().mkpath(root.filePath(".society-sync")));
        const auto path=root.filePath(".society-sync/journal.sqlite");
        const auto name="schema-probe-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(path);CTEST_CHECK(db.open());
            QSqlQuery query(db);CTEST_CHECK(query.exec("CREATE TABLE namespace_revisions(unexpected INTEGER)"));
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
        Replica replica;CTEST_CHECK(!replica.open(root.path(),scope));replica.close();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(path);CTEST_CHECK(db.open());
            QSqlQuery query(db);CTEST_CHECK(query.exec("PRAGMA user_version"));CTEST_CHECK(query.next());
            CTEST_COMPARE(query.value(0).toInt(),0);query.finish();
            CTEST_CHECK(!db.tables().contains("metadata"));CTEST_CHECK(!db.tables().contains("entries"));
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    void maintenancePreservesLeasedCapabilityProbeAndRetainedHardLink() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/lease-capability-probe-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        CTEST_CHECK(QDir().mkpath(root.filePath(".society-sync")));
        const auto probe = root.filePath(".society-sync/capability-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto retained = root.filePath("Files/retained");
        {
            iiFileProvider::TemporaryLease lease(std::filesystem::path(root.filePath(".society-sync").toStdU16String()),
                iiFileProvider::TemporaryLease::Mode::Shared);
            CTEST_CHECK(lease.locked()); writeFile(probe, "probe bytes");
            std::error_code error;
            std::filesystem::create_hard_link(std::filesystem::path(probe.toStdU16String()),
                std::filesystem::path((probe + ".link").toStdU16String()), error); CTEST_CHECK(!error);
            std::filesystem::create_hard_link(std::filesystem::path(probe.toStdU16String()),
                std::filesystem::path(retained.toStdU16String()), error); CTEST_CHECK(!error);
            const auto active = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(root.path().toStdU16String()), drive->identifier().toStdString());
            CTEST_CHECK(active.errors.empty()); CTEST_CHECK(active.skippedBusy > 0); CTEST_COMPARE(active.tempRemoved, std::uint64_t(0));
            CTEST_COMPARE(readFile(probe), QByteArray("probe bytes")); CTEST_COMPARE(readFile(probe + ".link"), QByteArray("probe bytes"));
        }
        const auto cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(root.path().toStdU16String()), drive->identifier().toStdString());
        CTEST_CHECK(cleanup.errors.empty()); CTEST_COMPARE(cleanup.skippedBusy, std::uint64_t(0));
        CTEST_COMPARE(cleanup.tempRemoved, std::uint64_t(2)); CTEST_CHECK(!QFileInfo::exists(probe)); CTEST_CHECK(!QFileInfo::exists(probe + ".link"));
        CTEST_COMPARE(readFile(retained), QByteArray("probe bytes"));
    }
    void leaseSentinelNeverBecomesAReplicatedObject() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/lease-private-sentinel-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        {
            iiFileProvider::TemporaryLease lease(std::filesystem::path(root.filePath("Files").toStdU16String()),
                iiFileProvider::TemporaryLease::Mode::Shared);
            CTEST_CHECK(lease.locked());
        }
        CTEST_CHECK(QFileInfo::exists(root.filePath("Files/.iisacc-temporary.lock")));
        writeFile(root.filePath("Files/public"), "visible");
        Replica replica; CTEST_CHECK(replica.open(root.path(), scope)); CTEST_CHECK(replica.scan());
        CTEST_CHECK(replica.record("files/.iisacc-temporary.lock").isEmpty());
        const auto entries = replica.changes().value("entries").toArray();
        for (const auto &entry : entries) CTEST_CHECK(!entry.toObject().value("path").toString().contains(".iisacc-temporary.lock"));
        auto forged = replica.record("files/public"); forged.insert("path", "files/.iisacc-temporary.lock");
        QJsonObject payload;
        for (const auto *key : {"path", "kind", "size", "hash", "clock"}) payload.insert(key, forged.value(key));
        forged.insert("version", QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
        CTEST_CHECK(!Replica::validRecord(forged));
        CTEST_CHECK(!replica.handle("sender", {{"action", "begin"}, {"entry", forged}}).value("ok").toBool());
        CTEST_COMPARE(QFileInfo(root.filePath("Files/.iisacc-temporary.lock")).size(), qint64(0));
    }
    void presentationsAreRegeneratedWhileUserClassificationStillReplicates() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/presentation-private-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        writeFile(root.filePath("Files/public.txt"), "visible original");
        writeFile(root.filePath("Files/public.txt.iisacc.json"), "{\"rating\":5,\"keywords\":[\"Work\"]}");
        writeFile(root.filePath("Files/.public.txt.iisacc-preview"), "local identity and preview bytes");
        writeFile(root.filePath("Files/.iifile-stage-fixture.iisacc-preview"), "partial preview");
        CTEST_CHECK(QDir().mkpath(root.filePath("Files/.iisacc-file-provider")));
        writeFile(root.filePath("Files/.iisacc-file-provider/catalog.sqlite"), "private catalog");
        Replica replica; CTEST_CHECK(replica.open(root.path(), scope)); CTEST_CHECK(replica.scan());
        CTEST_CHECK(!replica.record("files/public.txt").isEmpty());
        CTEST_CHECK(!replica.record("files/public.txt.iisacc.json").isEmpty());
        CTEST_CHECK(replica.record("files/.public.txt.iisacc-preview").isEmpty());
        CTEST_CHECK(replica.record("files/.iifile-stage-fixture.iisacc-preview").isEmpty());
        CTEST_CHECK(replica.record("files/.iisacc-file-provider").isEmpty());
        for (const auto &entry : replica.changes().value("entries").toArray()) {
            const auto path = entry.toObject().value("path").toString();
            CTEST_CHECK(!path.contains(".iisacc-preview"));
            CTEST_CHECK(!path.contains(".iisacc-file-provider"));
        }
    }
    void maintenancePreservesActiveTransferAndRemovesCompletedOrClosedOrphans() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/lease-source-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/lease-target-XXXXXX.societycontainer")));
        const auto sourceDrive = iiSocietyContainer::SocietyDrive::create(a.path()); CTEST_CHECK(sourceDrive);
        const auto targetDrive = iiSocietyContainer::SocietyDrive::create(b.path()); CTEST_CHECK(targetDrive);
        Replica source, target; CTEST_CHECK(source.open(a.path(), scope)); CTEST_CHECK(target.open(b.path(), scope));
        const QByteArray bytes("active transfer bytes"); writeFile(a.filePath("Files/item"), bytes); CTEST_CHECK(source.scan());
        const auto entry = source.record("files/item");
        const auto begin = target.handle("sender", {{"action", "begin"}, {"entry", entry}});
        CTEST_CHECK(begin.value("ok").toBool()); CTEST_CHECK(!begin.value("complete").toBool());
        const QDir transfers(b.filePath(".society-sync/transfers"));
        auto parts = transfers.entryList({"*.part"}, QDir::Files); CTEST_COMPARE(parts.size(), 1);
        const auto partial = transfers.filePath(parts.first());
        const auto prefix = bytes.left(7);
        CTEST_CHECK(target.handle("sender", {{"action", "chunk"}, {"entry", entry}, {"offset", "0"},
            {"data", QString::fromLatin1(prefix.toBase64())}}).value("ok").toBool());
        auto cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(b.path().toStdU16String()), targetDrive->identifier().toStdString());
        CTEST_CHECK(cleanup.errors.empty()); CTEST_CHECK(cleanup.skippedBusy > 0); CTEST_COMPARE(cleanup.tempRemoved, std::uint64_t(0));
        CTEST_COMPARE(readFile(partial), prefix); CTEST_CHECK(!QFileInfo::exists(b.filePath("Files/item")));
        CTEST_CHECK(target.handle("sender", {{"action", "chunk"}, {"entry", entry}, {"offset", QString::number(prefix.size())},
            {"data", QString::fromLatin1(bytes.mid(prefix.size()).toBase64())}}).value("ok").toBool());
        CTEST_CHECK(target.handle("sender", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool());
        CTEST_COMPARE(readFile(b.filePath("Files/item")), bytes); CTEST_CHECK(!QFileInfo::exists(partial));
        const auto orphan = transfers.filePath(QString(64, 'f') + ".part"); writeFile(orphan, "orphan");
        cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(b.path().toStdU16String()), targetDrive->identifier().toStdString());
        CTEST_CHECK(cleanup.errors.empty()); CTEST_COMPARE(cleanup.skippedBusy, std::uint64_t(0));
        CTEST_COMPARE(cleanup.tempRemoved, std::uint64_t(1)); CTEST_CHECK(!QFileInfo::exists(orphan));
        // A replica stays open while idle; only unfinished transfers hold a lease.
        CTEST_CHECK(target.isOpen());
        writeFile(a.filePath("Files/abandoned"), bytes); CTEST_CHECK(source.scan());
        const auto abandoned = source.record("files/abandoned");
        CTEST_CHECK(target.handle("sender", {{"action", "begin"}, {"entry", abandoned}}).value("ok").toBool());
        CTEST_CHECK(target.handle("sender", {{"action", "chunk"}, {"entry", abandoned}, {"offset", "0"},
            {"data", QString::fromLatin1(prefix.toBase64())}}).value("ok").toBool());
        parts = transfers.entryList({"*.part"}, QDir::Files); CTEST_COMPARE(parts.size(), 1);
        const auto abandonedPart = transfers.filePath(parts.first());
        target.close();
        cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(b.path().toStdU16String()), targetDrive->identifier().toStdString());
        CTEST_CHECK(cleanup.errors.empty()); CTEST_COMPARE(cleanup.skippedBusy, std::uint64_t(0));
        CTEST_COMPARE(cleanup.tempRemoved, std::uint64_t(1)); CTEST_CHECK(!QFileInfo::exists(abandonedPart));
        CTEST_CHECK(!QFileInfo::exists(b.filePath("Files/abandoned")));
        CTEST_CHECK(target.open(b.path(), scope));
        CTEST_COMPARE(target.handle("sender", {{"action", "begin"}, {"entry", abandoned}}).value("offset").toString(), "0");
    }
    void providerRestoreKeepsTemporaryLeaseWhileJournalLockIsReleased() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/lease-provider-host-XXXXXX.societycontainer"))), backing((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/lease-provider-backup-XXXXXX")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        const auto path = root.filePath("Files/stored"); const QByteArray bytes("provider restored bytes"); writeFile(path, bytes);
        Replica replica; CTEST_CHECK(replica.open(root.path(), scope)); CTEST_CHECK(replica.scan());
        MaintenanceProvider provider(backing.path(), root.path(), drive->identifier());
        CTEST_CHECK(replica.placeObject("files/stored", provider)); CTEST_CHECK(QFile::remove(path));
        CTEST_CHECK_MESSAGE(replica.restoreObject("files/stored", provider), qPrintable(replica.errorString()));
        CTEST_CHECK(provider.journalLockReleased); CTEST_CHECK(provider.protectedDuringGet); CTEST_CHECK(provider.report.errors.empty());
        CTEST_COMPARE(provider.report.tempRemoved, std::uint64_t(0)); CTEST_COMPARE(readFile(path), bytes);
        const auto orphan = root.filePath(".society-sync/transfers/provider-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".part");
        writeFile(orphan, "provider orphan");
        const auto cleanup = iiSocietyContainer::StorageMaintenance::runOnce(std::filesystem::path(root.path().toStdU16String()), drive->identifier().toStdString());
        CTEST_CHECK(cleanup.errors.empty()); CTEST_COMPARE(cleanup.skippedBusy, std::uint64_t(0));
        CTEST_COMPARE(cleanup.tempRemoved, std::uint64_t(1)); CTEST_CHECK(!QFileInfo::exists(orphan));
        CTEST_CHECK(replica.isOpen());
    }
    void formerDefaultNamesAcceptRemoteDeletionReplacementAndReplay() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/fixed-files-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(dir.path()));
        for (const auto *name : {"Documents", "Audios", "3D objects"})
            CTEST_CHECK(QDir().mkpath(dir.filePath("Files/" + QString(name))));
        Replica replica; CTEST_CHECK(replica.open(dir.path(), scope)); CTEST_CHECK(replica.scan());
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
            const auto deleted = revision(path, "deleted"); CTEST_CHECK(Replica::validRecord(deleted));
            for (int retry = 0; retry < 2; ++retry) {
                const auto response = replica.handle("legacy", {{"action", "apply"}, {"entry", deleted}});
                CTEST_CHECK_MESSAGE(response.value("ok").toBool(), qPrintable(QJsonDocument(response).toJson()));
                CTEST_CHECK(response.value("complete").toBool());
                CTEST_CHECK(!QFileInfo::exists(dir.filePath("Files/" + QString(name))));
                CTEST_COMPARE(replica.record(path).value("kind"), "deleted");
            }
        }
        const QByteArray contents("preserved legacy file");
        const auto replacement = revision("files/Documents", "file", contents);
        CTEST_CHECK(Replica::validRecord(replacement));
        auto response = replica.handle("legacy", {{"action", "begin"}, {"entry", replacement}});
        CTEST_CHECK(response.value("ok").toBool()); CTEST_CHECK(!response.value("complete").toBool());
        CTEST_CHECK(replica.handle("legacy", {{"action", "chunk"}, {"entry", replacement}, {"offset", "0"}, {"data", QString::fromLatin1(contents.toBase64())}}).value("ok").toBool());
        CTEST_CHECK(replica.handle("legacy", {{"action", "commit"}, {"entry", replacement}}).value("ok").toBool());
        CTEST_COMPARE(readFile(dir.filePath("Files/Documents")), contents);
        CTEST_COMPARE(replica.record("files/Documents").value("kind"), "file");
        CTEST_CHECK(QDir(dir.filePath("Files")).entryList({"Documents.sync-conflict-*"}, QDir::Files).isEmpty());
        CTEST_CHECK(replica.handle("legacy", {{"action", "begin"}, {"entry", replacement}}).value("complete").toBool());
        CTEST_CHECK(QFile::remove(dir.filePath("Files/Documents")));
        writeFile(dir.filePath("Files/Documents/movie.mp4"), "editable video"); CTEST_CHECK(replica.scan());
        const auto child = revision("files/Documents/movie.mp4", "deleted");
        CTEST_CHECK(replica.handle("legacy", {{"action", "apply"}, {"entry", child}}).value("ok").toBool());
        CTEST_CHECK(!QFileInfo::exists(dir.filePath("Files/Documents/movie.mp4")));
        CTEST_CHECK(QFileInfo(dir.filePath("Files/Documents")).isDir());
    }

    void hostAdoptionArchivesUserDirectoriesAndLeavesFilesEmpty() {
        QTemporaryDir local((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/fixed-adoption-XXXXXX.societycontainer"))), host((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/fixed-host-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(local.path()));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(host.path()));
        writeFile(local.filePath("Files/Documents/old.mp4"), "local video");
        for (int i=0;i<8;++i) writeFile(local.filePath("Files/old-"+QString::number(i)+".txt"),"entry-"+QByteArray::number(i));
        Replica client, remote; CTEST_CHECK(client.open(local.path(), scope)); CTEST_CHECK(remote.open(host.path(), scope));
        CTEST_CHECK(client.bindHost("host", remote.replicaId(), remote.containerId()));
        const auto recovery = client.binding().value("recovery").toString(); CTEST_CHECK(!recovery.isEmpty());
        CTEST_COMPARE(readFile(local.filePath(recovery + "/Files/Documents/old.mp4")), "local video");
        for (int i=0;i<8;++i)
            CTEST_COMPARE(readFile(local.filePath(recovery+"/Files/old-"+QString::number(i)+".txt")),"entry-"+QByteArray::number(i));
        CTEST_CHECK(client.bindHost("host",remote.replicaId(),remote.containerId()));
        CTEST_CHECK(!QFileInfo::exists(local.filePath("Files/Documents/old.mp4")));
        CTEST_CHECK(client.bootstrapping());
        CTEST_CHECK(QDir(local.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        client.close(); CTEST_CHECK(client.open(local.path(), scope));
        CTEST_CHECK(client.completeBootstrap());
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::open(local.path())->isReady());
    }
    void manifestHashAndOrderingAreCheckedWithoutWritingFileBytes() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/manifest-source-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/manifest-host-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica sender, host; CTEST_CHECK(sender.open(a.path(), scope)); CTEST_CHECK(host.open(b.path(), scope)); CTEST_CHECK(sender.scan());
        const auto baseline = sender.changes().value("through").toString().toLongLong();
        writeFile(a.filePath("Files/one"), "first"); writeFile(a.filePath("Files/two"), "second");
        CTEST_CHECK(sender.scan());
        const auto changes = sender.changes(baseline); auto entries = changes.value("entries").toArray(); CTEST_COMPARE(entries.size(), 2);
        for (qsizetype i = 0; i < entries.size(); ++i) {
            auto proposal = entries[i].toObject();
            for (const auto *key : {"namespace", "authority", "revision"}) proposal.remove(key);
            proposal.insert("baseRevision", ""); entries[i] = proposal;
        }
        QByteArray records; for (const auto &entry : entries) records += QJsonDocument(entry.toObject()).toJson(QJsonDocument::Compact) + '\n';
        const auto id = QString::fromLatin1(QCryptographicHash::hash(records, QCryptographicHash::Sha256).toHex());
        QJsonObject page{{"action", "manifest"}, {"container", host.containerId()}, {"replica", sender.replicaId()}, {"manifest", id},
            {"offset", "0"}, {"total", "2"}, {"after", QString::number(baseline)}, {"through", changes.value("through")}, {"entries", QJsonArray{entries[0]}}};
        auto response = host.handle("sender", page); CTEST_CHECK(response.value("ok").toBool()); CTEST_CHECK(!response.value("complete").toBool());
        CTEST_COMPARE(host.handle("sender", {{"action", "begin"}, {"manifest", id}, {"entry", entries[0]}}).value("error"), "manifest_required_before_transfer");
        auto outOfOrder = page; outOfOrder.insert("offset", "2"); outOfOrder.insert("entries", QJsonArray{});
        CTEST_CHECK(!host.handle("sender", outOfOrder).value("ok").toBool());
        page.insert("offset", "1"); page.insert("entries", QJsonArray{entries[1]});
        response = host.handle("sender", page); CTEST_CHECK(response.value("ok").toBool()); CTEST_CHECK(response.value("complete").toBool());
        CTEST_COMPARE(QDir(b.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0); CTEST_CHECK(host.record("files/one").isEmpty());
        CTEST_COMPARE(host.changes().value("through").toString(), "0");
        page.insert("offset", "0"); page.insert("entries", entries); page.insert("manifest", QString(64, 'f'));
        CTEST_COMPARE(host.handle("sender", page).value("error"), "manifest_hash_mismatch");
        CTEST_COMPARE(QDir(b.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
    }
    void sectionInventoryAndLocalBoundary() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/replica-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(dir.path()));
        Replica replica; CTEST_CHECK_MESSAGE(replica.open(dir.path(), scope), qPrintable(replica.errorString()));
        CTEST_CHECK_MESSAGE(replica.scan(), qPrintable(replica.errorString()));
        const auto initial = replica.changes();
        for (const auto &value : initial.value("entries").toArray()) {
            const auto entry = value.toObject(); CTEST_CHECK(Replica::validRecord(entry));
            CTEST_COMPARE(entry.value("kind").toString(), "directory");
            const auto path = entry.value("path").toString();
            CTEST_CHECK(path.startsWith("models/"));
        }
        const auto baseline = initial.value("through").toString().toLongLong();
        for (const auto section : iiSocietyContainer::allStoreSections())
            writeFile(dir.filePath(iiSocietyContainer::storeSectionName(section) + "/one.txt"), "data");
        writeFile(dir.filePath("private-cookie.json"), "must not cross devices");
        CTEST_CHECK_MESSAGE(replica.scan(), qPrintable(replica.errorString()));
        const auto manifest = replica.changes(baseline); CTEST_CHECK(manifest.value("ok").toBool());
        CTEST_COMPARE(manifest.value("entries").toArray().size(), 9);
        for (const auto &value : manifest.value("entries").toArray()) CTEST_CHECK(Replica::validRecord(value.toObject()));
        CTEST_CHECK(replica.record("private-cookie.json").isEmpty());
        const auto id = replica.replicaId(); replica.close();
        CTEST_CHECK(replica.open(dir.path(), scope)); CTEST_COMPARE(replica.replicaId(), id);
        CTEST_CHECK(!replica.open(dir.path(), QString(64, 'b')));
    }
    void chunksResumeAndBadDataCannotReplaceExistingFiles() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/send-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/receive-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        const QByteArray bytes(700000, 'x'); writeFile(a.filePath("Files/large"), bytes);
        Replica source, target; CTEST_CHECK(source.open(a.path(), scope)); CTEST_CHECK(target.open(b.path(), scope)); CTEST_CHECK(source.scan());
        const auto entry = source.record("files/large");
        const QJsonObject begin{{"action", "begin"}, {"entry", entry}};
        auto result = target.handle("sender", begin); CTEST_CHECK(result.value("ok").toBool());
        auto chunk = source.handle("receiver", {{"action", "read"}, {"entry", entry}, {"offset", "0"}});
        CTEST_CHECK(chunk.value("ok").toBool());
        QJsonObject put{{"action", "chunk"}, {"entry", entry}, {"offset", "0"}, {"data", chunk.value("data")}};
        CTEST_CHECK(target.handle("sender", put).value("ok").toBool());
        CTEST_CHECK(target.handle("sender", put).value("ok").toBool()); // Identical retry is idempotent.
        put.insert("data", "!!!!"); CTEST_CHECK(!target.handle("sender", put).value("ok").toBool());
        target.close(); CTEST_CHECK(target.open(b.path(), scope));
        result = target.handle("sender", begin); CTEST_COMPARE(result.value("offset").toString(), QString::number(Replica::ChunkBytes));
        CTEST_CHECK(!QFileInfo::exists(b.filePath("Files/large")));
        for (qint64 offset = Replica::ChunkBytes; offset < bytes.size(); offset += Replica::ChunkBytes) {
            chunk = source.handle("receiver", {{"action", "read"}, {"entry", entry}, {"offset", QString::number(offset)}});
            CTEST_CHECK(chunk.value("ok").toBool());
            CTEST_CHECK(target.handle("sender", {{"action", "chunk"}, {"entry", entry}, {"offset", QString::number(offset)}, {"data", chunk.value("data")}}).value("ok").toBool());
        }
        CTEST_CHECK_MESSAGE(target.handle("sender", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool(), qPrintable(target.errorString()));
        CTEST_COMPARE(readFile(b.filePath("Files/large")), bytes);
        CTEST_CHECK(target.handle("sender", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool());
        auto bad = entry; bad.insert("path", "files/../../outside"); CTEST_CHECK(!Replica::validRecord(bad));
        CTEST_CHECK(!target.handle("sender", {{"action", "begin"}, {"entry", bad}}).value("ok").toBool());
    }
    void deletionIsPersistentAndTheSameClockHasOneValue() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/delete-XXXXXX.societycontainer"))); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(dir.path()));
        writeFile(dir.filePath("Files/item"), "before"); Replica replica; CTEST_CHECK(replica.open(dir.path(), scope)); CTEST_CHECK(replica.scan());
        auto entry = replica.record("files/item");
        auto forged = entry; forged.insert("hash", QString(64, 'e'));
        CTEST_CHECK(!replica.handle("sender", {{"action", "begin"}, {"entry", forged}}).value("ok").toBool());
        CTEST_CHECK(QFile::remove(dir.filePath("Files/item"))); CTEST_CHECK(replica.scan());
        CTEST_COMPARE(replica.record("files/item").value("kind").toString(), "deleted");
        replica.close(); CTEST_CHECK(replica.open(dir.path(), scope));
        CTEST_COMPARE(replica.record("files/item").value("kind").toString(), "deleted");
    }
    void corruptCompleteTransferPreservesDestinationAndCanRetry() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/hash-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/hash-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica source, target; CTEST_CHECK(source.open(a.path(), scope)); CTEST_CHECK(target.open(b.path(), scope));
        CTEST_CHECK(target.scan());
        const auto baseline = target.changes().value("through").toString().toLongLong();
        auto transfer = [&](const QByteArray &data, bool corrupt) {
            writeFile(a.filePath("Files/item"), data); CTEST_CHECK(source.scan());
            const auto entry = source.record("files/item");
            CTEST_CHECK(target.handle("source", {{"action", "begin"}, {"entry", entry}}).value("ok").toBool());
            const auto bytes = corrupt ? QByteArray(data.size(), '?') : data;
            CTEST_CHECK(target.handle("source", {{"action", "chunk"}, {"entry", entry}, {"offset", "0"}, {"data", QString::fromLatin1(bytes.toBase64())}}).value("ok").toBool());
            CTEST_COMPARE(target.handle("source", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool(), !corrupt);
        };
        transfer("original", false);
        transfer("replacement", true);
        CTEST_COMPARE(readFile(b.filePath("Files/item")), QByteArray("original"));
        const auto entry = source.record("files/item");
        CTEST_COMPARE(target.handle("source", {{"action", "begin"}, {"entry", entry}}).value("offset").toString(), "0");
        transfer("replacement", false);
        CTEST_COMPARE(readFile(b.filePath("Files/item")), QByteArray("replacement"));
        const QDir recovery(b.filePath(".society-sync/recovery"));
        const auto metadataFiles = recovery.entryList({"*.json"}, QDir::Files); CTEST_COMPARE(metadataFiles.size(), 1);
        const auto metadata = QJsonDocument::fromJson(readFile(recovery.filePath(metadataFiles[0]))).object();
        CTEST_COMPARE(metadata.value("path").toString(), "Files/item");
        CTEST_COMPARE(readFile(b.filePath(metadata.value("backup").toString())), QByteArray("original"));
        CTEST_CHECK(target.scan()); CTEST_COMPARE(target.changes(baseline).value("entries").toArray().size(), 1);
    }
    void redirectedFilesAndReplacedContainerFailClosed() {
        QTemporaryDir parent((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/boundary-XXXXXX")));
        const auto root = parent.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(root));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root));
        writeFile(root + "/Files/item", "local"); writeFile(parent.filePath("outside"), "outside");
        Replica replica; CTEST_CHECK(replica.open(root, scope)); CTEST_CHECK(replica.scan());
        const auto entry = replica.record("files/item");
        CTEST_CHECK(QFile::remove(root + "/Files/item")); CTEST_CHECK(nativeTestLink(parent.filePath("outside"), root + "/Files/item"));
        CTEST_CHECK(!replica.scan());
        CTEST_CHECK(!replica.handle("peer", {{"action", "read"}, {"entry", entry}, {"offset", "0"}}).value("ok").toBool());
        CTEST_CHECK(!replica.handle("peer", {{"action", "begin"}, {"entry", entry}}).value("ok").toBool());
        CTEST_COMPARE(readFile(parent.filePath("outside")), QByteArray("outside"));
        CTEST_COMPARE(replica.record("files/item").value("version"), entry.value("version"));
#ifdef Q_OS_WIN
        CTEST_CHECK(!QDir(parent.path()).rename("drive.societycontainer", "old"));
        CTEST_CHECK(QFileInfo::exists(root + "/.society-drive.json"));
        replica.close();
        CTEST_CHECK(removeNativeTestLink(root + "/Files/item"));
#else
        CTEST_CHECK(QDir(parent.path()).rename("drive.societycontainer", "old"));
        CTEST_CHECK(QDir().mkdir(root));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root));
        CTEST_CHECK(!replica.scan());
        CTEST_CHECK(!replica.handle("peer", {{"action", "begin"}, {"entry", entry}}).value("ok").toBool());
        CTEST_CHECK(!QFileInfo::exists(root + "/Files/item"));
#endif
    }
    void redirectedJournalCannotWriteLocksOutsideContainer() {
        QTemporaryDir parent((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/journal-XXXXXX")));
        const auto root = parent.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(root)); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root));
        Replica replica; CTEST_CHECK(replica.open(root, scope));
#ifdef Q_OS_WIN
        CTEST_CHECK(!QDir(root).rename(".society-sync", "saved-state"));
        CTEST_CHECK(replica.scan());
        replica.close();
        CTEST_CHECK(QDir(root).rename(".society-sync", "saved-state"));
#else
        CTEST_CHECK(QDir(root).rename(".society-sync", "saved-state"));
        CTEST_CHECK(QDir().mkdir(parent.filePath("outside")));
        CTEST_CHECK(nativeTestLink(parent.filePath("outside"), root + "/.society-sync"));
        CTEST_CHECK(!replica.scan());
        CTEST_COMPARE(QDir(parent.filePath("outside")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
        replica.close();
        CTEST_CHECK(removeNativeTestLink(root + "/.society-sync"));
#endif
    }
    void filenameAliasesCannotOverwriteDistinctNames() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/case-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/case-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        writeFile(a.filePath("Files/item"), "incoming"); writeFile(b.filePath("Files/ITEM"), "original");
        if (!QFileInfo::exists(b.filePath("Files/item"))) helper_ctest::skip("This filesystem distinguishes filename case.");
        Replica source, target; CTEST_CHECK(source.open(a.path(), scope)); CTEST_CHECK(target.open(b.path(), scope)); CTEST_CHECK(source.scan());
        const auto result = target.handle("source", {{"action", "begin"}, {"entry", source.record("files/item")}});
        CTEST_CHECK(!result.value("ok").toBool()); CTEST_COMPARE(result.value("error").toString(), "filename_normalization_collision");
        CTEST_COMPARE(readFile(b.filePath("Files/ITEM")), QByteArray("original"));
        writeFile(a.filePath("Files/sub/new"), "child"); CTEST_CHECK(QDir().mkdir(b.filePath("Files/SUB"))); CTEST_CHECK(source.scan());
        const auto child = target.handle("source", {{"action", "begin"}, {"entry", source.record("files/sub/new")}});
        CTEST_COMPARE(child.value("error").toString(), "filename_normalization_collision");
        CTEST_CHECK(!QFileInfo::exists(b.filePath("Files/SUB/new")));
    }
    void manifestPagesKeepAStableCursorWhileNewChangesWaitForNextCycle() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/pages-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        Replica replica; CTEST_CHECK(replica.open(root.path(), scope)); CTEST_CHECK(replica.scan());
        const auto baseline = replica.changes().value("through").toString().toLongLong();
        for (int i = 0; i < 260; ++i) writeFile(root.filePath("Files/" + QString::number(i)), "data");
        CTEST_CHECK(replica.scan());
        auto page = replica.changes(baseline); CTEST_COMPARE(page.value("entries").toArray().size(), 128);
        const auto upper = page.value("through").toString().toLongLong();
        writeFile(root.filePath("Files/259"), "changed after page one"); CTEST_CHECK(replica.scan());
        QSet<QString> paths;
        while (true) {
            CTEST_CHECK(page.value("ok").toBool());
            CTEST_CHECK(QJsonDocument(page).toJson(QJsonDocument::Compact).size() < 300000);
            for (const auto &entry : page.value("entries").toArray()) {
                const auto path = entry.toObject().value("path").toString(); CTEST_CHECK(!paths.contains(path)); paths.insert(path);
            }
            if (!page.value("more").toBool()) break;
            page = replica.changes(page.value("next").toString().toLongLong(), upper);
        }
        CTEST_COMPARE(page.value("next").toString().toLongLong(), upper);
        page = replica.changes(upper); CTEST_COMPARE(page.value("entries").toArray().size(), 1);
        CTEST_COMPARE(page.value("entries").toArray()[0].toObject().value("path").toString(), "files/259");
        paths.insert("files/259"); CTEST_COMPARE(paths.size(), 260);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    ReplicaTests tests;
    runner.run("unchangedHostBindingPreservesItsDurableFile", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.unchangedHostBindingPreservesItsDurableFile(); });
    runner.run("inlineCommitRejectsCorruptionAndPreservesThePreviousFile", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.inlineCommitRejectsCorruptionAndPreservesThePreviousFile(); });
    runner.run("metadataPagesReadACommittedWalSnapshotWhileTheOperationLockIsHeld", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.metadataPagesReadACommittedWalSnapshotWhileTheOperationLockIsHeld(); });
    runner.run("unchangedPeerCursorsDoNotCreateAnotherDurableRevision", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.unchangedPeerCursorsDoNotCreateAnotherDurableRevision(); });
    runner.run("failedNamespaceMigrationRollsBackTheWholeNewJournal", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.failedNamespaceMigrationRollsBackTheWholeNewJournal(); });
    runner.run("maintenancePreservesLeasedCapabilityProbeAndRetainedHardLink", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.maintenancePreservesLeasedCapabilityProbeAndRetainedHardLink(); });
    runner.run("leaseSentinelNeverBecomesAReplicatedObject", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.leaseSentinelNeverBecomesAReplicatedObject(); });
    runner.run("presentationsAreRegeneratedWhileUserClassificationStillReplicates", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.presentationsAreRegeneratedWhileUserClassificationStillReplicates(); });
    runner.run("maintenancePreservesActiveTransferAndRemovesCompletedOrClosedOrphans", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.maintenancePreservesActiveTransferAndRemovesCompletedOrClosedOrphans(); });
    runner.run("providerRestoreKeepsTemporaryLeaseWhileJournalLockIsReleased", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.providerRestoreKeepsTemporaryLeaseWhileJournalLockIsReleased(); });
    runner.run("formerDefaultNamesAcceptRemoteDeletionReplacementAndReplay", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.formerDefaultNamesAcceptRemoteDeletionReplacementAndReplay(); });
    runner.run("hostAdoptionArchivesUserDirectoriesAndLeavesFilesEmpty", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.hostAdoptionArchivesUserDirectoriesAndLeavesFilesEmpty(); });
    runner.run("manifestHashAndOrderingAreCheckedWithoutWritingFileBytes", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.manifestHashAndOrderingAreCheckedWithoutWritingFileBytes(); });
    runner.run("sectionInventoryAndLocalBoundary", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.sectionInventoryAndLocalBoundary(); });
    runner.run("chunksResumeAndBadDataCannotReplaceExistingFiles", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.chunksResumeAndBadDataCannotReplaceExistingFiles(); });
    runner.run("deletionIsPersistentAndTheSameClockHasOneValue", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.deletionIsPersistentAndTheSameClockHasOneValue(); });
    runner.run("corruptCompleteTransferPreservesDestinationAndCanRetry", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.corruptCompleteTransferPreservesDestinationAndCanRetry(); });
    runner.run("redirectedFilesAndReplacedContainerFailClosed", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.redirectedFilesAndReplacedContainerFailClosed(); });
    runner.run("redirectedJournalCannotWriteLocksOutsideContainer", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.redirectedJournalCannotWriteLocksOutsideContainer(); });
    runner.run("filenameAliasesCannotOverwriteDistinctNames", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.filenameAliasesCannotOverwriteDistinctNames(); });
    runner.run("manifestPagesKeepAStableCursorWhileNewChangesWaitForNextCycle", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.manifestPagesKeepAStableCursorWhileNewChangesWaitForNextCycle(); });
    return runner.result();
}
