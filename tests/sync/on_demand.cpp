#include "../TestWorkspace.h"
#include <iiSocietyHelper/Sync/Synchronizer.h>
#include <SocietyDrive.h>
#include <StorageMap.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include "../SignalRecorder.h"
#include "../QtEventWait.h"
#include <QCoreApplication>
#include <QEvent>
#include <QSet>
#include <QTemporaryDir>
#include <QImage>
#include <QUuid>
using namespace iiSocietyHelper::Sync;
static void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    CTEST_CHECK(f.open(QIODevice::WriteOnly)); CTEST_COMPARE(f.write(data), data.size());
}
class OnDemandTests : public QObject {
    void cycle(Replica &client, Replica &host, QStringList &reads) {
        Synchronizer sync(&client); CTEST_CHECK(sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst));
        helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto request = wire.value("message").toObject();
            if (request.value("action") == "read") reads.append(request.value("entry").toObject().value("path").toString());
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", request)}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE_TIMEOUT(done.size(), 1, 15000);
        CTEST_CHECK_MESSAGE(done[0][1].toBool(), qPrintable(done[0][2].toString()));
    }
public:
    void targetedTransferCannotPublishAfterTheMirroredVersionChanges() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/targeted-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/targeted-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(a.filePath("Files/selected.bin"), QByteArray(100, 'a'));
        Replica host, phone; CTEST_CHECK(host.open(a.path(), QString(64, 'a'))); CTEST_CHECK(phone.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        const auto first = phone.record("files/selected.bin");
        const auto operation = [&](const QString &action, const QJsonObject &entry, QJsonObject extra = {}) {
            extra.insert("action", action); extra.insert("entry", entry); extra.insert("expectedVersion", first.value("version"));
            return phone.handle("host", extra, false);
        };
        CTEST_CHECK(operation("begin", first).value("ok").toBool());
        CTEST_CHECK(operation("chunk", first, {{"offset", "0"}, {"data", QString::fromLatin1(QByteArray(100, 'a').toBase64())}}).value("ok").toBool());
        put(a.filePath("Files/selected.bin"), QByteArray(101, 'b')); CTEST_CHECK(host.scan());
        cycle(phone, host, reads);
        CTEST_CHECK(phone.record("files/selected.bin").value("version") != first.value("version"));
        CTEST_COMPARE(operation("commit", first).value("error").toString(), "object_changed_select_again");
        CTEST_CHECK(!QFileInfo::exists(b.filePath("Files/selected.bin")));
        CTEST_CHECK(!operation("begin", phone.record("files/selected.bin")).value("ok").toBool());
    }
    void validatedHostDoesNotWaitForPendingUploads() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/ready-upload-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/ready-upload-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        put(a.filePath("Files/pending.bin"), QByteArray(800123, 'p'));
        Synchronizer sync(&phone); sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst);
        sync.setExpectedHost("host", host.containerId());
        helper_ctest::SignalRecorder validated(&sync, &Synchronizer::hostValidated); helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished);
        bool uploading = false;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto request = wire.value("message").toObject();
            if (request.value("action") == "chunk") { uploading = true; return; }
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", request)}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_VERIFY(uploading, 3000);
        CTEST_COMPARE(validated.size(), 1); CTEST_CHECK(done.isEmpty()); CTEST_CHECK(sync.busy()); sync.stop();
        validated.clear(); done.clear();
        sync.setExpectedHost("host", QUuid::createUuid().toString(QUuid::WithoutBraces));
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE(done.size(), 1);
        CTEST_CHECK(!done.first()[1].toBool()); CTEST_CHECK(validated.isEmpty());
    }
    void firstConnectionDoesNotWaitForThePhotoLibrary() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/first-gallery-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/first-gallery-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        for (int i = 0; i < 20; ++i) put(b.filePath(QString("Photos/%1.societyphoto").arg(i)), "photo identity");
        put(b.filePath("Models/Checkpoint/model.safetensors"), "remote model");
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        CTEST_CHECK(reads.isEmpty()); CTEST_CHECK(!phone.bootstrapping());
        auto drive = iiSocietyContainer::SocietyDrive::open(a.path()); CTEST_CHECK(drive && drive->isReady());
        iiSocietyContainer::StorageMap map(*drive);
        CTEST_COMPARE(map.object("models/Checkpoint/model.safetensors").value("kind"), "file");
        cycle(phone, host, reads); CTEST_COMPARE(reads.size(), 16);
        CTEST_COMPARE(QSet<QString>(reads.cbegin(), reads.cend()).size(), 16);
        // Remaining identities survive process restart and hydrate in the next bounded round.
        phone.close(); CTEST_CHECK(phone.open(a.path(), QString(64, 'a')));
        reads.clear(); cycle(phone, host, reads); CTEST_COMPARE(reads.size(), 4);
        for (int i = 0; i < 20; ++i) CTEST_CHECK(phone.resident(QString("photos/%1.societyphoto").arg(i)));
        CTEST_CHECK(!phone.resident("models/Checkpoint/model.safetensors"));
    }
    void metadataIsVisibleWhileAnUnrelatedConflictWaitsForBytes() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/metadata-priority-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/metadata-priority-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        put(a.filePath("Files/backlog.bin"), "pending phone edit");
        put(b.filePath("Files/backlog.bin"), "host version");
        put(b.filePath("Models/new.bin"), QByteArray(800123, 'm'));
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        Synchronizer sync(&phone); sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst);
        bool waiting = false;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject();
            if (message.value("action") == "read") { waiting = true; return; }
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", message)}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_VERIFY(waiting, 3000);
        CTEST_COMPARE(map.object("models/new.bin").value("hash"), host.record("models/new.bin").value("hash"));
        CTEST_CHECK(!map.object("models/new.bin").value("resident").toBool());
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Models/new.bin")));
        sync.stop();
        QFile pending(a.filePath("Files/backlog.bin")); CTEST_CHECK(pending.open(QIODevice::ReadOnly));
        CTEST_COMPARE(pending.readAll(), QByteArray("pending phone edit"));
    }
    void selectedDownloadFinishesBeforeUnrelatedConflictReconciliation() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/request-priority-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/request-priority-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        const QByteArray model(800123, 'm'); put(b.filePath("Models/selected.bin"), model);
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        put(a.filePath("Files/backlog.bin"), "pending phone edit");
        put(b.filePath("Files/backlog.bin"), "host version");
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto request = map.request({"models/selected.bin"}); CTEST_CHECK(!request.isEmpty());
        Synchronizer sync(&phone); sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst);
        helper_ctest::SignalRecorder finished(&sync, &Synchronizer::finished); bool waiting = false;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject();
            if (message.value("action") == "read" && message.value("entry").toObject().value("path") == "files/backlog.bin") {
                waiting = true; return;
            }
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", message)}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE_TIMEOUT(map.requestState(request).value("state"), "ready", 3000);
        CTEST_WAIT_VERIFY(waiting, 3000); CTEST_CHECK(finished.isEmpty()); CTEST_CHECK(sync.busy());
        CTEST_CHECK(map.available({"models/selected.bin"}));
        QFile downloaded(a.filePath("Models/selected.bin")); CTEST_CHECK(downloaded.open(QIODevice::ReadOnly));
        CTEST_COMPARE(downloaded.readAll(), model); sync.stop();
    }
    void newUploadIsAcknowledgedBeforeUnrelatedConflictReconciliation() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/upload-priority-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/upload-priority-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        put(a.filePath("Files/backlog.bin"), "pending phone edit");
        put(b.filePath("Files/backlog.bin"), "host version");
        put(a.filePath("Generation History/new.png"), QByteArray(100123, 'g'));
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        Synchronizer sync(&phone); sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst);
        helper_ctest::SignalRecorder finished(&sync, &Synchronizer::finished); bool waiting = false;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject();
            if (message.value("action") == "read" && message.value("entry").toObject().value("path") == "files/backlog.bin") {
                waiting = true; return;
            }
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", message)}});
        });
        CTEST_CHECK(sync.start("host"));
        CTEST_WAIT_VERIFY(map.object("generation-history/new.png").contains("revision"), 3000);
        CTEST_COMPARE(map.object("generation-history/new.png").value("revision"), host.record("generation-history/new.png").value("revision"));
        CTEST_WAIT_VERIFY(waiting, 3000); CTEST_CHECK(finished.isEmpty()); CTEST_CHECK(sync.busy());
        QFile uploaded(b.filePath("Generation History/new.png")); CTEST_CHECK(uploaded.open(QIODevice::ReadOnly));
        CTEST_COMPARE(uploaded.readAll(), QByteArray(100123, 'g')); sync.stop();
    }
    void previewBatchesYieldToSelectedDownloads() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/preview-batch-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/preview-batch-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        for (int index = 0; index < 20; ++index) {
            QImage image(64, 64, QImage::Format_RGB32); image.fill(QColor(index * 10, 0, 0));
            CTEST_CHECK(image.save(b.filePath(QString("Generation History/image-%1.png").arg(index))));
        }
        put(b.filePath("Models/selected.bin"), QByteArray(500000, 'm'));
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads); CTEST_CHECK(reads.isEmpty());
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto countPreviews = [&] {
            int count = 0;
            for (const auto &value : map.objects()) if (!map.previewPath(value.toObject()).isEmpty()) ++count;
            return count;
        };
        CTEST_COMPARE(countPreviews(), 16);
        const auto request = map.request({"models/selected.bin"}); CTEST_CHECK(!request.isEmpty());
        cycle(phone, host, reads);
        CTEST_COMPARE(map.requestState(request).value("state"), "ready");
        CTEST_CHECK(!reads.isEmpty());
        for (const auto &path : reads) CTEST_COMPARE(path, "models/selected.bin");
        CTEST_COMPARE(countPreviews(), 20);
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Generation History/image-0.png")));
    }
    void cancellationDuringTransferLeavesNoPublishedOriginal() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/cancel-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/cancel-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Models/large.bin"), QByteArray(4 * 1024 * 1024, 'm'));
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto request = map.request({"models/large.bin"}); CTEST_CHECK(!request.isEmpty());
        Synchronizer sync(&phone); sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst);
        helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", wire.value("message").toObject())}});
        });
        bool cancellationAttempted = false, cancellationsSucceeded = true;
        connect(&sync, &Synchronizer::progress, &sync, [&](auto, auto, auto) {
            cancellationAttempted = true;
            const bool cancelled = map.cancel(request);
            cancellationsSucceeded = cancellationsSucceeded && cancelled;
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE_TIMEOUT(done.size(), 1, 15000);
        CTEST_CHECK(cancellationAttempted); CTEST_CHECK(cancellationsSucceeded);
        CTEST_CHECK(done.first()[1].toBool()); CTEST_COMPARE(map.requestState(request).value("state"), "cancelled");
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Models/large.bin")));
        const auto resumed = map.request({"models/large.bin"}); cycle(phone, host, reads);
        CTEST_COMPARE(map.requestState(resumed).value("state"), "ready"); CTEST_CHECK(map.available({"models/large.bin"}));
    }
    void previewsAndCancellationDoNotFetchOriginals() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/preview-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/preview-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        QImage original(1024, 768, QImage::Format_RGB32); original.fill(Qt::red);
        CTEST_CHECK(original.save(b.filePath("Generation History/large.png")));
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads); CTEST_CHECK(reads.isEmpty());
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto entry = map.object("generation-history/large.png");
        const QImage preview(map.previewPath(entry)); CTEST_CHECK(!preview.isNull());
        CTEST_CHECK(preview.width() <= 256 && preview.height() <= 256);
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Generation History/large.png")));
        const auto id = map.request({"generation-history/large.png"}); CTEST_CHECK(map.cancel(id));
        cycle(phone, host, reads); CTEST_CHECK(reads.isEmpty());
        CTEST_COMPARE(map.requestState(id).value("state"), "cancelled");
        CTEST_CHECK(!map.available({"generation-history/large.png"}));
    }
    void metadataThenSelectedModelAndGeneratedUpload() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/demand-phone-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/demand-host-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Models/Checkpoint/selected.safetensors"), QByteArray(800123, 'm'));
        put(b.filePath("Models/Checkpoint/unselected.safetensors"), QByteArray(1600555, 'x'));
        put(b.filePath("Files/Documents/readme.txt"), "host document");
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        CTEST_CHECK(reads.isEmpty()); CTEST_CHECK(!phone.bootstrapping());
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Models/Checkpoint/selected.safetensors")));
        CTEST_CHECK(phone.scan()); CTEST_CHECK(phone.changes().value("entries").toArray().isEmpty());
        auto drive = iiSocietyContainer::SocietyDrive::open(a.path()); CTEST_CHECK(drive && drive->isReady());
        iiSocietyContainer::StorageMap map(*drive);
        CTEST_COMPARE(map.object("models/Checkpoint/selected.safetensors").value("kind"), "file");
        const auto request = map.request({"models/Checkpoint/selected.safetensors"}); CTEST_CHECK(!request.isEmpty());
        cycle(phone, host, reads); CTEST_CHECK(!reads.isEmpty());
        for (const auto &path : reads) CTEST_COMPARE(path, "models/Checkpoint/selected.safetensors");
        CTEST_COMPARE(map.requestState(request).value("state"), "ready");
        CTEST_CHECK(map.available({"models/Checkpoint/selected.safetensors"}));
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Models/Checkpoint/unselected.safetensors")));
        // A completed local image is a durable proposal and uploads even though
        // all unrelated host payloads remain remote-only.
        put(a.filePath("Generation History/generated.png"), "completed image fixture");
        reads.clear(); cycle(phone, host, reads);
        CTEST_CHECK(QFileInfo::exists(b.filePath("Generation History/generated.png")));
        CTEST_CHECK(phone.record("generation-history/generated.png").contains("revision"));
        CTEST_CHECK(reads.isEmpty());
        // Closing/reopening does not turn remote objects into deletions.
        phone.close(); CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(phone.scan());
        CTEST_CHECK(phone.changes().value("entries").toArray().isEmpty());
    }
    void staleRequestAndRemoteDeletion() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/demand-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/demand-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Models/model.bin"), "old");
        Replica phone, host; CTEST_CHECK(phone.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto id = map.request({"models/model.bin"}); CTEST_CHECK(!id.isEmpty());
        put(b.filePath("Models/model.bin"), "new version"); cycle(phone, host, reads);
        CTEST_COMPARE(map.requestState(id).value("state"), "failed");
        CTEST_CHECK(!map.available({"models/model.bin"}));
        const auto next = map.request({"models/model.bin"}); cycle(phone, host, reads);
        CTEST_COMPARE(map.requestState(next).value("state"), "ready");
        CTEST_CHECK(QFile::remove(b.filePath("Models/model.bin"))); cycle(phone, host, reads);
        CTEST_COMPARE(map.object("models/model.bin").value("kind"), "deleted");
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Models/model.bin")));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    OnDemandTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("targetedTransferCannotPublishAfterTheMirroredVersionChanges", [&] { FlushDeferredDeletes cleanup; tests.targetedTransferCannotPublishAfterTheMirroredVersionChanges(); });
    runner.run("validatedHostDoesNotWaitForPendingUploads", [&] { FlushDeferredDeletes cleanup; tests.validatedHostDoesNotWaitForPendingUploads(); });
    runner.run("firstConnectionDoesNotWaitForThePhotoLibrary", [&] { FlushDeferredDeletes cleanup; tests.firstConnectionDoesNotWaitForThePhotoLibrary(); });
    runner.run("metadataIsVisibleWhileAnUnrelatedConflictWaitsForBytes", [&] { FlushDeferredDeletes cleanup; tests.metadataIsVisibleWhileAnUnrelatedConflictWaitsForBytes(); });
    runner.run("selectedDownloadFinishesBeforeUnrelatedConflictReconciliation", [&] { FlushDeferredDeletes cleanup; tests.selectedDownloadFinishesBeforeUnrelatedConflictReconciliation(); });
    runner.run("newUploadIsAcknowledgedBeforeUnrelatedConflictReconciliation", [&] { FlushDeferredDeletes cleanup; tests.newUploadIsAcknowledgedBeforeUnrelatedConflictReconciliation(); });
    runner.run("previewBatchesYieldToSelectedDownloads", [&] { FlushDeferredDeletes cleanup; tests.previewBatchesYieldToSelectedDownloads(); });
    runner.run("cancellationDuringTransferLeavesNoPublishedOriginal", [&] { FlushDeferredDeletes cleanup; tests.cancellationDuringTransferLeavesNoPublishedOriginal(); });
    runner.run("previewsAndCancellationDoNotFetchOriginals", [&] { FlushDeferredDeletes cleanup; tests.previewsAndCancellationDoNotFetchOriginals(); });
    runner.run("metadataThenSelectedModelAndGeneratedUpload", [&] { FlushDeferredDeletes cleanup; tests.metadataThenSelectedModelAndGeneratedUpload(); });
    runner.run("staleRequestAndRemoteDeletion", [&] { FlushDeferredDeletes cleanup; tests.staleRequestAndRemoteDeletion(); });
    return runner.result();
}
