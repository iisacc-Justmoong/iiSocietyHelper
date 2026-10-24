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
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
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
    Q_OBJECT
    QTemporaryDir m_fixture{QStringLiteral(HELPER_TEST_DIRECTORY "/filesystem-XXXXXX")};
    static bool createText(const QString &path,const QByteArray &bytes) {
        QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();
    }
    QString createDrive(const QString &name)
    {
        const auto path = m_fixture.filePath(name + ".societycontainer");
        if (!QDir().mkdir(path) || !SocietyDrive::create(path)) return {};
        return path;
    }
private slots:
    void statelessCanvasExportRetainsDestinationRulesAndDriveState() {
        const auto root=createDrive("canvas-export-selected");QVERIFY(!root.isEmpty());
        iiFileProvider::native::Presentation::unwatch(std::filesystem::path(root.toStdU16String()));
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        {QFile marker(root+"/Files/untouched.txt");QVERIFY(marker.open(QIODevice::WriteOnly));marker.write("preserve selected drive");}
        const auto previous=qgetenv("SOCIETY_CONTAINER_PATH");qputenv("SOCIETY_CONTAINER_PATH",root.toUtf8());
        const auto restore=qScopeGuard([&]{if(previous.isNull())qunsetenv("SOCIETY_CONTAINER_PATH");else qputenv("SOCIETY_CONTAINER_PATH",previous);});
        const auto manifest=[&] {
            QVariantMap result;QDirIterator entries(root,QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
            while(entries.hasNext()){const auto path=entries.next();QFile file(path);if(!file.open(QIODevice::ReadOnly))return QVariantMap{};
                result.insert(QDir(root).relativeFilePath(path),QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256));}
            return result;
        };
        const auto original=manifest();QVERIFY(!original.isEmpty());
        iiSharedCanvas::Document document;document.extent={3,2};
        document.assets.emplace_back(iiSharedCanvas::RasterAsset{"pixels",makeRasterLayer(3,2,0xff4080c0U)});
        document.layers.emplace_back(iiSharedCanvas::StaticBitmapLayer{{"layer","Frame"},iiSharedCanvas::StaticSource{"pixels"}});
        const auto target=m_fixture.filePath("external-frame.iisc");
        const auto exported=iiSocietyHelper::exportCanvasCopy(document,QUrl::fromLocalFile(target));
        QVERIFY2(exported.success,qPrintable(exported.error));
        iiSharedCanvas::DocumentFile reopened;QVERIFY(reopened.open(target.toStdString()).ok());
        QCOMPARE(reopened.document()->extent.width,3);QCOMPARE(reopened.document()->extent.height,2);reopened.close();
        const auto originalFile=iiFileProvider::File::read(target);
        QVERIFY(!iiSocietyHelper::exportCanvasCopy(document,QUrl::fromLocalFile(target)).success);
        QCOMPARE(iiFileProvider::File::read(target),originalFile);
        QVERIFY(!iiSocietyHelper::exportCanvasCopy(document,QUrl::fromLocalFile(root+"/Generation History/forbidden.iisc")).success);
        QVERIFY(!iiSocietyHelper::mediaExportDestination(QUrl::fromLocalFile(root+"/wrong.png")).error.isEmpty());
        QVERIFY(!iiSocietyHelper::mediaExportDestination(QUrl::fromLocalFile(root+"/Files/not-media.iisc")).error.isEmpty());
        QVERIFY(iiSocietyHelper::mediaExportDestination(QUrl::fromLocalFile(root+"/Files/allowed.png")).error.isEmpty());
        QVERIFY(!iiSocietyHelper::mediaExportDestination(QUrl("https://example.invalid/output.png")).error.isEmpty());
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        QCOMPARE(manifest(),original);
        const auto inside=root+"/Files/frame.iisc";
        const auto insideResult=iiSocietyHelper::exportCanvasCopy(document,QUrl::fromLocalFile(inside));
        QVERIFY2(insideResult.success,qPrintable(insideResult.error));
        QVERIFY(reopened.open(inside.toStdString()).ok());QCOMPARE(reopened.document()->layers.size(),std::size_t(1));
    }
    void statelessMediaExportDoesNotOpenOrChangeTheSelectedDrive() {
        const auto root = createDrive("export-must-not-open"); QVERIFY(!root.isEmpty());
        // Drive creation starts its own presentation watcher. Quiesce this
        // fixture-owned producer before attributing any changes to export.
        iiFileProvider::native::Presentation::unwatch(std::filesystem::path(root.toStdU16String()));
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        { QFile marker(root + "/Files/untouched.txt"); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.write("preserve selected drive"); }
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
        const auto original = manifest(); QVERIFY(!original.isEmpty());
        const auto source = m_fixture.filePath("stateless-source.png"), target = m_fixture.filePath("stateless-target.png");
        QImage image(80, 40, QImage::Format_ARGB32); image.fill(Qt::red); QVERIFY(image.save(source));
        const auto exported = iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source), QUrl::fromLocalFile(target));
        QVERIFY2(exported.success, qPrintable(exported.error));
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        QCOMPARE(iiFileProvider::File::read(target), iiFileProvider::File::read(source));
        QCOMPARE(manifest(), original);
        iiSharedCanvas::Document canvas;canvas.extent={3,2};
        canvas.assets.emplace_back(iiSharedCanvas::RasterAsset{"pixels",makeRasterLayer(3,2,0x80ff0000U)});
        canvas.layers.emplace_back(iiSharedCanvas::StaticBitmapLayer{{"layer","Native"},iiSharedCanvas::StaticSource{"pixels"}});
        const auto encoded=iiSharedCanvas::encodeIisc(canvas);QVERIFY(encoded.ok());
        const auto native=m_fixture.filePath("stateless-native.iisc"),png=m_fixture.filePath("stateless-native.png");
        iiFileProvider::File::create(native,QByteArray(reinterpret_cast<const char *>(encoded.bytes.data()),qsizetype(encoded.bytes.size())));
        const auto nativeExport=iiSocietyHelper::exportMedia(QUrl::fromLocalFile(native),QUrl::fromLocalFile(png));
        QVERIFY2(nativeExport.success,qPrintable(nativeExport.error));
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        QCOMPARE(QImage(png).size(),QSize(3,2));QCOMPARE(manifest(),original);
        const auto invalid = m_fixture.filePath("stateless-invalid.png");
        { QFile file(invalid); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("invalid image"); }
        const auto rejected = iiSocietyHelper::exportMedia(QUrl::fromLocalFile(invalid), QUrl::fromLocalFile(target));
        QVERIFY(!rejected.success); QVERIFY(!rejected.error.isEmpty());
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        QCOMPARE(iiFileProvider::File::read(target), iiFileProvider::File::read(source));
        QCOMPARE(manifest(), original);
    }
    void nativeCanvasMediaExportUsesFullReadOnlyFrame_data() {
        QTest::addColumn<QString>("storage");
        for(const auto &kind:{"legacy19","snapshot","working","readonly-working","live-working"})QTest::newRow(kind)<<QString(kind);
    }
    void nativeCanvasMediaExportUsesFullReadOnlyFrame() {
        QFETCH(QString,storage);using namespace iiSharedCanvas;
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
            const auto created=writer.create(source.toStdString(),d);QVERIFY2(created.ok(),created.message.c_str());
            if(storage!="live-working")writer.close();
            if(storage=="readonly-working")QVERIFY(QFile::setPermissions(source,QFileDevice::ReadOwner|QFileDevice::ReadGroup|QFileDevice::ReadOther));
        } else {
            const auto encoded=encodeIisc(d);QVERIFY2(encoded.ok(),encoded.error.message.c_str());
            iiFileProvider::File::create(source,QByteArray(reinterpret_cast<const char *>(encoded.bytes.data()),qsizetype(encoded.bytes.size())));
        }
        const auto bytes=iiFileProvider::File::read(source);
        const auto modified=QFileInfo(source).lastModified();const auto revision=writer.revision();
        const auto exported=iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source),QUrl::fromLocalFile(target));
        QVERIFY2(exported.success,qPrintable(exported.error));
        QImageReader reader(target);const auto image=reader.read();QVERIFY2(!image.isNull(),qPrintable(reader.errorString()));
        QCOMPARE(image.size(),QSize(5001,2));QCOMPARE(image.pixel(0,0),0xff00ff00U);
        QCOMPARE(image.pixel(5000,0),0x80ff0000U);QCOMPARE(qAlpha(image.pixel(0,1)),0);
        QCOMPARE(image.text("parameters"),QString::fromStdString(metadata.generationParametersText));
        QCOMPARE(image.text("prompt"),QString::fromStdString(metadata.comfyUi.promptJson));
        QCOMPARE(image.text("workflow"),QString::fromStdString(metadata.comfyUi.workflowJson));
        QCOMPARE(image.text("Comment"),QString("\"native source\""));QCOMPARE(image.text("custom"),QString("preserved"));
        QCOMPARE(iiFileProvider::File::read(source),bytes);QCOMPARE(QFileInfo(source).lastModified(),modified);
        QCOMPARE(writer.revision(),revision);
        // Native reads use the same detached snapshot contract, including an open authoring owner.
        FileSystem files;const auto loaded=files.readCanvas(source);QVERIFY2(loaded.ok(),loaded.result.message.c_str());
        QCOMPARE(encodeIisc(loaded.document).bytes,encodeIisc(d).bytes);
        QCOMPARE(iiFileProvider::File::read(source),bytes);QCOMPARE(QFileInfo(source).lastModified(),modified);
        const auto output=iiFileProvider::File::read(target);
        const auto unsupported=m_fixture.filePath(storage+"-occupied.jpg");
        iiFileProvider::File::create(unsupported,"preserve existing destination");
        QVERIFY(!iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source),QUrl::fromLocalFile(unsupported)).success);
        QCOMPARE(iiFileProvider::File::read(unsupported),QByteArray("preserve existing destination"));
        QVERIFY(!iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source),QUrl::fromLocalFile(source)).success);
        QVERIFY(!iiSocietyHelper::exportMedia(QUrl::fromLocalFile(source),QUrl::fromLocalFile(target),true).success);
        const auto invalid=m_fixture.filePath(storage+"-corrupt.iisc");iiFileProvider::File::create(invalid,"corrupt canvas");
        QVERIFY(!iiSocietyHelper::exportMedia(QUrl::fromLocalFile(invalid),QUrl::fromLocalFile(target)).success);
        QCOMPARE(iiFileProvider::File::read(target),output);QCOMPARE(iiFileProvider::File::read(source),bytes);
    }
    void backgroundMirrorCoalescesWithoutBlockingInputAndRejectsOldSelections() {
        const auto root = createDrive("background-mirror"); QVERIFY(!root.isEmpty());
        FileSystem files; QVERIFY(files.open(root));
        for (int index = 0; index < 600; ++index) {
            QFile output(root + "/Files/item-" + QString::number(index) + ".txt");
            QVERIFY(output.open(QIODevice::WriteOnly)); output.write("inventory");
        }
        QSignalSpy completed(&files, &FileSystem::synchronizationFinished);
        int inputs = 0;
        QTimer input; input.setInterval(1);
        connect(&input, &QTimer::timeout, this, [&] { ++inputs; }); input.start();
        QElapsedTimer dispatch; dispatch.start();
        for (int request = 0; request < 300; ++request) files.requestSynchronization();
        const auto dispatchMs = dispatch.elapsed();
        // The owner publishes only a completed worker snapshot on its event loop.
        QVERIFY(files.container()->alias("files/item-599.txt").isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!files.container()->alias("files/item-599.txt").isEmpty(), 10000);
        QVERIFY2(dispatchMs < 100, qPrintable(QString("300 asynchronous requests took %1 ms").arg(dispatchMs)));
        QVERIFY(inputs > 0);
        QVERIFY(completed.count() >= 1);
        const auto expected = files.alias("files", "item-599.txt");
        QCOMPARE(files.container()->alias("files/item-599.txt"), expected);

        files.requestSynchronization();
        const auto other = createDrive("background-mirror-other"); QVERIFY(!other.isEmpty());
        QVERIFY(files.open(other));
        QTRY_VERIFY_WITH_TIMEOUT(completed.count() >= 2 && completed.last().first().toBool(), 10000);
        QCOMPARE(files.container()->rootPath(), other);
        QVERIFY(files.container()->alias("files/item-599.txt").isEmpty());
        QVERIFY(files.synchronize()); // The explicit durability/refresh contract remains synchronous.
    }
    void richInformationUsesTheSelectedContainerAndPreservesOriginals() {
        const auto root = createDrive("rich-information"); QVERIFY(!root.isEmpty());
        FileSystem files; QVERIFY(files.open(root));
        const auto path = root + "/Files/brief.txt";
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("Approved document"), 17); }
        const auto information = files.fileInformation("files", "brief.txt");
        QVERIFY2(!information.isEmpty(), qPrintable(files.errorString()));
        QCOMPARE(information.value("format").toString(), "iiFileProvider.FileInformation");
        QCOMPARE(information.value("preview").toMap().value("kind").toString(), "text");
        QCOMPARE(information.value("classification").toMap().value("status").toString(), "absent");
        QVERIFY(files.fileInformation("files", "../Models/escape").isEmpty());
        QVERIFY(files.fileInformation("files", ".brief.txt.iisacc-preview").isEmpty());
    }
    void nativeMediaExportUsesProviderAndPreservesFailedDestinations() {
        FileSystem files;
        const auto source = m_fixture.filePath("export-source.png");
        const auto target = m_fixture.filePath("export-target.png");
        QImage image(2, 1, QImage::Format_RGBA8888); image.fill(Qt::blue);
        QByteArray encoded; QBuffer buffer(&encoded); QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG")); buffer.close();
        iiFileProvider::File::create(source, encoded);
        QVERIFY2(files.exportMedia(QUrl::fromLocalFile(source), QUrl::fromLocalFile(target), false), qPrintable(files.errorString()));
        QCOMPARE(iiFileProvider::File::read(target), encoded);
        QVERIFY(files.exportMedia(QUrl::fromLocalFile(source), QUrl::fromLocalFile(source), false));
        const auto invalid = m_fixture.filePath("not-media.png");
        iiFileProvider::File::create(invalid, "opaque invalid media");
        QVERIFY(!files.exportMedia(QUrl::fromLocalFile(invalid), QUrl::fromLocalFile(target), false));
        QCOMPARE(iiFileProvider::File::read(target), encoded);
        QVERIFY(!files.exportMedia(QUrl::fromLocalFile(source), QUrl::fromLocalFile(m_fixture.filePath("invalid.iisc")), false));
        QVERIFY(!QFileInfo::exists(m_fixture.filePath("invalid.iisc")));
        QVERIFY(!files.exportMedia(QUrl::fromLocalFile(source), QUrl("https://example.invalid/output"), false));
        const auto video = m_fixture.filePath("synthetic.mp4");
        const QByteArray videoHeader("\x00\x00\x00\x18" "ftypisomfixture", 19);
        iiFileProvider::File::create(video, videoHeader);
        const auto videoTarget = m_fixture.filePath("video-export.mp4");
        QVERIFY(files.exportMedia(QUrl::fromLocalFile(video), QUrl::fromLocalFile(videoTarget), true));
        QCOMPARE(iiFileProvider::File::read(videoTarget), videoHeader);
    }
    void partialMonitorRetainsFallbackForChangesBeyondItsScanLimit() {
        const auto root = createDrive("Partial monitor"); QVERIFY(!root.isEmpty());
        QVERIFY(QDir(root + "/Files").mkdir("nested"));
        const auto drive = SocietyDrive::open(root); QVERIFY(drive);
        iiSocietyHelper::Monitoring::Publisher publisher;
        QVERIFY(publisher.start(std::filesystem::path(root.toStdU16String()), drive->identifier().toStdString(), {0, 9}));
        QVERIFY(publisher.poll()); QVERIFY(!publisher.state().complete);
        FileSystem files; QVERIFY(files.open(root));
        QTRY_VERIFY(files.containerState().value("daemonRunning").toBool());
        QVERIFY(publisher.poll()); // Settle any initial catalog creation.
        QFile output(root + "/Files/nested/beyond-limit.txt"); QVERIFY(output.open(QIODevice::WriteOnly)); output.write("fallback"); output.close();
        const auto changed = publisher.poll(); QVERIFY(changed && !*changed);
        QTimer heartbeat;
        heartbeat.setInterval(100);
        connect(&heartbeat, &QTimer::timeout, this, [&] { (void)publisher.poll(); });
        heartbeat.start();
        QTRY_VERIFY_WITH_TIMEOUT(files.container()->alias("files/nested/beyond-limit.txt")["exists"].toBool(), 10000);
        QVERIFY(files.containerState().value("daemonRunning").toBool());
        QVERIFY(!files.containerState().value("complete").toBool());
    }
    void containerDaemonUpdatesTwoGuiConsumersAndSurvivesSelectionChanges() {
        const auto root = createDrive("Monitor state"); QVERIFY(!root.isEmpty());
        const auto drive = SocietyDrive::open(root); QVERIFY(drive);
        iiSocietyHelper::Monitoring::Publisher publisher;
        QVERIFY(publisher.start(std::filesystem::path(root.toStdU16String()), drive->identifier().toStdString()));
        QVERIFY(publisher.poll());
        Helper society, dreamscapes;
        QVERIFY(society.fileSystem()->open(root)); QVERIFY(dreamscapes.fileSystem()->open(root));
        auto *files = society.fileSystem();
        QSignalSpy states(files, &FileSystem::containerStateChanged);
        QTRY_VERIFY_WITH_TIMEOUT(files->containerState().value("daemonRunning").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(dreamscapes.fileSystem()->containerState().value("daemonRunning").toBool(), 5000);
        QCOMPARE(files->thread(), QThread::currentThread());
        bool uiThread = true;
        connect(files, &FileSystem::containerStateChanged, files, [&] { uiThread &= QThread::currentThread() == files->thread(); });
        const auto revision = files->containerState().value("revision").toString();
        // Reopening the same pinned drive does not reset the subscription.
        QVERIFY(files->open(root)); QCOMPARE(files->containerState().value("revision").toString(), revision);
        QFile output(root + "/Files/monitor.txt"); QVERIFY(output.open(QIODevice::WriteOnly)); output.write("broadcast"); output.close();
        QVERIFY(publisher.poll());
        QTRY_VERIFY_WITH_TIMEOUT(files->containerState().value("revision").toString() != revision, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(dreamscapes.fileSystem()->containerState().value("revision"), files->containerState().value("revision"), 5000);
        // Daemon status is immediate; the independent native mirror worker
        // publishes its completed metadata through Container::stateChanged.
        QTRY_VERIFY(files->container()->alias("files/monitor.txt")["exists"].toBool());
        QVERIFY(uiThread);
        const auto other = createDrive("Monitor other"); QVERIFY(!other.isEmpty());
        QVERIFY(files->open(other));
        QTRY_COMPARE_WITH_TIMEOUT(files->containerState().value("rootPath").toString(), other, 5000);
        QVERIFY(!files->containerState().value("daemonRunning").toBool());
        publisher.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!dreamscapes.fileSystem()->containerState().value("daemonRunning").toBool(), 5000);
        QVERIFY(states.count() >= 3);
        QVERIFY(QFileInfo::exists(root + "/Files/monitor.txt"));
    }
    void maskStacksSurviveStorageOwnerAndPreview() {
        using namespace iiSharedCanvas;
        const auto root = createDrive("Mask stack"); QVERIFY(!root.isEmpty());
        QVERIFY(SharedStorage::setDefaultContainer(root)); FileSystem files;
        Document d; d.extent = {2,1};
        d.assets.emplace_back(RasterAsset{"source", {2,1,{0xffff0000,0xff0000ff}}});
        d.assets.emplace_back(RasterAsset{"mask", {2,1,{0xffffffff,0xff000000}}});
        d.layers.emplace_back(StaticBitmapLayer{{"layer","Masked"},StaticSource{"source"}});
        auto &appearance=layerProperties(d.layers[0]).appearance;
        appearance.maskStack={{"first","Painted",LayerMask{.assetId="mask",.outsideCoverage=0}}};
        const auto encoded=encodeIisc(d); QVERIFY(encoded.ok());
        const auto destination=files.path("files","mask.iisc");
        QVERIFY(!files.saveCanvas(QByteArray(reinterpret_cast<const char *>(encoded.bytes.data()),qsizetype(encoded.bytes.size())),destination).isEmpty());
        const auto reopened=files.readCanvas(destination); QVERIFY(reopened.ok());
        QVERIFY(layerProperties(reopened.document.layers[0]).appearance==appearance);
        QCOMPARE(std::get<RasterAsset>(reopened.document.assets[0]).pixels.pixels,std::get<RasterAsset>(d.assets[0]).pixels.pixels);
        const auto preview=importBitmap(files.preview(destination).toLocalFile().toStdString()); QVERIFY(preview.ok());
        QCOMPARE(preview.asset.pixels.pixels[0],0xffff0000U); QCOMPARE(preview.asset.pixels.pixels[1]>>24,0U);
    }
    void remoteAliasesExistWithoutLocalPayloadOrPlaceholderDirectories() {
        const auto root = createDrive("Remote alias"); QVERIFY(!root.isEmpty());
        const auto original = SocietyDrive::open(root); QVERIFY(original);
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto replica = SocietyDrive::adoptReplicaIdentity(root, original->identifier(), host); QVERIFY(replica);
        QVERIFY(SocietyDrive::completeReplica(root, host));
        const auto ready = SocietyDrive::open(root); QVERIFY(ready);
        StorageMap map(*ready);
        QVERIFY(map.publish({QJsonObject{{"path", "files/not-downloaded/document.iisc"}, {"kind", "file"},
            {"size", "1024"}, {"version", QString(64, 'a')}}}));
        iiSocietyHelper::Monitoring::Publisher publisher;
        QVERIFY(publisher.start(std::filesystem::path(root.toStdU16String()), ready->identifier().toStdString()));
        QVERIFY(publisher.poll());
        FileSystem files; QVERIFY(files.open(root)); QVERIFY(files.synchronize());
        QTRY_VERIFY(files.containerState().value("daemonRunning").toBool());
        const auto revision = files.containerState().value("revision").toString();
        const auto alias = files.container()->alias("files/not-downloaded/document.iisc");
        QVERIFY(alias["exists"].toBool()); QVERIFY(!alias["available"].toBool());
        QCOMPARE(alias["size"].toLongLong(), 1024);
        QVERIFY(alias["source"].toUrl().isEmpty());
        QVERIFY(!QFileInfo::exists(root + "/Files/not-downloaded"));
        QVERIFY(files.alias("files", "../Models/file.iisc").isEmpty());
        QVERIFY(map.publish({QJsonObject{{"path", "files/not-downloaded/second.iisc"}, {"kind", "file"}, {"size", "2048"}, {"version", QString(64, 'b')}}}));
        QVERIFY(publisher.poll());
        QTRY_VERIFY(files.containerState().value("revision").toString() != revision);
        QTRY_VERIFY(files.container()->alias("files/not-downloaded/second.iisc")["exists"].toBool());
        QVERIFY(!files.container()->alias("files/not-downloaded/second.iisc")["available"].toBool());
    }
    void containerAliasesFollowHelperSynchronizationAndNeverWritePlaceholders() {
        const auto root = createDrive("Alias snapshots"); QVERIFY(!root.isEmpty());
        QVERIFY(SharedStorage::setDefaultContainer(root));
        Helper reader, writer;
        auto *mirror = reader.fileSystem()->container();
        QCOMPARE(mirror->parent(), reader.fileSystem()); QVERIFY(mirror->available());
        const auto input = m_fixture.filePath("alias.png");
        QFile source(input); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(QByteArray::fromHex("89504e470d0a1a0a") + "PNG alias fixture"); source.close();
        const auto result = writer.fileSystem()->publishGeneratedImage(input, "alias.png"); QVERIFY(!result.isEmpty());
        QVERIFY(reader.fileSystem()->synchronize());
        const auto original = mirror->alias("generation-history/alias.png");
        QVERIFY(original["exists"].toBool()); QVERIFY(original["available"].toBool());
        QCOMPARE(original["previewSource"].toUrl().toLocalFile(), result);
        const auto revision = mirror->revision();
        QVERIFY(writer.fileSystem()->removeFile("generation-history", "alias.png"));
        QVERIFY(mirror->alias("generation-history/alias.png")["exists"].toBool()); // Snapshot, no direct disk probe.
        QVERIFY(reader.fileSystem()->synchronize());
        QVERIFY(mirror->alias("generation-history/alias.png").isEmpty()); QVERIFY(mirror->revision() > revision);
        const auto absent = reader.fileSystem()->alias("files", "not-downloaded.iisc");
        QVERIFY(!absent["exists"].toBool()); QVERIFY(!absent["available"].toBool());
        QVERIFY(!QFileInfo::exists(root + "/Files/not-downloaded.iisc"));
        QVERIFY(!reader.fileSystem()->open(m_fixture.filePath("not-a-drive")));
        QVERIFY(!mirror->available()); QVERIFY(mirror->aliases().isEmpty());
    }
    void helperOwnsCanvasSavePreviewAndVersionConflict() {
        using namespace iiSharedCanvas;
        const auto root = createDrive("Native alias"); QVERIFY(!root.isEmpty());
        QVERIFY(SharedStorage::setDefaultContainer(root));
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
        const auto first = files.saveCanvas(encode(document), destination); QVERIFY2(!first.isEmpty(), qPrintable(files.errorString()));
        QVERIFY(first["available"].toBool()); QVERIFY(!first["version"].toString().isEmpty());
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        const auto storedPreview = iiFileProvider::native::Presentation::load(std::filesystem::path(destination.toStdU16String()));
        QVERIFY(storedPreview); QCOMPARE(storedPreview->preview.kind, std::string("image"));
        const auto png = QImage::fromData(reinterpret_cast<const uchar *>(storedPreview->preview.bytes.data()), qsizetype(storedPreview->preview.bytes.size()));
        QVERIFY(!png.isNull()); QCOMPARE(png.pixel(0, 0), QRgb(0xff204080U));
        const auto recent=files.container()->recentFiles();
        QCOMPARE(recent.size(), 1);
        QVERIFY(files.container()->alias("files/document.iisc.lock").isEmpty());
        QVERIFY(files.container()->alias("files/.document.iisc.lock").isEmpty());
        const auto preview = files.preview(destination); QVERIFY(preview.isLocalFile());
        QVERIFY(preview.toLocalFile().startsWith(root + "/.society-runtime/"));
        const auto bitmap = importBitmap(preview.toLocalFile().toStdString()); QVERIFY(bitmap.ok());
        QCOMPARE(bitmap.asset.pixels.pixels[0], 0xff204080U);
        std::get<RasterAsset>(document.assets[0]).pixels.pixels[0] = 0xff804020U;
        const auto second = files.saveCanvas(encode(document), destination, first["version"].toString()); QVERIFY(!second.isEmpty());
        QVERIFY(first["version"] != second["version"]);
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(20)));
        const auto revisedPreview = iiFileProvider::native::Presentation::load(std::filesystem::path(destination.toStdU16String()));
        QVERIFY(revisedPreview); QVERIFY(revisedPreview->preview.bytes != storedPreview->preview.bytes);
        QVERIFY(files.preview(destination) != preview);
        const auto reopened = files.readCanvas(destination); QVERIFY(reopened.ok());
