#include <iiSocietyHelper.h>
#include <SharedStorage.h>
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

using namespace iiSocietyHelper;
using namespace iiSocietyContainer;

class FileSystemTests : public QObject
{
    Q_OBJECT
    QTemporaryDir m_fixture{QStringLiteral(HELPER_TEST_DIRECTORY "/filesystem-XXXXXX")};
    QString createDrive(const QString &name)
    {
        const auto path = m_fixture.filePath(name);
        if (!QDir().mkdir(path) || !SocietyDrive::create(path)) return {};
        return path;
    }
private slots:
    void init()
    {
        QVERIFY(m_fixture.isValid());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        const auto settings = m_fixture.filePath(QString(QTest::currentTestFunction()) + ".json");
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settings.toUtf8());
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
        QCOMPARE(storage->sections().size(), 8);
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
            QCOMPARE(result.value("sectionCount").toInt(), 8);
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
