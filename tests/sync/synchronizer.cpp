#include "../TestWorkspace.h"
#include <iiSocietyHelper/Sync/Synchronizer.h>
#include <SocietyDrive.h>
#include <QDir>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QFile>
#include <QJsonArray>
#include "../SignalRecorder.h"
#include "../QtEventWait.h"
#include <QCoreApplication>
#include <QEvent>
#include <exception>
#include <QTemporaryDir>
#include <QTimer>
#include <QElapsedTimer>

using namespace iiSocietyHelper::Sync;
static void put(const QString &path, QByteArray data) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); CTEST_CHECK(f.open(QIODevice::WriteOnly)); CTEST_COMPARE(f.write(data), data.size()); }
static QByteArray get(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
class SyncTests : public QObject {
    void cycle(Replica &client, Replica &host, QList<QJsonObject> *requests = nullptr) {
        Synchronizer sync(&client); helper_ctest::SignalRecorder completed(&sync, &Synchronizer::finished);
        bool scopeMatches = true;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](const QString &id, const QString &, const QJsonObject &wire) {
            if (requests) requests->append(wire.value("message").toObject());
            scopeMatches &= wire.value("scope").toString() == QString(64, 'a');
            const auto reply = host.handle("client", wire.value("message").toObject());
            sync.receive(id, {{"ok", true}, {"result", reply}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE_TIMEOUT(completed.size(), 1, 20000);
        CTEST_CHECK(scopeMatches);
        CTEST_CHECK_MESSAGE(completed[0][1].toBool(), qPrintable(completed[0][2].toString()));
    }
public:
    void accountHostReplacementPreservesOldEditsAndRejectsWrongContainer() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/account-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/account-b-XXXXXX.societycontainer"))), c((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/account-c-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(c.path()));
        Replica client, oldHost, newHost;
        CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(oldHost.open(b.path(), QString(64, 'a'))); CTEST_CHECK(newHost.open(c.path(), QString(64, 'a')));
        cycle(client, oldHost); put(a.filePath("Files/offline"), "old-host-only"); put(c.filePath("Files/new"), "new-host-data");
        Synchronizer sync(&client); helper_ctest::SignalRecorder finished(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            sync.receive(id, {{"ok", true}, {"result", newHost.handle("phone", wire.value("message").toObject())}});
        });
        sync.setExpectedHost("new-mac", oldHost.containerId()); CTEST_CHECK(sync.start("new-mac"));
        CTEST_WAIT_COMPARE(finished.size(), 1); CTEST_COMPARE(finished[0][2].toString(), QString("account_host_mismatch"));
        CTEST_COMPARE(client.containerId(), oldHost.containerId()); CTEST_COMPARE(get(a.filePath("Files/offline")), QByteArray("old-host-only"));
        finished.clear(); sync.setExpectedHost("new-mac", newHost.containerId()); CTEST_CHECK(sync.start("new-mac"));
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 20000); CTEST_CHECK_MESSAGE(finished[0][1].toBool(), qPrintable(finished[0][2].toString()));
        CTEST_COMPARE(client.binding().value("host").toString(), QString("new-mac")); CTEST_COMPARE(client.containerId(), newHost.containerId());
        CTEST_COMPARE(get(a.filePath("Files/new")), QByteArray("new-host-data")); CTEST_CHECK(!QFileInfo::exists(c.filePath("Files/offline")));
        CTEST_COMPARE(get(a.filePath(client.binding().value("recovery").toString() + "/Files/offline")), QByteArray("old-host-only"));
    }
    void boundedWindowHandlesReorderedReplies(int window, bool upload, bool legacy) {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/window-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/window-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        cycle(client, host);
        QByteArray bytes(5 * 1024 * 1024 + 123, Qt::Uninitialized);
        for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = char((i * 31 + i / 256) % 251);
        put((upload ? a : b).filePath("Models/window.bin"), bytes);
        Synchronizer sync(&client); CTEST_CHECK(sync.setTransferWindow(window)); CTEST_CHECK(!sync.setTransferWindow(0)); CTEST_CHECK(!sync.setTransferWindow(5));
        helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished); int outstanding = 0, peak = 0; qint64 previous = 0;
        bool progressOrderValid = true, progressTotalValid = true;
        connect(&sync, &Synchronizer::progress, &sync, [&](const auto &path, qint64 completed, qint64 total) {
            if (!path.endsWith("window.bin")) return;
            progressOrderValid &= completed > previous; previous = completed;
            progressTotalValid &= total == bytes.size();
        });
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject(); auto result = host.handle("client", message);
            if (legacy) result.remove("transferWindow");
            const bool chunk = message.value("action") == (upload ? "chunk" : "read");
            const auto position = message.value("offset").toString().toLongLong() / Replica::ChunkBytes;
            if (chunk) { ++outstanding; peak = qMax(peak, outstanding); }
            QTimer::singleShot(chunk ? 40 + (3 - position % 4) * 8 : 0, &sync, [&, id, result, chunk] {
                if (chunk) --outstanding;
                const QJsonObject response{{"ok", true}, {"result", result}};
                sync.receive(id, response); sync.receive(id, response); // A duplicate must not advance the cursor twice.
            });
        });
        QElapsedTimer elapsed; elapsed.start(); CTEST_CHECK(sync.start("host")); CTEST_CHECK(!sync.setTransferWindow(1));
        CTEST_WAIT_COMPARE_TIMEOUT(done.size(), 1, 15000);
        CTEST_CHECK_MESSAGE(done[0][1].toBool(), qPrintable(done[0][2].toString()));
        CTEST_CHECK(progressOrderValid); CTEST_CHECK(progressTotalValid);
        CTEST_COMPARE(peak, legacy ? 1 : window); CTEST_COMPARE(outstanding, 0); CTEST_COMPARE(previous, bytes.size());
        CTEST_COMPARE(get((upload ? b : a).filePath("Models/window.bin")), bytes);
        qInfo("transfer window=%d legacy=%d upload=%d bytes=%lld elapsed_ms=%lld", window, legacy, upload, qlonglong(bytes.size()), elapsed.elapsed());
    }
    void smallChangesAndDeletionDoNotWaitForBulkTransfer(bool upload) {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/fair-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/fair-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Files/z-note"), "old"); put(b.filePath("Files/z-delete"), "remove");
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        cycle(client, host);
        const QByteArray large(Replica::ChunkBytes * 5, 'L');
        put((upload ? a : b).filePath("Files/a-large"), large);
        Synchronizer sync(&client); helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished);
        bool changed = false, prioritized = false; QList<qint64> offsets;
        std::exception_ptr callbackFailure;
        connect(&sync, &Synchronizer::progress, &sync, [&](const QString &path, qint64 offset, qint64) {
            if (!path.endsWith("a-large") || offset <= Replica::ChunkBytes) return;
            const bool smallReady = get(a.filePath("Files/z-note")) == "new" && !QFileInfo::exists(a.filePath("Files/z-delete"));
            const bool uploadReady = !upload || get(b.filePath("Files/z-upload")) == "phone edit";
            prioritized |= smallReady && uploadReady && !QFileInfo::exists((upload ? b : a).filePath("Files/a-large"));
        });
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject();
            const bool largeBytes = message.value("action") == (upload ? "chunk" : "read")
                && message.value("entry").toObject().value("path").toString().endsWith("a-large");
            if (largeBytes) offsets.append(message.value("offset").toString().toLongLong());
            const auto response = host.handle("client", message);
            if (largeBytes && !changed) {
                changed = true;
                try {
                    put(b.filePath("Files/z-note"), "new"); CTEST_CHECK(QFile::remove(b.filePath("Files/z-delete")));
                    if (upload) put(a.filePath("Files/z-upload"), "phone edit");
                } catch (...) { callbackFailure = std::current_exception(); sync.stop(); return; }
                // A slow payload must not hold later edits until the bulk file completes.
                QTimer::singleShot(1100, &sync, [&, id, response] { sync.receive(id, {{"ok", true}, {"result", response}}); });
            } else sync.receive(id, {{"ok", true}, {"result", response}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_VERIFY(done.size() == 1 || bool(callbackFailure), 15000);
        if (callbackFailure) std::rethrow_exception(callbackFailure);
        CTEST_COMPARE(done.size(), 1);
        CTEST_CHECK_MESSAGE(done[0][1].toBool(), qPrintable(done[0][2].toString()));
        CTEST_CHECK_MESSAGE(prioritized, "A small edit/deletion waited behind the entire bulk payload.");
        CTEST_COMPARE(get((upload ? b : a).filePath("Files/a-large")), large);
        CTEST_COMPARE(offsets.size(), 5);
        for (int i = 0; i < offsets.size(); ++i) CTEST_COMPARE(offsets[i], qint64(i) * Replica::ChunkBytes);
    }
    void exchangesEveryManifestPageBeforeAnyFileBytes() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/metadata-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/metadata-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        cycle(client, host);
        for (int i = 0; i < 140; ++i) put(a.filePath("Files/local-" + QString::number(i)), "upload");
        put(b.filePath("Models/remote"), "download");
        Synchronizer sync(&client); helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished);
        bool manifestComplete = false, sawBytes = false; int pages = 0, announced = 0;
        bool metadataOnly = true, validRecords = true, validHashes = true, bytesAfterManifest = true, enoughAnnounced = true;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject(); const auto action = message.value("action").toString();
            if (action == "manifest") {
                ++pages; announced += message.value("entries").toArray().size();
                metadataOnly &= !message.contains("data");
                for (const auto &entry : message.value("entries").toArray()) {
                    validRecords &= Replica::validRecord(entry.toObject());
                    validHashes &= entry.toObject().value("hash").toString().size() == 64;
                }
            }
            if (action == "read" || action == "chunk") {
                bytesAfterManifest &= manifestComplete;
                enoughAnnounced &= announced >= 140; sawBytes = true;
            }
            const auto response = host.handle("client", message);
            if (action == "manifest" && response.value("complete").toBool()) manifestComplete = true;
            sync.receive(id, {{"ok", true}, {"result", response}});
        });
        CTEST_CHECK(sync.start("host"));
