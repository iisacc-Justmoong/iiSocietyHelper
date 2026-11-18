#include "../TestWorkspace.h"
#include "iiSocietyHelper/Sync/Sync.h"
#include <SocietyDrive.h>
#include <StorageMap.h>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QSemaphore>
#include "../QtEventWait.h"
#include <QCoreApplication>
#include <QEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QTcpServer>
#include <QScopeGuard>
#include <thread>
#include <iiFileProviderPresentation.h>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

using namespace iiSocietyHelper::Sync;
namespace {
bool tryPut(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
void put(const QString &path, const QByteArray &bytes) {
    CTEST_CHECK(tryPut(path, bytes));
}
const QString scope(64, 'a');
}
class IndexingTests : public QObject {
public:
    void derivedCompanionsDoNotBecomePublicDirectoryChanges() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/preview-index-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        QDir().mkpath(root.filePath("Files/Nested"));
        put(root.filePath("Files/Nested/asset"), "same public bytes");
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(root.path().toStdU16String()));
        Replica indexer; CTEST_CHECK(indexer.open(root.path(), scope));
        bool published = false;
        indexer.setVerificationProgress([&](const QString &path, qint64 done, qint64) {
            if(path == "Files/Nested/asset" && !done && !published) {
                published = true;
                (void)iiFileProvider::native::Presentation::prepare(
                    std::filesystem::path(root.filePath(path).toStdU16String()));
            }
        });
        CTEST_CHECK_MESSAGE(indexer.scan(), qPrintable(indexer.errorString()));
        CTEST_CHECK(published);
        CTEST_COMPARE(indexer.record("files/Nested/asset").value("kind"), "file");
    }
    void watchingALargeLibraryLeavesRoomForFilesAndSockets() {
#ifndef Q_OS_UNIX
        helper_ctest::skip("POSIX descriptor limit regression");
#else
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/watch-budget-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        for (int i = 0; i < 300; ++i) put(root.filePath(QString("Files/item-%1").arg(i)), "small");
        struct rlimit original{}; CTEST_CHECK(getrlimit(RLIMIT_NOFILE, &original) == 0);
        auto limited = original; limited.rlim_cur = qMin<rlim_t>(256, original.rlim_cur);
        CTEST_CHECK(setrlimit(RLIMIT_NOFILE, &limited) == 0);
        const auto restore = qScopeGuard([&] { setrlimit(RLIMIT_NOFILE, &original); });
        Controller controller({}); controller.open(root.path(), scope); CTEST_WAIT_DEFAULT(controller.available());
        QFile input(root.filePath("Files/item-0"));
        CTEST_CHECK_MESSAGE(input.open(QIODevice::ReadOnly), qPrintable(input.errorString()));
        CTEST_COMPARE(input.readAll(), QByteArray("small"));
        QTcpServer socket; CTEST_CHECK_MESSAGE(socket.listen(QHostAddress::LocalHost), qPrintable(socket.errorString()));
        put(root.filePath("Files/item-299"), "updated outside a small watch set");
        controller.synchronizeNow();
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(root.path()));
        const auto hash = QString::fromLatin1(QCryptographicHash::hash("updated outside a small watch set", QCryptographicHash::Sha256).toHex());
        CTEST_WAIT_COMPARE(map.object("files/item-299").value("hash").toString(), hash);
        controller.closeAndWait();