#if __has_include(<iiSharedCanvas/Metadata/PublicationMetadata.h>)
        QVERIFY(reopened.document.publicationMetadata==publication);
#endif
        QCOMPARE(std::get<RasterAsset>(reopened.document.assets[0]).pixels.pixels[0], 0xff804020U);
        QVERIFY(files.saveCanvas(encode(document), destination, first["version"].toString()).isEmpty());
        QVERIFY(files.errorString().contains("changed"));
        QVERIFY(files.saveCanvas(encode(document), root + "/Generation History/document.iisc").isEmpty());
        QVERIFY(!QFileInfo::exists(root + "/Generation History/document.iisc"));
        QVERIFY(files.saveCanvas(encode(document), destination).isEmpty()); // Existing document is preserved.
    }
    void deferredStorageDoesNotDiscoverTheDefaultDrive() {
        const auto root=createDrive("Deferred storage");QVERIFY(!root.isEmpty());QVERIFY(SharedStorage::setDefaultContainer(root));
        FileSystem deferred(FileSystem::Initialization::Deferred);
        QVERIFY(!deferred.isAvailable());QVERIFY(deferred.rootPath().isEmpty());QVERIFY(deferred.container()->aliases().isEmpty());
        QVERIFY(deferred.containerState().isEmpty());QVERIFY(deferred.errorString().isEmpty());
        FileSystem discovered;QVERIFY(discovered.isAvailable());QCOMPARE(discovered.rootPath(),root);
        QVERIFY(deferred.open(root));QVERIFY(deferred.isAvailable());QCOMPARE(deferred.rootPath(),root);
    }
    void preparedStorageSurvivesItsWorkerAndDoesNotRescanOnAdoption() {
        const auto root=createDrive("Prepared storage");QVERIFY(!root.isEmpty());
        QVERIFY(createText(root+"/Files/captured.txt","captured"));
        std::promise<PreparedStorage> prepared;
        auto future=prepared.get_future();
        auto *worker=QThread::create([root,&prepared] {
            FileSystem files(FileSystem::Initialization::Deferred);
            if(!files.open(root)){prepared.set_value({});return;}
            prepared.set_value(files.preparedStorage());
        });
        worker->start();QVERIFY(worker->wait(10000));delete worker;
        const auto state=future.get();QVERIFY(state.isValid());
        // The source owner is gone. A newly added file must not be discovered
        // until an explicit/asynchronous refresh of the transferred selection.
        QVERIFY(createText(root+"/Files/later.txt","later"));
        FileSystem adopted(FileSystem::Initialization::Deferred);QSignalSpy changed(&adopted,&FileSystem::storageChanged);
        QVERIFY(adopted.adoptStorage(state));QCOMPARE(changed.count(),1);
        QCOMPARE(adopted.rootPath(),root);QCOMPARE(adopted.container()->aliases().size(),1);
        QVERIFY(adopted.container()->alias("files/captured.txt")["available"].toBool());
        QVERIFY(adopted.container()->alias("files/later.txt").isEmpty());
        QVERIFY(adopted.synchronize());QCOMPARE(adopted.container()->aliases().size(),2);
        FileSystem independent(FileSystem::Initialization::Deferred);QVERIFY(independent.adoptStorage(state));QCOMPARE(independent.container()->aliases().size(),1);
    }
    void invalidPreparedStoragePreservesTheSelectedDrive() {
        const auto root=createDrive("Keep selected storage");QVERIFY(!root.isEmpty());
        FileSystem files;QVERIFY(files.open(root));const auto id=files.containerId();
        QVERIFY(!files.adoptStorage({}));QCOMPARE(files.rootPath(),root);QCOMPARE(files.containerId(),id);
        QVERIFY(!files.errorString().isEmpty());
        const auto captured=files.preparedStorage();
        const auto moved=root+".previous";QVERIFY(QDir().rename(root,moved));QVERIFY(QDir().mkdir(root));
        QVERIFY(SocietyDrive::create(root));QVERIFY(!files.adoptStorage(captured));
        QCOMPARE(files.container()->containerId(),id);
    }
    void generatedPngIsPublishedToThePinnedDriveWithoutRunningSociety() {
        const auto root = createDrive("Generated PNG"); QVERIFY(!root.isEmpty());
        QVERIFY(SharedStorage::setDefaultContainer(root));
        Helper helper; auto *storage = helper.fileSystem(); QVERIFY(!helper.isRunning());
        const auto bytes = QByteArray::fromHex("89504e470d0a1a0a") + "unchanged generated PNG";
        const auto source = m_fixture.filePath("generated.png");
        QFile input(source); QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write(bytes), bytes.size()); input.close();
        const auto path = storage->publishGeneratedImage(source, "result-0001.png");
        QCOMPARE(path, root + "/Generation History/result-0001.png"); QVERIFY(storage->errorString().isEmpty());
        FileSystem reader; QCOMPARE(reader.rootPath(), root);
        const auto rows = reader.entries("generation-history"); QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value("path").toString(), path);
        QFile saved(path); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), bytes); saved.close();
        const auto other = createDrive("Other PNG Drive"); QVERIFY(!other.isEmpty());
        QVERIFY(SharedStorage::setDefaultContainer(other));
        QCOMPARE(storage->publishGeneratedImage(source, "result-0002.png"), root + "/Generation History/result-0002.png");
        QVERIFY(!QFileInfo::exists(other + "/Generation History/result-0002.png"));
        QVERIFY(storage->publishGeneratedImage(source, "result-0001.png").isEmpty()); QVERIFY(!storage->errorString().isEmpty());
        QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), bytes);
        QVERIFY(storage->publishGeneratedImage(source, "result.iisc").isEmpty());
        QVERIFY(storage->refresh()); QCOMPARE(storage->rootPath(), other);
        QCOMPARE(storage->publishGeneratedImage(source, "result-0001.png"), other + "/Generation History/result-0001.png");
        QVERIFY(storage->errorString().isEmpty());
    }
    void defaultObservationBelongsToTheSelectedPackageAndLookupIsReadOnly()
    {
        const auto root = createDrive("Runtime Scope"); QVERIFY(!root.isEmpty());
        QVERIFY(SharedStorage::setDefaultContainer(root));
        const auto before = qgetenv("SOCIETY_HELPER_DIRECTORY");
        qunsetenv("SOCIETY_HELPER_DIRECTORY");
        const auto directory = Helper::defaultDirectory();
        QCOMPARE(directory, QDir(root).filePath(".society-runtime/helpers/v1"));
        QVERIFY(!QFileInfo::exists(directory));
        if (!before.isNull()) qputenv("SOCIETY_HELPER_DIRECTORY", before);
    }
    void init()
    {
        QVERIFY(m_fixture.isValid());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        const auto settings = m_fixture.filePath(QString(QTest::currentTestFunction()) + ".json");
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settings.toUtf8());
    }

    void modelsTrackOwnerChangesWithoutRestart()
    {
        const auto root = createDrive("Model Inventory"); QVERIFY(!root.isEmpty());
        QVERIFY(SharedStorage::setDefaultContainer(root));
        const auto drive = SocietyDrive::open(root); QVERIFY(drive);
        StorageMap map(*drive);
        QVERIFY(map.publish({QJsonObject{{"path", "models/Checkpoint/deleted.safetensors"},
            {"kind", "file"}, {"size", "7"}, {"version", QString(64, 'a')}}}));
        QFile primary(root + "/.society-sync/primary.json"); QVERIFY(primary.open(QIODevice::WriteOnly));
        primary.write(QJsonDocument(QJsonObject{{"schema", 1}, {"container", drive->identifier()},
            {"scope", QString(64, 'a')}, {"host", "test-host"}}).toJson()); primary.close();
        Helper helper; auto &files = *helper.fileSystem();
        QVERIFY(files.models().isEmpty()); QVERIFY(files.errorString().isEmpty());
        QVERIFY(QDir().mkpath(root + "/Models/Checkpoint"));
        QFile model(root + "/Models/Checkpoint/new.safetensors"); QVERIFY(model.open(QIODevice::WriteOnly));
        model.write("new weights"); model.close();
        const auto inventory = files.models(); QCOMPARE(inventory.size(), 1);
        QCOMPARE(inventory.first().id, "Checkpoint/new.safetensors"); QVERIFY(inventory.first().available);
        QVERIFY(model.rename(root + "/Deleted/new.safetensors"));
        QVERIFY(files.models().isEmpty()); QVERIFY(files.errorString().isEmpty());
        QVERIFY(!files.open(m_fixture.filePath("absent")));
        QVERIFY(files.models().isEmpty()); QVERIFY(!files.errorString().isEmpty());
    }

    void nativeIoAcrossIndependentApplications()
    {
        const auto root = createDrive("Native IO");
        QVERIFY(!root.isEmpty());
        QVERIFY(SharedStorage::setDefaultContainer(root));
        Helper helper;
        auto *storage = helper.fileSystem();
        QVERIFY(!helper.isRunning()); // File access needs neither presence nor a daemon.
        QCOMPARE(helper.property("fileSystem").value<FileSystem *>(), storage);
        QCOMPARE(storage->parent(), &helper);
        QVERIFY(storage->isAvailable());
        QCOMPARE(storage->rootPath(), root);
        QCOMPARE(storage->sections().size(), 9);
        QCOMPARE(storage->path("photos"), root + "/Photos");
        for (const auto &mode : {"write", "verify"}) {
            QProcess peer;
            peer.start(QStringLiteral(HELPER_FILESYSTEM_PEER), {mode});
            QVERIFY(peer.waitForStarted());
            QVERIFY(peer.waitForFinished(10000));
            QCOMPARE(peer.exitStatus(), QProcess::NormalExit);
            QVERIFY2(peer.exitCode() == 0, peer.readAllStandardError().constData());
            const auto result = QJsonDocument::fromJson(peer.readAllStandardOutput()).object();
            QCOMPARE(result.value("containerId").toString(), storage->containerId());
            QCOMPARE(result.value("rootPath").toString(), root);
            QCOMPARE(result.value("sectionCount").toInt(), 9);
            if (QString(mode) != "write") continue;
            for (const auto &value : storage->sections()) {
                const auto section = value.toMap();
                const auto key = section.value("key").toString();
                const auto relative = QStringLiteral("Helper IO/한글/shared #%.bin");
                const auto path = storage->path(key, relative);
                QCOMPARE(storage->url(key, relative).toLocalFile(), path);
                QVERIFY(storage->url(key, relative).isLocalFile());
                QCOMPARE(storage->path(key), section.value("path").toString());
                QFile file(path);
                QVERIFY(file.open(QIODevice::ReadOnly));
                QCOMPARE(file.readAll(), QByteArray("from-peer:") + key.toUtf8());
                file.close();
                QSaveFile output(path);
                QVERIFY(output.open(QIODevice::WriteOnly));
                const auto bytes = QByteArray("from-app:") + key.toUtf8();
                QCOMPARE(output.write(bytes), bytes.size());
                QVERIFY(output.commit());
            }
        }
        const auto original = storage->path("files", "Helper IO/한글/shared #%.bin");
        const auto renamed = storage->path("files", "Helper IO/한글/renamed.bin");
        QVERIFY(QFile::rename(original, renamed));
        const auto listed = storage->entries("files", "Helper IO/한글");
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed.first().toMap().value("name").toString(), "renamed.bin");
        QCOMPARE(listed.first().toMap().value("path").toString(), renamed);
        QVERIFY(!listed.first().toMap().value("isDirectory").toBool());
        QVERIFY(storage->entries("files", "../Models").isEmpty());
        QVERIFY(!storage->errorString().isEmpty());
        QVERIFY(QDir(QFileInfo(renamed).absolutePath()).entryList(QDir::Files).contains("renamed.bin"));
        QVERIFY(QFile::remove(renamed));
        QVERIFY(!QFileInfo::exists(renamed));
        QString invoked;
        QVERIFY(QMetaObject::invokeMethod(storage, "path", Q_RETURN_ARG(QString, invoked), Q_ARG(QString, "models")));
        QCOMPARE(invoked, root + "/Models");
    }

    void missingStorageRecoversAndDoesNotInterruptObservation()
    {
        Helper helper;
        auto *storage = helper.fileSystem();
        QVERIFY(!storage->isAvailable());
        QVERIFY(!storage->errorString().isEmpty());
        QVERIFY(storage->path("files").isEmpty());
        QVERIFY(helper.start({"com.iisacc.filesystem.test", "Test", "1"},
                             {m_fixture.filePath("presence"), 50, 400}));
        QVERIFY(helper.errorString().isEmpty());
        QSignalSpy changes(storage, &FileSystem::storageChanged);
        const auto root = createDrive("Late Storage");
        QVERIFY(SharedStorage::setDefaultContainer(root));
        QCOMPARE(storage->path("files"), root + "/Files"); // Retry a previously unavailable default.
        QVERIFY(storage->isAvailable());
        QVERIFY(storage->errorString().isEmpty());
        QVERIFY(!changes.isEmpty());
        helper.stop();
        QCOMPARE(storage->path("models"), root + "/Models");
    }

    void qmlContextUsesTheSameNativePaths()
    {
        const auto root = createDrive("QML Source");
        QVERIFY(SharedStorage::setDefaultContainer(root));
        Helper helper;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("societyHelper", &helper);
        QQmlExpression available(engine.rootContext(), nullptr, "societyHelper.fileSystem.available");
        QVERIFY(available.evaluate().toBool());
        QVERIFY2(!available.hasError(), qPrintable(available.error().toString()));
        QQmlExpression path(engine.rootContext(), nullptr, "societyHelper.fileSystem.path('models')");
        QCOMPARE(path.evaluate().toString(), root + "/Models");
        QVERIFY2(!path.hasError(), qPrintable(path.error().toString()));
        QQmlExpression url(engine.rootContext(), nullptr, "societyHelper.fileSystem.url('files', '한글 #%.png')");
        QCOMPARE(url.evaluate().toUrl().toLocalFile(), root + "/Files/한글 #%.png");
        QVERIFY2(!url.hasError(), qPrintable(url.error().toString()));
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
                QVERIFY(SharedStorage::setDefaultContainer(root));
            }
            QProcess diagnostic;
            diagnostic.setProcessEnvironment(environment);
            diagnostic.start(QStringLiteral(HELPER_DIAGNOSTIC_EXECUTABLE), {"--filesystem"});
            QVERIFY(diagnostic.waitForStarted());
            QVERIFY(diagnostic.waitForFinished(10000));
            QCOMPARE(diagnostic.exitStatus(), QProcess::NormalExit);
            QCOMPARE(diagnostic.exitCode(), configured ? 0 : 1);
            const auto result = QJsonDocument::fromJson(diagnostic.readAllStandardOutput()).object();
            QCOMPARE(result.value("available").toBool(), configured);
            QCOMPARE(result.value("rootPath").toString(), root);
            QCOMPARE(result.value("error").toString().isEmpty(), configured);
            QVERIFY(!QFileInfo::exists(presence));
        }
    }

    void selectionIsPinnedUntilExplicitRefresh()
    {
        const auto first = createDrive("First"), second = createDrive("Second");
        QVERIFY(SharedStorage::setDefaultContainer(first));
        Helper helper;
        auto *storage = helper.fileSystem();
        const auto firstId = storage->containerId();
        QVERIFY(SharedStorage::setDefaultContainer(second));
        QCOMPARE(storage->rootPath(), first);
        QCOMPARE(storage->containerId(), firstId);
        QVERIFY(storage->refresh());
        QCOMPARE(storage->rootPath(), second);
        QVERIFY(storage->open(first));
        qputenv("SOCIETY_CONTAINER_PATH", second.toUtf8());
        QVERIFY(storage->refresh());
        QCOMPARE(storage->rootPath(), first); // Explicit selection wins over env/settings.
        QVERIFY(storage->open());
        QCOMPARE(storage->rootPath(), second);
        QVERIFY(!storage->open(m_fixture.filePath("Missing")));
        QVERIFY(storage->rootPath().isEmpty());
        QVERIFY(storage->path("files").isEmpty()); // No fallback to another drive.
        QVERIFY(storage->open());
        QCOMPARE(storage->rootPath(), second);
    }

    void errorsRejectInvalidPathsAndReplacedDrives()
    {
        const auto root = createDrive("Boundaries");
        QVERIFY(SharedStorage::setDefaultContainer(root));
        Helper helper;
        auto *storage = helper.fileSystem();
        QSignalSpy errors(storage, &FileSystem::errorChanged);
        QVERIFY(storage->path("unknown").isEmpty());
        QVERIFY(!storage->errorString().isEmpty());
        QVERIFY(storage->url("models", "../Files/escape").isEmpty());
        QVERIFY(storage->ensureDirectory("models", "../Files/escape").isEmpty());
        QVERIFY(!errors.isEmpty());
        QCOMPARE(storage->path("models"), root + "/Models");
        QVERIFY(storage->errorString().isEmpty());
        const auto oldId = storage->containerId();
        QVERIFY(QFile::remove(root + "/.society-drive.json"));
        QVERIFY(SocietyDrive::create(root));
        QVERIFY(SharedStorage::setDefaultContainer(root));
        QVERIFY(!storage->isAvailable());
        QVERIFY(storage->rootPath().isEmpty());
        QVERIFY(storage->sections().isEmpty());
        QVERIFY(storage->path("models", "new.bin").isEmpty());
        QVERIFY(!storage->errorString().isEmpty());
        QVERIFY(storage->refresh());
        QVERIFY(storage->containerId() != oldId);
    }
};

QTEST_GUILESS_MAIN(FileSystemTests)
#include "filesystem.moc"