#ifdef Q_OS_WIN
        constexpr int manifestTimeout = 60000; // Native metadata probes on NTFS.
#else
        constexpr int manifestTimeout = 20000;
#endif
        CTEST_WAIT_COMPARE_TIMEOUT(done.size(), 1, manifestTimeout);
        CTEST_CHECK_MESSAGE(done[0][1].toBool(), qPrintable(done[0][2].toString()));
        CTEST_CHECK(metadataOnly); CTEST_CHECK(validRecords); CTEST_CHECK(validHashes);
        CTEST_CHECK_MESSAGE(bytesAfterManifest, "File bytes were sent before all outgoing identity pages were acknowledged.");
        CTEST_CHECK(enoughAnnounced); CTEST_CHECK(pages >= 2); CTEST_CHECK(sawBytes);
        CTEST_COMPARE(get(a.filePath("Models/remote")), QByteArray("download"));
        CTEST_COMPARE(get(b.filePath("Files/local-139")), QByteArray("upload"));
    }
    void rejectedManifestAcknowledgementPreventsByteTransfer() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/rejected-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/rejected-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        cycle(client, host); put(a.filePath("Files/local"), "upload"); put(b.filePath("Models/remote"), "download");
        Synchronizer sync(&client); helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished); int bytes = 0;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject();
            if (message.value("action") == "read" || message.value("action") == "chunk") ++bytes;
            auto response = host.handle("client", message);
            if (message.value("action") == "manifest") response.insert("manifest", QString(64, 'f'));
            sync.receive(id, {{"ok", true}, {"result", response}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE(done.size(), 1);
        CTEST_CHECK(!done[0][1].toBool()); CTEST_COMPARE(bytes, 0);
    }
    void firstJoinAdoptsOneLogicalDriveAndPreservesFormerContentsPrivately() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/mirror-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/mirror-b-XXXXXX.societycontainer"))), c((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/mirror-c-XXXXXX.societycontainer")));
        const auto old = iiSocietyContainer::SocietyDrive::create(a.path()); CTEST_CHECK(old);
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(c.path()));
        for (const auto section : iiSocietyContainer::allStoreSections()) {
            const auto name = iiSocietyContainer::storeSectionName(section);
            put(a.filePath(name + "/legacy/old"), "private old content");
            put(b.filePath(name + "/from-host/nested/item"), name.toUtf8());
        }
        Replica client, host, second;
        CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a'))); CTEST_CHECK(second.open(c.path(), QString(64, 'a')));
        cycle(client, host); cycle(second, host);
        CTEST_COMPARE(client.containerId(), host.containerId()); CTEST_COMPARE(second.containerId(), host.containerId());
        CTEST_CHECK(client.replicaId() != host.replicaId()); CTEST_CHECK(second.replicaId() != client.replicaId());
        CTEST_CHECK(!old->isValid()); CTEST_CHECK(!client.bootstrapping());
        const auto binding = Replica::binding(a.path()); CTEST_CHECK(binding.value("complete").toBool());
        CTEST_COMPARE(binding.value("container").toString(), host.containerId());
        for (const auto section : iiSocietyContainer::allStoreSections()) {
            const auto name = iiSocietyContainer::storeSectionName(section);
            for (const auto &path : {a.path(), c.path()}) {
                CTEST_COMPARE(get(QDir(path).filePath(name + "/from-host/nested/item")), name.toUtf8());
                CTEST_CHECK(!QFileInfo::exists(QDir(path).filePath(name + "/legacy")));
            }
            CTEST_CHECK(!QFileInfo::exists(b.filePath(name + "/legacy")));
            CTEST_COMPARE(get(a.filePath(binding.value("recovery").toString() + '/' + name + "/legacy/old")), QByteArray("private old content"));
        }
        const auto deviceReplica = client.replicaId(); client.close(); CTEST_CHECK(client.open(a.path(), QString(64, 'a')));
        CTEST_COMPARE(client.containerId(), host.containerId()); CTEST_COMPARE(client.replicaId(), deviceReplica);
        put(a.filePath("Files/offline/new"), "new on phone"); cycle(client, host); cycle(second, host);
        CTEST_COMPARE(get(c.filePath("Files/offline/new")), QByteArray("new on phone"));
        CTEST_CHECK(QFile::remove(c.filePath("Files/offline/new"))); cycle(second, host); cycle(client, host);
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Files/offline/new"))); CTEST_CHECK(!QFileInfo::exists(b.filePath("Files/offline/new")));
    }
    void interruptedInitialDownloadResumesWithoutUploadingLegacyFiles() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/bootstrap-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/bootstrap-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(a.filePath("Files/old"), "preserve me"); put(b.filePath("Models/large"), QByteArray(900000, 'h'));
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        {
            Synchronizer sync(&client); bool interrupted = false, noChunks = true;
            connect(&sync, &Synchronizer::progress, &sync, [&](auto, auto, auto) { interrupted = true; sync.stop(); });
            connect(&sync, &Synchronizer::requestReady, &sync, [&](const auto &id, auto, const auto &wire) {
                const auto message = wire.value("message").toObject();
                noChunks &= message.value("action") != "chunk";
                sync.receive(id, {{"ok", true}, {"result", host.handle("client", message)}});
            });
            CTEST_CHECK(sync.start("host")); CTEST_WAIT_DEFAULT(interrupted); CTEST_CHECK(noChunks);
        }
        CTEST_CHECK(client.bootstrapping()); CTEST_COMPARE(client.containerId(), host.containerId());
        const auto recovery = client.binding().value("recovery").toString();
        client.close(); CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(client.bootstrapping());
        QList<QJsonObject> requests; cycle(client, host, &requests);
        for (const auto &r : requests) if (r.value("action") == "read") CTEST_CHECK(r.value("offset").toString().toLongLong() >= Replica::ChunkBytes);
        CTEST_COMPARE(get(a.filePath("Models/large")), QByteArray(900000, 'h'));
        CTEST_CHECK(!QFileInfo::exists(b.filePath("Files/old")));
        CTEST_COMPARE(get(a.filePath(recovery + "/Files/old")), QByteArray("preserve me"));
        CTEST_CHECK(!client.bootstrapping());
    }
    void recoveryResumesAfterIdentityCommitAndOnlySomeEntriesMoved() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/adoption-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/adoption-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(a.filePath("Files/one"), "one"); put(a.filePath("Models/two"), "two");
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        client.setCancellation([&] { return !QFileInfo::exists(a.filePath("Files/one")); });
        CTEST_CHECK(!client.bindHost("host", host.replicaId(), host.containerId()));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto recovery = Replica::binding(a.path()).value("recovery").toString(); CTEST_CHECK(!recovery.isEmpty());
        client.close(); client.setCancellation({}); CTEST_CHECK_MESSAGE(client.open(a.path(), QString(64, 'a')), qPrintable(client.errorString()));
        cycle(client, host); CTEST_COMPARE(client.containerId(), host.containerId());
        CTEST_COMPARE(get(a.filePath(recovery + "/Files/one")), QByteArray("one"));
        CTEST_COMPARE(get(a.filePath(recovery + "/Models/two")), QByteArray("two"));
        CTEST_COMPARE(QDir(a.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
        const QDir clientModels(a.filePath("Models")), hostModels(b.filePath("Models"));
        const auto modelFolders = hostModels.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
        CTEST_COMPARE(clientModels.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name), modelFolders);
        for (const auto &folder : modelFolders) {
            CTEST_CHECK(QFileInfo(clientModels.filePath(folder)).isDir());
            CTEST_CHECK(QDir(clientModels.filePath(folder)).isEmpty());
        }
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Models/two")));
    }
    void bootstrapCatchesHostChangesBeforePublishingTheMirror() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/catchup-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/catchup-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Files/changing"), "before");
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        Synchronizer sync(&client); helper_ctest::SignalRecorder completed(&sync, &Synchronizer::finished); bool changed = false;
        std::exception_ptr callbackFailure;
        connect(&sync, &Synchronizer::progress, &sync, [&](auto, auto, auto) {
            if (!changed) {
                changed = true;
                try { put(b.filePath("Files/changing"), "after"); put(b.filePath("Models/late"), "late"); }
                catch (...) { callbackFailure = std::current_exception(); sync.stop(); }
            }
        });
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            sync.receive(id, {{"ok", true}, {"result", host.handle("client", wire.value("message").toObject())}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_DEFAULT(completed.size() == 1 || bool(callbackFailure));
        if (callbackFailure) std::rethrow_exception(callbackFailure);
        CTEST_COMPARE(completed.size(), 1); CTEST_CHECK(completed[0][1].toBool());
        CTEST_COMPARE(get(a.filePath("Files/changing")), QByteArray("after")); CTEST_COMPARE(get(a.filePath("Models/late")), QByteArray("late"));
        CTEST_CHECK(!client.bootstrapping());
    }
    void aPrimaryClaimIsPersistentAndCannotPromoteAMirror() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/primary-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/primary-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        const QString scope(64, 'a');
        CTEST_CHECK(Replica::claimPrimaryHost(b.path(), scope, "host")); CTEST_COMPARE(Replica::primaryHost(b.path(), scope), QString("host"));
        CTEST_CHECK(!Replica::claimPrimaryHost(b.path(), scope, "intruder")); CTEST_CHECK(Replica::primaryHost(b.path(), QString(64, 'b')).isEmpty());
        Replica client, host; CTEST_CHECK(client.open(a.path(), scope)); CTEST_CHECK(host.open(b.path(), scope)); cycle(client, host);
        CTEST_CHECK(!Replica::claimPrimaryHost(a.path(), scope, "client"));
    }
    void emptyHostAndHostDriveReplacementNeverMergeIndependentContents() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/replace-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/replace-b-XXXXXX.societycontainer"))), c((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/replace-c-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(c.path()));
        put(a.filePath("Files/independent"), "legacy");
        Replica client, host, replacement; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        cycle(client, host); CTEST_COMPARE(QDir(a.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
        put(a.filePath("Files/pending"), "offline edit for old drive"); put(c.filePath("Files/new-host"), "new drive");
        CTEST_CHECK(replacement.open(c.path(), QString(64, 'a'))); cycle(client, replacement);
        CTEST_COMPARE(client.containerId(), replacement.containerId());
        CTEST_COMPARE(get(a.filePath("Files/new-host")), QByteArray("new drive"));
        CTEST_CHECK(!QFileInfo::exists(c.filePath("Files/pending")));
        CTEST_COMPARE(get(a.filePath(client.binding().value("recovery").toString() + "/Files/pending")), QByteArray("offline edit for old drive"));
        CTEST_CHECK(!client.bindHost("different-host", host.replicaId(), host.containerId()));
        CTEST_COMPARE(client.errorString(), QString("different_primary_host"));
    }
    void smallUploadsNegotiateInlineCommitAndRetainTheLegacyFallback() {
        for(const bool capable:{false,true}) {
            QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/inline-a-XXXXXX.societycontainer"))),b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/inline-b-XXXXXX.societycontainer")));
            CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path()));CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
            Replica client,host;CTEST_CHECK(client.open(a.path(),QString(64,'a')));CTEST_CHECK(host.open(b.path(),QString(64,'a')));cycle(client,host);
            put(a.filePath("Files/small"),"small verified payload");
            Synchronizer sync(&client);helper_ctest::SignalRecorder done(&sync,&Synchronizer::finished);int inlined=0,chunks=0;
            connect(&sync,&Synchronizer::requestReady,&sync,[&](auto id,auto,auto wire){
                const auto request=wire.value("message").toObject();
                if(request.contains("inlineData"))++inlined;if(request.value("action")=="chunk")++chunks;
                auto response=host.handle("client",request);if(!capable)response.remove("inlineBytes");
                sync.receive(id,{{"ok",true},{"result",response}});
            });
            CTEST_CHECK(sync.start("host"));CTEST_WAIT_COMPARE_TIMEOUT(done.size(),1,20000);CTEST_CHECK(done[0][1].toBool());
            CTEST_COMPARE(get(b.filePath("Files/small")),QByteArray("small verified payload"));
            CTEST_COMPARE(inlined,capable?1:0);CTEST_COMPARE(chunks,capable?0:1);
        }
    }
    void anIdleProviderLeaseSentinelDoesNotPreventDirectoryDeletion() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/lease-delete-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/lease-delete-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Files/sub/item"), "original");
        Replica client, host;CTEST_CHECK(client.open(a.path(),QString(64,'a')));CTEST_CHECK(host.open(b.path(),QString(64,'a')));
        cycle(client,host);cycle(client,host);
        // FileProvider keeps this stable, empty lease inode after its work ends.
        put(b.filePath("Files/sub/.iisacc-temporary.lock"),{});
        CTEST_CHECK(QDir(a.filePath("Files/sub")).removeRecursively());cycle(client,host);cycle(client,host);
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Files/sub")));CTEST_CHECK(!QFileInfo::exists(b.filePath("Files/sub")));
    }
    void bothDirectionsDirectoriesDeletionAndIncrementalRequests() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/sync-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/sync-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Files/sub/client"), QByteArray(700000, 'c')); put(b.filePath("Thinking Space/host"), "host");
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        cycle(client, host); cycle(client, host);
        CTEST_COMPARE(get(b.filePath("Files/sub/client")), QByteArray(700000, 'c')); CTEST_COMPARE(get(a.filePath("Thinking Space/host")), QByteArray("host"));
        QList<QJsonObject> requests; cycle(client, host, &requests);
        for (const auto &r : requests) CTEST_CHECK(r.value("action") != "read" && r.value("action") != "chunk");
        put(b.filePath("Files/sub/client"), "edited on host"); cycle(client, host); CTEST_COMPARE(get(a.filePath("Files/sub/client")), QByteArray("edited on host"));
        CTEST_CHECK(QDir(a.filePath("Files/sub")).removeRecursively()); cycle(client, host); cycle(client, host);
        CTEST_CHECK(!QFileInfo::exists(b.filePath("Files/sub"))); CTEST_CHECK(!QFileInfo::exists(a.filePath("Files/sub")));
        // A newly joined device has never created the deleted parent. Old
        // tombstones must still be acknowledged so its first cycle can finish.
        QTemporaryDir c((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/late-join-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(c.path()));
        Replica late; CTEST_CHECK(late.open(c.path(), QString(64, 'a'))); cycle(late, host);
        CTEST_CHECK(!QFileInfo::exists(c.filePath("Files/sub")));
        CTEST_COMPARE(get(c.filePath("Thinking Space/host")), QByteArray("host"));
    }
    void concurrentEditsAndDeleteAgainstEditKeepBothResults() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/conflict-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/conflict-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Files/note"), "initial");
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        cycle(client, host); cycle(client, host);
        put(a.filePath("Files/note"), "client edit"); put(b.filePath("Files/note"), "host edit");
        cycle(client, host); cycle(client, host); cycle(client, host);
        CTEST_COMPARE(get(a.filePath("Files/note")), get(b.filePath("Files/note")));
        const auto copies = QDir(a.filePath("Files")).entryList({"*.sync-conflict-*"}, QDir::Files);
        CTEST_COMPARE(copies.size(), 1); CTEST_COMPARE(QDir(b.filePath("Files")).entryList({"*.sync-conflict-*"}, QDir::Files), copies);
        const QSet<QByteArray> values{get(a.filePath("Files/note")), get(a.filePath("Files/" + copies[0]))};
        CTEST_COMPARE(values, (QSet<QByteArray>{"client edit", "host edit"}));
        CTEST_CHECK(QFile::remove(a.filePath("Files/note"))); put(b.filePath("Files/note"), "concurrent rescue");
        cycle(client, host); cycle(client, host);
        CTEST_COMPARE(get(a.filePath("Files/note")), QByteArray("concurrent rescue")); CTEST_COMPARE(get(b.filePath("Files/note")), QByteArray("concurrent rescue"));
    }
    void lostAcknowledgementAndRestartDoNotResendCompletedBytes() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/restart-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/restart-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        cycle(client, host);
        put(a.filePath("Models/large.bin"), QByteArray(900000, 'm'));
        {
            Synchronizer sync(&client); bool interrupted = false;
            connect(&sync, &Synchronizer::requestReady, &sync, [&](const QString &id, const QString &, const QJsonObject &wire) {
                const auto message = wire.value("message").toObject(); const auto reply = host.handle("client", message);
                if (message.value("action") == "chunk") { interrupted = true; sync.stop(); return; }
                sync.receive(id, {{"ok", true}, {"result", reply}});
            });
            CTEST_CHECK(sync.start("host")); CTEST_WAIT_DEFAULT(interrupted);
        }
        client.close(); host.close(); CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        QList<QJsonObject> requests; cycle(client, host, &requests);
        for (const auto &r : requests) if (r.value("action") == "chunk") CTEST_CHECK(r.value("offset").toString().toLongLong() >= Replica::ChunkBytes);
        CTEST_COMPARE(get(b.filePath("Models/large.bin")), QByteArray(900000, 'm'));
    }
    void directoryAndLongFilenameConflictsPreserveData() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/types-a-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/types-b-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        const QString name(200, 'x');
        Replica client, host; CTEST_CHECK(client.open(a.path(), QString(64, 'a'))); CTEST_CHECK(host.open(b.path(), QString(64, 'a')));
        cycle(client, host);
        put(a.filePath("Files/" + name), "file variant");
        put(b.filePath("Files/" + name + "/child"), "directory child");
        cycle(client, host); cycle(client, host); cycle(client, host);
        CTEST_COMPARE(get(a.filePath("Files/" + name + "/child")), QByteArray("directory child"));
        CTEST_COMPARE(get(b.filePath("Files/" + name + "/child")), QByteArray("directory child"));
        const auto copies = QDir(a.filePath("Files")).entryList({"*.sync-conflict-*"}, QDir::Files);
        CTEST_COMPARE(copies.size(), 1); CTEST_CHECK(copies[0].toUtf8().size() <= 220);
        CTEST_COMPARE(get(a.filePath("Files/" + copies[0])), QByteArray("file variant"));
        CTEST_COMPARE(get(b.filePath("Files/" + copies[0])), QByteArray("file variant"));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    SyncTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("accountHostReplacementPreservesOldEditsAndRejectsWrongContainer", [&] { FlushDeferredDeletes cleanup; tests.accountHostReplacementPreservesOldEditsAndRejectsWrongContainer(); });
    runner.run("boundedWindowHandlesReorderedReplies:serial-download", [&] { FlushDeferredDeletes cleanup; tests.boundedWindowHandlesReorderedReplies(1, false, false); });
    runner.run("boundedWindowHandlesReorderedReplies:window-download", [&] { FlushDeferredDeletes cleanup; tests.boundedWindowHandlesReorderedReplies(4, false, false); });
    runner.run("boundedWindowHandlesReorderedReplies:window-upload", [&] { FlushDeferredDeletes cleanup; tests.boundedWindowHandlesReorderedReplies(4, true, false); });
    runner.run("boundedWindowHandlesReorderedReplies:legacy-download", [&] { FlushDeferredDeletes cleanup; tests.boundedWindowHandlesReorderedReplies(4, false, true); });
    runner.run("smallChangesAndDeletionDoNotWaitForBulkTransfer:download", [&] { FlushDeferredDeletes cleanup; tests.smallChangesAndDeletionDoNotWaitForBulkTransfer(false); });
    runner.run("smallChangesAndDeletionDoNotWaitForBulkTransfer:upload", [&] { FlushDeferredDeletes cleanup; tests.smallChangesAndDeletionDoNotWaitForBulkTransfer(true); });
    runner.run("exchangesEveryManifestPageBeforeAnyFileBytes", [&] { FlushDeferredDeletes cleanup; tests.exchangesEveryManifestPageBeforeAnyFileBytes(); });
    runner.run("rejectedManifestAcknowledgementPreventsByteTransfer", [&] { FlushDeferredDeletes cleanup; tests.rejectedManifestAcknowledgementPreventsByteTransfer(); });
    runner.run("firstJoinAdoptsOneLogicalDriveAndPreservesFormerContentsPrivately", [&] { FlushDeferredDeletes cleanup; tests.firstJoinAdoptsOneLogicalDriveAndPreservesFormerContentsPrivately(); });
    runner.run("interruptedInitialDownloadResumesWithoutUploadingLegacyFiles", [&] { FlushDeferredDeletes cleanup; tests.interruptedInitialDownloadResumesWithoutUploadingLegacyFiles(); });
    runner.run("recoveryResumesAfterIdentityCommitAndOnlySomeEntriesMoved", [&] { FlushDeferredDeletes cleanup; tests.recoveryResumesAfterIdentityCommitAndOnlySomeEntriesMoved(); });
    runner.run("bootstrapCatchesHostChangesBeforePublishingTheMirror", [&] { FlushDeferredDeletes cleanup; tests.bootstrapCatchesHostChangesBeforePublishingTheMirror(); });
    runner.run("aPrimaryClaimIsPersistentAndCannotPromoteAMirror", [&] { FlushDeferredDeletes cleanup; tests.aPrimaryClaimIsPersistentAndCannotPromoteAMirror(); });
    runner.run("emptyHostAndHostDriveReplacementNeverMergeIndependentContents", [&] { FlushDeferredDeletes cleanup; tests.emptyHostAndHostDriveReplacementNeverMergeIndependentContents(); });
    runner.run("smallUploadsNegotiateInlineCommitAndRetainTheLegacyFallback", [&] { FlushDeferredDeletes cleanup; tests.smallUploadsNegotiateInlineCommitAndRetainTheLegacyFallback(); });
    runner.run("anIdleProviderLeaseSentinelDoesNotPreventDirectoryDeletion", [&] { FlushDeferredDeletes cleanup; tests.anIdleProviderLeaseSentinelDoesNotPreventDirectoryDeletion(); });
    runner.run("bothDirectionsDirectoriesDeletionAndIncrementalRequests", [&] { FlushDeferredDeletes cleanup; tests.bothDirectionsDirectoriesDeletionAndIncrementalRequests(); });
    runner.run("concurrentEditsAndDeleteAgainstEditKeepBothResults", [&] { FlushDeferredDeletes cleanup; tests.concurrentEditsAndDeleteAgainstEditKeepBothResults(); });
    runner.run("lostAcknowledgementAndRestartDoNotResendCompletedBytes", [&] { FlushDeferredDeletes cleanup; tests.lostAcknowledgementAndRestartDoNotResendCompletedBytes(); });
    runner.run("directoryAndLongFilenameConflictsPreserveData", [&] { FlushDeferredDeletes cleanup; tests.directoryAndLongFilenameConflictsPreserveData(); });
    return runner.result();
}