#endif
    }
    void openingWaitsForABriefConcurrentIndexCommit() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/index-open-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        Replica replica; CTEST_CHECK(replica.open(root.path(), scope)); replica.close();
        QSemaphore entered; bool locked = false;
        std::jthread writer([&] {
            QLockFile lock(root.filePath(".society-sync/operation.lock")); lock.setStaleLockTime(0);
            locked = lock.tryLock(); entered.release(); QThread::msleep(100);
        });
        entered.acquire();
        const bool opened = replica.open(root.path(), scope);
        writer.join(); CTEST_CHECK(locked); CTEST_CHECK_MESSAGE(opened, qPrintable(replica.errorString()));
    }
    void smallObjectsAreDurableAndReadableWhileLargeHashIsCancelled() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/index-cancel-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        put(root.filePath("Files/previous"), "previous");
        Replica indexer, reader; CTEST_CHECK(indexer.open(root.path(), scope)); CTEST_CHECK(indexer.scan());
        CTEST_CHECK(reader.open(root.path(), scope));
        CTEST_CHECK(QFile::remove(root.filePath("Files/previous")));
        put(root.filePath("Files/small"), "small metadata first");
        put(root.filePath("Photos/thumbnail.jpg"), "thumbnail");
        put(root.filePath("Models/large"), QByteArray(4 * 1024 * 1024, 'm'));
        bool cancelled = false, readableDuringHash = false, smallVisible = false, previewVisible = false;
        indexer.setCancellation([&] { return cancelled; });
        indexer.setVerificationProgress([&](const QString &path, qint64 done, qint64) {
            if (path != "Models/large") return;
            if (!done) {
                readableDuringHash = reader.changes().value("ok").toBool();
                smallVisible = reader.record("files/small").value("kind") == "file";
                previewVisible = reader.record("photos/thumbnail.jpg").value("kind") == "file";
            } else cancelled = true;
        });
        CTEST_CHECK(!indexer.scan()); CTEST_COMPARE(indexer.errorString(), QString("cancelled"));
        CTEST_CHECK_MESSAGE(readableDuringHash, "Hashing held the replication operation lock");
        CTEST_CHECK_MESSAGE(smallVisible && previewVisible, "Small objects waited behind the large model");
        CTEST_CHECK(reader.record("models/large").isEmpty());
        CTEST_COMPARE(reader.record("files/previous").value("kind"), "file"); // No deletion from an unfinished pass.
        const auto revision = reader.record("files/small").value("revision");
        indexer.close(); cancelled = false; CTEST_CHECK(indexer.open(root.path(), scope));
        QStringList rehashed;
        indexer.setVerificationProgress([&](const QString &path, qint64 done, qint64) { if (!done) rehashed.append(path); });
        CTEST_CHECK_MESSAGE(indexer.scan(), qPrintable(indexer.errorString()));
        CTEST_COMPARE(rehashed, QStringList{"Models/large"});
        CTEST_COMPARE(reader.record("files/small").value("revision"), revision);
        CTEST_COMPARE(reader.record("files/previous").value("kind"), "deleted");
    }
    void recreationDuringAHashDoesNotBecomeAFalseDeletion() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/index-recreate-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        put(root.filePath("Files/recreated"), "old");
        Replica indexer; CTEST_CHECK(indexer.open(root.path(), scope)); CTEST_CHECK(indexer.scan());
        CTEST_CHECK(QFile::remove(root.filePath("Files/recreated")));
        put(root.filePath("Models/large"), QByteArray(2 * 1024 * 1024, 'm'));
        bool recreated = false;
        indexer.setVerificationProgress([&](const QString &path, qint64 done, qint64) {
            if (path == "Models/large" && !done && !recreated) {
                recreated = tryPut(root.filePath("Files/recreated"), "revived");
            }
        });
        CTEST_CHECK(indexer.scan()); CTEST_CHECK(recreated);
        CTEST_COMPARE(indexer.record("files/recreated").value("kind"), "file");
        CTEST_CHECK(indexer.scan());
        CTEST_COMPARE(indexer.record("files/recreated").value("hash").toString(),
                 QString::fromLatin1(QCryptographicHash::hash("revived", QCryptographicHash::Sha256).toHex()));
    }
    void controllerServesCommittedMetadataBeforeLargeHashCompletes() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/index-responsive-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        put(root.filePath("Files/small"), "available during model indexing");
        QFile model(root.filePath("Models/large")); CTEST_CHECK(model.open(QIODevice::WriteOnly));
        CTEST_CHECK(model.resize(2LL * 1024 * 1024 * 1024)); model.close();
        Controller host({}); bool modelStarted = false, modelFinished = false;
        connect(&host, &Controller::changed, this, [&] {
            if (!host.errorString().isEmpty()) qInfo() << "Host indexing status:" << host.errorString();
        });
        connect(&host, &Controller::verificationProgress, this, [&](const QString &path, qint64 done, qint64 total) {
            if (path != "Models/large") return;
            modelStarted = true; if (done == total) modelFinished = true;
        });
        host.open(root.path(), scope); CTEST_WAIT_DEFAULT(host.available()); host.setPeers({"phone"}, {});
        host.synchronizeNow(); CTEST_WAIT_MESSAGE_TIMEOUT(modelStarted, qPrintable(host.errorString()), 5000);
        const QJsonObject request{{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1}, {"scope", scope},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"message", QJsonObject{{"action", "changes"}, {"container", drive->identifier()}}}};
        QElapsedTimer elapsed; elapsed.start(); QJsonObject response;
        do {
            response = host.handle("phone", request).value("result").toObject();
            if (!response.isEmpty()) break;
            helper_ctest::processFor(1);
        } while (elapsed.elapsed() < 3000);
        CTEST_CHECK_MESSAGE(response.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson())));
        CTEST_CHECK_MESSAGE(!modelFinished, "Metadata response waited for all model bytes to be hashed");
        qInfo() << "Metadata response milliseconds during a 2 GiB model hash:" << elapsed.elapsed();
        bool found = false;
        for (const auto &item : response.value("entries").toArray())
            if (item.toObject().value("path") == "files/small") found = true;
        CTEST_CHECK(found);
        host.closeAndWait();
        CTEST_CHECK(!QFileInfo::exists(root.filePath(".society-sync/operation.lock")));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    IndexingTests tests;
    runner.run("derivedCompanionsDoNotBecomePublicDirectoryChanges", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.derivedCompanionsDoNotBecomePublicDirectoryChanges(); });
    runner.run("watchingALargeLibraryLeavesRoomForFilesAndSockets", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.watchingALargeLibraryLeavesRoomForFilesAndSockets(); });
    runner.run("openingWaitsForABriefConcurrentIndexCommit", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.openingWaitsForABriefConcurrentIndexCommit(); });
    runner.run("smallObjectsAreDurableAndReadableWhileLargeHashIsCancelled", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.smallObjectsAreDurableAndReadableWhileLargeHashIsCancelled(); });
    runner.run("recreationDuringAHashDoesNotBecomeAFalseDeletion", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.recreationDuringAHashDoesNotBecomeAFalseDeletion(); });
    runner.run("controllerServesCommittedMetadataBeforeLargeHashCompletes", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.controllerServesCommittedMetadataBeforeLargeHashCompletes(); });
    return runner.result();
}
