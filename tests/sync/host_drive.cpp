#include "../TestWorkspace.h"
#include <iiSocietyHelper/Sync/Controller.h>
#include <iiSocietyHelper/Sync/Replica.h>
#include <iiSocietyHelper/Sync/RemoteFiles.h>
#include "TestLink.h"
#include <SocietyDrive.h>
#include <StorageMaintenance.h>
#include <iiFileProviderPresentation.h>
#include <FileCatalog.h>
#include <QBuffer>
#include <QDateTime>
#include <QImage>
#include <QJsonDocument>
#include <QFile>
#include <QJsonArray>
#include <QLockFile>
#include <QTemporaryDir>
#include "../CTestSupport.h"
#include "../QtEventWait.h"
#include <QCoreApplication>
#include <QEvent>
#include <QUuid>

using namespace iiSocietyHelper::Sync;
class HostDriveTests {
    const QString scope = QString(64, 'a');
    static void put(const QString &path, const QByteArray &bytes) {
        QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(bytes), bytes.size());
    }
    static QByteArray get(const QString &path) {
        QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
public:
    void pinnedReadsDoNotContendWithTheMutationLock() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/read-lock-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        put(root.filePath("Files/payload.bin"), "original bytes");
        Replica host; CTEST_CHECK(host.open(root.path(), scope));
        const auto call = [&](QJsonObject request) {
            request.insert("container", drive->identifier()); return host.handle("phone", request, false);
        };
        const auto stat = call({{"action", "drive.stat"}, {"path", "files/payload.bin"}});
        CTEST_CHECK_MESSAGE(stat.value("ok").toBool(), qPrintable(stat.value("error").toString()));
        const auto version = stat.value("version").toString(); CTEST_CHECK(!version.isEmpty());
        QLockFile mutation(root.filePath(".society-sync/operation.lock")); mutation.setStaleLockTime(0);
        CTEST_CHECK(mutation.tryLock());
        const auto read = call({{"action", "drive.read"}, {"path", "files/payload.bin"}, {"offset", "0"}, {"version", version}});
        CTEST_CHECK_MESSAGE(read.value("ok").toBool(), qPrintable(read.value("error").toString()));
        CTEST_COMPARE(QByteArray::fromBase64(read.value("data").toString().toLatin1()), QByteArray("original bytes"));
        const auto unpinned = call({{"action", "drive.read"}, {"path", "files/payload.bin"}, {"offset", "0"}});
        CTEST_COMPARE(unpinned.value("error").toString(), QString("sync_store_busy"));
        const auto write = call({{"action", "drive.write"}, {"path", "files/other.bin"}, {"data", "eA=="}});
        CTEST_COMPARE(write.value("error").toString(), QString("sync_store_busy"));
        put(root.filePath("Files/payload.bin"), "changed bytes!");
        const auto stale = call({{"action", "drive.read"}, {"path", "files/payload.bin"}, {"offset", "0"}, {"version", version}});
        CTEST_CHECK(!stale.value("ok").toBool()); CTEST_CHECK(!stale.contains("data"));
    }
    void richInformationIsBoundedVersionedAndIndependentOfPayloadReads() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/rich-host-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        Replica host; CTEST_CHECK(host.open(root.path(), scope));
        const auto call = [&](QJsonObject request) {
            request.insert("container", drive->identifier()); return host.handle("phone", request, false);
        };
        iiFileProvider::FileCatalog catalog(std::filesystem::path(root.filePath("Files").toStdU16String()));
        iiFileProvider::FileMetadata annotation; annotation.rating = 4; annotation.keywords = {"host", "campaign"};
        const std::string payload = "An original that never has to be downloaded for inspection";
        CTEST_CHECK(catalog.createFile("remote.txt", {reinterpret_cast<const std::byte *>(payload.data()), payload.size()}, annotation).metadataWritten);
        // An incompressible but valid bounded domain thumbnail exercises more
        // than one transport chunk without a large original or catalog scan.
        QImage image(512, 512, QImage::Format_RGB32); quint32 random = 1;
        for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            image.setPixel(x, y, 0xff000000u | (random & 0x00ffffff));
        }
        QByteArray png; QBuffer buffer(&png); CTEST_CHECK(buffer.open(QIODevice::WriteOnly)); CTEST_CHECK(image.save(&buffer, "PNG"));
        iiFileProvider::native::FilePreview preview{"image/png", "image", png.toStdString(), 512, 512};
        (void)iiFileProvider::native::Presentation::attach(std::filesystem::path(root.filePath("Files/remote.txt").toStdU16String()), preview);
        const auto first = call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}});
        CTEST_CHECK_MESSAGE(first.value("ok").toBool(), qPrintable(first.value("error").toString()));
        CTEST_CHECK(!first.value("eof").toBool());
        auto document = QByteArray::fromBase64(first.value("data").toString().toLatin1());
        CTEST_COMPARE(document.size(), Replica::ChunkBytes);
        const auto version = first.value("version").toString(), token = first.value("informationVersion").toString();
        while (document.size() < first.value("size").toString().toLongLong()) {
            const auto next = call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}, {"offset", QString::number(document.size())},
                {"version", version}, {"informationVersion", token}});
            CTEST_CHECK_MESSAGE(next.value("ok").toBool(), qPrintable(next.value("error").toString()));
            const auto chunk = QByteArray::fromBase64(next.value("data").toString().toLatin1());
            CTEST_CHECK(!chunk.isEmpty()); CTEST_CHECK(chunk.size() <= Replica::ChunkBytes); document += chunk;
        }
        const auto information = QJsonDocument::fromJson(document).object();
        CTEST_COMPARE(information.value("format").toString(), "iiFileProvider.FileInformation");
        CTEST_COMPARE(information.value("classification").toObject().value("metadata").toObject().value("rating").toInt(), 4);
        CTEST_COMPARE(QByteArray::fromBase64(information.value("preview").toObject().value("data").toString().toLatin1()), png);
        CTEST_CHECK(!information.contains("rootPath")); CTEST_COMPARE(get(root.filePath("Files/remote.txt")), QByteArray::fromStdString(payload));
        CTEST_COMPARE(call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}, {"offset", "1"}}).value("error").toString(), "inspection_version_required");
        const auto current = catalog.lookupPath("remote.txt"); CTEST_CHECK(current);
        // Refresh registration after a domain attachment changes native ctime.
        const auto refreshed = catalog.registerFile("remote.txt"); annotation.rating = 5;
        CTEST_CHECK(catalog.updateMetadata(refreshed.file.key, refreshed.file.revision, annotation).metadataWritten);
        CTEST_COMPARE(call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}, {"offset", "1"}, {"version", version},
            {"informationVersion", token}}).value("error").toString(), "drive_information_changed");
        put(root.filePath("Files/remote.txt"), "changed source");
        CTEST_COMPARE(call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}, {"version", version}}).value("error").toString(), "drive_file_changed");
        CTEST_CHECK(!call({{"action", "drive.inspect"}, {"path", "files/remote.txt.iisacc.json"}}).value("ok").toBool());
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(root.path().toStdU16String()));
    }
    void legacyShareHidesDerivedDocumentsWhileServingOriginalBytes() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/direct-presentation-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        put(root.filePath("Files/report.txt"), "public source bytes");
        put(root.filePath("Files/.report.txt.iisacc-preview"), "private derived fixture");
        put(root.filePath("Files/report.txt.iisacc.json"), "{\"rating\":5}");
        const auto handle = filesHandler(root.path());
        const auto listed = handle("peer", {{"op", "list"}, {"path", ""}});
        CTEST_CHECK(listed.value("ok").toBool());
        const auto entries = listed.value("entries").toArray(); CTEST_COMPARE(entries.size(), 1);
        CTEST_COMPARE(entries.first().toObject().value("name").toString(), "report.txt");
        for (const auto &name : {".report.txt.iisacc-preview", "report.txt.iisacc.json"})
            CTEST_CHECK(!handle("peer", {{"op", "stat"}, {"path", name}}).value("ok").toBool());
        const auto read = handle("peer", {{"op", "read"}, {"path", "report.txt"}, {"offset", "0"}});
        CTEST_CHECK(read.value("ok").toBool());
        CTEST_COMPARE(QByteArray::fromBase64(read.value("data").toString().toLatin1()), QByteArray("public source bytes"));
    }
    void abandonedDirectWriteUsesTheExistingMaintenanceContract() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/direct-orphan-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        put(root.filePath("Files/.society-write-abandoned"), "incomplete staging");
        put(root.filePath("Files/published.bin"), "complete publication");
        const auto report = iiSocietyContainer::StorageMaintenance::runOnce(
            std::filesystem::path(root.path().toStdU16String()), drive->identifier().toStdString());
        // SocietyDrive also schedules maintenance; its worker may win the
        // reclamation race before this explicit scan. The filesystem outcome
        // and preservation of published content are the shared contract.
        CTEST_CHECK(report.errors.empty());
        CTEST_WAIT_VERIFY(!QFileInfo::exists(root.filePath("Files/.society-write-abandoned")), 3000);
        CTEST_COMPARE(get(root.filePath("Files/published.bin")), QByteArray("complete publication"));
    }
    void liveAccessDoesNotRequireTheIndexOrLocalPayload() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/direct-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        Replica host; CTEST_CHECK(host.open(root.path(), scope));
        const auto call = [&](const QString &action, QJsonObject data = {}) {
            data.insert("action", "drive." + action); data.insert("container", drive->identifier());
            return host.handle("phone", data, false);
        };
        const auto descriptor = call("describe"); CTEST_CHECK_MESSAGE(descriptor.value("ok").toBool(), qPrintable(descriptor.value("error").toString()));
        CTEST_COMPARE(descriptor.value("driveProtocol").toInt(), 1);
        CTEST_COMPARE(descriptor.value("sections").toArray().size(), 9);
        CTEST_CHECK(!descriptor.contains("rootPath"));
        const QByteArray bytes(Replica::ChunkBytes * 2 + 37, 'x');
        put(root.filePath("Models/live.bin"), bytes); // Deliberately not scanned.
        const auto listed = call("list", {{"path", "models"}});
        CTEST_CHECK(listed.value("ok").toBool());
        CTEST_COMPARE(listed.value("entries").toArray().first().toObject().value("path").toString(), "models/live.bin");
        const auto stat = call("stat", {{"path", "models/live.bin"}});
        CTEST_CHECK(stat.value("ok").toBool()); CTEST_COMPARE(stat.value("size").toString().toLongLong(), bytes.size());
        const auto native = std::filesystem::path(root.filePath("Models/live.bin").toStdU16String());
        CTEST_CHECK(iiFileProvider::native::Presentation::load(native));
        CTEST_CHECK(iiFileProvider::native::Presentation::prepare(native).cacheHit);
        put(root.filePath("Models/live.bin.iisacc.json"), "{\"rating\":5}");
        const auto publicRows = call("list", {{"path", "models"}}).value("entries").toArray();
        CTEST_COMPARE(publicRows.size(), 1);
        CTEST_COMPARE(publicRows.first().toObject().value("path").toString(), "models/live.bin");
        for (const auto &control : {"models/.live.bin.iisacc-preview", "models/live.bin.iisacc.json", "models/.iisacc-temporary.lock"})
            CTEST_COMPARE(call("stat", {{"path", control}}).value("error").toString(), "invalid_drive_path");
        const auto version = stat.value("version").toString(); CTEST_CHECK(!version.isEmpty());
        QByteArray received;
        while (received.size() < bytes.size()) {
            const auto read = call("read", {{"path", "models/live.bin"}, {"offset", QString::number(received.size())}, {"version", version}});
            CTEST_CHECK(read.value("ok").toBool());
            const auto chunk = QByteArray::fromBase64(read.value("data").toString().toLatin1());
            CTEST_CHECK(!chunk.isEmpty()); CTEST_CHECK(chunk.size() <= Replica::ChunkBytes); received += chunk;
            CTEST_COMPARE(read.value("eof").toBool(), received.size() == bytes.size());
        }
        CTEST_COMPARE(received, bytes);
        put(root.filePath("Models/live.bin"), "changed");
        CTEST_CHECK(!call("read", {{"path", "models/live.bin"}, {"offset", "0"}, {"version", version}}).value("ok").toBool());
        const auto directory = call("mkdir", {{"path", "files/new"}}); CTEST_CHECK(directory.value("ok").toBool());
        CTEST_CHECK(!call("mkdir", {{"path", "files/new"}}).value("ok").toBool());
        const QJsonObject write{{"path", "files/new/empty.dat"}, {"data", ""}};
        CTEST_CHECK(call("write", write).value("ok").toBool()); CTEST_CHECK(QFileInfo::exists(root.filePath("Files/new/empty.dat")));
        const auto encoded = QString::fromLatin1(QByteArray("direct host bytes").toBase64());
        CTEST_CHECK(call("write", {{"path", "generation-history/result.png"}, {"data", encoded}}).value("ok").toBool());
        CTEST_CHECK(!call("write", {{"path", "generation-history/result.png"}, {"data", "b3ZlcndyaXRl"}}).value("ok").toBool());
        CTEST_COMPARE(get(root.filePath("Generation History/result.png")), QByteArray("direct host bytes"));
        CTEST_CHECK(host.scan());
        const auto catalog = call("catalog"); CTEST_CHECK(catalog.value("ok").toBool());
        CTEST_CHECK(catalog.value("entries").toArray().size() >= 3);
        CTEST_CHECK(!catalog.value("revision").toString().isEmpty());
    }
    void pagingIsBoundedAndRevisionChecked() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/pages-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        const auto privateUse = QString(QChar(0xe000)), emoji = QString::fromUtf8("\xf0\x9f\x98\x80");
        for (int i = 0; i < 135; ++i)
            put(root.filePath("Files/" + (i < 127 ? privateUse : emoji) + QString("item-%1").arg(i, 3, 10, QChar('0'))), "a");
        Replica host; CTEST_CHECK(host.open(root.path(), scope)); CTEST_CHECK(host.scan());
        const auto call = [&](QJsonObject data) { data.insert("container", drive->identifier()); return host.handle("phone", data, false); };
        auto page = call({{"action", "drive.catalog"}}); CTEST_CHECK(page.value("ok").toBool());
        CTEST_COMPARE(page.value("entries").toArray().size(), 128);
        const auto epochNow = QDateTime::currentMSecsSinceEpoch();
        bool sawModified = false;
        for (const auto &value : page.value("entries").toArray()) {
            const auto entry = value.toObject();
            // Catalog metadata is omitted when the live file no longer
            // matches the scan. Check timestamps on the still-current entries.
            if (entry.contains("modified")) {
                bool validTime = false;
                const auto modified = entry.value("modified").toString().toLongLong(&validTime);
                CTEST_CHECK(validTime);
                CTEST_CHECK(modified > epochNow - 60000 && modified <= epochNow + 1000);
                sawModified = true;
            }
        }
        CTEST_CHECK(sawModified);
        // The wire catalog uses UTF-8 binary order, which differs from UTF-16
        // string order across supplementary characters and private-use names.
        CTEST_CHECK(page.value("entries").toArray().first().toObject().value("path").toString().startsWith("files/" + privateUse));
        CTEST_CHECK(page.value("entries").toArray().last().toObject().value("path").toString().startsWith("files/" + emoji));
        const auto cursor = page.value("nextCursor").toString(), revision = page.value("revision").toString(); CTEST_CHECK(!cursor.isEmpty());
        page = call({{"action", "drive.catalog"}, {"cursor", cursor}, {"revision", revision}});
        CTEST_CHECK(page.value("ok").toBool()); CTEST_COMPARE(page.value("entries").toArray().size(), 7); CTEST_CHECK(page.value("nextCursor").toString().isEmpty());
        put(root.filePath("Files/added"), "b"); CTEST_CHECK(host.scan());
        CTEST_COMPARE(call({{"action", "drive.catalog"}, {"cursor", cursor}, {"revision", revision}}).value("error").toString(), "drive_snapshot_changed");
        // Pin a stable directory after preview companions finish changing its
        // native metadata. Intake behavior is covered independently above.
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(root.path().toStdU16String()));
        page = call({{"action", "drive.list"}, {"path", "files"}});
        CTEST_CHECK_MESSAGE(page.value("ok").toBool(), qPrintable(page.value("error").toString()));
        CTEST_COMPARE(page.value("entries").toArray().size(), 128);
        CTEST_CHECK(!call({{"action", "drive.list"}, {"path", "files"}, {"cursor", page.value("nextCursor")}}).value("ok").toBool());
    }
    void rejectsEscapesPrivateStateAndOtherContainers() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/boundary-XXXXXX.societycontainer"))), outside((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/outside-XXXXXX")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        put(outside.filePath("secret"), "outside");
        CTEST_CHECK(nativeTestLink(outside.path(), root.filePath("Files/redirect")));
        Replica host; CTEST_CHECK(host.open(root.path(), scope));
        for (const auto &path : QStringList{"../secret", "/etc/passwd", "files/../secret", "files//secret", "files\\secret", ".society-sync/journal.sqlite", "files/.society-sync/a", "files/.iisacc-temporary.lock", "files/redirect/secret"}) {
            const auto result = host.handle("phone", {{"action", "drive.read"}, {"container", drive->identifier()}, {"path", path}, {"offset", "0"}}, false);
            CTEST_CHECK_MESSAGE(!result.value("ok").toBool(), qPrintable(path));
            CTEST_CHECK(!result.contains("data"));
        }
        CTEST_CHECK(!host.handle("phone", {{"action", "drive.mkdir"}, {"container", drive->identifier()}, {"path", "files"}}, false).value("ok").toBool());
        CTEST_CHECK(!host.handle("phone", {{"action", "drive.describe"}, {"container", QUuid::createUuid().toString(QUuid::WithoutBraces)}}, false).value("ok").toBool());
        CTEST_CHECK(!host.handle("phone", {{"action", "drive.describe"}}, false).value("ok").toBool());
        CTEST_COMPARE(get(outside.filePath("secret")), QByteArray("outside"));
    }
    void controllerRequiresPeerScopeAndExactRequestToken() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/authority-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        Controller host({}); host.open(root.path(), scope); CTEST_WAIT_DEFAULT(host.available()); host.setPeers({"phone"}, {});
        QJsonObject request{{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1}, {"scope", scope},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"message", QJsonObject{{"action", "drive.describe"}, {"container", drive->identifier()}}}};
        CTEST_CHECK(!host.handle("stranger", request).value("ok").toBool());
        auto other = request; other.insert("scope", QString(64, 'b')); CTEST_CHECK(!host.handle("phone", other).value("ok").toBool());
        QJsonObject response;
        CTEST_WAIT_DEFAULT((response = host.handle("phone", request)).contains("result"));
        CTEST_CHECK(response.value("result").toObject().value("ok").toBool());
        auto changed = request; changed.insert("message", QJsonObject{{"action", "drive.list"}, {"container", drive->identifier()}});
        CTEST_COMPARE(host.handle("phone", changed).value("error").toString(), "different_request_token_reuse");
        CTEST_COMPARE(host.handle("phone", request), response);
        host.closeAndWait();
        host.openHostAccess(root.path(), scope); CTEST_WAIT_DEFAULT(host.available()); host.setPeers({"phone"}, {});
        auto syncOnly = request;
        syncOnly.insert("token", QUuid::createUuid().toString(QUuid::WithoutBraces));
        syncOnly.insert("message", QJsonObject{{"action", "describe"}});
        CTEST_COMPARE(host.handle("phone", syncOnly).value("error").toString(), "host_access_operation_required");
        request.insert("token", QUuid::createUuid().toString(QUuid::WithoutBraces));
        CTEST_WAIT_DEFAULT((response = host.handle("phone", request)).contains("result"));
        CTEST_CHECK(response.value("result").toObject().value("ok").toBool());
        host.closeAndWait();
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    HostDriveTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("pinnedReadsDoNotContendWithTheMutationLock", [&] { FlushDeferredDeletes cleanup; tests.pinnedReadsDoNotContendWithTheMutationLock(); });
    runner.run("richInformationIsBoundedVersionedAndIndependentOfPayloadReads", [&] { FlushDeferredDeletes cleanup; tests.richInformationIsBoundedVersionedAndIndependentOfPayloadReads(); });
    runner.run("legacyShareHidesDerivedDocumentsWhileServingOriginalBytes", [&] { FlushDeferredDeletes cleanup; tests.legacyShareHidesDerivedDocumentsWhileServingOriginalBytes(); });
    runner.run("abandonedDirectWriteUsesTheExistingMaintenanceContract", [&] { FlushDeferredDeletes cleanup; tests.abandonedDirectWriteUsesTheExistingMaintenanceContract(); });
    runner.run("liveAccessDoesNotRequireTheIndexOrLocalPayload", [&] { FlushDeferredDeletes cleanup; tests.liveAccessDoesNotRequireTheIndexOrLocalPayload(); });
    runner.run("pagingIsBoundedAndRevisionChecked", [&] { FlushDeferredDeletes cleanup; tests.pagingIsBoundedAndRevisionChecked(); });
    runner.run("rejectsEscapesPrivateStateAndOtherContainers", [&] { FlushDeferredDeletes cleanup; tests.rejectsEscapesPrivateStateAndOtherContainers(); });
    runner.run("controllerRequiresPeerScopeAndExactRequestToken", [&] { FlushDeferredDeletes cleanup; tests.controllerRequiresPeerScopeAndExactRequestToken(); });
    return runner.result();
}
