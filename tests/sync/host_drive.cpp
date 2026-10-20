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
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace iiSocietyHelper::Sync;
class HostDriveTests : public QObject {
    Q_OBJECT
    const QString scope = QString(64, 'a');
    static void put(const QString &path, const QByteArray &bytes) {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
    }
    static QByteArray get(const QString &path) {
        QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
private slots:
    void richInformationIsBoundedVersionedAndIndependentOfPayloadReads() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/rich-host-XXXXXX.societycontainer");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        Replica host; QVERIFY(host.open(root.path(), scope));
        const auto call = [&](QJsonObject request) {
            request.insert("container", drive->identifier()); return host.handle("phone", request, false);
        };
        iiFileProvider::FileCatalog catalog(std::filesystem::path(root.filePath("Files").toStdU16String()));
        iiFileProvider::FileMetadata annotation; annotation.rating = 4; annotation.keywords = {"host", "campaign"};
        const std::string payload = "An original that never has to be downloaded for inspection";
        QVERIFY(catalog.createFile("remote.txt", {reinterpret_cast<const std::byte *>(payload.data()), payload.size()}, annotation).metadataWritten);
        // An incompressible but valid bounded domain thumbnail exercises more
        // than one transport chunk without a large original or catalog scan.
        QImage image(512, 512, QImage::Format_RGB32); quint32 random = 1;
        for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            image.setPixel(x, y, 0xff000000u | (random & 0x00ffffff));
        }
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        iiFileProvider::native::FilePreview preview{"image/png", "image", png.toStdString(), 512, 512};
        (void)iiFileProvider::native::Presentation::attach(std::filesystem::path(root.filePath("Files/remote.txt").toStdU16String()), preview);
        const auto first = call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}});
        QVERIFY2(first.value("ok").toBool(), qPrintable(first.value("error").toString()));
        QVERIFY(!first.value("eof").toBool());
        auto document = QByteArray::fromBase64(first.value("data").toString().toLatin1());
        QCOMPARE(document.size(), Replica::ChunkBytes);
        const auto version = first.value("version").toString(), token = first.value("informationVersion").toString();
        while (document.size() < first.value("size").toString().toLongLong()) {
            const auto next = call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}, {"offset", QString::number(document.size())},
                {"version", version}, {"informationVersion", token}});
            QVERIFY2(next.value("ok").toBool(), qPrintable(next.value("error").toString()));
            const auto chunk = QByteArray::fromBase64(next.value("data").toString().toLatin1());
            QVERIFY(!chunk.isEmpty()); QVERIFY(chunk.size() <= Replica::ChunkBytes); document += chunk;
        }
        const auto information = QJsonDocument::fromJson(document).object();
        QCOMPARE(information.value("format").toString(), "iiFileProvider.FileInformation");
        QCOMPARE(information.value("classification").toObject().value("metadata").toObject().value("rating").toInt(), 4);
        QCOMPARE(QByteArray::fromBase64(information.value("preview").toObject().value("data").toString().toLatin1()), png);
        QVERIFY(!information.contains("rootPath")); QCOMPARE(get(root.filePath("Files/remote.txt")), QByteArray::fromStdString(payload));
        QCOMPARE(call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}, {"offset", "1"}}).value("error").toString(), "inspection_version_required");
        const auto current = catalog.lookupPath("remote.txt"); QVERIFY(current);
        // Refresh registration after a domain attachment changes native ctime.
        const auto refreshed = catalog.registerFile("remote.txt"); annotation.rating = 5;
        QVERIFY(catalog.updateMetadata(refreshed.file.key, refreshed.file.revision, annotation).metadataWritten);
        QCOMPARE(call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}, {"offset", "1"}, {"version", version},
            {"informationVersion", token}}).value("error").toString(), "drive_information_changed");
        put(root.filePath("Files/remote.txt"), "changed source");
        QCOMPARE(call({{"action", "drive.inspect"}, {"path", "files/remote.txt"}, {"version", version}}).value("error").toString(), "drive_file_changed");
        QVERIFY(!call({{"action", "drive.inspect"}, {"path", "files/remote.txt.iisacc.json"}}).value("ok").toBool());
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(root.path().toStdU16String()));
    }
    void legacyShareHidesDerivedDocumentsWhileServingOriginalBytes() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/direct-presentation-XXXXXX.societycontainer");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        put(root.filePath("Files/report.txt"), "public source bytes");
        put(root.filePath("Files/.report.txt.iisacc-preview"), "private derived fixture");
        put(root.filePath("Files/report.txt.iisacc.json"), "{\"rating\":5}");
        const auto handle = filesHandler(root.path());
        const auto listed = handle("peer", {{"op", "list"}, {"path", ""}});
        QVERIFY(listed.value("ok").toBool());
        const auto entries = listed.value("entries").toArray(); QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().toObject().value("name").toString(), "report.txt");
        for (const auto &name : {".report.txt.iisacc-preview", "report.txt.iisacc.json"})
            QVERIFY(!handle("peer", {{"op", "stat"}, {"path", name}}).value("ok").toBool());
        const auto read = handle("peer", {{"op", "read"}, {"path", "report.txt"}, {"offset", "0"}});
        QVERIFY(read.value("ok").toBool());
        QCOMPARE(QByteArray::fromBase64(read.value("data").toString().toLatin1()), QByteArray("public source bytes"));
    }
    void abandonedDirectWriteUsesTheExistingMaintenanceContract() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/direct-orphan-XXXXXX.societycontainer");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        put(root.filePath("Files/.society-write-abandoned"), "incomplete staging");
        put(root.filePath("Files/published.bin"), "complete publication");
        const auto report = iiSocietyContainer::StorageMaintenance::runOnce(
            std::filesystem::path(root.path().toStdU16String()), drive->identifier().toStdString());
        // SocietyDrive also schedules maintenance; its worker may win the
        // reclamation race before this explicit scan. The filesystem outcome
        // and preservation of published content are the shared contract.
        QVERIFY(report.errors.empty());
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root.filePath("Files/.society-write-abandoned")), 3000);
        QCOMPARE(get(root.filePath("Files/published.bin")), QByteArray("complete publication"));
    }
    void liveAccessDoesNotRequireTheIndexOrLocalPayload() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/direct-XXXXXX.societycontainer");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        Replica host; QVERIFY(host.open(root.path(), scope));
        const auto call = [&](const QString &action, QJsonObject data = {}) {
            data.insert("action", "drive." + action); data.insert("container", drive->identifier());
            return host.handle("phone", data, false);
        };
        const auto descriptor = call("describe"); QVERIFY2(descriptor.value("ok").toBool(), qPrintable(descriptor.value("error").toString()));
        QCOMPARE(descriptor.value("driveProtocol").toInt(), 1);
        QCOMPARE(descriptor.value("sections").toArray().size(), 9);
        QVERIFY(!descriptor.contains("rootPath"));
        const QByteArray bytes(Replica::ChunkBytes * 2 + 37, 'x');
        put(root.filePath("Models/live.bin"), bytes); // Deliberately not scanned.
        const auto listed = call("list", {{"path", "models"}});
        QVERIFY(listed.value("ok").toBool());
        QCOMPARE(listed.value("entries").toArray().first().toObject().value("path").toString(), "models/live.bin");
        const auto stat = call("stat", {{"path", "models/live.bin"}});
        QVERIFY(stat.value("ok").toBool()); QCOMPARE(stat.value("size").toString().toLongLong(), bytes.size());
        const auto native = std::filesystem::path(root.filePath("Models/live.bin").toStdU16String());
        QVERIFY(iiFileProvider::native::Presentation::load(native));
        QVERIFY(iiFileProvider::native::Presentation::prepare(native).cacheHit);
        put(root.filePath("Models/live.bin.iisacc.json"), "{\"rating\":5}");
        const auto publicRows = call("list", {{"path", "models"}}).value("entries").toArray();
        QCOMPARE(publicRows.size(), 1);
        QCOMPARE(publicRows.first().toObject().value("path").toString(), "models/live.bin");
        for (const auto &control : {"models/.live.bin.iisacc-preview", "models/live.bin.iisacc.json", "models/.iisacc-temporary.lock"})
            QCOMPARE(call("stat", {{"path", control}}).value("error").toString(), "invalid_drive_path");
        const auto version = stat.value("version").toString(); QVERIFY(!version.isEmpty());
        QByteArray received;
        while (received.size() < bytes.size()) {
            const auto read = call("read", {{"path", "models/live.bin"}, {"offset", QString::number(received.size())}, {"version", version}});
            QVERIFY(read.value("ok").toBool());
            const auto chunk = QByteArray::fromBase64(read.value("data").toString().toLatin1());
            QVERIFY(!chunk.isEmpty()); QVERIFY(chunk.size() <= Replica::ChunkBytes); received += chunk;
            QCOMPARE(read.value("eof").toBool(), received.size() == bytes.size());
        }
        QCOMPARE(received, bytes);
        put(root.filePath("Models/live.bin"), "changed");
        QVERIFY(!call("read", {{"path", "models/live.bin"}, {"offset", "0"}, {"version", version}}).value("ok").toBool());
        const auto directory = call("mkdir", {{"path", "files/new"}}); QVERIFY(directory.value("ok").toBool());
        QVERIFY(!call("mkdir", {{"path", "files/new"}}).value("ok").toBool());
        const QJsonObject write{{"path", "files/new/empty.dat"}, {"data", ""}};
        QVERIFY(call("write", write).value("ok").toBool()); QVERIFY(QFileInfo::exists(root.filePath("Files/new/empty.dat")));
        const auto encoded = QString::fromLatin1(QByteArray("direct host bytes").toBase64());
        QVERIFY(call("write", {{"path", "generation-history/result.png"}, {"data", encoded}}).value("ok").toBool());
        QVERIFY(!call("write", {{"path", "generation-history/result.png"}, {"data", "b3ZlcndyaXRl"}}).value("ok").toBool());
        QCOMPARE(get(root.filePath("Generation History/result.png")), QByteArray("direct host bytes"));
        QVERIFY(host.scan());
        const auto catalog = call("catalog"); QVERIFY(catalog.value("ok").toBool());
        QVERIFY(catalog.value("entries").toArray().size() >= 3);
        QVERIFY(!catalog.value("revision").toString().isEmpty());
    }
    void pagingIsBoundedAndRevisionChecked() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/pages-XXXXXX.societycontainer");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        const auto privateUse = QString(QChar(0xe000)), emoji = QString::fromUtf8("\xf0\x9f\x98\x80");
        for (int i = 0; i < 135; ++i)
            put(root.filePath("Files/" + (i < 127 ? privateUse : emoji) + QString("item-%1").arg(i, 3, 10, QChar('0'))), "a");
        Replica host; QVERIFY(host.open(root.path(), scope)); QVERIFY(host.scan());
        const auto call = [&](QJsonObject data) { data.insert("container", drive->identifier()); return host.handle("phone", data, false); };
        auto page = call({{"action", "drive.catalog"}}); QVERIFY(page.value("ok").toBool());
        QCOMPARE(page.value("entries").toArray().size(), 128);
        const auto epochNow = QDateTime::currentMSecsSinceEpoch();
        bool sawModified = false;
        for (const auto &value : page.value("entries").toArray()) {
            const auto entry = value.toObject();
            // Catalog metadata is omitted when the live file no longer
            // matches the scan. Check timestamps on the still-current entries.
            if (entry.contains("modified")) {
                bool validTime = false;
                const auto modified = entry.value("modified").toString().toLongLong(&validTime);
                QVERIFY(validTime);
                QVERIFY(modified > epochNow - 60000 && modified <= epochNow + 1000);
                sawModified = true;
            }
        }
        QVERIFY(sawModified);
        // The wire catalog uses UTF-8 binary order, which differs from UTF-16
        // string order across supplementary characters and private-use names.
        QVERIFY(page.value("entries").toArray().first().toObject().value("path").toString().startsWith("files/" + privateUse));
        QVERIFY(page.value("entries").toArray().last().toObject().value("path").toString().startsWith("files/" + emoji));
        const auto cursor = page.value("nextCursor").toString(), revision = page.value("revision").toString(); QVERIFY(!cursor.isEmpty());
        page = call({{"action", "drive.catalog"}, {"cursor", cursor}, {"revision", revision}});
        QVERIFY(page.value("ok").toBool()); QCOMPARE(page.value("entries").toArray().size(), 7); QVERIFY(page.value("nextCursor").toString().isEmpty());
        put(root.filePath("Files/added"), "b"); QVERIFY(host.scan());
        QCOMPARE(call({{"action", "drive.catalog"}, {"cursor", cursor}, {"revision", revision}}).value("error").toString(), "drive_snapshot_changed");
        // Pin a stable directory after preview companions finish changing its
        // native metadata. Intake behavior is covered independently above.
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(root.path().toStdU16String()));
        page = call({{"action", "drive.list"}, {"path", "files"}});
        QVERIFY2(page.value("ok").toBool(), qPrintable(page.value("error").toString()));
        QCOMPARE(page.value("entries").toArray().size(), 128);
        QVERIFY(!call({{"action", "drive.list"}, {"path", "files"}, {"cursor", page.value("nextCursor")}}).value("ok").toBool());
    }
    void rejectsEscapesPrivateStateAndOtherContainers() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/boundary-XXXXXX.societycontainer"), outside(SYNC_TEST_DIRECTORY "/outside-XXXXXX");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        put(outside.filePath("secret"), "outside");
        QVERIFY(nativeTestLink(outside.path(), root.filePath("Files/redirect")));
        Replica host; QVERIFY(host.open(root.path(), scope));
        for (const auto &path : QStringList{"../secret", "/etc/passwd", "files/../secret", "files//secret", "files\\secret", ".society-sync/journal.sqlite", "files/.society-sync/a", "files/.iisacc-temporary.lock", "files/redirect/secret"}) {
            const auto result = host.handle("phone", {{"action", "drive.read"}, {"container", drive->identifier()}, {"path", path}, {"offset", "0"}}, false);
            QVERIFY2(!result.value("ok").toBool(), qPrintable(path));
            QVERIFY(!result.contains("data"));
        }
        QVERIFY(!host.handle("phone", {{"action", "drive.mkdir"}, {"container", drive->identifier()}, {"path", "files"}}, false).value("ok").toBool());
        QVERIFY(!host.handle("phone", {{"action", "drive.describe"}, {"container", QUuid::createUuid().toString(QUuid::WithoutBraces)}}, false).value("ok").toBool());
        QVERIFY(!host.handle("phone", {{"action", "drive.describe"}}, false).value("ok").toBool());
        QCOMPARE(get(outside.filePath("secret")), QByteArray("outside"));
    }
    void controllerRequiresPeerScopeAndExactRequestToken() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/authority-XXXXXX.societycontainer");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        Controller host({}); host.open(root.path(), scope); QTRY_VERIFY(host.available()); host.setPeers({"phone"}, {});
        QJsonObject request{{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1}, {"scope", scope},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"message", QJsonObject{{"action", "drive.describe"}, {"container", drive->identifier()}}}};
        QVERIFY(!host.handle("stranger", request).value("ok").toBool());
        auto other = request; other.insert("scope", QString(64, 'b')); QVERIFY(!host.handle("phone", other).value("ok").toBool());
        QJsonObject response;
        QTRY_VERIFY((response = host.handle("phone", request)).contains("result"));
        QVERIFY(response.value("result").toObject().value("ok").toBool());
        auto changed = request; changed.insert("message", QJsonObject{{"action", "drive.list"}, {"container", drive->identifier()}});
        QCOMPARE(host.handle("phone", changed).value("error").toString(), "different_request_token_reuse");
        QCOMPARE(host.handle("phone", request), response);
        host.closeAndWait();
        host.openHostAccess(root.path(), scope); QTRY_VERIFY(host.available()); host.setPeers({"phone"}, {});
        auto syncOnly = request;
        syncOnly.insert("token", QUuid::createUuid().toString(QUuid::WithoutBraces));
        syncOnly.insert("message", QJsonObject{{"action", "describe"}});
        QCOMPARE(host.handle("phone", syncOnly).value("error").toString(), "host_access_operation_required");
        request.insert("token", QUuid::createUuid().toString(QUuid::WithoutBraces));
        QTRY_VERIFY((response = host.handle("phone", request)).contains("result"));
        QVERIFY(response.value("result").toObject().value("ok").toBool());
        host.closeAndWait();
    }
};
QTEST_GUILESS_MAIN(HostDriveTests)
#include "host_drive.moc"
