#include <iiSocietyHelper.h>
#include <ApplicationStorage.h>
#include <iiSocietyHelper/Sync/Controller.h>
#include <LanPeer.h>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>
#include <QJsonArray>

class ApplicationStorageTests final : public QObject {
    Q_OBJECT
private slots:
    void differentProductsUseTheSameSuiteStorageWithoutAnExplicitContainer() {
        QTemporaryDir isolated(HELPER_TEST_DIRECTORY "/suite-helper-XXXXXX"); QVERIFY(isolated.isValid());
        const auto selected = qgetenv("SOCIETY_CONTAINER_PATH"), settings = qgetenv("SOCIETY_STORAGE_SETTINGS_PATH"), suite = qgetenv("IISACC_SHARED_STORAGE_ROOT");
        const auto restore = qScopeGuard([&] {
            selected.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", selected);
            settings.isNull() ? qunsetenv("SOCIETY_STORAGE_SETTINGS_PATH") : qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settings);
            suite.isNull() ? qunsetenv("IISACC_SHARED_STORAGE_ROOT") : qputenv("IISACC_SHARED_STORAGE_ROOT", suite);
        });
        qunsetenv("SOCIETY_CONTAINER_PATH"); qputenv("SOCIETY_STORAGE_SETTINGS_PATH", isolated.filePath("selection.json").toUtf8());
        qputenv("IISACC_SHARED_STORAGE_ROOT", isolated.filePath("suite").toUtf8());
        iiSocietyHelper::FileSystem first(iiSocietyHelper::FileSystem::Initialization::Deferred);
        QVERIFY2(first.openForApplication("com.iisacc.dreamscapes"), qPrintable(first.errorString()));
        QFile model(first.path("models", "shared.safetensors")); QVERIFY(model.open(QIODevice::WriteOnly));
        QCOMPARE(model.write("one shared original"), 19); model.close();
        iiSocietyHelper::FileSystem second(iiSocietyHelper::FileSystem::Initialization::Deferred);
        QVERIFY2(second.openForApplication("com.iisacc.another"), qPrintable(second.errorString()));
        QCOMPARE(second.rootPath(), first.rootPath()); QCOMPARE(second.containerId(), first.containerId());
        QCOMPARE(second.path("models", "shared.safetensors"), model.fileName());
        QCOMPARE(second.models().size(), 1);
        QVERIFY(!second.openForApplication("../invalid"));
    }
    void modelsAndGeneratedImagesWorkWithoutASocietyProcessAndSurviveAdoption() {
        QTemporaryDir group(HELPER_TEST_DIRECTORY "/logical-helper-XXXXXX"); QString error;
        const auto storage = iiSocietyContainer::ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error);
        QVERIFY2(storage, qPrintable(error)); const auto selected = qgetenv("SOCIETY_CONTAINER_PATH");
        qputenv("SOCIETY_CONTAINER_PATH", storage->drive().rootPath().toUtf8());
        const auto restore = qScopeGuard([&] { if (selected.isNull()) qunsetenv("SOCIETY_CONTAINER_PATH"); else qputenv("SOCIETY_CONTAINER_PATH", selected); });
        iiSocietyHelper::FileSystem files(iiSocietyHelper::FileSystem::Initialization::Deferred);
        QVERIFY2(files.openForApplication("com.iisacc.dreamscapes"), qPrintable(files.errorString()));
        const auto root = files.rootPath(), identifier = files.containerId();
        QFile model(files.path("models", "downloaded.safetensors")); QVERIFY(model.open(QIODevice::WriteOnly));
        const QByteArray weights(4096, 'm'); QCOMPARE(model.write(weights), weights.size()); model.close();
        const auto models = files.models(); QCOMPARE(models.size(), 1); QVERIFY(models.first().available);
        QImage image(4, 4, QImage::Format_RGBA8888); image.fill(Qt::red);
        const auto original = group.filePath("generated-original.png"); QVERIFY(image.save(original));
        const auto published = files.publishGeneratedImage(original, "generated.png"); QVERIFY2(!published.isEmpty(), qPrintable(files.errorString()));
        const auto alias = files.alias("generation-history", "generated.png"); QVERIFY(alias.value("available").toBool());
        const auto journal = files.fileJournal(); QVERIFY(!journal.isEmpty());
        QVERIFY2(iiSocietyContainer::ApplicationStorage::adoptInGroup(group.path(), &error), qPrintable(error));
        QVERIFY(files.refresh()); QCOMPARE(files.rootPath(), root); QCOMPARE(files.containerId(), identifier);
        QCOMPARE(files.fileJournal(), journal); QCOMPARE(files.models().first().id, models.first().id);
        QFile after(published); QVERIFY(after.open(QIODevice::ReadOnly)); QFile before(original); QVERIFY(before.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), before.readAll()); QVERIFY(!QFileInfo::exists(group.filePath("Society.societycontainer")));
    }
    void anUnavailableSuiteRootDoesNotCreatePrivateFallbackStorage() {
        QTemporaryDir isolated(HELPER_TEST_DIRECTORY "/logical-unentitled-XXXXXX");
        const auto selected = qgetenv("SOCIETY_CONTAINER_PATH"), settings = qgetenv("SOCIETY_STORAGE_SETTINGS_PATH"), suite = qgetenv("IISACC_SHARED_STORAGE_ROOT");
        qunsetenv("SOCIETY_CONTAINER_PATH"); qputenv("SOCIETY_STORAGE_SETTINGS_PATH", isolated.filePath("missing.json").toUtf8());
        qputenv("IISACC_SHARED_STORAGE_ROOT", "invalid-relative-suite-root");
        const auto restore = qScopeGuard([&] {
            if (selected.isNull()) qunsetenv("SOCIETY_CONTAINER_PATH"); else qputenv("SOCIETY_CONTAINER_PATH", selected);
            if (settings.isNull()) qunsetenv("SOCIETY_STORAGE_SETTINGS_PATH"); else qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settings);
            if (suite.isNull()) qunsetenv("IISACC_SHARED_STORAGE_ROOT"); else qputenv("IISACC_SHARED_STORAGE_ROOT", suite);
        });
        iiSocietyHelper::FileSystem files(iiSocietyHelper::FileSystem::Initialization::Deferred);
        QVERIFY(!files.openForApplication("com.iisacc.dreamscapes")); QVERIFY(!files.errorString().isEmpty()); QVERIFY(files.rootPath().isEmpty());
        QVERIFY(QDir(isolated.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    }
    void logicalFilesMigrateToAnAuthenticatedHostAndModelsDownloadOverTls() {
        using namespace iiSocietyHelper::Sync;
        QTemporaryDir group(HELPER_TEST_DIRECTORY "/logical-tls-XXXXXX"), hostDirectory(HELPER_TEST_DIRECTORY "/logical-host-XXXXXX"); QString error;
        const auto local = iiSocietyContainer::ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); QVERIFY2(local, qPrintable(error));
        const auto desktop = iiSocietyContainer::SocietyDrive::createAt(hostDirectory.path(), &error); QVERIFY2(desktop, qPrintable(error));
        const auto root = local->drive().rootPath(), physicalId = local->drive().storageIdentifier();
        const QByteArray image("created before Society installation"), model(200123, 'm');
        QFile imageFile(local->filePath(iiSocietyContainer::StoreSection::GenerationHistory, "before-society.png"));
        QVERIFY(imageFile.open(QIODevice::WriteOnly)); QCOMPARE(imageFile.write(image), image.size()); imageFile.close();
        QVERIFY2(local->recordFile(iiSocietyContainer::StoreSection::GenerationHistory, "before-society.png", &error), qPrintable(error));
        const auto history = local->fileTracking(iiSocietyContainer::StoreSection::GenerationHistory, "before-society.png", &error);
        QFile modelFile(QDir(desktop->rootPath()).filePath("Models/download.safetensors"));
        QVERIFY(modelFile.open(QIODevice::WriteOnly)); QCOMPARE(modelFile.write(model), model.size()); modelFile.close();
        const auto localCollision = QDir(root).filePath("Generation History/collision.png");
        QFile localOriginal(localCollision), hostOriginal(QDir(desktop->rootPath()).filePath("Generation History/collision.png"));
        QVERIFY(localOriginal.open(QIODevice::WriteOnly)); QCOMPARE(localOriginal.write("local before Society"), 20); localOriginal.close();
        QVERIFY(hostOriginal.open(QIODevice::WriteOnly)); QCOMPARE(hostOriginal.write("existing host original"), 22); hostOriginal.close();
        iiWebServer::LanPeer hostPeer, clientPeer;
        Controller hosting({}), syncing([&](const auto &peer, const auto &wire) { return clientPeer.request(peer, wire); });
        syncing.setContentPolicy(Synchronizer::ContentPolicy::FullReplica);
        const QString scope(64, 'a'); hosting.open(desktop->rootPath(), scope); syncing.open(root, scope);
        QTRY_VERIFY2_WITH_TIMEOUT(hosting.available() && syncing.available(),
            qPrintable(hosting.errorString() + " / " + syncing.errorString()), 30000);
        hosting.setPeers({"dreamscapes"}, {});
        QVERIFY(hostPeer.startHost("society-host", "Host", [&](const auto &peer, const auto &wire) {
            return wire.value("op") == "society.sync" ? hosting.handle(peer, wire)
                : QJsonObject{{"ok", true}, {"entries", QJsonArray()}};
        }, {"127.0.0.1"}, iiWebServer::Address::loopback()));
        QVERIFY2(clientPeer.join(hostPeer.createOffer(), "dreamscapes", "Dreamscapes"), qPrintable(clientPeer.errorString()));
        QTRY_VERIFY2_WITH_TIMEOUT(clientPeer.connected(), qPrintable(clientPeer.errorString()), 30000);
        connect(&clientPeer, &iiWebServer::LanPeer::completed, &syncing, [&](auto id, auto result, auto) { syncing.receive(id, result); });
        QSignalSpy completed(&syncing, &Controller::synchronized); syncing.setPeers({"society-host"}, {"society-host"});
        QTRY_VERIFY2_WITH_TIMEOUT(!completed.isEmpty(), qPrintable(syncing.errorString()), 60000);
        const auto uploaded = QDir(desktop->rootPath()).filePath("Generation History/before-society.png");
        const auto downloaded = QDir(root).filePath("Models/download.safetensors");
        QTRY_VERIFY2_WITH_TIMEOUT(QFileInfo::exists(uploaded) && QFileInfo::exists(downloaded), qPrintable(syncing.errorString()), 60000);
        QFile uploadedFile(uploaded), downloadedFile(downloaded); QVERIFY(uploadedFile.open(QIODevice::ReadOnly)); QVERIFY(downloadedFile.open(QIODevice::ReadOnly));
        QCOMPARE(uploadedFile.readAll(), image); QCOMPARE(downloadedFile.readAll(), model);
        // The first completion may precede the follow-up proposal round. Wait
        // for this path's resolved state rather than unrelated transferred files.
        QTRY_COMPARE_WITH_TIMEOUT(QDir(root + "/Generation History").entryList({"collision.png.sync-conflict-*"}, QDir::Files).size(), 1, 60000);
        const auto conflicts = QDir(root + "/Generation History").entryList({"collision.png.sync-conflict-*"}, QDir::Files);
        QCOMPARE(conflicts.size(), 1); QFile preserved(QDir(root + "/Generation History").filePath(conflicts.first()));
        QVERIFY(preserved.open(QIODevice::ReadOnly)); QCOMPARE(preserved.readAll(), QByteArray("local before Society"));
        QVERIFY(localOriginal.open(QIODevice::ReadOnly)); QCOMPARE(localOriginal.readAll(), QByteArray("existing host original"));
        syncing.closeAndWait(); hosting.closeAndWait(); clientPeer.stop(); hostPeer.stop();
        const auto adopted = iiSocietyContainer::ApplicationStorage::adoptInGroup(group.path(), &error); QVERIFY2(adopted, qPrintable(error));
        QCOMPARE(adopted->drive().rootPath(), root); QCOMPARE(adopted->drive().storageIdentifier(), physicalId);
        QCOMPARE(adopted->drive().identifier(), desktop->identifier());
        const auto after = adopted->fileTracking(iiSocietyContainer::StoreSection::GenerationHistory, "before-society.png", &error);
        QCOMPARE(after.value("objectKey"), history.value("objectKey"));
        QVERIFY2(!after.isEmpty(), qPrintable(error));
    }
};
QTEST_GUILESS_MAIN(ApplicationStorageTests)
#include "application_storage.moc"
