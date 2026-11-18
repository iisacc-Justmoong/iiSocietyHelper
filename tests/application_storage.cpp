#include "CTestSupport.h"
#include "QtEventWait.h"
#include "SignalRecorder.h"
#include <QCoreApplication>
#include "TestWorkspace.h"
#include <iiSocietyHelper.h>
#include <ApplicationStorage.h>
#include <iiSocietyHelper/Sync/Controller.h>
#include <LanPeer.h>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QJsonArray>

class ApplicationStorageTests final : public QObject {
public:
    void differentProductsUseTheSameSuiteStorageWithoutAnExplicitContainer() {
        QTemporaryDir isolated((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/suite-helper-XXXXXX"))); CTEST_CHECK(isolated.isValid());
        const auto selected = qgetenv("SOCIETY_CONTAINER_PATH"), settings = qgetenv("SOCIETY_STORAGE_SETTINGS_PATH"), suite = qgetenv("IISACC_SHARED_STORAGE_ROOT");
        const auto restore = qScopeGuard([&] {
            selected.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", selected);
            settings.isNull() ? qunsetenv("SOCIETY_STORAGE_SETTINGS_PATH") : qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settings);
            suite.isNull() ? qunsetenv("IISACC_SHARED_STORAGE_ROOT") : qputenv("IISACC_SHARED_STORAGE_ROOT", suite);
        });
        qunsetenv("SOCIETY_CONTAINER_PATH"); qputenv("SOCIETY_STORAGE_SETTINGS_PATH", isolated.filePath("selection.json").toUtf8());
        qputenv("IISACC_SHARED_STORAGE_ROOT", isolated.filePath("suite").toUtf8());
        iiSocietyHelper::FileSystem first(iiSocietyHelper::FileSystem::Initialization::Deferred);
        CTEST_CHECK_MESSAGE(first.openForApplication("com.iisacc.dreamscapes"), qPrintable(first.errorString()));
        QFile model(first.path("models", "shared.safetensors")); CTEST_CHECK(model.open(QIODevice::WriteOnly));
        CTEST_COMPARE(model.write("one shared original"), 19); model.close();
        iiSocietyHelper::FileSystem second(iiSocietyHelper::FileSystem::Initialization::Deferred);
        CTEST_CHECK_MESSAGE(second.openForApplication("com.iisacc.another"), qPrintable(second.errorString()));
        CTEST_COMPARE(second.rootPath(), first.rootPath()); CTEST_COMPARE(second.containerId(), first.containerId());
        CTEST_COMPARE(second.path("models", "shared.safetensors"), model.fileName());
        CTEST_COMPARE(second.models().size(), 1);
        CTEST_CHECK(!second.openForApplication("../invalid"));
    }
    void modelsAndGeneratedImagesWorkWithoutASocietyProcessAndSurviveAdoption() {
        QTemporaryDir group((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/logical-helper-XXXXXX"))); QString error;
        const auto storage = iiSocietyContainer::ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error);
        CTEST_CHECK_MESSAGE(storage, qPrintable(error)); const auto selected = qgetenv("SOCIETY_CONTAINER_PATH");
        qputenv("SOCIETY_CONTAINER_PATH", storage->drive().rootPath().toUtf8());
        const auto restore = qScopeGuard([&] { if (selected.isNull()) qunsetenv("SOCIETY_CONTAINER_PATH"); else qputenv("SOCIETY_CONTAINER_PATH", selected); });
        iiSocietyHelper::FileSystem files(iiSocietyHelper::FileSystem::Initialization::Deferred);
        CTEST_CHECK_MESSAGE(files.openForApplication("com.iisacc.dreamscapes"), qPrintable(files.errorString()));
        const auto root = files.rootPath(), identifier = files.containerId();
        QFile model(files.path("models", "downloaded.safetensors")); CTEST_CHECK(model.open(QIODevice::WriteOnly));
        const QByteArray weights(4096, 'm'); CTEST_COMPARE(model.write(weights), weights.size()); model.close();
        const auto models = files.models(); CTEST_COMPARE(models.size(), 1); CTEST_CHECK(models.first().available);
        QImage image(4, 4, QImage::Format_RGBA8888); image.fill(Qt::red);
        const auto original = group.filePath("generated-original.png"); CTEST_CHECK(image.save(original));
        const auto published = files.publishGeneratedImage(original, "generated.png"); CTEST_CHECK_MESSAGE(!published.isEmpty(), qPrintable(files.errorString()));
        const auto alias = files.alias("generation-history", "generated.png"); CTEST_CHECK(alias.value("available").toBool());
        const auto journal = files.fileJournal(); CTEST_CHECK(!journal.isEmpty());
        CTEST_CHECK_MESSAGE(iiSocietyContainer::ApplicationStorage::adoptInGroup(group.path(), &error), qPrintable(error));
        CTEST_CHECK(files.refresh()); CTEST_COMPARE(files.rootPath(), root); CTEST_COMPARE(files.containerId(), identifier);
        CTEST_COMPARE(files.fileJournal(), journal); CTEST_COMPARE(files.models().first().id, models.first().id);
        QFile after(published); CTEST_CHECK(after.open(QIODevice::ReadOnly)); QFile before(original); CTEST_CHECK(before.open(QIODevice::ReadOnly));
        CTEST_COMPARE(after.readAll(), before.readAll()); CTEST_CHECK(!QFileInfo::exists(group.filePath("Society.societycontainer")));
    }
    void anUnavailableSuiteRootDoesNotCreatePrivateFallbackStorage() {
        QTemporaryDir isolated((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/logical-unentitled-XXXXXX")));
        const auto selected = qgetenv("SOCIETY_CONTAINER_PATH"), settings = qgetenv("SOCIETY_STORAGE_SETTINGS_PATH"), suite = qgetenv("IISACC_SHARED_STORAGE_ROOT");
        qunsetenv("SOCIETY_CONTAINER_PATH"); qputenv("SOCIETY_STORAGE_SETTINGS_PATH", isolated.filePath("missing.json").toUtf8());
        qputenv("IISACC_SHARED_STORAGE_ROOT", "invalid-relative-suite-root");
        const auto restore = qScopeGuard([&] {
            if (selected.isNull()) qunsetenv("SOCIETY_CONTAINER_PATH"); else qputenv("SOCIETY_CONTAINER_PATH", selected);
            if (settings.isNull()) qunsetenv("SOCIETY_STORAGE_SETTINGS_PATH"); else qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settings);
            if (suite.isNull()) qunsetenv("IISACC_SHARED_STORAGE_ROOT"); else qputenv("IISACC_SHARED_STORAGE_ROOT", suite);
        });
        iiSocietyHelper::FileSystem files(iiSocietyHelper::FileSystem::Initialization::Deferred);
        CTEST_CHECK(!files.openForApplication("com.iisacc.dreamscapes")); CTEST_CHECK(!files.errorString().isEmpty()); CTEST_CHECK(files.rootPath().isEmpty());
        CTEST_CHECK(QDir(isolated.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    }
    void logicalFilesMigrateToAnAuthenticatedHostAndModelsDownloadOverTls() {
        using namespace iiSocietyHelper::Sync;
        QTemporaryDir group((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/logical-tls-XXXXXX"))), hostDirectory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/logical-host-XXXXXX"))); QString error;
        const auto local = iiSocietyContainer::ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); CTEST_CHECK_MESSAGE(local, qPrintable(error));
        const auto desktop = iiSocietyContainer::SocietyDrive::createAt(hostDirectory.path(), &error); CTEST_CHECK_MESSAGE(desktop, qPrintable(error));
        const auto root = local->drive().rootPath(), physicalId = local->drive().storageIdentifier();
        const QByteArray image("created before Society installation"), model(200123, 'm');
        QFile imageFile(local->filePath(iiSocietyContainer::StoreSection::GenerationHistory, "before-society.png"));
        CTEST_CHECK(imageFile.open(QIODevice::WriteOnly)); CTEST_COMPARE(imageFile.write(image), image.size()); imageFile.close();
        CTEST_CHECK_MESSAGE(local->recordFile(iiSocietyContainer::StoreSection::GenerationHistory, "before-society.png", &error), qPrintable(error));
        const auto history = local->fileTracking(iiSocietyContainer::StoreSection::GenerationHistory, "before-society.png", &error);
        QFile modelFile(QDir(desktop->rootPath()).filePath("Models/download.safetensors"));
        CTEST_CHECK(modelFile.open(QIODevice::WriteOnly)); CTEST_COMPARE(modelFile.write(model), model.size()); modelFile.close();
        const auto localCollision = QDir(root).filePath("Generation History/collision.png");
        QFile localOriginal(localCollision), hostOriginal(QDir(desktop->rootPath()).filePath("Generation History/collision.png"));
        CTEST_CHECK(localOriginal.open(QIODevice::WriteOnly)); CTEST_COMPARE(localOriginal.write("local before Society"), 20); localOriginal.close();
        CTEST_CHECK(hostOriginal.open(QIODevice::WriteOnly)); CTEST_COMPARE(hostOriginal.write("existing host original"), 22); hostOriginal.close();
        iiWebServer::LanPeer hostPeer, clientPeer;
        Controller hosting({}), syncing([&](const auto &peer, const auto &wire) { return clientPeer.request(peer, wire); });
        syncing.setContentPolicy(Synchronizer::ContentPolicy::FullReplica);
        const QString scope(64, 'a'); hosting.open(desktop->rootPath(), scope); syncing.open(root, scope);
        CTEST_WAIT_MESSAGE_TIMEOUT(hosting.available() && syncing.available(),
            qPrintable(hosting.errorString() + " / " + syncing.errorString()), 30000);
        hosting.setPeers({"dreamscapes"}, {});
        CTEST_CHECK(hostPeer.startHost("society-host", "Host", [&](const auto &peer, const auto &wire) {
            return wire.value("op") == "society.sync" ? hosting.handle(peer, wire)
                : QJsonObject{{"ok", true}, {"entries", QJsonArray()}};
        }, {"127.0.0.1"}, iiWebServer::Address::loopback()));
        CTEST_CHECK_MESSAGE(clientPeer.join(hostPeer.createOffer(), "dreamscapes", "Dreamscapes"), qPrintable(clientPeer.errorString()));
        CTEST_WAIT_MESSAGE_TIMEOUT(clientPeer.connected(), qPrintable(clientPeer.errorString()), 30000);
        connect(&clientPeer, &iiWebServer::LanPeer::completed, &syncing, [&](auto id, auto result, auto) { syncing.receive(id, result); });
        helper_ctest::SignalRecorder completed(&syncing, &Controller::synchronized); syncing.setPeers({"society-host"}, {"society-host"});
        CTEST_WAIT_MESSAGE_TIMEOUT(!completed.isEmpty(), qPrintable(syncing.errorString()), 60000);
        const auto uploaded = QDir(desktop->rootPath()).filePath("Generation History/before-society.png");
        const auto downloaded = QDir(root).filePath("Models/download.safetensors");
        CTEST_WAIT_MESSAGE_TIMEOUT(QFileInfo::exists(uploaded) && QFileInfo::exists(downloaded), qPrintable(syncing.errorString()), 60000);
        QFile uploadedFile(uploaded), downloadedFile(downloaded); CTEST_CHECK(uploadedFile.open(QIODevice::ReadOnly)); CTEST_CHECK(downloadedFile.open(QIODevice::ReadOnly));
        CTEST_COMPARE(uploadedFile.readAll(), image); CTEST_COMPARE(downloadedFile.readAll(), model);
        // The first completion may precede the follow-up proposal round. Wait
        // for this path's resolved state rather than unrelated transferred files.
        CTEST_WAIT_COMPARE_TIMEOUT(QDir(root + "/Generation History").entryList({"collision.png.sync-conflict-*"}, QDir::Files).size(), 1, 60000);
        const auto conflicts = QDir(root + "/Generation History").entryList({"collision.png.sync-conflict-*"}, QDir::Files);
        CTEST_COMPARE(conflicts.size(), 1); QFile preserved(QDir(root + "/Generation History").filePath(conflicts.first()));
        CTEST_CHECK(preserved.open(QIODevice::ReadOnly)); CTEST_COMPARE(preserved.readAll(), QByteArray("local before Society"));
        CTEST_CHECK(localOriginal.open(QIODevice::ReadOnly)); CTEST_COMPARE(localOriginal.readAll(), QByteArray("existing host original"));
        syncing.closeAndWait(); hosting.closeAndWait(); clientPeer.stop(); hostPeer.stop();
        const auto adopted = iiSocietyContainer::ApplicationStorage::adoptInGroup(group.path(), &error); CTEST_CHECK_MESSAGE(adopted, qPrintable(error));
        CTEST_COMPARE(adopted->drive().rootPath(), root); CTEST_COMPARE(adopted->drive().storageIdentifier(), physicalId);
        CTEST_COMPARE(adopted->drive().identifier(), desktop->identifier());
        const auto after = adopted->fileTracking(iiSocietyContainer::StoreSection::GenerationHistory, "before-society.png", &error);
        CTEST_COMPARE(after.value("objectKey"), history.value("objectKey"));
        CTEST_CHECK_MESSAGE(!after.isEmpty(), qPrintable(error));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--offline")) {
        std::cerr << "Expected --offline" << std::endl; return 1;
    }
    const bool offline = argc == 2;
    helper_ctest::CTestRunner runner;
    ApplicationStorageTests tests;
    runner.run("differentProductsUseTheSameSuiteStorageWithoutAnExplicitContainer", [&] { tests.differentProductsUseTheSameSuiteStorageWithoutAnExplicitContainer(); });
    if (!offline)
        runner.run("modelsAndGeneratedImagesWorkWithoutASocietyProcessAndSurviveAdoption", [&] { tests.modelsAndGeneratedImagesWorkWithoutASocietyProcessAndSurviveAdoption(); });
    runner.run("anUnavailableSuiteRootDoesNotCreatePrivateFallbackStorage", [&] { tests.anUnavailableSuiteRootDoesNotCreatePrivateFallbackStorage(); });
    if (!offline)
        runner.run("logicalFilesMigrateToAnAuthenticatedHostAndModelsDownloadOverTls", [&] { tests.logicalFilesMigrateToAnAuthenticatedHostAndModelsDownloadOverTls(); });
    return runner.result();
}
