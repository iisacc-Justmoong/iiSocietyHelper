#include "TestWorkspace.h"
#include <iiSocietyHelper.h>
#include <iiSocietyHelper/MediaExport.h>
#include <SharedStorage.h>
#include <StorageMap.h>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QSaveFile>
#include "SignalRecorder.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QEvent>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <iiSharedCanvas/Bitmap/BitmapCodec.h>
#include <iiFileProviderPresentation.h>
#include <iiSharedCanvas/Serialization/IiscCodec.h>
#include <iiSharedCanvas/File/DocumentFile.h>
#include <iiSocietyHelper/Monitoring/Monitor.h>
#include <File.h>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QDirIterator>
#include <future>

using namespace iiSocietyHelper;
using namespace iiSocietyContainer;

class FileSystemTests : public QObject
{
    QTemporaryDir m_fixture{(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/filesystem-XXXXXX"))};
    static bool createText(const QString &path,const QByteArray &bytes) {
        QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();
    }
    QString createDrive(const QString &name)
    {
        const auto path = m_fixture.filePath(name + ".societycontainer");
        if (!QDir().mkdir(path) || !SocietyDrive::create(path)) return {};
        return path;
    }
public:
    void statelessCanvasExportRetainsDestinationRulesAndDriveState() {
        const auto root=createDrive("canvas-export-selected");CTEST_CHECK(!root.isEmpty());
        iiFileProvider::native::Presentation::unwatch(std::filesystem::path(root.toStdU16String()));
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        {QFile marker(root+"/Files/untouched.txt");CTEST_CHECK(marker.open(QIODevice::WriteOnly));marker.write("preserve selected drive");}
        const auto previous=qgetenv("SOCIETY_CONTAINER_PATH");qputenv("SOCIETY_CONTAINER_PATH",root.toUtf8());
        const auto restore=qScopeGuard([&]{if(previous.isNull())qunsetenv("SOCIETY_CONTAINER_PATH");else qputenv("SOCIETY_CONTAINER_PATH",previous);});
        const auto manifest=[&] {
            QVariantMap result;QDirIterator entries(root,QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
            while(entries.hasNext()){const auto path=entries.next();QFile file(path);if(!file.open(QIODevice::ReadOnly))return QVariantMap{};
                result.insert(QDir(root).relativeFilePath(path),QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256));}
            return result;
        };
        const auto original=manifest();CTEST_CHECK(!original.isEmpty());
        iiSharedCanvas::Document document;document.extent={3,2};
        document.assets.emplace_back(iiSharedCanvas::RasterAsset{"pixels",makeRasterLayer(3,2,0xff4080c0U)});
        document.layers.emplace_back(iiSharedCanvas::StaticBitmapLayer{{"layer","Frame"},iiSharedCanvas::StaticSource{"pixels"}});
        const auto target=m_fixture.filePath("external-frame.iisc");
        const auto exported=iiSocietyHelper::exportCanvasCopy(document,QUrl::fromLocalFile(target));
        CTEST_CHECK_MESSAGE(exported.success,qPrintable(exported.error));
        iiSharedCanvas::DocumentFile reopened;CTEST_CHECK(reopened.open(target.toStdString()).ok());
        CTEST_COMPARE(reopened.document()->extent.width,3);CTEST_COMPARE(reopened.document()->extent.height,2);reopened.close();
        const auto originalFile=iiFileProvider::File::read(target);
        CTEST_CHECK(!iiSocietyHelper::exportCanvasCopy(document,QUrl::fromLocalFile(target)).success);
        CTEST_COMPARE(iiFileProvider::File::read(target),originalFile);
        CTEST_CHECK(!iiSocietyHelper::exportCanvasCopy(document,QUrl::fromLocalFile(root+"/Generation History/forbidden.iisc")).success);
        CTEST_CHECK(!iiSocietyHelper::mediaExportDestination(QUrl::fromLocalFile(root+"/wrong.png")).error.isEmpty());
        CTEST_CHECK(!iiSocietyHelper::mediaExportDestination(QUrl::fromLocalFile(root+"/Files/not-media.iisc")).error.isEmpty());
        CTEST_CHECK(iiSocietyHelper::mediaExportDestination(QUrl::fromLocalFile(root+"/Files/allowed.png")).error.isEmpty());
        CTEST_CHECK(!iiSocietyHelper::mediaExportDestination(QUrl("https://example.invalid/output.png")).error.isEmpty());
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        CTEST_COMPARE(manifest(),original);
        const auto inside=root+"/Files/frame.iisc";
        const auto insideResult=iiSocietyHelper::exportCanvasCopy(document,QUrl::fromLocalFile(inside));
        CTEST_CHECK_MESSAGE(insideResult.success,qPrintable(insideResult.error));
        CTEST_CHECK(reopened.open(inside.toStdString()).ok());CTEST_COMPARE(reopened.document()->layers.size(),std::size_t(1));
    }
    void statelessMediaExportDoesNotOpenOrChangeTheSelectedDrive() {
        const auto root = createDrive("export-must-not-open"); CTEST_CHECK(!root.isEmpty());
        // Drive creation starts its own presentation watcher. Quiesce this
        // fixture-owned producer before attributing any changes to export.
        iiFileProvider::native::Presentation::unwatch(std::filesystem::path(root.toStdU16String()));
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        { QFile marker(root + "/Files/untouched.txt"); CTEST_CHECK(marker.open(QIODevice::WriteOnly)); marker.write("preserve selected drive"); }
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        qputenv("SOCIETY_CONTAINER_PATH", root.toUtf8());
        const auto restore = qScopeGuard([&] {
            if (previous.isNull()) qunsetenv("SOCIETY_CONTAINER_PATH");
            else qputenv("SOCIETY_CONTAINER_PATH", previous);
        });
        const auto manifest = [&] {
            QVariantMap files;
            QDirIterator entries(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (entries.hasNext()) {
                const auto path = entries.next(); QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) return QVariantMap{};
                files.insert(QDir(root).relativeFilePath(path), QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
            }
            return files;
        };
        const auto original = manifest(); CTEST_CHECK(!original.isEmpty());
        const auto source = m_fixture.filePath("stateless-source.png"), target = m_fixture.filePath("stateless-target.png");
        QImage image(80, 40, QImage::Format_ARGB32); image.fill(Qt::red); CTEST_CHECK(image.save(source));
        const auto exported = iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source), QUrl::fromLocalFile(target));
        CTEST_CHECK_MESSAGE(exported.success, qPrintable(exported.error));
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        CTEST_COMPARE(iiFileProvider::File::read(target), iiFileProvider::File::read(source));
        CTEST_COMPARE(manifest(), original);
        iiSharedCanvas::Document canvas;canvas.extent={3,2};
        canvas.assets.emplace_back(iiSharedCanvas::RasterAsset{"pixels",makeRasterLayer(3,2,0x80ff0000U)});
        canvas.layers.emplace_back(iiSharedCanvas::StaticBitmapLayer{{"layer","Native"},iiSharedCanvas::StaticSource{"pixels"}});
        const auto encoded=iiSharedCanvas::encodeIisc(canvas);CTEST_CHECK(encoded.ok());
        const auto native=m_fixture.filePath("stateless-native.iisc"),png=m_fixture.filePath("stateless-native.png");
        iiFileProvider::File::create(native,QByteArray(reinterpret_cast<const char *>(encoded.bytes.data()),qsizetype(encoded.bytes.size())));
        const auto nativeExport=iiSocietyHelper::exportMedia(QUrl::fromLocalFile(native),QUrl::fromLocalFile(png));
        CTEST_CHECK_MESSAGE(nativeExport.success,qPrintable(nativeExport.error));
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        CTEST_COMPARE(QImage(png).size(),QSize(3,2));CTEST_COMPARE(manifest(),original);
        const auto invalid = m_fixture.filePath("stateless-invalid.png");
        { QFile file(invalid); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("invalid image"); }
        const auto rejected = iiSocietyHelper::exportMedia(QUrl::fromLocalFile(invalid), QUrl::fromLocalFile(target));
        CTEST_CHECK(!rejected.success); CTEST_CHECK(!rejected.error.isEmpty());
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        CTEST_COMPARE(iiFileProvider::File::read(target), iiFileProvider::File::read(source));
        CTEST_COMPARE(manifest(), original);
    }
    void nativeCanvasMediaExportUsesFullReadOnlyFrame(const QString &storage) {
        using namespace iiSharedCanvas;
        Document d;d.extent={5001,2};d.timeline={{24,1},2};
        auto pixels=makeRasterLayer(5001,2,0xff123456U);
        pixels.pixels[5000]=0x80ff0000U;pixels.pixels[5001]=0;
        d.assets.emplace_back(RasterAsset{"base",std::move(pixels)});
        d.assets.emplace_back(RasterAsset{"first",makeRasterLayer(1,1,0xff00ff00U)});
        d.assets.emplace_back(RasterAsset{"second",makeRasterLayer(1,1,0xffff0000U)});
        d.layers.emplace_back(StaticBitmapLayer{{"base-layer","Base"},StaticSource{"base"}});
        DynamicBitmapLayer animated;animated.properties.id="animated";animated.properties.name="Frame source";
        animated.content.frameIndices={0,1};d.layers.emplace_back(std::move(animated));
        d.frames={{0,{{"animated","first"}}},{1,{{"animated","second"}}}};
        StableDiffusionMetadata metadata;metadata.positivePrompt="Full native canvas";
        metadata.generationParametersText="Full native canvas\nSteps: 20, Seed: 7";
        metadata.comfyUi.promptJson="{\"7\":{\"class_type\":\"KSampler\"}}";
        metadata.comfyUi.workflowJson="{\"nodes\":[]}";
        metadata.comfyUi.extraPngInfo={{"Comment","\"native source\""}};
        metadata.extraParameters={{"custom","preserved"}};d.stableDiffusionMetadata=metadata;
        if(storage=="legacy19")d.formatVersion.minor=19;
        const auto source=m_fixture.filePath(storage+"-full.IISC"),target=m_fixture.filePath(storage+"-full.png");
        DocumentFile writer;
        if(storage.contains("working")) {
            const auto created=writer.create(source.toStdString(),d);CTEST_CHECK_MESSAGE(created.ok(),created.message.c_str());
            if(storage!="live-working")writer.close();
            if(storage=="readonly-working")CTEST_CHECK(QFile::setPermissions(source,QFileDevice::ReadOwner|QFileDevice::ReadGroup|QFileDevice::ReadOther));
        } else {
            const auto encoded=encodeIisc(d);CTEST_CHECK_MESSAGE(encoded.ok(),encoded.error.message.c_str());
            iiFileProvider::File::create(source,QByteArray(reinterpret_cast<const char *>(encoded.bytes.data()),qsizetype(encoded.bytes.size())));
        }
        const auto bytes=iiFileProvider::File::read(source);
        const auto modified=QFileInfo(source).lastModified();const auto revision=writer.revision();
        const auto exported=iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source),QUrl::fromLocalFile(target));
        CTEST_CHECK_MESSAGE(exported.success,qPrintable(exported.error));
        QImageReader reader(target);const auto image=reader.read();CTEST_CHECK_MESSAGE(!image.isNull(),qPrintable(reader.errorString()));
        CTEST_COMPARE(image.size(),QSize(5001,2));CTEST_COMPARE(image.pixel(0,0),0xff00ff00U);
        CTEST_COMPARE(image.pixel(5000,0),0x80ff0000U);CTEST_COMPARE(qAlpha(image.pixel(0,1)),0);
        CTEST_COMPARE(image.text("parameters"),QString::fromStdString(metadata.generationParametersText));
        CTEST_COMPARE(image.text("prompt"),QString::fromStdString(metadata.comfyUi.promptJson));
        CTEST_COMPARE(image.text("workflow"),QString::fromStdString(metadata.comfyUi.workflowJson));
        CTEST_COMPARE(image.text("Comment"),QString("\"native source\""));CTEST_COMPARE(image.text("custom"),QString("preserved"));
        CTEST_COMPARE(iiFileProvider::File::read(source),bytes);CTEST_COMPARE(QFileInfo(source).lastModified(),modified);
        CTEST_COMPARE(writer.revision(),revision);
        // Native reads use the same detached snapshot contract, including an open authoring owner.
        FileSystem files;const auto loaded=files.readCanvas(source);CTEST_CHECK_MESSAGE(loaded.ok(),loaded.result.message.c_str());
        CTEST_COMPARE(encodeIisc(loaded.document).bytes,encodeIisc(d).bytes);
        CTEST_COMPARE(iiFileProvider::File::read(source),bytes);CTEST_COMPARE(QFileInfo(source).lastModified(),modified);
        const auto output=iiFileProvider::File::read(target);
        const auto unsupported=m_fixture.filePath(storage+"-occupied.jpg");
        iiFileProvider::File::create(unsupported,"preserve existing destination");
        CTEST_CHECK(!iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source),QUrl::fromLocalFile(unsupported)).success);
        CTEST_COMPARE(iiFileProvider::File::read(unsupported),QByteArray("preserve existing destination"));
        CTEST_CHECK(!iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source),QUrl::fromLocalFile(source)).success);
        CTEST_CHECK(!iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source),QUrl::fromLocalFile(target),true).success);
        const auto invalid=m_fixture.filePath(storage+"-corrupt.iisc");iiFileProvider::File::create(invalid,"corrupt canvas");
        CTEST_CHECK(!iiSocietyHelper::exportMedia(QUrl::fromLocalFile(invalid),QUrl::fromLocalFile(target)).success);
        CTEST_COMPARE(iiFileProvider::File::read(target),output);CTEST_COMPARE(iiFileProvider::File::read(source),bytes);
    }
    void backgroundMirrorCoalescesWithoutBlockingInputAndRejectsOldSelections() {
        const auto root = createDrive("background-mirror"); CTEST_CHECK(!root.isEmpty());
        FileSystem files; CTEST_CHECK(files.open(root));
        for (int index = 0; index < 600; ++index) {
            QFile output(root + "/Files/item-" + QString::number(index) + ".txt");
            CTEST_CHECK(output.open(QIODevice::WriteOnly)); output.write("inventory");
        }
        helper_ctest::SignalRecorder completed(&files, &FileSystem::synchronizationFinished);
        int inputs = 0;
        QTimer input; input.setInterval(1);
        connect(&input, &QTimer::timeout, this, [&] { ++inputs; }); input.start();
        QElapsedTimer dispatch; dispatch.start();
        for (int request = 0; request < 300; ++request) files.requestSynchronization();
        const auto dispatchMs = dispatch.elapsed();
        // The owner publishes only a completed worker snapshot on its event loop.
        CTEST_CHECK(files.container()->alias("files/item-599.txt").isEmpty());
        CTEST_WAIT_VERIFY(!files.container()->alias("files/item-599.txt").isEmpty(), 10000);
        CTEST_CHECK_MESSAGE(dispatchMs < 100, qPrintable(QString("300 asynchronous requests took %1 ms").arg(dispatchMs)));
        CTEST_CHECK(inputs > 0);
        CTEST_CHECK(completed.count() >= 1);
        const auto expected = files.alias("files", "item-599.txt");
        CTEST_COMPARE(files.container()->alias("files/item-599.txt"), expected);

        files.requestSynchronization();
        const auto other = createDrive("background-mirror-other"); CTEST_CHECK(!other.isEmpty());
        CTEST_CHECK(files.open(other));
        CTEST_WAIT_VERIFY(completed.count() >= 2 && completed.last().first().toBool(), 10000);
        CTEST_COMPARE(files.container()->rootPath(), other);
        CTEST_CHECK(files.container()->alias("files/item-599.txt").isEmpty());
        CTEST_CHECK(files.synchronize()); // The explicit durability/refresh contract remains synchronous.
    }
    void richInformationUsesTheSelectedContainerAndPreservesOriginals() {
        const auto root = createDrive("rich-information"); CTEST_CHECK(!root.isEmpty());
        FileSystem files; CTEST_CHECK(files.open(root));
        const auto path = root + "/Files/brief.txt";
        { QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write("Approved document"), 17); }
        const auto information = files.fileInformation("files", "brief.txt");
        CTEST_CHECK_MESSAGE(!information.isEmpty(), qPrintable(files.errorString()));
        CTEST_COMPARE(information.value("format").toString(), "iiFileProvider.FileInformation");
        CTEST_COMPARE(information.value("preview").toMap().value("kind").toString(), "text");
        CTEST_COMPARE(information.value("classification").toMap().value("status").toString(), "absent");
        CTEST_CHECK(files.fileInformation("files", "../Models/escape").isEmpty());
        CTEST_CHECK(files.fileInformation("files", ".brief.txt.iisacc-preview").isEmpty());
    }
    void nativeMediaExportUsesProviderAndPreservesFailedDestinations() {
        FileSystem files;
        const auto source = m_fixture.filePath("export-source.png");
        const auto target = m_fixture.filePath("export-target.png");
        QImage image(2, 1, QImage::Format_RGBA8888); image.fill(Qt::blue);
        QByteArray encoded; QBuffer buffer(&encoded); CTEST_CHECK(buffer.open(QIODevice::WriteOnly));
        CTEST_CHECK(image.save(&buffer, "PNG")); buffer.close();
        iiFileProvider::File::create(source, encoded);
        CTEST_CHECK_MESSAGE(files.exportMedia(QUrl::fromLocalFile(source), QUrl::fromLocalFile(target), false), qPrintable(files.errorString()));
        CTEST_COMPARE(iiFileProvider::File::read(target), encoded);
        CTEST_CHECK(files.exportMedia(QUrl::fromLocalFile(source), QUrl::fromLocalFile(source), false));
        const auto invalid = m_fixture.filePath("not-media.png");
        iiFileProvider::File::create(invalid, "opaque invalid media");
        CTEST_CHECK(!files.exportMedia(QUrl::fromLocalFile(invalid), QUrl::fromLocalFile(target), false));
        CTEST_COMPARE(iiFileProvider::File::read(target), encoded);
        CTEST_CHECK(!files.exportMedia(QUrl::fromLocalFile(source), QUrl::fromLocalFile(m_fixture.filePath("invalid.iisc")), false));
        CTEST_CHECK(!QFileInfo::exists(m_fixture.filePath("invalid.iisc")));
        CTEST_CHECK(!files.exportMedia(QUrl::fromLocalFile(source), QUrl("https://example.invalid/output"), false));
        const auto video = m_fixture.filePath("synthetic.mp4");
        const QByteArray videoHeader("\x00\x00\x00\x18" "ftypisomfixture", 19);
        iiFileProvider::File::create(video, videoHeader);
        const auto videoTarget = m_fixture.filePath("video-export.mp4");
        CTEST_CHECK(files.exportMedia(QUrl::fromLocalFile(video), QUrl::fromLocalFile(videoTarget), true));
        CTEST_COMPARE(iiFileProvider::File::read(videoTarget), videoHeader);
    }
    void partialMonitorRetainsFallbackForChangesBeyondItsScanLimit() {
        const auto root = createDrive("Partial monitor"); CTEST_CHECK(!root.isEmpty());
        CTEST_CHECK(QDir(root + "/Files").mkdir("nested"));
        const auto drive = SocietyDrive::open(root); CTEST_CHECK(drive);
        iiSocietyHelper::Monitoring::Publisher publisher;
        CTEST_CHECK(publisher.start(std::filesystem::path(root.toStdU16String()), drive->identifier().toStdString(), {0, 9}));
        CTEST_CHECK(publisher.poll()); CTEST_CHECK(!publisher.state().complete);
        FileSystem files; CTEST_CHECK(files.open(root));
        CTEST_WAIT_DEFAULT(files.containerState().value("daemonRunning").toBool());
        CTEST_CHECK(publisher.poll()); // Settle any initial catalog creation.
        QFile output(root + "/Files/nested/beyond-limit.txt"); CTEST_CHECK(output.open(QIODevice::WriteOnly)); output.write("fallback"); output.close();
        const auto changed = publisher.poll(); CTEST_CHECK(changed && !*changed);
        QTimer heartbeat;
        heartbeat.setInterval(100);
        connect(&heartbeat, &QTimer::timeout, this, [&] { (void)publisher.poll(); });
        heartbeat.start();
        CTEST_WAIT_VERIFY(files.container()->alias("files/nested/beyond-limit.txt")["exists"].toBool(), 10000);
        CTEST_CHECK(files.containerState().value("daemonRunning").toBool());
        CTEST_CHECK(!files.containerState().value("complete").toBool());
    }
    void containerDaemonUpdatesTwoGuiConsumersAndSurvivesSelectionChanges() {
        const auto root = createDrive("Monitor state"); CTEST_CHECK(!root.isEmpty());
        const auto drive = SocietyDrive::open(root); CTEST_CHECK(drive);
        iiSocietyHelper::Monitoring::Publisher publisher;
        CTEST_CHECK(publisher.start(std::filesystem::path(root.toStdU16String()), drive->identifier().toStdString()));
        CTEST_CHECK(publisher.poll());
        Helper society, dreamscapes;
        CTEST_CHECK(society.fileSystem()->open(root)); CTEST_CHECK(dreamscapes.fileSystem()->open(root));
        auto *files = society.fileSystem();
        helper_ctest::SignalRecorder states(files, &FileSystem::containerStateChanged);
        CTEST_WAIT_VERIFY(files->containerState().value("daemonRunning").toBool(), 5000);
        CTEST_WAIT_VERIFY(dreamscapes.fileSystem()->containerState().value("daemonRunning").toBool(), 5000);
        CTEST_COMPARE(files->thread(), QThread::currentThread());
        bool uiThread = true;
        connect(files, &FileSystem::containerStateChanged, files, [&] { uiThread &= QThread::currentThread() == files->thread(); });
        const auto revision = files->containerState().value("revision").toString();
        // Reopening the same pinned drive does not reset the subscription.
        CTEST_CHECK(files->open(root)); CTEST_COMPARE(files->containerState().value("revision").toString(), revision);
        QFile output(root + "/Files/monitor.txt"); CTEST_CHECK(output.open(QIODevice::WriteOnly)); output.write("broadcast"); output.close();
        CTEST_CHECK(publisher.poll());
        CTEST_WAIT_VERIFY(files->containerState().value("revision").toString() != revision, 5000);
        CTEST_WAIT_COMPARE_TIMEOUT(dreamscapes.fileSystem()->containerState().value("revision"), files->containerState().value("revision"), 5000);
        // Daemon status is immediate; the independent native mirror worker
        // publishes its completed metadata through Container::stateChanged.
        CTEST_WAIT_DEFAULT(files->container()->alias("files/monitor.txt")["exists"].toBool());
        CTEST_CHECK(uiThread);
        const auto other = createDrive("Monitor other"); CTEST_CHECK(!other.isEmpty());
        CTEST_CHECK(files->open(other));
        CTEST_WAIT_COMPARE_TIMEOUT(files->containerState().value("rootPath").toString(), other, 5000);
        CTEST_CHECK(!files->containerState().value("daemonRunning").toBool());
        publisher.stop();
        CTEST_WAIT_VERIFY(!dreamscapes.fileSystem()->containerState().value("daemonRunning").toBool(), 5000);
        CTEST_CHECK(states.count() >= 3);
        CTEST_CHECK(QFileInfo::exists(root + "/Files/monitor.txt"));
    }
    void maskStacksSurviveStorageOwnerAndPreview() {
        using namespace iiSharedCanvas;
        const auto root = createDrive("Mask stack"); CTEST_CHECK(!root.isEmpty());
        CTEST_CHECK(SharedStorage::setDefaultContainer(root)); FileSystem files;
        Document d; d.extent = {2,1};
        d.assets.emplace_back(RasterAsset{"source", {2,1,{0xffff0000,0xff0000ff}}});
        d.assets.emplace_back(RasterAsset{"mask", {2,1,{0xffffffff,0xff000000}}});
        d.layers.emplace_back(StaticBitmapLayer{{"layer","Masked"},StaticSource{"source"}});
        auto &appearance=layerProperties(d.layers[0]).appearance;
        appearance.maskStack={{"first","Painted",LayerMask{.assetId="mask",.outsideCoverage=0}}};
        const auto encoded=encodeIisc(d); CTEST_CHECK(encoded.ok());
        const auto destination=files.path("files","mask.iisc");
        CTEST_CHECK(!files.saveCanvas(QByteArray(reinterpret_cast<const char *>(encoded.bytes.data()),qsizetype(encoded.bytes.size())),destination).isEmpty());
        const auto reopened=files.readCanvas(destination); CTEST_CHECK(reopened.ok());
        CTEST_CHECK(layerProperties(reopened.document.layers[0]).appearance==appearance);
        CTEST_COMPARE(std::get<RasterAsset>(reopened.document.assets[0]).pixels.pixels,std::get<RasterAsset>(d.assets[0]).pixels.pixels);
        const auto preview=importBitmap(files.preview(destination).toLocalFile().toStdString()); CTEST_CHECK(preview.ok());
        CTEST_COMPARE(preview.asset.pixels.pixels[0],0xffff0000U); CTEST_COMPARE(preview.asset.pixels.pixels[1]>>24,0U);
    }
    void remoteAliasesExistWithoutLocalPayloadOrPlaceholderDirectories() {
        const auto root = createDrive("Remote alias"); CTEST_CHECK(!root.isEmpty());
        const auto original = SocietyDrive::open(root); CTEST_CHECK(original);
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto replica = SocietyDrive::adoptReplicaIdentity(root, original->identifier(), host); CTEST_CHECK(replica);
        CTEST_CHECK(SocietyDrive::completeReplica(root, host));
        const auto ready = SocietyDrive::open(root); CTEST_CHECK(ready);
        StorageMap map(*ready);
        CTEST_CHECK(map.publish({QJsonObject{{"path", "files/not-downloaded/document.iisc"}, {"kind", "file"},
            {"size", "1024"}, {"version", QString(64, 'a')}}}));
        iiSocietyHelper::Monitoring::Publisher publisher;
        CTEST_CHECK(publisher.start(std::filesystem::path(root.toStdU16String()), ready->identifier().toStdString()));
        CTEST_CHECK(publisher.poll());
        FileSystem files; CTEST_CHECK(files.open(root)); CTEST_CHECK(files.synchronize());
        CTEST_WAIT_DEFAULT(files.containerState().value("daemonRunning").toBool());
        const auto revision = files.containerState().value("revision").toString();
        const auto alias = files.container()->alias("files/not-downloaded/document.iisc");
        CTEST_CHECK(alias["exists"].toBool()); CTEST_CHECK(!alias["available"].toBool());
        CTEST_COMPARE(alias["size"].toLongLong(), 1024);
        CTEST_CHECK(alias["source"].toUrl().isEmpty());
        CTEST_CHECK(!QFileInfo::exists(root + "/Files/not-downloaded"));
        CTEST_CHECK(files.alias("files", "../Models/file.iisc").isEmpty());
        CTEST_CHECK(map.publish({QJsonObject{{"path", "files/not-downloaded/second.iisc"}, {"kind", "file"}, {"size", "2048"}, {"version", QString(64, 'b')}}}));
        CTEST_CHECK(publisher.poll());
        CTEST_WAIT_DEFAULT(files.containerState().value("revision").toString() != revision);
        CTEST_WAIT_DEFAULT(files.container()->alias("files/not-downloaded/second.iisc")["exists"].toBool());
        CTEST_CHECK(!files.container()->alias("files/not-downloaded/second.iisc")["available"].toBool());
    }
    void containerAliasesFollowHelperSynchronizationAndNeverWritePlaceholders() {
        const auto root = createDrive("Alias snapshots"); CTEST_CHECK(!root.isEmpty());
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        Helper reader, writer;
        auto *mirror = reader.fileSystem()->container();
        CTEST_COMPARE(mirror->parent(), reader.fileSystem()); CTEST_CHECK(mirror->available());
        const auto input = m_fixture.filePath("alias.png");
        QFile source(input); CTEST_CHECK(source.open(QIODevice::WriteOnly));
        source.write(QByteArray::fromHex("89504e470d0a1a0a") + "PNG alias fixture"); source.close();
        const auto result = writer.fileSystem()->publishGeneratedImage(input, "alias.png"); CTEST_CHECK(!result.isEmpty());
        CTEST_CHECK(reader.fileSystem()->synchronize());
        const auto original = mirror->alias("generation-history/alias.png");
        CTEST_CHECK(original["exists"].toBool()); CTEST_CHECK(original["available"].toBool());
        CTEST_COMPARE(original["previewSource"].toUrl().toLocalFile(), result);
        const auto revision = mirror->revision();
        CTEST_CHECK(writer.fileSystem()->removeFile("generation-history", "alias.png"));
        CTEST_CHECK(mirror->alias("generation-history/alias.png")["exists"].toBool()); // Snapshot, no direct disk probe.
        CTEST_CHECK(reader.fileSystem()->synchronize());
        CTEST_CHECK(mirror->alias("generation-history/alias.png").isEmpty()); CTEST_CHECK(mirror->revision() > revision);
        const auto absent = reader.fileSystem()->alias("files", "not-downloaded.iisc");
        CTEST_CHECK(!absent["exists"].toBool()); CTEST_CHECK(!absent["available"].toBool());
        CTEST_CHECK(!QFileInfo::exists(root + "/Files/not-downloaded.iisc"));
        CTEST_CHECK(!reader.fileSystem()->open(m_fixture.filePath("not-a-drive")));
        CTEST_CHECK(!mirror->available()); CTEST_CHECK(mirror->aliases().isEmpty());
    }
    void helperOwnsCanvasSavePreviewAndVersionConflict() {
        using namespace iiSharedCanvas;
        const auto root = createDrive("Native alias"); CTEST_CHECK(!root.isEmpty());
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        FileSystem files;
        Document document; document.extent = {3, 2};
        document.assets.emplace_back(RasterAsset{"pixels", {3, 2, std::vector<std::uint32_t>(6, 0xff204080U)}});
        document.layers.emplace_back(StaticBitmapLayer{{"fill", "Canvas"}, StaticSource{"pixels"}});
#if __has_include(<iiSharedCanvas/Metadata/PublicationMetadata.h>)
        PublicationMetadata publication;
        publication.title="Common publication"; publication.description=std::string(80*1024,'x');
        publication.canvases={{"","Caption 한국어","Alternative text"}};
        publication.settings={{"adult",false},{"license",std::string("All rights reserved")}};
        document.publicationMetadata=publication;
#endif
        const auto encode = [](const Document &value) {
            const auto data = encodeIisc(value); return QByteArray(reinterpret_cast<const char *>(data.bytes.data()), qsizetype(data.bytes.size()));
        };
        const auto destination = files.path("files", "document.iisc");
        const auto first = files.saveCanvas(encode(document), destination); CTEST_CHECK_MESSAGE(!first.isEmpty(), qPrintable(files.errorString()));
        CTEST_CHECK(first["available"].toBool()); CTEST_CHECK(!first["version"].toString().isEmpty());
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        const auto storedPreview = iiFileProvider::native::Presentation::load(std::filesystem::path(destination.toStdU16String()));
        CTEST_CHECK(storedPreview); CTEST_COMPARE(storedPreview->preview.kind, std::string("image"));
        const auto png = QImage::fromData(reinterpret_cast<const uchar *>(storedPreview->preview.bytes.data()), qsizetype(storedPreview->preview.bytes.size()));
        CTEST_CHECK(!png.isNull()); CTEST_COMPARE(png.pixel(0, 0), QRgb(0xff204080U));
        const auto recent=files.container()->recentFiles();
        CTEST_COMPARE(recent.size(), 1);
        CTEST_CHECK(files.container()->alias("files/document.iisc.lock").isEmpty());
        CTEST_CHECK(files.container()->alias("files/.document.iisc.lock").isEmpty());
        const auto preview = files.preview(destination); CTEST_CHECK(preview.isLocalFile());
        CTEST_CHECK(preview.toLocalFile().startsWith(root + "/.society-runtime/"));
        const auto bitmap = importBitmap(preview.toLocalFile().toStdString()); CTEST_CHECK(bitmap.ok());
        CTEST_COMPARE(bitmap.asset.pixels.pixels[0], 0xff204080U);
        std::get<RasterAsset>(document.assets[0]).pixels.pixels[0] = 0xff804020U;
        const auto second = files.saveCanvas(encode(document), destination, first["version"].toString()); CTEST_CHECK(!second.isEmpty());
        CTEST_CHECK(first["version"] != second["version"]);
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        const auto revisedPreview = iiFileProvider::native::Presentation::load(std::filesystem::path(destination.toStdU16String()));
        CTEST_CHECK(revisedPreview); CTEST_CHECK(revisedPreview->preview.bytes != storedPreview->preview.bytes);
        CTEST_CHECK(files.preview(destination) != preview);
        const auto reopened = files.readCanvas(destination); CTEST_CHECK(reopened.ok());
#if __has_include(<iiSharedCanvas/Metadata/PublicationMetadata.h>)
        CTEST_CHECK(reopened.document.publicationMetadata==publication);
#endif
        CTEST_COMPARE(std::get<RasterAsset>(reopened.document.assets[0]).pixels.pixels[0], 0xff804020U);
        CTEST_CHECK(files.saveCanvas(encode(document), destination, first["version"].toString()).isEmpty());
        CTEST_CHECK(files.errorString().contains("changed"));
        CTEST_CHECK(files.saveCanvas(encode(document), root + "/Generation History/document.iisc").isEmpty());
        CTEST_CHECK(!QFileInfo::exists(root + "/Generation History/document.iisc"));
        CTEST_CHECK(files.saveCanvas(encode(document), destination).isEmpty()); // Existing document is preserved.
    }
    void deferredStorageDoesNotDiscoverTheDefaultDrive() {
        const auto root=createDrive("Deferred storage");CTEST_CHECK(!root.isEmpty());CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        FileSystem deferred(FileSystem::Initialization::Deferred);
        CTEST_CHECK(!deferred.isAvailable());CTEST_CHECK(deferred.rootPath().isEmpty());CTEST_CHECK(deferred.container()->aliases().isEmpty());
        CTEST_CHECK(deferred.containerState().isEmpty());CTEST_CHECK(deferred.errorString().isEmpty());
        FileSystem discovered;CTEST_CHECK(discovered.isAvailable());CTEST_COMPARE(discovered.rootPath(),root);
        CTEST_CHECK(deferred.open(root));CTEST_CHECK(deferred.isAvailable());CTEST_COMPARE(deferred.rootPath(),root);
    }
    void preparedStorageSurvivesItsWorkerAndDoesNotRescanOnAdoption() {
        const auto root=createDrive("Prepared storage");CTEST_CHECK(!root.isEmpty());
        CTEST_CHECK(createText(root+"/Files/captured.txt","captured"));
        std::promise<PreparedStorage> prepared;
        auto future=prepared.get_future();
        auto *worker=QThread::create([root,&prepared] {
            FileSystem files(FileSystem::Initialization::Deferred);
            if(!files.open(root)){prepared.set_value({});return;}
            prepared.set_value(files.preparedStorage());
        });
        worker->start();CTEST_CHECK(worker->wait(10000));delete worker;
        const auto state=future.get();CTEST_CHECK(state.isValid());
        // The source owner is gone. A newly added file must not be discovered
        // until an explicit/asynchronous refresh of the transferred selection.
        CTEST_CHECK(createText(root+"/Files/later.txt","later"));
        FileSystem adopted(FileSystem::Initialization::Deferred);helper_ctest::SignalRecorder changed(&adopted,&FileSystem::storageChanged);
        CTEST_CHECK(adopted.adoptStorage(state));CTEST_COMPARE(changed.count(),1);
        CTEST_COMPARE(adopted.rootPath(),root);CTEST_COMPARE(adopted.container()->aliases().size(),1);
        CTEST_CHECK(adopted.container()->alias("files/captured.txt")["available"].toBool());
        CTEST_CHECK(adopted.container()->alias("files/later.txt").isEmpty());
        CTEST_CHECK(adopted.synchronize());CTEST_COMPARE(adopted.container()->aliases().size(),2);
        FileSystem independent(FileSystem::Initialization::Deferred);CTEST_CHECK(independent.adoptStorage(state));CTEST_COMPARE(independent.container()->aliases().size(),1);
    }
    void invalidPreparedStoragePreservesTheSelectedDrive() {
        const auto root=createDrive("Keep selected storage");CTEST_CHECK(!root.isEmpty());
        FileSystem files;CTEST_CHECK(files.open(root));const auto id=files.containerId();
        CTEST_CHECK(files.matchesStorage(root,id));
        CTEST_CHECK(!files.matchesStorage(root,id+"-wrong"));
        CTEST_CHECK(!files.matchesStorage(root+"-wrong",id));
        CTEST_CHECK(!files.adoptStorage({}));CTEST_COMPARE(files.rootPath(),root);CTEST_COMPARE(files.containerId(),id);
        CTEST_CHECK(!files.errorString().isEmpty());
        const auto captured=files.preparedStorage();
        const auto moved=root+".previous";CTEST_CHECK(QDir().rename(root,moved));CTEST_CHECK(QDir().mkdir(root));
        CTEST_CHECK(SocietyDrive::create(root));CTEST_CHECK(!files.adoptStorage(captured));
        CTEST_CHECK(!files.matchesStorage(root,id));
        CTEST_COMPARE(files.container()->containerId(),id);
    }
    void generatedPngIsPublishedToThePinnedDriveWithoutRunningSociety() {
        const auto root = createDrive("Generated PNG"); CTEST_CHECK(!root.isEmpty());
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        Helper helper; auto *storage = helper.fileSystem(); CTEST_CHECK(!helper.isRunning());
        const auto bytes = QByteArray::fromHex("89504e470d0a1a0a") + "unchanged generated PNG";
        const auto source = m_fixture.filePath("generated.png");
        QFile input(source); CTEST_CHECK(input.open(QIODevice::WriteOnly)); CTEST_COMPARE(input.write(bytes), bytes.size()); input.close();
        const auto path = storage->publishGeneratedImage(source, "result-0001.png");
        CTEST_COMPARE(path, root + "/Generation History/result-0001.png"); CTEST_CHECK(storage->errorString().isEmpty());
        FileSystem reader; CTEST_COMPARE(reader.rootPath(), root);
        const auto rows = reader.entries("generation-history"); CTEST_COMPARE(rows.size(), 1);
        CTEST_COMPARE(rows.first().toMap().value("path").toString(), path);
        QFile saved(path); CTEST_CHECK(saved.open(QIODevice::ReadOnly)); CTEST_COMPARE(saved.readAll(), bytes); saved.close();
        const auto other = createDrive("Other PNG Drive"); CTEST_CHECK(!other.isEmpty());
        CTEST_CHECK(SharedStorage::setDefaultContainer(other));
        CTEST_COMPARE(storage->publishGeneratedImage(source, "result-0002.png"), root + "/Generation History/result-0002.png");
        CTEST_CHECK(!QFileInfo::exists(other + "/Generation History/result-0002.png"));
        CTEST_CHECK(storage->publishGeneratedImage(source, "result-0001.png").isEmpty()); CTEST_CHECK(!storage->errorString().isEmpty());
        CTEST_CHECK(saved.open(QIODevice::ReadOnly)); CTEST_COMPARE(saved.readAll(), bytes);
        CTEST_CHECK(storage->publishGeneratedImage(source, "result.iisc").isEmpty());
        CTEST_CHECK(storage->refresh()); CTEST_COMPARE(storage->rootPath(), other);
        CTEST_COMPARE(storage->publishGeneratedImage(source, "result-0001.png"), other + "/Generation History/result-0001.png");
        CTEST_CHECK(storage->errorString().isEmpty());
    }
    void defaultObservationBelongsToTheSelectedPackageAndLookupIsReadOnly()
    {
        const auto root = createDrive("Runtime Scope"); CTEST_CHECK(!root.isEmpty());
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        const auto before = qgetenv("SOCIETY_HELPER_DIRECTORY");
        qunsetenv("SOCIETY_HELPER_DIRECTORY");
        const auto directory = Helper::defaultDirectory();
        CTEST_COMPARE(directory, QDir(root).filePath(".society-runtime/helpers/v1"));
        CTEST_CHECK(!QFileInfo::exists(directory));
        if (!before.isNull()) qputenv("SOCIETY_HELPER_DIRECTORY", before);
    }
    void init(const char *caseName)
    {
        CTEST_CHECK(m_fixture.isValid());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        const auto settings = m_fixture.filePath(QString::fromUtf8(caseName) + ".json");
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settings.toUtf8());
    }

    void modelsTrackOwnerChangesWithoutRestart()
    {
        const auto root = createDrive("Model Inventory"); CTEST_CHECK(!root.isEmpty());
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        const auto drive = SocietyDrive::open(root); CTEST_CHECK(drive);
        StorageMap map(*drive);
        CTEST_CHECK(map.publish({QJsonObject{{"path", "models/Checkpoint/deleted.safetensors"},
            {"kind", "file"}, {"size", "7"}, {"version", QString(64, 'a')}}}));
        QFile primary(root + "/.society-sync/primary.json"); CTEST_CHECK(primary.open(QIODevice::WriteOnly));
        primary.write(QJsonDocument(QJsonObject{{"schema", 1}, {"container", drive->identifier()},
            {"scope", QString(64, 'a')}, {"host", "test-host"}}).toJson()); primary.close();
        Helper helper; auto &files = *helper.fileSystem();
        CTEST_CHECK(files.models().isEmpty()); CTEST_CHECK(files.errorString().isEmpty());
        CTEST_CHECK(QDir().mkpath(root + "/Models/Checkpoint"));
        QFile model(root + "/Models/Checkpoint/new.safetensors"); CTEST_CHECK(model.open(QIODevice::WriteOnly));
        model.write("new weights"); model.close();
        const auto inventory = files.models(); CTEST_COMPARE(inventory.size(), 1);
        CTEST_COMPARE(inventory.first().id, "Checkpoint/new.safetensors"); CTEST_CHECK(inventory.first().available);
        CTEST_CHECK(model.rename(root + "/Deleted/new.safetensors"));
        CTEST_CHECK(files.models().isEmpty()); CTEST_CHECK(files.errorString().isEmpty());
        CTEST_CHECK(!files.open(m_fixture.filePath("absent")));
        CTEST_CHECK(files.models().isEmpty()); CTEST_CHECK(!files.errorString().isEmpty());
    }

    void nativeIoAcrossIndependentApplications()
    {
        const auto root = createDrive("Native IO");
        CTEST_CHECK(!root.isEmpty());
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        Helper helper;
        auto *storage = helper.fileSystem();
        CTEST_CHECK(!helper.isRunning()); // File access needs neither presence nor a daemon.
        CTEST_COMPARE(helper.property("fileSystem").value<FileSystem *>(), storage);
        CTEST_COMPARE(storage->parent(), &helper);
        CTEST_CHECK(storage->isAvailable());
        CTEST_COMPARE(storage->rootPath(), root);
        CTEST_COMPARE(storage->sections().size(), 9);
        CTEST_COMPARE(storage->path("photos"), root + "/Photos");
        for (const auto &mode : {"write", "verify"}) {
            QProcess peer;
            peer.start(QStringLiteral(HELPER_FILESYSTEM_PEER), {mode});
            CTEST_CHECK(peer.waitForStarted());
            CTEST_CHECK(peer.waitForFinished(10000));
            CTEST_COMPARE(peer.exitStatus(), QProcess::NormalExit);
            CTEST_CHECK_MESSAGE(peer.exitCode() == 0, peer.readAllStandardError().constData());
            const auto result = QJsonDocument::fromJson(peer.readAllStandardOutput()).object();
            CTEST_COMPARE(result.value("containerId").toString(), storage->containerId());
            CTEST_COMPARE(result.value("rootPath").toString(), root);
            CTEST_COMPARE(result.value("sectionCount").toInt(), 9);
            if (QString(mode) != "write") continue;
            for (const auto &value : storage->sections()) {
                const auto section = value.toMap();
                const auto key = section.value("key").toString();
                const auto relative = QStringLiteral("Helper IO/한글/shared #%.bin");
                const auto path = storage->path(key, relative);
                CTEST_COMPARE(storage->url(key, relative).toLocalFile(), path);
                CTEST_CHECK(storage->url(key, relative).isLocalFile());
                CTEST_COMPARE(storage->path(key), section.value("path").toString());
                QFile file(path);
                CTEST_CHECK(file.open(QIODevice::ReadOnly));
                CTEST_COMPARE(file.readAll(), QByteArray("from-peer:") + key.toUtf8());
                file.close();
                QSaveFile output(path);
                CTEST_CHECK(output.open(QIODevice::WriteOnly));
                const auto bytes = QByteArray("from-app:") + key.toUtf8();
                CTEST_COMPARE(output.write(bytes), bytes.size());
                CTEST_CHECK(output.commit());
            }
        }
        const auto original = storage->path("files", "Helper IO/한글/shared #%.bin");
        const auto renamed = storage->path("files", "Helper IO/한글/renamed.bin");
        CTEST_CHECK(QFile::rename(original, renamed));
        const auto listed = storage->entries("files", "Helper IO/한글");
        CTEST_COMPARE(listed.size(), 1);
        CTEST_COMPARE(listed.first().toMap().value("name").toString(), "renamed.bin");
        CTEST_COMPARE(listed.first().toMap().value("path").toString(), renamed);
        CTEST_CHECK(!listed.first().toMap().value("isDirectory").toBool());
        CTEST_CHECK(storage->entries("files", "../Models").isEmpty());
        CTEST_CHECK(!storage->errorString().isEmpty());
        CTEST_CHECK(QDir(QFileInfo(renamed).absolutePath()).entryList(QDir::Files).contains("renamed.bin"));
        CTEST_CHECK(QFile::remove(renamed));
        CTEST_CHECK(!QFileInfo::exists(renamed));
        QString invoked;
        CTEST_CHECK(QMetaObject::invokeMethod(storage, "path", Q_RETURN_ARG(QString, invoked), Q_ARG(QString, "models")));
        CTEST_COMPARE(invoked, root + "/Models");
    }

    void missingStorageRecoversAndDoesNotInterruptObservation()
    {
        Helper helper;
        auto *storage = helper.fileSystem();
        CTEST_CHECK(!storage->isAvailable());
        CTEST_CHECK(!storage->errorString().isEmpty());
        CTEST_CHECK(storage->path("files").isEmpty());
        CTEST_CHECK(helper.start({"com.iisacc.filesystem.test", "Test", "1"},
                             {m_fixture.filePath("presence"), 50, 400}));
        CTEST_CHECK(helper.errorString().isEmpty());
        helper_ctest::SignalRecorder changes(storage, &FileSystem::storageChanged);
        const auto root = createDrive("Late Storage");
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        CTEST_COMPARE(storage->path("files"), root + "/Files"); // Retry a previously unavailable default.
        CTEST_CHECK(storage->isAvailable());
        CTEST_CHECK(storage->errorString().isEmpty());
        CTEST_CHECK(!changes.isEmpty());
        helper.stop();
        CTEST_COMPARE(storage->path("models"), root + "/Models");
    }

    void qmlContextUsesTheSameNativePaths()
    {
        const auto root = createDrive("QML Source");
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        Helper helper;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("societyHelper", &helper);
        QQmlExpression available(engine.rootContext(), nullptr, "societyHelper.fileSystem.available");
        CTEST_CHECK(available.evaluate().toBool());
        CTEST_CHECK_MESSAGE(!available.hasError(), qPrintable(available.error().toString()));
        QQmlExpression path(engine.rootContext(), nullptr, "societyHelper.fileSystem.path('models')");
        CTEST_COMPARE(path.evaluate().toString(), root + "/Models");
        CTEST_CHECK_MESSAGE(!path.hasError(), qPrintable(path.error().toString()));
        QQmlExpression url(engine.rootContext(), nullptr, "societyHelper.fileSystem.url('files', '한글 #%.png')");
        CTEST_COMPARE(url.evaluate().toUrl().toLocalFile(), root + "/Files/한글 #%.png");
        CTEST_CHECK_MESSAGE(!url.hasError(), qPrintable(url.error().toString()));
    }

    void diagnosticOnlyReadsStorageAndReportsMissingDefaults()
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        const auto presence = m_fixture.filePath("diagnostic-presence");
        environment.insert("SOCIETY_HELPER_DIRECTORY", presence);
        for (const bool configured : {false, true}) {
            QString root;
            if (configured) {
                root = createDrive("Diagnostic Source");
                CTEST_CHECK(SharedStorage::setDefaultContainer(root));
            }
            QProcess diagnostic;
            diagnostic.setProcessEnvironment(environment);
            diagnostic.start(QStringLiteral(HELPER_DIAGNOSTIC_EXECUTABLE), {"--filesystem"});
            CTEST_CHECK(diagnostic.waitForStarted());
            CTEST_CHECK(diagnostic.waitForFinished(10000));
            CTEST_COMPARE(diagnostic.exitStatus(), QProcess::NormalExit);
            CTEST_COMPARE(diagnostic.exitCode(), configured ? 0 : 1);
            const auto result = QJsonDocument::fromJson(diagnostic.readAllStandardOutput()).object();
            CTEST_COMPARE(result.value("available").toBool(), configured);
            CTEST_COMPARE(result.value("rootPath").toString(), root);
            CTEST_COMPARE(result.value("error").toString().isEmpty(), configured);
            CTEST_CHECK(!QFileInfo::exists(presence));
        }
    }

    void selectionIsPinnedUntilExplicitRefresh()
    {
        const auto first = createDrive("First"), second = createDrive("Second");
        CTEST_CHECK(SharedStorage::setDefaultContainer(first));
        Helper helper;
        auto *storage = helper.fileSystem();
        const auto firstId = storage->containerId();
        CTEST_CHECK(SharedStorage::setDefaultContainer(second));
        CTEST_COMPARE(storage->rootPath(), first);
        CTEST_COMPARE(storage->containerId(), firstId);
        CTEST_CHECK(storage->refresh());
        CTEST_COMPARE(storage->rootPath(), second);
        CTEST_CHECK(storage->open(first));
        qputenv("SOCIETY_CONTAINER_PATH", second.toUtf8());
        CTEST_CHECK(storage->refresh());
        CTEST_COMPARE(storage->rootPath(), first); // Explicit selection wins over env/settings.
        CTEST_CHECK(storage->open());
        CTEST_COMPARE(storage->rootPath(), second);
        CTEST_CHECK(!storage->open(m_fixture.filePath("Missing")));
        CTEST_CHECK(storage->rootPath().isEmpty());
        CTEST_CHECK(storage->path("files").isEmpty()); // No fallback to another drive.
        CTEST_CHECK(storage->open());
        CTEST_COMPARE(storage->rootPath(), second);
    }

    void errorsRejectInvalidPathsAndReplacedDrives()
    {
        const auto root = createDrive("Boundaries");
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        Helper helper;
        auto *storage = helper.fileSystem();
        helper_ctest::SignalRecorder errors(storage, &FileSystem::errorChanged);
        CTEST_CHECK(storage->path("unknown").isEmpty());
        CTEST_CHECK(!storage->errorString().isEmpty());
        CTEST_CHECK(storage->url("models", "../Files/escape").isEmpty());
        CTEST_CHECK(storage->ensureDirectory("models", "../Files/escape").isEmpty());
        CTEST_CHECK(!errors.isEmpty());
        CTEST_COMPARE(storage->path("models"), root + "/Models");
        CTEST_CHECK(storage->errorString().isEmpty());
        const auto oldId = storage->containerId();
        CTEST_CHECK(QFile::remove(root + "/.society-drive.json"));
        CTEST_CHECK(SocietyDrive::create(root));
        CTEST_CHECK(SharedStorage::setDefaultContainer(root));
        CTEST_CHECK(!storage->isAvailable());
        CTEST_CHECK(storage->rootPath().isEmpty());
        CTEST_CHECK(storage->sections().isEmpty());
        CTEST_CHECK(storage->path("models", "new.bin").isEmpty());
        CTEST_CHECK(!storage->errorString().isEmpty());
        CTEST_CHECK(storage->refresh());
        CTEST_CHECK(storage->containerId() != oldId);
    }
    // Process-wide quiescence is irreversible, so this test must remain last.
    void shutdownPreparationRetiresMonitorAndStopsAdmission() {
        const auto root=createDrive("shutdown-preparation");CTEST_CHECK(!root.isEmpty());
        auto files=std::make_unique<FileSystem>(FileSystem::Initialization::Deferred);
        CTEST_CHECK(files->open(root));files->requestSynchronization();
        QElapsedTimer elapsed;elapsed.start();files->prepareForShutdown();CTEST_CHECK(elapsed.elapsed()<250);
        CTEST_CHECK(!files->open(root));files.reset();
        auto barrier=std::async(std::launch::async,[]{FileSystem::quiesceBackgroundWork();});
        CTEST_WAIT_VERIFY(barrier.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,20000);
        barrier.get();
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("statelessCanvasExportRetainsDestinationRulesAndDriveState", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("statelessCanvasExportRetainsDestinationRulesAndDriveState"); tests.statelessCanvasExportRetainsDestinationRulesAndDriveState(); });
    runner.run("statelessMediaExportDoesNotOpenOrChangeTheSelectedDrive", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("statelessMediaExportDoesNotOpenOrChangeTheSelectedDrive"); tests.statelessMediaExportDoesNotOpenOrChangeTheSelectedDrive(); });
    runner.run("nativeCanvasMediaExportUsesFullReadOnlyFrame:legacy19", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("nativeCanvasMediaExportUsesFullReadOnlyFrame"); tests.nativeCanvasMediaExportUsesFullReadOnlyFrame(QStringLiteral("legacy19")); });
    runner.run("nativeCanvasMediaExportUsesFullReadOnlyFrame:snapshot", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("nativeCanvasMediaExportUsesFullReadOnlyFrame"); tests.nativeCanvasMediaExportUsesFullReadOnlyFrame(QStringLiteral("snapshot")); });
    runner.run("nativeCanvasMediaExportUsesFullReadOnlyFrame:working", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("nativeCanvasMediaExportUsesFullReadOnlyFrame"); tests.nativeCanvasMediaExportUsesFullReadOnlyFrame(QStringLiteral("working")); });
    runner.run("nativeCanvasMediaExportUsesFullReadOnlyFrame:readonly-working", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("nativeCanvasMediaExportUsesFullReadOnlyFrame"); tests.nativeCanvasMediaExportUsesFullReadOnlyFrame(QStringLiteral("readonly-working")); });
    runner.run("nativeCanvasMediaExportUsesFullReadOnlyFrame:live-working", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("nativeCanvasMediaExportUsesFullReadOnlyFrame"); tests.nativeCanvasMediaExportUsesFullReadOnlyFrame(QStringLiteral("live-working")); });
    runner.run("backgroundMirrorCoalescesWithoutBlockingInputAndRejectsOldSelections", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("backgroundMirrorCoalescesWithoutBlockingInputAndRejectsOldSelections"); tests.backgroundMirrorCoalescesWithoutBlockingInputAndRejectsOldSelections(); });
    runner.run("richInformationUsesTheSelectedContainerAndPreservesOriginals", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("richInformationUsesTheSelectedContainerAndPreservesOriginals"); tests.richInformationUsesTheSelectedContainerAndPreservesOriginals(); });
    runner.run("nativeMediaExportUsesProviderAndPreservesFailedDestinations", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("nativeMediaExportUsesProviderAndPreservesFailedDestinations"); tests.nativeMediaExportUsesProviderAndPreservesFailedDestinations(); });
    runner.run("partialMonitorRetainsFallbackForChangesBeyondItsScanLimit", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("partialMonitorRetainsFallbackForChangesBeyondItsScanLimit"); tests.partialMonitorRetainsFallbackForChangesBeyondItsScanLimit(); });
    runner.run("containerDaemonUpdatesTwoGuiConsumersAndSurvivesSelectionChanges", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("containerDaemonUpdatesTwoGuiConsumersAndSurvivesSelectionChanges"); tests.containerDaemonUpdatesTwoGuiConsumersAndSurvivesSelectionChanges(); });
    runner.run("maskStacksSurviveStorageOwnerAndPreview", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("maskStacksSurviveStorageOwnerAndPreview"); tests.maskStacksSurviveStorageOwnerAndPreview(); });
    runner.run("remoteAliasesExistWithoutLocalPayloadOrPlaceholderDirectories", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("remoteAliasesExistWithoutLocalPayloadOrPlaceholderDirectories"); tests.remoteAliasesExistWithoutLocalPayloadOrPlaceholderDirectories(); });
    runner.run("containerAliasesFollowHelperSynchronizationAndNeverWritePlaceholders", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("containerAliasesFollowHelperSynchronizationAndNeverWritePlaceholders"); tests.containerAliasesFollowHelperSynchronizationAndNeverWritePlaceholders(); });
    runner.run("helperOwnsCanvasSavePreviewAndVersionConflict", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("helperOwnsCanvasSavePreviewAndVersionConflict"); tests.helperOwnsCanvasSavePreviewAndVersionConflict(); });
    runner.run("deferredStorageDoesNotDiscoverTheDefaultDrive", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("deferredStorageDoesNotDiscoverTheDefaultDrive"); tests.deferredStorageDoesNotDiscoverTheDefaultDrive(); });
    runner.run("preparedStorageSurvivesItsWorkerAndDoesNotRescanOnAdoption", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("preparedStorageSurvivesItsWorkerAndDoesNotRescanOnAdoption"); tests.preparedStorageSurvivesItsWorkerAndDoesNotRescanOnAdoption(); });
    runner.run("invalidPreparedStoragePreservesTheSelectedDrive", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("invalidPreparedStoragePreservesTheSelectedDrive"); tests.invalidPreparedStoragePreservesTheSelectedDrive(); });
    runner.run("generatedPngIsPublishedToThePinnedDriveWithoutRunningSociety", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("generatedPngIsPublishedToThePinnedDriveWithoutRunningSociety"); tests.generatedPngIsPublishedToThePinnedDriveWithoutRunningSociety(); });
    runner.run("defaultObservationBelongsToTheSelectedPackageAndLookupIsReadOnly", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("defaultObservationBelongsToTheSelectedPackageAndLookupIsReadOnly"); tests.defaultObservationBelongsToTheSelectedPackageAndLookupIsReadOnly(); });
    runner.run("modelsTrackOwnerChangesWithoutRestart", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("modelsTrackOwnerChangesWithoutRestart"); tests.modelsTrackOwnerChangesWithoutRestart(); });
    runner.run("nativeIoAcrossIndependentApplications", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("nativeIoAcrossIndependentApplications"); tests.nativeIoAcrossIndependentApplications(); });
    runner.run("missingStorageRecoversAndDoesNotInterruptObservation", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("missingStorageRecoversAndDoesNotInterruptObservation"); tests.missingStorageRecoversAndDoesNotInterruptObservation(); });
    runner.run("qmlContextUsesTheSameNativePaths", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("qmlContextUsesTheSameNativePaths"); tests.qmlContextUsesTheSameNativePaths(); });
    runner.run("diagnosticOnlyReadsStorageAndReportsMissingDefaults", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("diagnosticOnlyReadsStorageAndReportsMissingDefaults"); tests.diagnosticOnlyReadsStorageAndReportsMissingDefaults(); });
    runner.run("selectionIsPinnedUntilExplicitRefresh", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("selectionIsPinnedUntilExplicitRefresh"); tests.selectionIsPinnedUntilExplicitRefresh(); });
    runner.run("errorsRejectInvalidPathsAndReplacedDrives", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("errorsRejectInvalidPathsAndReplacedDrives"); tests.errorsRejectInvalidPathsAndReplacedDrives(); });
    runner.run("shutdownPreparationRetiresMonitorAndStopsAdmission", [&] { FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("shutdownPreparationRetiresMonitorAndStopsAdmission"); tests.shutdownPreparationRetiresMonitorAndStopsAdmission(); });
    return runner.result();
}
