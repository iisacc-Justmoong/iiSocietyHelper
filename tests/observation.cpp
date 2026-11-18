#include "TestWorkspace.h"
#include "iiSocietyHelper.h"
#include "DeliveryStore.h"
#include <iiFileProviderNative.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include "SignalRecorder.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <semaphore>
#include <QTemporaryDir>
#include <QThread>
#include <SharedStorage.h>
#include <QScopeGuard>
#include <QTimer>
#include <QUuid>
#include <algorithm>

using namespace iiSocietyHelper;

class ObservationTests : public QObject
{
public:
    void asynchronousLifecycleKeepsOwnerResponsive() {
        QTemporaryDir directory(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/async-presence-XXXXXX");
        CTEST_CHECK(directory.isValid());
        auto startEntered=std::make_shared<std::atomic_bool>(false), stopEntered=std::make_shared<std::atomic_bool>(false);
        auto startGate=std::make_shared<std::binary_semaphore>(0), stopGate=std::make_shared<std::binary_semaphore>(0);
        bool startReleased=false, stopReleased=false;
        Helper helper(FileSystem::Initialization::Deferred);
        // Release worker gates before Helper destruction, including on failure.
        const auto release=qScopeGuard([&]{if(!startReleased)startGate->release();if(!stopReleased)stopGate->release();});
        helper_ctest::SignalRecorder started(&helper, &Helper::startFinished); helper_ctest::SignalRecorder stopped(&helper, &Helper::stopFinished);
        HelperLifecycleHooks hooks;
        hooks.beforeStartIo=[startEntered,startGate]{*startEntered=true;startGate->acquire();};
        hooks.beforeStopIo=[stopEntered,stopGate]{*stopEntered=true;stopGate->acquire();};
        QElapsedTimer elapsed; elapsed.start();
        CTEST_CHECK(helper.startAsync({"com.iisacc.async", "Async", "1"}, {directory.path(),100,5000}, hooks));
        CTEST_CHECK(elapsed.elapsed()<250); CTEST_WAIT_DEFAULT(startEntered->load());
        bool heartbeat=false; QTimer::singleShot(0,&helper,[&]{heartbeat=true;}); CTEST_WAIT_DEFAULT(heartbeat);
        CTEST_CHECK(helper.isStarting()); CTEST_CHECK(!helper.isRunning());
        startReleased=true;startGate->release();CTEST_WAIT_COMPARE_TIMEOUT(started.size(),1,10000);
        CTEST_CHECK(started.first().first().toBool());CTEST_CHECK(helper.isRunning());
        const auto record=directory.filePath(helper.self().instanceId+".json");CTEST_CHECK(QFileInfo::exists(record));
        elapsed.restart();helper.stopAsync();CTEST_CHECK(elapsed.elapsed()<250);CTEST_WAIT_DEFAULT(stopEntered->load());
        heartbeat=false;QTimer::singleShot(0,&helper,[&]{heartbeat=true;});CTEST_WAIT_DEFAULT(heartbeat);
        CTEST_CHECK(helper.isStopping());CTEST_CHECK(QFileInfo::exists(record));
        stopReleased=true;stopGate->release();CTEST_WAIT_COMPARE_TIMEOUT(stopped.size(),1,10000);
        CTEST_CHECK(!helper.isRunning());CTEST_CHECK(!helper.isStopping());CTEST_CHECK(!QFileInfo::exists(record));
    }
    void asynchronousStartCanBeStoppedBeforeIoCompletes() {
        QTemporaryDir directory(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/async-cancel-XXXXXX");
        CTEST_CHECK(directory.isValid());auto entered=std::make_shared<std::atomic_bool>(false);
        auto gate=std::make_shared<std::binary_semaphore>(0);bool released=false;
        Helper helper(FileSystem::Initialization::Deferred);
        const auto release=qScopeGuard([&]{if(!released)gate->release();});
        helper_ctest::SignalRecorder stopped(&helper,&Helper::stopFinished);
        HelperLifecycleHooks hooks;hooks.beforeStartIo=[entered,gate]{*entered=true;gate->acquire();};
        CTEST_CHECK(helper.startAsync({"com.iisacc.async", "Async", "1"},{directory.path(),100,5000},hooks));
        CTEST_WAIT_DEFAULT(entered->load());helper.stopAsync();CTEST_CHECK(helper.isStopping());
        released=true;gate->release();CTEST_WAIT_COMPARE_TIMEOUT(stopped.size(),1,10000);
        CTEST_CHECK(!helper.isRunning());CTEST_CHECK(!helper.isStarting());CTEST_CHECK(!helper.isStopping());
        CTEST_CHECK(QDir(directory.path()).entryList({"*.json"},QDir::Files).isEmpty());
    }
    void asynchronousStopPreservesSynchronousDeliveryAndAllowsRestart() {
        QTemporaryDir directory(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/async-restart-XXXXXX");
        CTEST_CHECK(directory.isValid());Helper helper(FileSystem::Initialization::Deferred);
        const ObservationOptions options{directory.path(),100,5000};
        CTEST_CHECK(helper.start({"com.iisacc.async", "Async", "1"},options));
        CTEST_CHECK(!helper.sendData("test.saved",{{"value",42}}).isEmpty());
        helper_ctest::SignalRecorder stopped(&helper,&Helper::stopFinished); helper_ctest::SignalRecorder started(&helper,&Helper::startFinished);
        CTEST_CHECK(helper.stopAsync());CTEST_WAIT_COMPARE_TIMEOUT(stopped.size(),1,10000);
        DeliveryStore delivery;QString error;CTEST_CHECK_MESSAGE(delivery.open(directory.path(),&error),qPrintable(error));
        CTEST_CHECK(delivery.pendingCount()>=3);delivery.close();
        CTEST_CHECK(helper.startAsync({"com.iisacc.async", "Async", "2"},options));
        CTEST_WAIT_COMPARE_TIMEOUT(started.size(),1,10000);CTEST_CHECK(started.first().first().toBool());CTEST_CHECK(helper.isRunning());
        CTEST_CHECK(helper.stopAsync());CTEST_WAIT_COMPARE_TIMEOUT(stopped.size(),2,10000);CTEST_CHECK(!helper.isRunning());
    }
    void helperCanDeferStorageDiscovery() {
        QTemporaryDir directory(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY))
            + QStringLiteral("/helper-deferred-XXXXXX.societycontainer"));
        CTEST_CHECK(directory.isValid());
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(directory.path()));
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto restore = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH")
                                                               : qputenv("SOCIETY_CONTAINER_PATH", previous); });
        qputenv("SOCIETY_CONTAINER_PATH", directory.path().toUtf8());
        Helper helper(FileSystem::Initialization::Deferred);
        CTEST_CHECK(!helper.fileSystem()->isAvailable());
        CTEST_CHECK(helper.fileSystem()->containerId().isEmpty());
        const auto presence = directory.filePath(".society-runtime/deferred-test-presence");
        CTEST_CHECK(helper.start({"com.iisacc.deferred", "Deferred", "1"}, {presence, 100, 5000}));
        CTEST_CHECK(!helper.fileSystem()->isAvailable());
        CTEST_CHECK(helper.fileSystem()->open(directory.path()));
        CTEST_CHECK(helper.fileSystem()->isAvailable());
        helper.stop();
    }
    void transientUnreadableRecordDoesNotWithdrawAnObservedPeer()
    {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/presence-record-XXXXXX")));
        Helper observer;
        CTEST_CHECK(observer.start({"com.iisacc.record.observer", "Observer", "1"}, {directory.path(), 50, 5000}));
        helper_ctest::SignalRecorder appeared(&observer, &Helper::peerAppeared);
        helper_ctest::SignalRecorder departed(&observer, &Helper::peerDisappeared);
        const auto instance = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto path = std::filesystem::path(directory.filePath(instance + ".json").toStdU16String());
        QJsonObject record{{"protocol", 1}, {"instanceId", instance},
            {"applicationId", "com.iisacc.record.peer"}, {"name", "Peer"}, {"version", "1"},
            {"processId", QCoreApplication::applicationPid()}, {"activity", "unknown"},
            {"startedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        const auto publish = [&](int sequence) {
            record["heartbeat"] = QString::number(sequence);
            iiFileProvider::native::File::write(path, QJsonDocument(record).toJson(QJsonDocument::Compact).toStdString());
        };
        publish(1); helper_ctest::processFor(200);
        publish(2); CTEST_WAIT_COMPARE(observer.peers().size(), 1);
        CTEST_COMPARE(appeared.size(), 1);
        iiFileProvider::native::File::write(path, std::string_view("{}"));
        helper_ctest::processFor(250);
        CTEST_COMPARE(observer.peers().size(), 1);
        CTEST_COMPARE(departed.size(), 0);
        publish(3); helper_ctest::processFor(200);
        CTEST_COMPARE(observer.peers().size(), 1);
        CTEST_COMPARE(appeared.size(), 1);
        CTEST_CHECK(iiFileProvider::native::File::remove(path));
        CTEST_WAIT_DEFAULT(observer.peers().isEmpty());
        CTEST_COMPARE(departed.size(), 1);
        CTEST_COMPARE(qvariant_cast<DepartureReason>(departed.first()[1]), DepartureReason::Withdrawn);
    }

    void presenceSurvivesAnOpenProviderReadSnapshot()
    {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/presence-snapshot-XXXXXX")));
        CTEST_CHECK(directory.isValid());
        Helper helper;
        helper_ctest::SignalRecorder errors(&helper, &Helper::errorOccurred);
        CTEST_CHECK(helper.start({"com.iisacc.snapshot", "Snapshot", "1"}, {directory.path(), 20, 5000}));
        const auto path = std::filesystem::path(directory.filePath(helper.self().instanceId + ".json").toStdU16String());
        {
            iiFileProvider::native::ReadFile snapshot(path);
            const auto before = QJsonDocument::fromJson(QByteArray::fromStdString(snapshot.read(8192))).object();
            helper_ctest::processFor(500);
            CTEST_CHECK(helper.isRunning());
            CTEST_COMPARE(errors.size(), 0);
            const auto after = QJsonDocument::fromJson(QByteArray::fromStdString(
                iiFileProvider::native::File::read(path, 8192))).object();
            CTEST_CHECK(after.value("heartbeat").toString().toULongLong() > before.value("heartbeat").toString().toULongLong());
        }
        helper.stop();
        CTEST_CHECK(!QFileInfo::exists(QString::fromStdU16String(path.u16string())));
    }

    void presenceResponsiveness()
    {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/responsiveness-XXXXXX")));
        CTEST_CHECK(directory.isValid());
        Helper a, b;
        const ObservationOptions options{directory.path(), 100, 5000};
        CTEST_CHECK(a.start({"com.iisacc.input.probe", "Input probe", "1"}, options));
        CTEST_CHECK(b.start({"com.iisacc.peer.probe", "Peer probe", "1"}, options));
        bool wrongThread = false;
        connect(&a, &Helper::peerAppeared, this, [&] { wrongThread |= QThread::currentThread() != thread(); });
        CTEST_WAIT_COMPARE(a.peers().size(), 1);
        CTEST_WAIT_COMPARE(b.peers().size(), 1);
        helper_ctest::SignalRecorder departedA(&a, &Helper::peerDisappeared);
        helper_ctest::SignalRecorder departedB(&b, &Helper::peerDisappeared);
        QElapsedTimer clock;
        clock.start();
        qint64 previous = 0, maximumGap = 0;
        QList<qint64> gaps;
        QTimer input;
        input.setInterval(5);
        connect(&input, &QTimer::timeout, this, [&] {
            const auto now = clock.nsecsElapsed();
            gaps.append(now - previous);
            maximumGap = std::max(maximumGap, now - previous);
            previous = now;
        });
        input.start();
        helper_ctest::processFor(3000);
        input.stop();
        CTEST_CHECK(!wrongThread);
        CTEST_CHECK(gaps.size() >= 10);
        CTEST_WAIT_COMPARE(a.peers().size(), 1);
        CTEST_COMPARE(departedA.size(), 0);
        CTEST_COMPARE(departedB.size(), 0);
        std::sort(gaps.begin(), gaps.end());
        qInfo().nospace() << "PRESENCE_RESPONSIVENESS {\"ticks\":" << gaps.size()
            << ",\"p95Ms\":" << gaps[gaps.size() * 95 / 100] / 1000000.0
            << ",\"maxMs\":" << maximumGap / 1000000.0 << '}';
    }

    void stoppedWorkCannotRepublishIntoTheNextSession()
    {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/epochs-XXXXXX")));
        Helper helper;
        const ObservationOptions options{directory.path(), 20, 5000};
        CTEST_CHECK(helper.start({"com.iisacc.epochs", "Epochs", "1"}, options));
        const auto old = helper.self().instanceId;
        for (int i = 0; i < 20; ++i)
            helper.setActivity(i % 2 ? Activity::Foreground : Activity::Background);
        helper.stop();
        CTEST_CHECK(!QFileInfo::exists(directory.filePath(old + ".json")));
        CTEST_CHECK(helper.start({"com.iisacc.epochs", "Epochs", "2"}, options));
        CTEST_CHECK(helper.self().instanceId != old);
        helper_ctest::processFor(200);
        CTEST_CHECK(!QFileInfo::exists(directory.filePath(old + ".json")));
        helper.stop();
        CTEST_CHECK(QDir(directory.path()).entryList({"*.json"}, QDir::Files).isEmpty());
        DeliveryStore outbox;
        CTEST_CHECK(outbox.open(directory.path()));
        // Two starts, two stops and every one of the twenty activity transitions.
        CTEST_COMPARE(outbox.pendingCount(), 24);
    }

    void mutualDiscoveryAndDistinctInstances()
    {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/presence-XXXXXX")));
        CTEST_CHECK(directory.isValid());
        // Keep the production expiry window: shared-volume flushes can exceed 400 ms.
        const ObservationOptions options{directory.path(), 100, 5000};
        Helper society, dreamscapes, secondDreamscapes;
        helper_ctest::SignalRecorder appeared(&society, &Helper::peerAppeared);
        helper_ctest::SignalRecorder updated(&society, &Helper::peerUpdated);
        helper_ctest::SignalRecorder departed(&society, &Helper::peerDisappeared);
        CTEST_CHECK(society.start({"com.iisacc.society", "Society", "1"}, options));
        CTEST_CHECK(dreamscapes.start({"com.iisacc.dreamscapes", "Dreamscapes", "2"}, options));
        CTEST_CHECK(secondDreamscapes.start({"com.iisacc.dreamscapes", "Dreamscapes", "2"}, options));
        CTEST_WAIT_COMPARE(society.peers().size(), 2);
        CTEST_WAIT_COMPARE(dreamscapes.peers().size(), 2);
        CTEST_WAIT_COMPARE(secondDreamscapes.peers().size(), 2);
        CTEST_CHECK(dreamscapes.self().instanceId != secondDreamscapes.self().instanceId);
        for (const auto &peer : society.peers()) {
            CTEST_COMPARE(peer.application.id, "com.iisacc.dreamscapes");
            CTEST_CHECK(peer.instanceId != society.self().instanceId);
            CTEST_COMPARE(peer.processId, QCoreApplication::applicationPid());
        }
        helper_ctest::processFor(200);
        CTEST_COMPARE(appeared.size(), 2);
        CTEST_COMPARE(updated.size(), 0); // A heartbeat alone is not a metadata change.
        dreamscapes.setActivity(Activity::Background);
        CTEST_WAIT_COMPARE(updated.size(), 1);
        CTEST_COMPARE(qvariant_cast<Peer>(updated[0][0]).activity, Activity::Background);
        dreamscapes.stop();
        CTEST_WAIT_COMPARE(society.peers().size(), 1);
        CTEST_COMPARE(departed.size(), 1);
        CTEST_COMPARE(qvariant_cast<DepartureReason>(departed[0][1]), DepartureReason::Withdrawn);
        society.stop();
        secondDreamscapes.stop();
        CTEST_CHECK(QDir(directory.path()).entryList({"*.json"}, QDir::Files).isEmpty());
    }

    void staleAndMalformedRecordsNeverBecomePeers()
    {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/stale-XXXXXX")));
        const ObservationOptions options{directory.path(), 50, 400};
        Helper old;
        CTEST_CHECK(old.start({"com.iisacc.old", "Old", "1"}, options));
        const QString oldPath = QDir(directory.path()).filePath(old.self().instanceId + ".json");
        QFile record(oldPath);
        CTEST_CHECK(record.open(QIODevice::ReadOnly));
        const auto bytes = record.readAll();
        record.close();
        old.stop();
        CTEST_CHECK(record.open(QIODevice::WriteOnly));
        CTEST_COMPARE(record.write(bytes), bytes.size());
        record.close();
        QFile garbage(QDir(directory.path()).filePath("garbage.json"));
        CTEST_CHECK(garbage.open(QIODevice::WriteOnly));
        garbage.write("{broken");
        garbage.close();
        Helper observer;
        helper_ctest::SignalRecorder appeared(&observer, &Helper::peerAppeared);
        CTEST_CHECK(observer.start({"com.iisacc.observer", "Observer", "1"}, options));
        helper_ctest::processFor(700);
        CTEST_CHECK(observer.peers().isEmpty());
        CTEST_COMPARE(appeared.size(), 0);
        CTEST_CHECK(QFile::exists(oldPath)); // A peer never deletes another peer's record.
    }

    void invalidConfigurationIsReported()
    {
        Helper helper;
        QString error;
        CTEST_CHECK(!helper.start({"", "Name", "1"}, {}, &error));
        CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(!helper.isRunning());
        CTEST_CHECK(!helper.start({"com.iisacc.test", "Test", "1"}, {"relative/path", 50, 400}, &error));
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/invalid-XXXXXX")));
        CTEST_CHECK(!helper.start({"com.iisacc.test", "Test", "1"}, {directory.path(), 0, 400}, &error));
        CTEST_CHECK(!helper.start({"com.iisacc.test", "Test", "1"}, {directory.path(), 100, 150}, &error));
        CTEST_CHECK(helper.start({"com.iisacc.test", "Test", "1"}, {directory.path(), 50, 400}, &error));
        CTEST_CHECK(error.isEmpty());
        const auto instance = helper.self().instanceId;
        CTEST_CHECK(!helper.start({"com.iisacc.test", "Test", "1"}, {}, &error));
        CTEST_COMPARE(helper.self().instanceId, instance);
        helper.stop();
        CTEST_CHECK(helper.start({"com.iisacc.test", "Test", "1"}, {directory.path(), 50, 400}));
        CTEST_CHECK(helper.self().instanceId != instance);
    }

    void redirectedDirectoryStopsObservation()
    {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/redirect-XXXXXX")));
        const QString root = directory.path() + "/registry";
        Helper helper;
        helper_ctest::SignalRecorder errors(&helper, &Helper::errorOccurred);
        helper_ctest::SignalRecorder stopped(&helper, &Helper::stopFinished);
        CTEST_CHECK(helper.start({"com.iisacc.test", "Test", "1"}, {root, 50, 400}));
#ifdef Q_OS_WIN
        // The native storage backend pins this directory without delete sharing.
        CTEST_CHECK(!QDir().rename(root, root + "-moved"));
        helper_ctest::processFor(200);
        CTEST_CHECK(helper.isRunning());
        CTEST_CHECK(errors.isEmpty());
        helper.stop();
        CTEST_CHECK(QDir().rename(root, root + "-moved"));
#else
        CTEST_CHECK(QDir().rename(root, root + "-moved"));
        CTEST_CHECK(QDir().mkpath(root + "-target"));
#ifdef Q_OS_UNIX
        CTEST_CHECK(QFile::link(root + "-target", root));
#endif
        CTEST_WAIT_DEFAULT(!helper.isRunning());
        CTEST_CHECK(!errors.isEmpty());
        CTEST_CHECK(!helper.errorString().isEmpty());
        CTEST_CHECK(QDir(root + "-target").entryList(QDir::Files).isEmpty());
        CTEST_WAIT_COMPARE_TIMEOUT(stopped.size(), 1, 10000);
#endif
    }

    void independentDirectoriesDoNotMix()
    {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/isolation-XXXXXX")));
        Helper a, b;
        CTEST_CHECK(a.start({"com.iisacc.society", "Society", "1"}, {directory.path() + "/a", 50, 400}));
        CTEST_CHECK(b.start({"com.iisacc.dreamscapes", "Dreamscapes", "1"}, {directory.path() + "/b", 50, 400}));
        helper_ctest::processFor(600);
        CTEST_CHECK(a.peers().isEmpty());
        CTEST_CHECK(b.peers().isEmpty());
        const auto previousHelperDirectory = qgetenv("SOCIETY_HELPER_DIRECTORY");
        const auto restoreHelperDirectory = qScopeGuard([&] { previousHelperDirectory.isNull() ? qunsetenv("SOCIETY_HELPER_DIRECTORY") : qputenv("SOCIETY_HELPER_DIRECTORY", previousHelperDirectory); });
        qputenv("SOCIETY_HELPER_DIRECTORY", directory.path().toUtf8());
        CTEST_CHECK(Helper::defaultDirectory().isEmpty());
        qunsetenv("SOCIETY_HELPER_DIRECTORY");
        const auto package = directory.path() + "/selected.societycontainer";
        CTEST_CHECK(QDir().mkdir(package));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(package));
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto cleanup = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", previous); });
        qputenv("SOCIETY_CONTAINER_PATH", package.toUtf8());
        const auto inside = package + "/.society-runtime/isolated";
        qputenv("SOCIETY_HELPER_DIRECTORY", inside.toUtf8());
        CTEST_COMPARE(Helper::defaultDirectory(), inside);
        CTEST_CHECK(!QFileInfo::exists(inside));
        qunsetenv("SOCIETY_HELPER_DIRECTORY");
    }

    void resumedObserverRequiresFreshEvidence()
    {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/resume-XXXXXX")));
        Helper a, b;
        const ObservationOptions options{directory.path(), 100, 5000};
        CTEST_CHECK(a.start({"com.iisacc.a", "A", "1"}, options));
        CTEST_CHECK(b.start({"com.iisacc.b", "B", "1"}, options));
        CTEST_WAIT_COMPARE(a.peers().size(), 1);
        helper_ctest::SignalRecorder departed(&a, &Helper::peerDisappeared);
        helper_ctest::SignalRecorder appeared(&a, &Helper::peerAppeared);
        // Simulate a suspended app event loop; wall-clock timestamps are irrelevant.
        QThread::msleep(5500);
        CTEST_WAIT_COMPARE(departed.size(), 1);
        CTEST_COMPARE(qvariant_cast<DepartureReason>(departed.first()[1]), DepartureReason::TimedOut);
        CTEST_WAIT_COMPARE(appeared.size(), 1);
        CTEST_COMPARE(a.peers().first().instanceId, b.self().instanceId);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    ObservationTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("asynchronousLifecycleKeepsOwnerResponsive", [&] { FlushDeferredDeletes cleanup; tests.asynchronousLifecycleKeepsOwnerResponsive(); });
    runner.run("asynchronousStartCanBeStoppedBeforeIoCompletes", [&] { FlushDeferredDeletes cleanup; tests.asynchronousStartCanBeStoppedBeforeIoCompletes(); });
    runner.run("asynchronousStopPreservesSynchronousDeliveryAndAllowsRestart", [&] { FlushDeferredDeletes cleanup; tests.asynchronousStopPreservesSynchronousDeliveryAndAllowsRestart(); });
    runner.run("helperCanDeferStorageDiscovery", [&] { FlushDeferredDeletes cleanup; tests.helperCanDeferStorageDiscovery(); });
    runner.run("transientUnreadableRecordDoesNotWithdrawAnObservedPeer", [&] { FlushDeferredDeletes cleanup; tests.transientUnreadableRecordDoesNotWithdrawAnObservedPeer(); });
    runner.run("presenceSurvivesAnOpenProviderReadSnapshot", [&] { FlushDeferredDeletes cleanup; tests.presenceSurvivesAnOpenProviderReadSnapshot(); });
    runner.run("presenceResponsiveness", [&] { FlushDeferredDeletes cleanup; tests.presenceResponsiveness(); });
    runner.run("stoppedWorkCannotRepublishIntoTheNextSession", [&] { FlushDeferredDeletes cleanup; tests.stoppedWorkCannotRepublishIntoTheNextSession(); });
    runner.run("mutualDiscoveryAndDistinctInstances", [&] { FlushDeferredDeletes cleanup; tests.mutualDiscoveryAndDistinctInstances(); });
    runner.run("staleAndMalformedRecordsNeverBecomePeers", [&] { FlushDeferredDeletes cleanup; tests.staleAndMalformedRecordsNeverBecomePeers(); });
    runner.run("invalidConfigurationIsReported", [&] { FlushDeferredDeletes cleanup; tests.invalidConfigurationIsReported(); });
    runner.run("redirectedDirectoryStopsObservation", [&] { FlushDeferredDeletes cleanup; tests.redirectedDirectoryStopsObservation(); });
    runner.run("independentDirectoriesDoNotMix", [&] { FlushDeferredDeletes cleanup; tests.independentDirectoriesDoNotMix(); });
    runner.run("resumedObserverRequiresFreshEvidence", [&] { FlushDeferredDeletes cleanup; tests.resumedObserverRequiresFreshEvidence(); });
    return runner.result();
}
